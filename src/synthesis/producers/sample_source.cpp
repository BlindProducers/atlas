/* Copyright 2013-2019 Matt Tytel
 *
 * vital is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * vital is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with vital.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "sample_source.h"
#include "futils.h"
#include "sample_slices.h"
#include "prepared_effect_module.h"
#include "sample_stretch.h"
#include "synth_constants.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <new>
#include <thread>

namespace vital {

  namespace {
    const std::string kDefaultName = "White Noise";
    constexpr int kSampleTransposeQuantizeCustomScale = 14;
    constexpr int kSampleTransposeQuantizeScaleMasks[] = {
      0,
      (1 << 12) - 1,
      (1 << 0) | (1 << 2) | (1 << 4) | (1 << 5) | (1 << 7) | (1 << 9) | (1 << 11),
      (1 << 0) | (1 << 2) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 8) | (1 << 10),
      (1 << 0) | (1 << 2) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 8) | (1 << 11),
      (1 << 0) | (1 << 2) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 9) | (1 << 11),
      (1 << 0) | (1 << 2) | (1 << 4) | (1 << 7) | (1 << 9),
      (1 << 0) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 10),
      (1 << 0) | (1 << 3) | (1 << 5) | (1 << 6) | (1 << 7) | (1 << 10),
      (1 << 0) | (1 << 2) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 9) | (1 << 10),
      (1 << 0) | (1 << 1) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 8) | (1 << 10),
      (1 << 0) | (1 << 2) | (1 << 4) | (1 << 6) | (1 << 7) | (1 << 9) | (1 << 11),
      (1 << 0) | (1 << 2) | (1 << 4) | (1 << 5) | (1 << 7) | (1 << 9) | (1 << 10),
      (1 << 0) | (1 << 1) | (1 << 3) | (1 << 5) | (1 << 6) | (1 << 8) | (1 << 10),
      -1,
    };

    int rotateSampleTransposeQuantizeMask(int mask, int key) {
      int result = 0;
      key = std::max(0, std::min(kNotesPerOctave - 1, key));
      for (int i = 0; i < kNotesPerOctave; ++i) {
        if ((mask >> i) & 1)
          result |= 1 << ((i + key) % kNotesPerOctave);
      }
      return result;
    }

    const mono_float kUpsampleCoefficients[SampleSource::kNumUpsampleTaps] = {
      -0.000159813115702086552469274316479186382f,
      0.000225405365781280835058009159865832771f,
      -0.000378616814007205900686342525673921955f,
      0.000594907533596884547516525643118256994f,
      -0.000890530515941817101682742574553230952f,
      0.001284040046393844676508866342601322685f,
      -0.001796543223638378920792302295694753411f,
      0.002451862103068884121692683208948437823f,
      -0.003276873018553504678107568537370752892f,
      0.004302012661141991003987961050825106213f,
      -0.005561976429934398571952591794342879439f,
      0.007097105459677621741576558633823879063f,
      -0.008955232561651555595050311353588767815f,
      0.011195057708851860467369476737076183781f,
      -0.013890548104646217864033275191104621626f,
      0.017139719620821350365424962092220084742f,
      -0.021077036318492142763503238711564335972f,
      0.025897497908177177783350941808748757467f,
      -0.03189749744607761616776997470878995955f,
      0.039555400754278852160084056777122896165f,
      -0.049699764879031965714162311087420675904f,
      0.063901297378209126476278356676630210131f,
      -0.08553732517833501081128133591846562922f,
      0.123410206086688845061871688812971115112f,
      -0.209837893291539345774765479291090741754f,
      0.63582677174146173815216798175242729485f,
      0.63582677174146173815216798175242729485f,
      -0.209837893291539345774765479291090741754f,
      0.123410206086688845061871688812971115112f,
      -0.08553732517833501081128133591846562922f,
      0.063901297378209126476278356676630210131f,
      -0.049699764879031965714162311087420675904f,
      0.039555400754278852160084056777122896165f,
      -0.03189749744607761616776997470878995955f,
      0.025897497908177177783350941808748757467f,
      -0.021077036318492142763503238711564335972f,
      0.017139719620821350365424962092220084742f,
      -0.013890548104646217864033275191104621626f,
      0.011195057708851860467369476737076183781f,
      -0.008955232561651555595050311353588767815f,
      0.007097105459677621741576558633823879063f,
      -0.005561976429934398571952591794342879439f,
      0.004302012661141991003987961050825106213f,
      -0.003276873018553504678107568537370752892f,
      0.002451862103068884121692683208948437823f,
      -0.001796543223638378920792302295694753411f,
      0.001284040046393844676508866342601322685f,
      -0.000890530515941817101682742574553230952f,
      0.000594907533596884547516525643118256994f,
      -0.000378616814007205900686342525673921955f,
      0.000225405365781280835058009159865832771f,
      -0.000159813115702086552469274316479186382f
    };

    const mono_float kDownsampleCoefficients[SampleSource::kNumDownsampleTaps] = {
      -0.0013796309221920304f,
      -0.0008322130675804714f,
      0.0030100376204235577f,
      0.00666031332700994f,
      0.0040620073330527315f,
      -0.003019073425031439f,
      -0.004450269579432283f,
      0.0030526281279541555f,
      0.007614361286489334f,
      -0.000546514301955849f,
      -0.010099270019478761f,
      -0.003465846383906444f,
      0.011760981765402261f,
      0.009402148654924303f,
      -0.011429260748035207f,
      -0.016935843679984037f,
      0.008026778073943279f,
      0.025557280950428782f,
      -0.0002093220301655805f,
      -0.03448379812688787f,
      -0.013983156365753766f,
      0.04279770831566429f,
      0.03889228625534586f,
      -0.049566024787935245f,
      -0.09025827224454164f,
      0.05398926693924448f,
      0.31285587793730246f,
      0.4444714418837066f,
      0.31285587793730246f,
      0.05398926693924448f,
      -0.09025827224454164f,
      -0.049566024787935245f,
      0.03889228625534586f,
      0.04279770831566429f,
      -0.013983156365753766f,
      -0.03448379812688787f,
      -0.0002093220301655805f,
      0.025557280950428782f,
      0.008026778073943279f,
      -0.016935843679984037f,
      -0.011429260748035207f,
      0.009402148654924303f,
      0.011760981765402261f,
      -0.003465846383906444f,
      -0.010099270019478761f,
      -0.000546514301955849f,
      0.007614361286489334f,
      0.0030526281279541555f,
      -0.004450269579432283f,
      -0.003019073425031439f,
      0.0040620073330527315f,
      0.00666031332700994f,
      0.0030100376204235577f,
      -0.0008322130675804714f,
      -0.0013796309221920304f
    };

    force_inline mono_float getFilteredSample(const mono_float* buffer, int index, int size) {
      int radius = SampleSource::kNumDownsampleTaps / 2;
      int start = std::max(0, index - radius);
      int end = std::min(size - 1, index + radius);
      mono_float total = 0.0f;
      for (int i = start; i <= end; ++i) {
        mono_float coefficient = kDownsampleCoefficients[i - index + radius];
        total += coefficient *  buffer[i];
      }
      return total;
    }

    // The same, reading the buffer as one cycle of a loop whose samples lie
    // `stride` apart.
    force_inline mono_float getFilteredLoopSample(const mono_float* buffer, int index, int size, int stride = 1) {
      int radius = SampleSource::kNumDownsampleTaps / 2;
      mono_float total = 0.0f;
      // Away from the ends of the cycle no tap wraps, which spares each one
      // its remainder: most of what this costs on a long file.
      const long long reach = static_cast<long long>(radius) * stride;
      if (index >= reach && index + reach < size) {
        const mono_float* first = buffer + (index - radius * stride);
        for (int i = 0; i < SampleSource::kNumDownsampleTaps; ++i)
          total += kDownsampleCoefficients[i] * first[i * stride];
        return total;
      }
      for (int i = 0; i < SampleSource::kNumDownsampleTaps; ++i) {
        int buffer_index = (index + (i - radius) * stride) % size;
        total += kDownsampleCoefficients[i] * buffer[buffer_index < 0 ? buffer_index + size : buffer_index];
      }
      return total;
    }


    force_inline mono_float getInterpolatedSample(const mono_float* buffer, int index, int size) {
      int radius = SampleSource::kNumUpsampleTaps / 2;
      int start = std::max(0, index - radius + 1);
      int end = std::min(size - 1, index + radius);
      mono_float total = 0.0f;
      for (int i = start; i <= end; ++i) {
        int coefficient_index = i - index + radius - 1;
        VITAL_ASSERT(coefficient_index >= 0 && coefficient_index < SampleSource::kNumUpsampleTaps);
        mono_float coefficient = kUpsampleCoefficients[coefficient_index];
        total += coefficient * buffer[i];
      }
      return total;
    }

    // The same, reading the buffer as one cycle of a loop.
    force_inline mono_float getInterpolatedLoopSample(const mono_float* buffer, int index, int size) {
      int radius = SampleSource::kNumUpsampleTaps / 2;
      mono_float total = 0.0f;
      // As in getFilteredLoopSample: no tap wraps away from the ends.
      if (index >= radius - 1 && index + radius < size) {
        const mono_float* first = buffer + (index - radius + 1);
        for (int i = 0; i < SampleSource::kNumUpsampleTaps; ++i)
          total += kUpsampleCoefficients[i] * first[i];
        return total;
      }
      for (int i = index - radius + 1; i <= index + radius; ++i)
        total += kUpsampleCoefficients[i - index + radius - 1] * buffer[(i % size + size) % size];
      return total;
    }

    void upsample(const mono_float* original, mono_float* dest, int original_size, int dest_size) {
      for (int i = 0; i < original_size; ++i) {
        float value1 = original[i];
        float value2 = getInterpolatedSample(original, i, original_size);
        dest[2 * i] = value1;
        dest[2 * i + 1] = value2;
      }
    }

    void upsampleLoop(const mono_float* original, mono_float* dest, int original_size) {
      for (int i = 0; i < original_size; ++i) {
        dest[2 * i] = original[i];
        dest[2 * i + 1] = getInterpolatedLoopSample(original, i, original_size);
      }
    }

    // Pads a loop buffer's `size` samples on both sides with the other end of
    // the loop, so the reads across its wrap see one cycle run into the next.
    void wrapLoopPadding(mono_float* buffer, int size) {
      mono_float* data = buffer + Sample::kBufferSamples;
      for (int i = 1; i <= Sample::kBufferSamples; ++i) {
        data[-i] = data[(size - i % size) % size];
        data[size + i - 1] = data[(i - 1) % size];
      }
    }

    void downsample(const mono_float* original, mono_float* dest, int original_size, int dest_size) {
      for (int i = 0; i < dest_size; ++i)
        dest[i] = getFilteredSample(original, 2 * i, original_size);
    }

    void downsampleLoop(const mono_float* original, mono_float* dest, int original_size, int dest_size) {
      for (int i = 0; i < dest_size; ++i)
        dest[i] = getFilteredLoopSample(original, 2 * i, original_size);
    }

    void createBandLimitedBuffers(std::vector<std::unique_ptr<mono_float[]>>& destination,
                                  std::vector<std::unique_ptr<mono_float[]>>& loop_destination,
                                  const mono_float* buffer, int size) {
      destination.push_back(std::make_unique<mono_float[]>(size + 2 * Sample::kBufferSamples));
      loop_destination.push_back(std::make_unique<mono_float[]>(size + 2 * Sample::kBufferSamples));

      mono_float* play_buffer = destination.back().get();
      mono_float* loop_buffer = loop_destination.back().get();
      memcpy(play_buffer + Sample::kBufferSamples, buffer, size * sizeof(mono_float));
      memcpy(loop_buffer + Sample::kBufferSamples, buffer, size * sizeof(mono_float));

      for (int i = 0; i < Sample::kBufferSamples; ++i) {
        play_buffer[i] = 0.0f;
        play_buffer[size + Sample::kBufferSamples + i] = 0.0f;
      }
      wrapLoopPadding(loop_buffer, size);

      // The loop buffers hold the file as one cycle of a loop at every
      // resolution, so a loop of the whole file (a single cycle) wraps cleanly.
      int current_size = size;
      for (int i = 0; i < Sample::kUpsampleTimes; ++i) {
        int upsampled_size = current_size * 2;
        int num_samples = upsampled_size + 2 * Sample::kBufferSamples;
        destination.insert(destination.begin(), std::make_unique<mono_float[]>(num_samples));
        loop_destination.insert(loop_destination.begin(), std::make_unique<mono_float[]>(num_samples));

        mono_float* next_buffer = destination.front().get();
        mono_float* next_loop_buffer = loop_destination.front().get();

        upsample(play_buffer + Sample::kBufferSamples, next_buffer + Sample::kBufferSamples,
                 current_size, upsampled_size);
        upsampleLoop(loop_buffer + Sample::kBufferSamples, next_loop_buffer + Sample::kBufferSamples, current_size);
        wrapLoopPadding(next_loop_buffer, upsampled_size);
        play_buffer = next_buffer;
        loop_buffer = next_loop_buffer;
        current_size = upsampled_size;
      }

      play_buffer = destination.back().get();
      loop_buffer = loop_destination.back().get();
      current_size = size;

      // Halving a cycle of odd length leaves it a fraction of a sample long,
      // which a loop buffer cannot wrap at. A short file's loop is filtered
      // at its own rate instead, where every resolution's samples (every
      // stride-th sample of the cycle) wrap exactly. A longer file's wrap is
      // too rare to hear, and halves like its play buffer.
      constexpr int kMaxExactLoopSize = 1 << 14;
      const bool exact_loop = size <= kMaxExactLoopSize;
      std::vector<mono_float> cycle, filtered;
      if (exact_loop) {
        cycle.assign(buffer, buffer + size);
        filtered.resize(static_cast<size_t>(size));
      }
      int stride = 1;

      while (current_size >= Sample::kMinSize) {
        int next_size = (current_size + 1) / 2;
        destination.push_back(std::make_unique<mono_float[]>(next_size + 2 * Sample::kBufferSamples));
        loop_destination.push_back(std::make_unique<mono_float[]>(next_size + 2 * Sample::kBufferSamples));

        mono_float* next_buffer = destination.back().get();
        mono_float* next_loop_buffer = loop_destination.back().get();

        downsample(play_buffer + Sample::kBufferSamples, next_buffer + Sample::kBufferSamples,
                   current_size, next_size);
        if (exact_loop) {
          for (int i = 0; i < size; ++i)
            filtered[static_cast<size_t>(i)] = getFilteredLoopSample(cycle.data(), i, size, stride);
          std::swap(cycle, filtered);
          stride *= 2;
          for (int i = -Sample::kBufferSamples; i < next_size + Sample::kBufferSamples; ++i) {
            const int index = static_cast<int>((static_cast<long long>(i) * stride) % size);
            next_loop_buffer[Sample::kBufferSamples + i] = cycle[static_cast<size_t>(index < 0 ? index + size : index)];
          }
        }
        else {
          downsampleLoop(loop_buffer + Sample::kBufferSamples, next_loop_buffer + Sample::kBufferSamples,
                         current_size, next_size);
          wrapLoopPadding(next_loop_buffer, next_size);
        }

        for (int i = 0; i < Sample::kBufferSamples; ++i) {
          next_buffer[i] = 0.0f;
          next_buffer[next_size + Sample::kBufferSamples + i] = 0.0f;
        }

        play_buffer = next_buffer;
        loop_buffer = next_loop_buffer;
        current_size = next_size;
      }
    }
  }

  namespace {
    mono_float peakOf(const mono_float* buffer, int size) {
      mono_float peak = 0.0f;
      for (int i = 0; i < size; ++i)
        peak = std::max(peak, std::fabs(buffer[i]));
      return peak;
    }

    // The built-in default, a second of white noise filtered into every
    // resolution. It never changes and takes milliseconds to build, and an
    // instance has eight slots, so the process builds each variant once and
    // every sample on it shares the data. A variant is a noise of its own:
    // two slots playing the same one would double instead of mixing.
    std::shared_ptr<Sample::SampleData> defaultData(int variant) {
      struct Pool {
        // Makes SampleData's leak counter (debug builds) before the pool, so
        // that it outlives the data the pool still holds as the process ends.
        Pool() { Sample::SampleData counted(0, kDefaultSampleRate, false); }
        std::mutex mutex;
        std::map<int, std::shared_ptr<Sample::SampleData>> data;
      };
      static Pool pool;
      std::lock_guard<std::mutex> lock(pool.mutex);
      std::shared_ptr<Sample::SampleData>& data = pool.data[variant];
      if (data == nullptr) {
        std::unique_ptr<mono_float[]> buffer = std::make_unique<mono_float[]>(Sample::kDefaultSampleLength);
        utils::RandomGenerator random_generator(-0.9f, 0.9f);
        for (int i = 0; i < Sample::kDefaultSampleLength; ++i)
          buffer[i] = random_generator.next();
        data = Sample::createData(buffer.get(), nullptr, Sample::kDefaultSampleLength, kDefaultSampleRate);
      }
      return data;
    }
  }

  Sample::Sample(int default_variant) : name_(kDefaultName), current_data_(nullptr), active_audio_data_(nullptr),
                                        default_variant_(default_variant) {
    init();
  }

  std::unique_ptr<Sample::SampleData> Sample::createData(const mono_float* left_buffer, const mono_float* right_buffer,
                                                         int size, int sample_rate) {
    size = std::min(size, kMaxLength);
    auto data = std::make_unique<SampleData>(size, sample_rate, right_buffer != nullptr);
    createBandLimitedBuffers(data->left_buffers, data->left_loop_buffers, left_buffer, size);
    data->peak = peakOf(left_buffer, size);
    if (right_buffer != nullptr) {
      createBandLimitedBuffers(data->right_buffers, data->right_loop_buffers, right_buffer, size);
      data->peak = std::max(data->peak, peakOf(right_buffer, size));
    }
    return data;
  }

  void Sample::loadData(std::unique_ptr<SampleData> data) {
    installData(std::move(data));
  }

  void Sample::installData(std::shared_ptr<SampleData> data) {
    VITAL_ASSERT(active_audio_data_.is_lock_free());
    is_default_ = false;
    std::shared_ptr<SampleData> old_data = std::move(data_);
    data_ = std::move(data);

    current_data_ = data_.get();
    while (active_audio_data_.load())
      std::this_thread::yield(); // Wait for audio thread to finish using old_data.
    resetSlices();

    last_browsed_file_.clear();
    if (load_callback_) {
      const mono_float* right = rightChannel();
      load_callback_(leftChannel(), right != nullptr ? right : leftChannel(), data_->length, data_->sample_rate);
    }
  }

  void Sample::loadSample(const mono_float* buffer, int size, int sample_rate) {
    loadData(createData(buffer, nullptr, size, sample_rate));
  }

  void Sample::loadSample(const mono_float* left_buffer, const mono_float* right_buffer, int size, int sample_rate) {
    loadData(createData(left_buffer, right_buffer, size, sample_rate));
  }

  void Sample::resetSlices() {
    setSlices(sample_slices::grid(data_ ? data_->length : 0, kDefaultSliceDivisions));
  }

  void Sample::setSlices(const std::vector<int>& starts, const std::vector<mono_float>& pitches) {
    std::vector<Slice> list(starts.size());
    for (size_t i = 0; i < starts.size(); ++i) {
      list[i].start = starts[i];
      list[i].pitch = i < pitches.size() ? pitches[i] : 0.0f;
    }
    setSlices(list);
  }

  // Keeps the slices whose starts fit: ascending, at least kMinSliceLength
  // apart, inside the file, and the first at 0, up to kMaxSlices.
  void Sample::setSlices(const std::vector<Slice>& list) {
    auto slices = std::make_unique<Slices>();
    const int length = data_ ? data_->length : 0;
    const auto place = [&](int at, const Slice& slice) {
      slices->starts[at] = at == 0 ? 0 : slice.start;
      slices->pitches[at] = std::max(-kMaxSlicePitch, std::min(kMaxSlicePitch, slice.pitch));
      slices->gains[at] = utils::dbToMagnitude(std::max(kMinSliceGain, std::min(kMaxSliceGain, slice.gain)));
      slices->reverses[at] = slice.reverse ? 1.0f : 0.0f;
    };
    place(0, list.empty() ? Slice() : list.front());
    slices->count = 1;
    for (size_t i = 1; i < list.size(); ++i) {
      if (slices->count >= kMaxSlices)
        break;
      const int start = list[i].start;
      const int previous = slices->starts[slices->count - 1];
      if (start >= previous + kMinSliceLength && start <= length - kMinSliceLength)
        place(slices->count++, list[i]);
    }
    Slices* old = current_slices_.exchange(slices.get(), std::memory_order_acq_rel);
    while (active_slices_.load(std::memory_order_acquire) == old && old != nullptr)
      std::this_thread::yield(); // Wait for the audio thread to let go of the old set.
    slices_ = std::move(slices);
  }

  std::vector<Sample::Slice> Sample::sliceList() const {
    const Slices* slices = current_slices_.load(std::memory_order_acquire);
    std::vector<Slice> list;
    if (slices == nullptr)
      return list;
    list.resize(static_cast<size_t>(slices->count));
    for (int i = 0; i < slices->count; ++i) {
      list[static_cast<size_t>(i)].start = slices->starts[i];
      list[static_cast<size_t>(i)].pitch = slices->pitches[i];
      list[static_cast<size_t>(i)].gain = utils::magnitudeToDb(slices->gains[i]);
      list[static_cast<size_t>(i)].reverse = slices->reverses[i] != 0.0f;
    }
    return list;
  }

  std::vector<int> Sample::slices() const {
    const Slices* slices = current_slices_.load(std::memory_order_acquire);
    if (slices == nullptr)
      return {};
    return std::vector<int>(slices->starts, slices->starts + slices->count);
  }

  std::vector<mono_float> Sample::slicePitches() const {
    const Slices* slices = current_slices_.load(std::memory_order_acquire);
    if (slices == nullptr)
      return {};
    return std::vector<mono_float>(slices->pitches, slices->pitches + slices->count);
  }

  json Sample::slicesToJson() const {
    json starts = json::array(), pitches = json::array(), gains = json::array(), reverses = json::array();
    for (const Slice& slice : sliceList()) {
      starts.push_back(slice.start);
      pitches.push_back(std::round(slice.pitch * 100.0f) / 100.0f);
      gains.push_back(std::round(slice.gain * 100.0f) / 100.0f);
      reverses.push_back(slice.reverse ? 1 : 0);
    }
    json data;
    data["starts"] = std::move(starts);
    data["pitches"] = std::move(pitches);
    data["gains"] = std::move(gains);
    data["reverses"] = std::move(reverses);
    return data;
  }

  void Sample::slicesFromJson(const json& data) {
    std::vector<Slice> list;
    const json* starts = data.is_array() ? &data : data.is_object() && data.count("starts") ? &data["starts"] : nullptr;
    if (starts != nullptr && starts->is_array()) {
      for (const auto& start : *starts) {
        if (start.is_number_integer()) {
          Slice slice;
          slice.start = start.get<int>();
          list.push_back(slice);
        }
      }
    }
    const auto read = [&](const char* key, const std::function<void(Slice&, const json&)>& apply) {
      if (!data.is_object() || !data.count(key) || !data[key].is_array())
        return;
      const json& values = data[key];
      for (size_t i = 0; i < list.size() && i < values.size(); ++i) {
        if (values[i].is_number())
          apply(list[i], values[i]);
      }
    };
    read("pitches", [](Slice& slice, const json& value) { slice.pitch = value.get<mono_float>(); });
    read("gains", [](Slice& slice, const json& value) { slice.gain = value.get<mono_float>(); });
    read("reverses", [](Slice& slice, const json& value) { slice.reverse = value.get<mono_float>() != 0.0f; });
    setSlices(list);
  }

  void Sample::init() {
    name_ = kDefaultName;
    installData(defaultData(default_variant_));
    is_default_ = true;
  }

  json Sample::stateToJson() {
    json data;
    data["name"] = name_;
    // Embedded audio remains self-contained; this optional path only anchors
    // next/previous browsing when the original file still exists locally.
    if (!last_browsed_file_.empty())
      data["last_browsed_file"] = last_browsed_file_;
    data["length"] = data_->length;
    data["sample_rate"] = data_->sample_rate;
    std::unique_ptr<int16_t[]> pcm_data = std::make_unique<int16_t[]>(data_->length);
    utils::floatToPcmData(pcm_data.get(), data_->left_buffers[kUpsampleTimes].get(), data_->length);
    String encoded = Base64::toBase64(pcm_data.get(), sizeof(int16_t) * data_->length);
    data["samples"] = encoded.toStdString();
    if (data_->stereo) {
      utils::floatToPcmData(pcm_data.get(), data_->right_buffers[kUpsampleTimes].get(), data_->length);
      String encoded_stereo = Base64::toBase64(pcm_data.get(), sizeof(int16_t) * data_->length);
      data["samples_stereo"] = encoded_stereo.toStdString();
    }
    return data;
  }

  void Sample::jsonToState(json data) {
    name_ = "";
    if (data.count("name"))
      name_ = data["name"].get<std::string>();

    // A damaged or hand-edited preset can claim any length: keep to what the
    // encoded data holds, and leave the sample alone when there is none.
    static constexpr int kMaxSamples = 1 << 27;
    const int claimed_length = data.count("length") && data["length"].is_number() ? data["length"].get<int>() : 0;
    const int sample_rate = data.count("sample_rate") && data["sample_rate"].is_number()
                                ? data["sample_rate"].get<int>() : 0;
    if (claimed_length <= 0 || claimed_length > kMaxSamples || sample_rate <= 0 ||
        !data.count("samples") || !data["samples"].is_string())
      return;

    MemoryOutputStream decoded;
    std::string wave_data = data["samples"];
    Base64::convertFromBase64(decoded, wave_data);
    const int length = std::min<int>(claimed_length, static_cast<int>(decoded.getDataSize() / sizeof(int16_t)));
    if (length <= 0)
      return;
    std::unique_ptr<int16_t[]> pcm_data = std::make_unique<int16_t[]>(length);
    memcpy(pcm_data.get(), decoded.getData(), length * sizeof(int16_t));
    std::unique_ptr<mono_float[]> buffer = std::make_unique<mono_float[]>(length);
    utils::pcmToFloatData(buffer.get(), pcm_data.get(), length);

    bool stereo = data.count("samples_stereo") && data["samples_stereo"].is_string();
    MemoryOutputStream decoded_stereo;
    if (stereo) {
      std::string wave_data_stereo = data["samples_stereo"];
      Base64::convertFromBase64(decoded_stereo, wave_data_stereo);
      stereo = decoded_stereo.getDataSize() >= static_cast<size_t>(length) * sizeof(int16_t);
    }
    if (stereo) {
      std::unique_ptr<int16_t[]> pcm_data_stereo = std::make_unique<int16_t[]>(length);
      memcpy(pcm_data_stereo.get(), decoded_stereo.getData(), length * sizeof(int16_t));

      std::unique_ptr<mono_float[]> buffer_stereo = std::make_unique<mono_float[]>(length);
      utils::pcmToFloatData(buffer_stereo.get(), pcm_data_stereo.get(), length);
      loadSample(buffer.get(), buffer_stereo.get(), length, sample_rate);
    }
    else
      loadSample(buffer.get(), length, sample_rate);
    if (data.count("last_browsed_file") && data["last_browsed_file"].is_string())
      last_browsed_file_ = data["last_browsed_file"].get<std::string>();
  }

  struct SampleSource::StretchState {
    struct Voice {
      atlas::SampleStretcher stretcher;
      mono_float owed = 0.0f;   // input samples due to the stretcher but not yet read, below one
      bool running = false;     // started since the voice began or the mode changed
    };
    std::array<Voice, kVoicesPerProcessor> voices;
    // The voices feed their stretchers one after another, through these.
    std::vector<mono_float> input[atlas::SampleStretcher::kChannels];
    std::vector<mono_float> start[atlas::SampleStretcher::kChannels];
  };

  namespace {
    constexpr int kMaxStretchInput = static_cast<int>(kMaxBufferSize * kMaxOversample * atlas::SampleStretcher::kMaxSpeed) + 2;
    constexpr mono_float kSampleRetriggerFadeSeconds = 0.003f;
    // A slice fades in over its first 2 ms and out over its last, so the cuts
    // the slicer makes in the file never click.
    constexpr mono_float kSliceFadeSeconds = 0.002f;
    constexpr int kStutterRepeats[SampleSource::kNumStutterChoices] = { 0, 2, 3, 4, 6, 8, 12, 16 };
    // One step of a voice's Stutter Chance sequence (a 32-bit LCG), in [0, 1).
    force_inline mono_float nextStutterRoll(uint32_t& seed) {
      seed = seed * 1664525u + 1013904223u;
      return static_cast<mono_float>(seed >> 8) * (1.0f / 16777216.0f);
    }
    // Slice Decay at 100% takes a slice 40 dB down in 50 ms; lower settings
    // take longer, as the square of the setting.
    constexpr mono_float kSliceDecayShortSeconds = 0.05f;
    constexpr mono_float kSliceDecayRangeLog2 = 6.643856f;   // log2(100), 40 dB
  }

  SampleSource::SampleSource() : SampleSource(nullptr) { }

  SampleSource::SampleSource(std::shared_ptr<Sample> sample) :
      Processor(kNumInputs, kNumOutputs), pan_amplitude_(0.0f), phase_inc_(0.0f),
      random_generator_(0.0f, 1.0f), stretch_shared_(std::make_shared<StretchShared>()) {
    transpose_quantize_ = 0;
    last_quantized_transpose_ = 0.0f;
    slice_index_ = -1.0f;
    slice_pitch_ = 0.0f;
    slice_gain_ = 1.0f;
    slice_reverse_mask_ = 0;
    stutter_count_ = 0.0f;
    stutter_remaining_ = -1.0f;
    reverse_mask_ = 0;
    sample_index_ = 0;
    sample_fraction_ = 0.0f;
    global_sample_index_ = 0;
    global_sample_fraction_ = 0.0f;
    low_cut_.reset(constants::kFullMask);
    high_cut_.reset(constants::kFullMask);
    last_output_ = 0.0f;

    sample_ = sample ? std::move(sample) : std::make_shared<Sample>();
    phase_output_ = std::make_shared<cr::Output>();
    std::lock_guard<std::mutex> lock(stretch_shared_->mutex);
    stretch_shared_->sources.push_back(this);
  }

  // A voice copy: the same state as the default copy made, plus its own stretchers.
  SampleSource::SampleSource(const SampleSource& other) :
      Processor(other), pan_amplitude_(other.pan_amplitude_), normalize_gain_(other.normalize_gain_),
      transpose_quantize_(other.transpose_quantize_), last_quantized_transpose_(other.last_quantized_transpose_),
      slice_index_(other.slice_index_), slice_pitch_(other.slice_pitch_), slice_gain_(other.slice_gain_),
      slice_reverse_mask_(other.slice_reverse_mask_), stutter_count_(other.stutter_count_),
      stutter_remaining_(other.stutter_remaining_), stutter_seed_(other.stutter_seed_), reverse_mask_(other.reverse_mask_),
      sample_index_(other.sample_index_), sample_fraction_(other.sample_fraction_),
      global_sample_index_(other.global_sample_index_), global_sample_fraction_(other.global_sample_fraction_),
      phase_inc_(other.phase_inc_), bounce_mask_(other.bounce_mask_), low_cut_(other.low_cut_),
      high_cut_(other.high_cut_), last_output_(other.last_output_), phase_output_(other.phase_output_),
      random_generator_(other.random_generator_), modulation_(other.modulation_), sample_(other.sample_),
      stretch_shared_(other.stretch_shared_) {
    std::lock_guard<std::mutex> lock(stretch_shared_->mutex);
    stretch_shared_->sources.push_back(this);
    if (stretch_shared_->reserved)
      reserveStretchVoices();
  }

  SampleSource::~SampleSource() {
    std::lock_guard<std::mutex> lock(stretch_shared_->mutex);
    auto& sources = stretch_shared_->sources;
    sources.erase(std::remove(sources.begin(), sources.end(), this), sources.end());
  }

  void SampleSource::setStretchMemory(bool reserve) {
    std::lock_guard<std::mutex> lock(stretch_shared_->mutex);
    stretch_shared_->reserved = reserve;
    for (SampleSource* source : stretch_shared_->sources) {
      if (reserve)
        source->reserveStretchVoices();
      else
        source->stretch_.reset();
    }
  }

  void SampleSource::reserveStretchVoices() {
    const int rate = getSampleRate();
    if (stretch_ != nullptr && stretch_rate_ == rate)
      return;
    try {
      auto state = std::make_unique<StretchState>();
      for (StretchState::Voice& voice : state->voices)
        voice.stretcher.configure(rate);
      for (int channel = 0; channel < atlas::SampleStretcher::kChannels; ++channel) {
        state->input[channel].assign(kMaxStretchInput, 0.0f);
        state->start[channel].assign(state->voices[0].stretcher.startLength(atlas::SampleStretcher::kMaxSpeed) + 1, 0.0f);
      }
      stretch_ = std::move(state);
      stretch_rate_ = rate;
    }
    catch (const std::bad_alloc&) {
      // Out of memory: the source plays plain.
      stretch_.reset();
      stretch_rate_ = 0;
    }
  }

  SampleSource::StretchState* SampleSource::stretchStateForBlock(int mode) {
    if (mode == kStretchOff)
      return nullptr;
    if (stretch_ == nullptr || stretch_rate_ != getSampleRate()) {
      // Offline, allocate now. In realtime, play plain until the message
      // thread has (a mode switched on by automation, or a new sample rate).
      if (RealtimeEffectScope::active()) {
        stretch_shared_->requested.store(true, std::memory_order_release);
        return nullptr;
      }
      setStretchMemory(true);
      if (stretch_ == nullptr || stretch_rate_ != getSampleRate())
        return nullptr;
    }
    return stretch_.get();
  }

  // Hides a step in one lane's output at `start`: the difference from the
  // sample before it dies away over 3 ms.
  void SampleSource::fadeFromPrevious(poly_float* raw_output, int num_samples, int lane, int start) {
    const int fade_samples = std::max(1, static_cast<int>(kSampleRetriggerFadeSeconds * getSampleRate()));
    if (start < 0 || start >= num_samples)
      return;
    const int end = std::min(num_samples, start + fade_samples);
    const mono_float previous_output = start > 0 ? raw_output[start - 1][lane] : last_output_[lane];
    const mono_float correction = previous_output - raw_output[start][lane];
    for (int i = start; i < end; ++i) {
      mono_float fade = 1.0f - (i - start + 1.0f) / fade_samples;
      raw_output[i].set(lane, raw_output[i][lane] + correction * fade);
    }
  }

  void SampleSource::process(int num_samples) {
    if (num_samples <= 0)
      return;
    sample_->markUsed();

    poly_float current_pan_amplitude = pan_amplitude_;
    poly_float input_pan = utils::clamp(input(kPan)->at(0), -1.0f, 1.0f);
    pan_amplitude_ = futils::panAmplitude(input_pan);

    // In Slicer mode a key picks a slice, not a pitch: slice 1 on the root
    // key and one more per semitone up, each at the file's own pitch.
    const bool slicer = utils::iclamp(static_cast<int>(input(kMode)->at(0)[0] + 0.5f), 0, kNumModes - 1) == kSlicer;
    poly_float input_midi = 0.0f;
    if (!slicer && input(kKeytrack)->at(0)[0])
      input_midi = input(kMidi)->at(0) - input(kRootKey)->at(0);

    int transpose_quantize = static_cast<int>(input(kTransposeQuantize)->at(0)[0]);
    int quantize_scale = static_cast<int>(input(kTransposeQuantizeScale)->at(0)[0]);
    if (quantize_scale != kSampleTransposeQuantizeCustomScale) {
      quantize_scale = std::max(0, std::min(kSampleTransposeQuantizeCustomScale - 1, quantize_scale));
      int key = static_cast<int>(input(kTransposeQuantizeKey)->at(0)[0]);
      int mode = static_cast<int>(input(kTransposeQuantizeMode)->at(0)[0]);
      transpose_quantize = rotateSampleTransposeQuantizeMask(kSampleTransposeQuantizeScaleMasks[quantize_scale], key);
      if (mode >= 1)
        transpose_quantize |= 1 << kNotesPerOctave;
    }

    poly_mask reset_mask = getResetMask(kReset);
    poly_mask retrigger_mask = getResetMask(kRetrigger);
    const poly_mask latch_mask = retrigger_mask | reset_mask;
    if (slicer) {
      // A note picks its slice, and the slice's own pitch, as it starts.
      const poly_float chosen = utils::round(input(kMidi)->at(0) - input(kRootKey)->at(0));
      slice_index_ = utils::maskLoad(slice_index_, chosen, latch_mask);
      const Sample::Slices* slices = sample_->activeSlices();
      const int count = slices != nullptr ? slices->count : 0;
      poly_float reversed = utils::toFloat(poly_int(slice_reverse_mask_ & 1u));
      for (int lane = 0; lane < poly_float::kSize; ++lane) {
        if (!latch_mask[lane])
          continue;
        const int index = static_cast<int>(slice_index_[lane]);
        const bool valid = index >= 0 && index < count;
        slice_pitch_.set(lane, valid ? slices->pitches[index] : 0.0f);
        slice_gain_.set(lane, valid ? slices->gains[index] : 1.0f);
        reversed.set(lane, valid ? slices->reverses[index] : 0.0f);
      }
      slice_reverse_mask_ = poly_float::greaterThan(reversed, 0.5f);
    }

    poly_float transpose = snapTranspose(input_midi, input(kTranspose)->at(0), transpose_quantize);
    if (slicer)
      transpose += slice_pitch_;
    transpose = utils::clamp(transpose + input(kTune)->at(0), kMinTranspose, kMaxTranspose);

    const int stretch_mode = utils::iclamp(static_cast<int>(input(slicer ? kSliceStretchMode : kStretchMode)->at(0)[0] + 0.5f),
                                           0, kNumStretchModes - 1);
    StretchState* stretch = stretchStateForBlock(stretch_mode);
    const bool stretch_transition = (stretch != nullptr) != stretching_;
    stretching_ = stretch != nullptr;

    mono_float sample_rate_ratio = (1.0f * sample_->activeSampleRate()) / getSampleRate();
    poly_float current_phase_inc = phase_inc_;
    if (stretch) {
      // The stretcher moves the pitch. The file is read at its own speed, and
      // the time stretch is in how many samples each block reads.
      phase_inc_ = sample_rate_ratio * (1 << Sample::kUpsampleTimes);
      current_phase_inc = phase_inc_;
    }
    else
      phase_inc_ = utils::centsToRatio(transpose * kCentsPerNote) * sample_rate_ratio * (1 << Sample::kUpsampleTimes);

    int audio_length = sample_->activeLength();
    poly_float audio_end = audio_length;
    poly_float minimum_region = Sample::kMinSize * (1 << Sample::kUpsampleTimes);
    // Either order of two points marks the part between them.
    poly_float region_a = utils::clamp(input(kStart)->at(0), 0.0f, 1.0f) * audio_end;
    poly_float region_b = utils::clamp(input(kEnd)->at(0), 0.0f, 1.0f) * audio_end;
    // Slicer: each lane's region is the slice its note picked when it began.
    // A key outside the slices plays nothing.
    poly_mask slice_valid_mask = constants::kFullMask;
    const bool slice_loop = slicer && input(kSliceLoop)->at(0)[0] != 0.0f;
    // A pass that rolls under Stutter Chance plays the first part of its slice
    // Stutter times over, filling the slice's length. A note rolls as it
    // starts, and a looping slice again at every pass.
    poly_float stutter_repeats = 0.0f;
    const poly_float stutter_chance = input(kStutterChance)->at(0);
    // Where each lane's slice ends, and where a pass that stutters now would.
    poly_float slice_end = 0.0f;
    poly_float stutter_end = 0.0f;
    const auto roll_pass = [&](int lane) {
      const int repeats = static_cast<int>(stutter_repeats[lane]);
      const bool stutters = nextStutterRoll(stutter_seed_[lane]) < stutter_chance[lane] && repeats > 0;
      stutter_count_.set(lane, stutters ? static_cast<mono_float>(repeats) : 0.0f);
      stutter_remaining_.set(lane, stutters ? static_cast<mono_float>(repeats - 1) : -1.0f);
    };
    if (slicer) {
      const Sample::Slices* slices = sample_->activeSlices();
      const int count = slices != nullptr ? slices->count : 0;
      // Play through runs on to the end of the file; a looping slice stays a slice.
      const bool play_through = input(kPlayThrough)->at(0)[0] != 0.0f && !slice_loop;
      const poly_float stutter_choice = input(kStutter)->at(0);
      for (int lane = 0; lane < poly_float::kSize; ++lane) {
        const int choice = utils::iclamp(static_cast<int>(stutter_choice[lane] + 0.5f), 0, kNumStutterChoices - 1);
        stutter_repeats.set(lane, static_cast<mono_float>(kStutterRepeats[choice]));
      }
      // A starting note seeds its voice's sequence once for both channels.
      for (int lane = 0; lane < poly_float::kSize; lane += 2) {
        if (!latch_mask[lane])
          continue;
        const uint32_t seed = static_cast<uint32_t>(random_generator_.next() * 16777216.0f);
        stutter_seed_[lane] = seed;
        stutter_seed_[lane + 1] = seed;
        roll_pass(lane);
        roll_pass(lane + 1);
      }
      poly_float valid = 0.0f;
      for (int lane = 0; lane < poly_float::kSize; ++lane) {
        const int index = static_cast<int>(slice_index_[lane]);
        if (index < 0 || index >= count) {
          region_a.set(lane, 0.0f);
          region_b.set(lane, 0.0f);
          continue;
        }
        const int start = std::min(audio_length, slices->starts[index] << Sample::kUpsampleTimes);
        const int end_of_slice = index + 1 < count
            ? std::min(audio_length, slices->starts[index + 1] << Sample::kUpsampleTimes) : audio_length;
        const auto part_end = [&](int repeats) {
          return start + std::max(Sample::kMinSize << Sample::kUpsampleTimes, (end_of_slice - start) / repeats);
        };
        const int repeats = static_cast<int>(stutter_count_[lane]);
        const int next_repeats = static_cast<int>(stutter_repeats[lane]);
        region_a.set(lane, static_cast<mono_float>(start));
        region_b.set(lane, static_cast<mono_float>(repeats > 0 ? part_end(repeats)
                                                                : play_through ? audio_length : end_of_slice));
        slice_end.set(lane, static_cast<mono_float>(end_of_slice));
        stutter_end.set(lane, static_cast<mono_float>(next_repeats > 0 ? part_end(next_repeats) : end_of_slice));
        valid.set(lane, 1.0f);
      }
      slice_valid_mask = poly_float::greaterThan(valid, 0.5f);
    }
    poly_float sample_start = utils::floor(utils::clamp(utils::min(region_a, region_b), 0.0f, audio_end - minimum_region));
    const auto region_end = [&](poly_float end) {
      return utils::floor(utils::clamp(end, sample_start + minimum_region, audio_end));
    };
    poly_float sample_end = region_end(utils::max(region_a, region_b));
    if (slice_loop) {
      slice_end = region_end(slice_end);
      stutter_end = region_end(stutter_end);
    }

    // A looping slice loops the whole slice, as it is.
    poly_float loop_a = slicer ? sample_start : utils::clamp(input(kLoopStart)->at(0), 0.0f, 1.0f) * audio_end;
    poly_float loop_b = slicer ? sample_end : utils::clamp(input(kLoopEnd)->at(0), 0.0f, 1.0f) * audio_end;
    // Loop end before Loop start loops that part backwards: the sample plays
    // forward to the loop, then reads it mirrored, and Bounce alternates from there.
    const poly_mask reverse_loop_mask = poly_float::lessThan(loop_b, loop_a);
    poly_float loop_start = utils::clamp(utils::min(loop_a, loop_b), sample_start, sample_end - minimum_region);
    poly_float loop_end = utils::clamp(utils::max(loop_a, loop_b), loop_start + minimum_region, sample_end);
    // Loop shift moves the loop as a whole, as far as the region allows at
    // its full length.
    poly_float loop_shift = slicer ? poly_float(0.0f) : utils::clamp(input(kLoopShift)->at(0), -1.0f, 1.0f) * audio_end;
    loop_shift = utils::clamp(loop_shift, sample_start - loop_start, sample_end - loop_end);
    loop_start = utils::floor(loop_start + loop_shift);
    loop_end = utils::floor(loop_end + loop_shift);
    poly_float loop_length = utils::max(loop_end - loop_start, 1.0f);
    poly_float loop_crossfade = slicer ? poly_float(0.0f)
                                       : utils::clamp(input(kLoopCrossfade)->at(0), 0.0f, 1.0f) * loop_length;
    loop_crossfade = utils::min(loop_crossfade, loop_length * 0.5f);

    // Reverse plays the region from its end. The index still counts up, over
    // the loop mirrored into that direction, and each read mirrors it back.
    // It is modulated, so each lane may read its own way, and may turn
    // around mid-note (the step that makes fades out below).
    // A reversed slice turns the slot's Reverse around.
    const poly_mask reverse_mask = poly_float::greaterThanOrEqual(input(kReverse)->at(0), 0.5f) ^
                                   (slicer ? slice_reverse_mask_ : poly_mask(0));
    poly_float region_mirror = sample_start + sample_end;
    {
      const poly_float reversed_loop_start = region_mirror - loop_end;
      const poly_float reversed_loop_end = region_mirror - loop_start;
      loop_start = utils::maskLoad(loop_start, reversed_loop_start, reverse_mask);
      loop_end = utils::maskLoad(loop_end, reversed_loop_end, reverse_mask);
    }

    int playback_mode = slicer ? kRetriggerPerNote : static_cast<int>(input(kPlaybackMode)->at(0)[0] + 0.5f);
    playback_mode = utils::iclamp(playback_mode, 0, kNumPlaybackModes - 1);
    poly_mask sample_reset_mask = playback_mode == kRetriggerPerNote ? retrigger_mask : poly_mask(0);
    poly_mask hard_sample_reset_mask = sample_reset_mask & reset_mask;
    poly_mask soft_sample_reset_mask = sample_reset_mask & ~reset_mask;
    poly_int retrigger_offset = input(kRetrigger)->source->trigger_offset;
    poly_float reset_offset = utils::toFloat(retrigger_offset);
    current_pan_amplitude = utils::maskLoad(current_pan_amplitude, pan_amplitude_, reset_mask);
    current_phase_inc = utils::maskLoad(current_phase_inc, phase_inc_, reset_mask | hard_sample_reset_mask);
    bounce_mask_ = bounce_mask_ & ~hard_sample_reset_mask;
    reset_offset *= current_phase_inc;
    
    poly_float reset_value = sample_start - reset_offset;
    poly_float soft_reset_value = sample_start;
    if (!slicer && input(kRandomPhase)->at(0)[0]) {
      reset_value = sample_start + (sample_end - sample_start) * random_generator_.next();
      reset_value = utils::maskLoad(reset_value,
                                    sample_start + (sample_end - sample_start) * random_generator_.next(),
                                    constants::kFirstMask);
      soft_reset_value = reset_value;
      reset_value -= reset_offset;
    }
    
    if (playback_mode == kContinuousGlobal) {
      sample_index_ = global_sample_index_;
      sample_fraction_ = global_sample_fraction_;
    }

    // Plain, a restarting voice counts back from the block start so that it
    // reaches the start at its trigger. Stretched, it restarts at the trigger.
    const poly_mask block_hard_reset_mask = stretch ? poly_mask(0) : hard_sample_reset_mask;
    sample_index_ = utils::maskLoad(sample_index_, utils::floor(reset_value), block_hard_reset_mask);
    sample_fraction_ = utils::maskLoad(sample_fraction_, reset_value - sample_index_, block_hard_reset_mask);
    sample_index_ = utils::maskLoad(utils::clamp(sample_index_, sample_start, sample_end), sample_index_,
                                    block_hard_reset_mask);
    sample_fraction_ = utils::clamp(sample_fraction_, 0.0f, 1.0f);
    low_cut_.reset(reset_mask);
    high_cut_.reset(reset_mask);

    bool loop = slicer ? slice_loop : input(kLoop)->at(0)[0] != 0.0f;
    poly_mask loop_enabled_mask = loop ? poly_mask(constants::kFullMask) : poly_mask(0);
    // Without Slice Loop a stuttering lane loops its slice's first part while
    // repeats remain, then stops.
    if (slicer && !loop)
      loop_enabled_mask = poly_float::greaterThanOrEqual(stutter_remaining_, 0.0f);

    bool bounce = !slicer && input(kBounce)->at(0)[0] != 0.0f;
    poly_mask bounce_enabled_mask = 0;
    if (bounce)
      bounce_enabled_mask = constants::kFullMask;
    else
      bounce_mask_ = 0;

    const mono_float* audio_buffers[poly_float::kSize];
    poly_float phase_mult = 1.0f;
    for (int i = 0; i < poly_float::kSize; ++i) {
      int index = sample_->getActiveIndex(phase_inc_[i]);
      if (loop && !bounce && reverse_loop_mask[i] == 0) {
        if (i % 2)
          audio_buffers[i] = sample_->getActiveRightLoopBuffer(index);
        else
          audio_buffers[i] = sample_->getActiveLeftLoopBuffer(index);
      }
      else {
        if (i % 2)
          audio_buffers[i] = sample_->getActiveRightBuffer(index);
        else
          audio_buffers[i] = sample_->getActiveLeftBuffer(index);
      }

      phase_mult.set(i, 1.0f / (1 << index));
    }

    mono_float sample_inc = 1.0f / num_samples;
    poly_float delta_pan_amplitude = (pan_amplitude_ - current_pan_amplitude) * sample_inc;
    poly_float delta_phase_inc = (phase_inc_ - current_phase_inc) * sample_inc;

    poly_float* raw_output = output(kRaw)->buffer;
    poly_float current_fraction = sample_fraction_;
    poly_float current_index = utils::maskLoad(utils::clamp(sample_index_, sample_start, sample_end), sample_index_,
                                              block_hard_reset_mask);

    poly_mask current_bounce = bounce_mask_;
    poly_float active_start = loop ? loop_start : sample_start;
    poly_float active_end = loop ? loop_end : sample_end;
    const poly_mask crossfade_loop_mask = loop_enabled_mask & ~bounce_enabled_mask & ~reverse_loop_mask &
                                          poly_float::greaterThan(loop_crossfade, 1.0f);
    const poly_mask reverse_loop = reverse_loop_mask & loop_enabled_mask;
    const SlotModulation::Block modulation = modulation_.resolve(*this, kDistortionType, kDistortionAmount,
                                                                 kSecondaryDistortionType,
                                                                 kSecondaryDistortionAmount, true);
    const bool fm_active = modulation.anyFm();
    poly_float region_length = active_end - active_start;
    const mono_float slice_fade_inverse =
        1.0f / std::max(1.0f, kSliceFadeSeconds * sample_->activeSampleRate() * (1 << Sample::kUpsampleTimes));
    // Slice Decay: each lane's gain falls by 2^(position * decay_exponent)
    // from the start of what it plays (the slice, or each stutter).
    poly_float decay_exponent = 0.0f;
    bool decay_active = false;
    if (slicer) {
      const poly_float decay = utils::clamp(input(kSliceDecay)->at(0), 0.0f, 1.0f);
      decay_active = poly_float::greaterThan(decay, 0.0f).anyMask() != 0;
      const mono_float file_rate = sample_->activeSampleRate() * (1 << Sample::kUpsampleTimes);
      decay_exponent = decay * decay * (-kSliceDecayRangeLog2 / std::max(1.0f, kSliceDecayShortSeconds * file_rate));
    }

    // One sample at the current position of every lane, with the loop
    // crossfade; zero past the end of a one-shot.
    const auto read_sample = [&]() -> poly_float {
      // A bounce pass also stops at active_end (the index counts up in both
      // directions). Keeping finished one-shot bounces "playing" held the
      // sample-start value as DC until the voice died.
      poly_mask playing_mask = loop_enabled_mask | poly_float::lessThan(current_index, active_end);
      const poly_mask mirror_mask = current_bounce ^
                                    (reverse_loop & poly_float::greaterThanOrEqual(current_index, active_start));
      poly_float adjusted = utils::maskLoad(current_index, active_start + active_end - current_index, mirror_mask);
      // Reversed lanes read the position mirrored about the region: a whole
      // index and a fraction, so long files keep their precision.
      adjusted = utils::maskLoad(adjusted, region_mirror - adjusted - 1.0f, reverse_mask);
      const poly_float read_fraction = utils::maskLoad(current_fraction, poly_float(1.0f) - current_fraction, reverse_mask);
      adjusted = utils::clamp(adjusted, 0.0f, audio_end);
      poly_float index_phase = utils::max(adjusted, 0.0f) * phase_mult;
      poly_float fraction_phase = read_fraction * phase_mult;
      
      poly_int start_indices = utils::floorToInt(index_phase);
      poly_float rounded_down_phase = utils::toFloat(start_indices);
      poly_float t = index_phase - rounded_down_phase + fraction_phase;

      t = utils::maskLoad(t, poly_float(1.0f) - t, mirror_mask);

      VITAL_ASSERT(poly_float::lessThan(utils::toFloat(start_indices), 0.0f).anyMask() == 0);
      VITAL_ASSERT(poly_float::greaterThan(utils::toFloat(start_indices), audio_length).anyMask() == 0);

      matrix interpolation_matrix = utils::getCatmullInterpolationMatrix(t);
      matrix value_matrix = utils::getValueMatrix(audio_buffers, start_indices);
      value_matrix.transpose();
      poly_float sample_value = interpolation_matrix.multiplyAndSumRows(value_matrix);

      poly_mask crossfade_mask = crossfade_loop_mask &
                                 poly_float::greaterThanOrEqual(current_index, loop_end - loop_crossfade);
      if (crossfade_mask.anyMask()) {
        poly_float loop_position = current_index - (loop_end - loop_crossfade);
        poly_float alternate_index = loop_start + loop_position;
        alternate_index = utils::maskLoad(alternate_index, region_mirror - alternate_index - 1.0f, reverse_mask);
        poly_float alternate_phase = utils::clamp(alternate_index, 0.0f, audio_end) * phase_mult;
        poly_int alternate_indices = utils::floorToInt(alternate_phase);
        poly_float alternate_t = alternate_phase - utils::toFloat(alternate_indices) + fraction_phase;
        matrix alternate_interpolation = utils::getCatmullInterpolationMatrix(alternate_t);
        matrix alternate_values = utils::getValueMatrix(audio_buffers, alternate_indices);
        alternate_values.transpose();
        poly_float alternate_sample = alternate_interpolation.multiplyAndSumRows(alternate_values);
        poly_float fade = utils::clamp(loop_position / utils::max(loop_crossfade, 1.0f), 0.0f, 1.0f);
        sample_value = utils::maskLoad(sample_value, utils::interpolate(sample_value, alternate_sample, fade),
                                       crossfade_mask);
      }
      if (slicer) {
        // Fade in from the slice's start and out towards its end (the loop's
        // ends while it loops), measured in the file.
        const poly_float edge = utils::min(current_index - active_start, active_end - current_index);
        sample_value *= utils::clamp(edge * slice_fade_inverse, 0.0f, 1.0f) * slice_gain_;
        if (decay_active) {
          const poly_float position = utils::max(current_index - active_start, 0.0f);
          sample_value *= futils::exp2(utils::max(position * decay_exponent, -30.0f));
        }
        playing_mask = playing_mask & slice_valid_mask;
      }
      return sample_value & playing_mask;
    };

    // Moves the lanes in `mask` on by `rate` samples of the file, wrapping
    // the loop and bouncing; the other lanes stay where they are.
    const auto advance = [&](poly_float rate, poly_mask mask) {
      const poly_float held_index = current_index;
      const poly_float held_fraction = current_fraction;
      const poly_mask held_bounce = current_bounce;

      current_fraction += rate;
      poly_float increment = utils::floor(current_fraction);
      current_fraction -= increment;

      const poly_float previous_index = current_index;
      current_index += increment;
      poly_mask done_mask = poly_float::greaterThanOrEqual(current_index, active_end);
      poly_mask bounced_mask = done_mask & ~current_bounce & bounce_enabled_mask;
      poly_mask loop_over_mask = done_mask & (current_bounce | ~bounce_enabled_mask) & loop_enabled_mask;
      current_bounce = (bounced_mask | current_bounce) & ~loop_over_mask;

      poly_float wrapped_index = active_start + (current_index - active_end);
      // With a loop crossfade the end of the loop has already faded into the
      // material at loop_start + crossfade, so continue from there. Wrapping
      // to loop_start would jump back by the crossfade length and click.
      wrapped_index += loop_crossfade & (loop_over_mask & crossfade_loop_mask);
      current_index = utils::maskLoad(current_index, wrapped_index, bounced_mask | loop_over_mask);
      current_index = utils::min(active_end, current_index);
      current_fraction = current_fraction & ~(done_mask & ~loop_enabled_mask);
      if (slicer) {
        // A wrap with no repeats left ends a pass: the whole slice, or a
        // stutter's last repeat.
        const poly_mask wrapped = loop_over_mask & mask;
        const poly_mask stutter_wrap = wrapped & poly_float::greaterThanOrEqual(stutter_remaining_, 0.0f);
        stutter_remaining_ = utils::maskLoad(stutter_remaining_, stutter_remaining_ - 1.0f, stutter_wrap);
        const poly_mask pass_over = wrapped & poly_float::lessThan(stutter_remaining_, 0.0f);
        if (slice_loop) {
          // A looping slice rolls again and plays the next pass from its
          // start: the whole slice, or its first part. A key outside the
          // slices plays nothing and needs no roll.
          const poly_mask new_pass = pass_over & slice_valid_mask;
          if (new_pass.anyMask()) {
            for (int lane = 0; lane < poly_float::kSize; ++lane) {
              if (new_pass[lane])
                roll_pass(lane);
            }
            const poly_mask stutters = poly_float::greaterThanOrEqual(stutter_remaining_, 0.0f);
            const poly_float pass_end = utils::maskLoad(slice_end, stutter_end, stutters);
            sample_end = utils::maskLoad(sample_end, pass_end, new_pass);
            loop_end = sample_end;
            active_end = sample_end;
            region_mirror = sample_start + sample_end;
            region_length = active_end - active_start;
          }
        }
        else {
          // Only stuttering lanes loop here, and the last repeat ends the note.
          loop_enabled_mask = loop_enabled_mask & ~pass_over;
          current_index = utils::maskLoad(current_index, active_end, pass_over);
          current_fraction = current_fraction & ~pass_over;
        }
      }
      if (fm_active) {
        // A finished one-shot stays finished instead of backing into its tail.
        poly_mask finished_mask = ~loop_enabled_mask & poly_float::greaterThanOrEqual(previous_index, active_end);
        current_index = utils::maskLoad(current_index, active_end, finished_mask);
        // Through-zero FM can run the index backwards past the region start.
        poly_mask under_mask = poly_float::lessThan(current_index, active_start);
        if (under_mask.anyMask()) {
          poly_float wrapped_under = current_index + region_length;
          current_index = utils::maskLoad(current_index, wrapped_under, under_mask & loop_enabled_mask);
          current_index = utils::max(current_index, active_start);
        }
      }
      current_index = utils::maskLoad(held_index, current_index, mask);
      current_fraction = utils::maskLoad(held_fraction, current_fraction, mask);
      current_bounce = utils::maskLoad(held_bounce, current_bounce, mask);
    };

    if (stretch == nullptr) {
      if (stretch_ != nullptr) {
        for (StretchState::Voice& voice : stretch_->voices)
          voice.running = false;
      }
      for (int i = 0; i < num_samples; ++i) {
        poly_mask soft_trigger_mask = soft_sample_reset_mask & poly_int::equal(i, retrigger_offset);
        if (soft_trigger_mask.anyMask()) {
          poly_float reset_index = utils::floor(soft_reset_value);
          current_index = utils::maskLoad(current_index, reset_index, soft_trigger_mask);
          current_fraction = utils::maskLoad(current_fraction, soft_reset_value - reset_index, soft_trigger_mask);
          current_phase_inc = utils::maskLoad(current_phase_inc, phase_inc_, soft_trigger_mask);
          current_bounce = current_bounce & ~soft_trigger_mask;
        }

        current_phase_inc += delta_phase_inc;
        raw_output[i] = read_sample();
        VITAL_ASSERT(utils::isContained(raw_output[i]));

        if (fm_active)
          advance(current_phase_inc * modulation.rateScale(i), constants::kFullMask);
        else
          advance(current_phase_inc, constants::kFullMask);
      }
    }
    else {
      // Each voice feeds its own stereo stretcher the file at its own speed
      // (input samples per output sample) and takes the block's output back.
      // FM of the read rate has no meaning here; ring modulation still applies.
      using Stretcher = atlas::SampleStretcher;
      static constexpr int kChannels = Stretcher::kChannels;
      poly_float speed = 1.0f;
      if (stretch_mode == kStretchTempo) {
        const mono_float host_bpm = input(kBeatsPerSecond)->at(0)[0] * 60.0f;
        const mono_float file_bpm = input(kBpm)->at(0)[0];
        if (host_bpm > 0.0f && file_bpm > 0.0f)
          speed = host_bpm / file_bpm;
      }
      else
        speed = poly_float(1.0f) / utils::max(input(kStretchLength)->at(0), 0.01f);
      speed = utils::clamp(speed, Stretcher::kMinSpeed, Stretcher::kMaxSpeed);
      const bool formants = input(kFormants)->at(0)[0] != 0.0f;
      const poly_float active_voices = input(kActiveVoices)->at(0);
      const poly_int reset_offset_samples = input(kReset)->source->trigger_offset;
      const poly_mask kVoiceMasks[kVoicesPerProcessor] = {
        poly_int(poly_int::kFullMask, poly_int::kFullMask, 0u, 0u),
        poly_int(0u, 0u, poly_int::kFullMask, poly_int::kFullMask)
      };

      struct VoiceRun {
        bool active = false;
        bool restart = false;        // begins again at `offset`
        bool reset_index = false;    // ... from the region start (a retrigger), else from where it is
        bool silent_before = false;  // nothing before the offset: a voice starting (cleared below anyway)
        int offset = 0;
        mono_float speed = 1.0f;
      };
      VoiceRun runs[kVoicesPerProcessor];
      for (int voice = 0; voice < kVoicesPerProcessor; ++voice) {
        const int lane = 2 * voice;
        VoiceRun& run = runs[voice];
        run.speed = speed[lane];
        run.restart = sample_reset_mask[lane] || reset_mask[lane];
        run.reset_index = sample_reset_mask[lane] != 0;
        run.silent_before = reset_mask[lane] != 0;
        const int offset = sample_reset_mask[lane] ? retrigger_offset[lane] : reset_offset_samples[lane];
        run.offset = run.restart ? utils::iclamp(offset, 0, num_samples - 1) : num_samples;
        run.active = active_voices[lane] >= 0.5f || run.restart;
        StretchState::Voice& state = stretch->voices[voice];
        state.stretcher.setTranspose(transpose[lane]);
        state.stretcher.setPreserveFormants(formants);
      }

      mono_float out[kVoicesPerProcessor][kChannels][kMaxBufferSize * kMaxOversample];
      // Reads `count` samples into `dest` for the lanes of `voice`; the other
      // voice's lanes keep their place.
      const auto read_into = [&](int voice, mono_float* const dest[kChannels], int count) {
        const int lane = 2 * voice;
        for (int i = 0; i < count; ++i) {
          const poly_float value = read_sample();
          dest[0][i] = value[lane];
          dest[1][i] = value[lane + 1];
          advance(current_phase_inc, kVoiceMasks[voice]);
        }
      };
      const auto start_voice = [&](int voice) {
        StretchState::Voice& state = stretch->voices[voice];
        mono_float* const buffers[kChannels] = { stretch->start[0].data(), stretch->start[1].data() };
        read_into(voice, buffers, state.stretcher.startLength(runs[voice].speed));
        state.stretcher.start(buffers, runs[voice].speed);
        state.owed = 0.0f;
        state.running = true;
      };

      int position = 0;
      while (position < num_samples) {
        int end = num_samples;
        for (const VoiceRun& run : runs) {
          if (run.restart && run.offset > position && run.offset < end)
            end = run.offset;
        }
        const int length = end - position;
        for (int voice = 0; voice < kVoicesPerProcessor; ++voice) {
          VoiceRun& run = runs[voice];
          StretchState::Voice& state = stretch->voices[voice];
          mono_float* const block[kChannels] = { out[voice][0] + position, out[voice][1] + position };
          const bool waiting = run.restart && run.offset > position && run.silent_before;
          if (!run.active || waiting) {
            std::fill(block[0], block[0] + length, 0.0f);
            std::fill(block[1], block[1] + length, 0.0f);
            continue;
          }
          if (run.restart && run.offset == position) {
            if (run.reset_index) {
              const poly_float reset_index = utils::floor(soft_reset_value);
              current_index = utils::maskLoad(current_index, reset_index, kVoiceMasks[voice]);
              current_fraction = utils::maskLoad(current_fraction, soft_reset_value - reset_index, kVoiceMasks[voice]);
              current_bounce = current_bounce & ~kVoiceMasks[voice];
            }
            start_voice(voice);
            run.restart = false;
          }
          else if (!state.running)
            start_voice(voice);

          const mono_float due = state.owed + run.speed * length;
          const int count = std::min(kMaxStretchInput, static_cast<int>(due));
          state.owed = due - count;
          mono_float* const in[kChannels] = { stretch->input[0].data(), stretch->input[1].data() };
          read_into(voice, in, count);
          state.stretcher.process(in, count, block, length);
        }
        position = end;
      }
      for (int i = 0; i < num_samples; ++i)
        raw_output[i] = poly_float(out[0][0][i], out[0][1][i], out[1][0][i], out[1][1][i]);
    }

    bounce_mask_ = current_bounce;
    if (reset_mask.anyMask())
      clearOutputBufferForReset(reset_mask, kReset, kRaw);
    if (hard_sample_reset_mask.anyMask())
      clearOutputBufferForReset(hard_sample_reset_mask, kRetrigger, kRaw);

    poly_float low_cutoff = utils::clamp(input(kLowCutoff)->at(0), 8.0f, 136.0f);
    poly_float high_cutoff = utils::clamp(input(kHighCutoff)->at(0), 8.0f, 136.0f);
    const int slope = static_cast<int>(input(kFilterSlope)->at(0)[0] + 0.5f);
    low_cut_.configure(low_cutoff, slope, getSampleRate());
    high_cut_.configure(high_cutoff, slope, getSampleRate());
    // Normalize plays the file with its peak at 0 dB, up to 60 dB louder. The
    // gain ramps over the block, so switching it does not click.
    static constexpr mono_float kMaxNormalizeGain = 1000.0f;
    const mono_float peak = sample_->activePeak();
    const bool normalize = input(kNormalize)->at(0)[0] != 0.0f && peak > 0.0f;
    const poly_float target_gain = normalize ? std::min(1.0f / peak, kMaxNormalizeGain) : 1.0f;
    poly_float gain = normalize_gain_;
    const poly_float delta_gain = (target_gain - gain) * sample_inc;
    normalize_gain_ = target_gain;
    for (int i = 0; i < num_samples; ++i) {
      gain += delta_gain;
      poly_float sample_value = raw_output[i];
      sample_value = high_cut_.tick(low_cut_.tick(sample_value));
      raw_output[i] = sample_value * gain;
    }

    if (soft_sample_reset_mask.anyMask()) {
      for (int lane = 0; lane < poly_float::kSize; ++lane) {
        if (soft_sample_reset_mask[lane])
          fadeFromPrevious(raw_output, num_samples, lane, std::max(0, static_cast<int>(retrigger_offset[lane])));
      }
    }
    // A mode change swaps the plain read for the stretched one mid-note (or
    // back), and a lane turning around reads from the mirrored position; the
    // output steps, so the step fades out.
    const poly_mask reverse_changed = (reverse_mask ^ reverse_mask_) & ~reset_mask;
    reverse_mask_ = reverse_mask;
    for (int lane = 0; lane < poly_float::kSize; ++lane) {
      if (stretch_transition || reverse_changed[lane])
        fadeFromPrevious(raw_output, num_samples, lane, 0);
    }

    modulation.applyRingMod(raw_output, num_samples);

    const poly_float* level_input = input(kLevel)->source->buffer;
    poly_float* levelled_output = output(kLevelled)->buffer;
    poly_float zero = 0.0f;
    poly_float max = kMaxAmplitude;
    for (int i = 0; i < num_samples; ++i) {
      current_pan_amplitude += delta_pan_amplitude;
      poly_float level = utils::clamp(level_input[i], zero, max);
      levelled_output[i] = current_pan_amplitude * level * level * raw_output[i];
    }

    if (num_samples > 0)
      last_output_ = raw_output[num_samples - 1];

    sample_index_ = current_index;
    sample_fraction_ = current_fraction;
    if (playback_mode == kContinuousGlobal) {
      global_sample_index_ = poly_float(current_index[0]);
      global_sample_fraction_ = poly_float(current_fraction[0]);
      sample_index_ = global_sample_index_;
      sample_fraction_ = global_sample_fraction_;
    }

    poly_float phase = utils::maskLoad(sample_index_, active_start + active_end - sample_index_, bounce_mask_);
    phase = (phase - sample_start) / utils::max(sample_end - sample_start, 1.0f);
    phase = utils::clamp(phase, 0.0f, 1.0f);
    phase_output_->buffer[0] = utils::encodePhaseAndVoice(phase, input(kNoteCount)->at(0));

    sample_->markUnused();
  }

  force_inline poly_float SampleSource::snapTranspose(poly_float input_midi, poly_float transpose, int quantize) {
    if (quantize == 0)
      return input_midi + transpose;

    bool global_transpose = utils::isTransposeQuantizeGlobal(quantize);
    poly_float pre_add = 0.0f;
    poly_float post_add = input_midi;
    if (global_transpose) {
      pre_add = input_midi;
      post_add = 0.0f;
    }

    poly_float snapped = utils::snapTranspose(pre_add + transpose, quantize);

    if (transpose_quantize_)
      phase_inc_ *= utils::noteOffsetToRatio(snapped - last_quantized_transpose_);

    last_quantized_transpose_ = snapped;
    transpose_quantize_ = quantize;
    return post_add + snapped;
  }
} // namespace vital
