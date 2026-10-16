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
#include "synth_constants.h"

#include <algorithm>
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

    force_inline mono_float getFilteredLoopSample(const mono_float* buffer, int index, int size) {
      int radius = SampleSource::kNumDownsampleTaps / 2;
      int start = index - radius;
      int end = index + radius;
      mono_float total = 0.0f;
      for (int i = start; i <= end; ++i) {
        int buffer_index = (i + size * radius) % size;
        mono_float coefficient = kDownsampleCoefficients[i - index + radius];
        total += coefficient * buffer[buffer_index];
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

    void upsample(const mono_float* original, mono_float* dest, int original_size, int dest_size) {
      for (int i = 0; i < original_size; ++i) {
        float value1 = original[i];
        float value2 = getInterpolatedSample(original, i, original_size);
        dest[2 * i] = value1;
        dest[2 * i + 1] = value2;
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

        loop_buffer[i] = loop_buffer[size + i];
        loop_buffer[size + Sample::kBufferSamples + i] = loop_buffer[Sample::kBufferSamples + i];
      }

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
        memcpy(next_loop_buffer, next_buffer, num_samples * sizeof(mono_float));
        current_size = upsampled_size;
      }

      play_buffer = destination.back().get();
      loop_buffer = loop_destination.back().get();
      current_size = size;

      while (current_size >= Sample::kMinSize) {
        int next_size = (current_size + 1) / 2;
        destination.push_back(std::make_unique<mono_float[]>(next_size + 2 * Sample::kBufferSamples));
        loop_destination.push_back(std::make_unique<mono_float[]>(next_size + 2 * Sample::kBufferSamples));

        mono_float* next_buffer = destination.back().get();
        mono_float* next_loop_buffer = loop_destination.back().get();

        downsample(play_buffer + Sample::kBufferSamples, next_buffer + Sample::kBufferSamples,
                   current_size, next_size);
        downsampleLoop(loop_buffer + Sample::kBufferSamples, next_loop_buffer + Sample::kBufferSamples,
                       current_size, next_size);

        for (int i = 0; i < Sample::kBufferSamples; ++i) {
          next_buffer[i] = 0.0f;
          next_buffer[next_size + Sample::kBufferSamples + i] = 0.0f;

          next_loop_buffer[i] = loop_buffer[next_size + i];
          next_loop_buffer[next_size + Sample::kBufferSamples + i] = next_loop_buffer[Sample::kBufferSamples + i];
        }

        play_buffer = next_buffer;
        loop_buffer = next_loop_buffer;
        current_size = next_size;
      }
    }
  }

  Sample::Sample() : name_(kDefaultName), current_data_(nullptr), active_audio_data_(nullptr) {
    init();
  }

  void Sample::loadSample(const mono_float* buffer, int size, int sample_rate) {
    is_default_ = false;
    static constexpr int kMaxSize = 1764000;

    VITAL_ASSERT(active_audio_data_.is_lock_free());

    size = std::min(size, kMaxSize);
    std::unique_ptr<SampleData> old_data = std::move(data_);
    data_ = std::make_unique<SampleData>(size, sample_rate, false);
    createBandLimitedBuffers(data_->left_buffers, data_->left_loop_buffers, buffer, size);

    current_data_ = data_.get();
    while (active_audio_data_.load())
      std::this_thread::yield(); // Wait for audio thread to finish using old_data.

    last_browsed_file_.clear();
    if (load_callback_)
      load_callback_(buffer, buffer, size, sample_rate);
  }

  void Sample::loadSample(const mono_float* left_buffer, const mono_float* right_buffer, int size, int sample_rate) {
    is_default_ = false;
    static constexpr int kMaxSize = 1764000;

    size = std::min(size, kMaxSize);
    std::unique_ptr<SampleData> old_data = std::move(data_);
    data_ = std::make_unique<SampleData>(size, sample_rate, true);
    createBandLimitedBuffers(data_->left_buffers, data_->left_loop_buffers, left_buffer, size);
    createBandLimitedBuffers(data_->right_buffers, data_->right_loop_buffers, right_buffer, size);

    current_data_ = data_.get();
    while (active_audio_data_.load())
      std::this_thread::yield(); // Wait for audio thread to finish using old_data.

    last_browsed_file_.clear();
    if (load_callback_)
      load_callback_(left_buffer, right_buffer, size, sample_rate);
  }

  void Sample::init() {
    name_ = kDefaultName;
    mono_float buffer[kDefaultSampleLength];
    utils::RandomGenerator random_generator(-0.9f, 0.9f);

    for (int i = 0; i < kDefaultSampleLength; ++i)
      buffer[i] = random_generator.next();

    loadSample(buffer, kDefaultSampleLength, kDefaultSampleRate);
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

  SampleSource::SampleSource() : SampleSource(nullptr) { }

  SampleSource::SampleSource(std::shared_ptr<Sample> sample) :
      Processor(kNumInputs, kNumOutputs), pan_amplitude_(0.0f), phase_inc_(0.0f),
      random_generator_(0.0f, 1.0f) {
    transpose_quantize_ = 0;
    last_quantized_transpose_ = 0.0f;
    sample_index_ = 0;
    sample_fraction_ = 0.0f;
    global_sample_index_ = 0;
    global_sample_fraction_ = 0.0f;
    low_cut_.reset(constants::kFullMask);
    high_cut_.reset(constants::kFullMask);
    last_output_ = 0.0f;

    sample_ = sample ? std::move(sample) : std::make_shared<Sample>();
    phase_output_ = std::make_shared<cr::Output>();
  }

  void SampleSource::process(int num_samples) {
    if (num_samples <= 0)
      return;
    sample_->markUsed();

    poly_float current_pan_amplitude = pan_amplitude_;
    poly_float input_pan = utils::clamp(input(kPan)->at(0), -1.0f, 1.0f);
    pan_amplitude_ = futils::panAmplitude(input_pan);

    poly_float input_midi = 0.0f;
    if (input(kKeytrack)->at(0)[0])
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

    poly_float transpose = snapTranspose(input_midi, input(kTranspose)->at(0), transpose_quantize);
    transpose = utils::clamp(transpose + input(kTune)->at(0), kMinTranspose, kMaxTranspose);

    mono_float sample_rate_ratio = (1.0f * sample_->activeSampleRate()) / getSampleRate();
    poly_float current_phase_inc = phase_inc_;
    phase_inc_ = utils::centsToRatio(transpose * kCentsPerNote) * sample_rate_ratio * (1 << Sample::kUpsampleTimes);

    int audio_length = sample_->activeLength();
    poly_float audio_end = audio_length;
    poly_float minimum_region = Sample::kMinSize * (1 << Sample::kUpsampleTimes);
    poly_float sample_start = utils::clamp(input(kStart)->at(0), 0.0f, 1.0f) * audio_end;
    poly_float sample_end = utils::clamp(input(kEnd)->at(0), 0.0f, 1.0f) * audio_end;
    sample_start = utils::floor(utils::clamp(sample_start, 0.0f, audio_end - minimum_region));
    sample_end = utils::floor(utils::clamp(sample_end, sample_start + minimum_region, audio_end));

    poly_float loop_start = utils::clamp(input(kLoopStart)->at(0), 0.0f, 1.0f) * audio_end;
    poly_float loop_end = utils::clamp(input(kLoopEnd)->at(0), 0.0f, 1.0f) * audio_end;
    loop_start = utils::floor(utils::clamp(loop_start, sample_start, sample_end - minimum_region));
    loop_end = utils::floor(utils::clamp(loop_end, loop_start + minimum_region, sample_end));
    poly_float loop_length = utils::max(loop_end - loop_start, 1.0f);
    poly_float loop_crossfade = utils::clamp(input(kLoopCrossfade)->at(0), 0.0f, 1.0f) * loop_length;
    loop_crossfade = utils::min(loop_crossfade, loop_length * 0.5f);

    poly_mask reset_mask = getResetMask(kReset);
    poly_mask retrigger_mask = getResetMask(kRetrigger);
    int playback_mode = static_cast<int>(input(kPlaybackMode)->at(0)[0] + 0.5f);
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
    if (input(kRandomPhase)->at(0)[0]) {
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

    sample_index_ = utils::maskLoad(sample_index_, utils::floor(reset_value), hard_sample_reset_mask);
    sample_fraction_ = utils::maskLoad(sample_fraction_, reset_value - sample_index_, hard_sample_reset_mask);
    sample_index_ = utils::maskLoad(utils::clamp(sample_index_, sample_start, sample_end), sample_index_,
                                    hard_sample_reset_mask);
    sample_fraction_ = utils::clamp(sample_fraction_, 0.0f, 1.0f);
    low_cut_.reset(reset_mask);
    high_cut_.reset(reset_mask);

    bool loop = input(kLoop)->at(0)[0] != 0.0f;
    poly_mask loop_enabled_mask = 0;
    if (loop)
      loop_enabled_mask = constants::kFullMask;

    bool bounce = input(kBounce)->at(0)[0] != 0.0f;
    poly_mask bounce_enabled_mask = 0;
    if (bounce)
      bounce_enabled_mask = constants::kFullMask;
    else
      bounce_mask_ = 0;

    const mono_float* audio_buffers[poly_float::kSize];
    poly_float phase_mult = 1.0f;
    for (int i = 0; i < poly_float::kSize; ++i) {
      int index = sample_->getActiveIndex(phase_inc_[i]);
      if (loop && !bounce) {
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
                                              hard_sample_reset_mask);

    poly_mask current_bounce = bounce_mask_;
    poly_float active_start = loop ? loop_start : sample_start;
    poly_float active_end = loop ? loop_end : sample_end;
    const poly_mask crossfade_loop_mask = loop_enabled_mask & ~bounce_enabled_mask &
                                          poly_float::greaterThan(loop_crossfade, 1.0f);
    const SlotModulation::Block modulation = modulation_.resolve(*this, kDistortionType, kDistortionAmount,
                                                                 kSecondaryDistortionType,
                                                                 kSecondaryDistortionAmount, true);
    const bool fm_active = modulation.anyFm();
    const poly_float region_length = active_end - active_start;
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

      // A bounce pass also stops at active_end (the index counts up in both
      // directions). Keeping finished one-shot bounces "playing" held the
      // sample-start value as DC until the voice died.
      poly_mask playing_mask = loop_enabled_mask | poly_float::lessThan(current_index, active_end);
      poly_float adjusted = utils::maskLoad(current_index, active_start + active_end - current_index, current_bounce);
      adjusted = utils::clamp(adjusted, 0.0f, audio_end);
      poly_float index_phase = utils::max(adjusted, 0.0f) * phase_mult;
      poly_float fraction_phase = current_fraction * phase_mult;
      
      poly_int start_indices = utils::floorToInt(index_phase);
      poly_float rounded_down_phase = utils::toFloat(start_indices);
      poly_float t = index_phase - rounded_down_phase + fraction_phase;

      t = utils::maskLoad(t, poly_float(1.0f) - t, current_bounce);

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

      raw_output[i] = sample_value & playing_mask;
      VITAL_ASSERT(utils::isContained(raw_output[i]));

      if (fm_active)
        current_fraction += current_phase_inc * modulation.rateScale(i);
      else
        current_fraction += current_phase_inc;
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
    for (int i = 0; i < num_samples; ++i) {
      poly_float sample_value = raw_output[i];
      sample_value = high_cut_.tick(low_cut_.tick(sample_value));
      raw_output[i] = sample_value;
    }

    if (soft_sample_reset_mask.anyMask()) {
      static constexpr mono_float kSampleRetriggerFadeSeconds = 0.003f;
      const int fade_samples = std::max(1, static_cast<int>(kSampleRetriggerFadeSeconds * getSampleRate()));
      for (int lane = 0; lane < poly_float::kSize; ++lane) {
        if (!soft_sample_reset_mask[lane])
          continue;

        const int start = std::min(num_samples, std::max(0, static_cast<int>(retrigger_offset[lane])));
        if (start >= num_samples)
          continue;

        const int end = std::min(num_samples, start + fade_samples);
        const mono_float previous_output = start > 0 ? raw_output[start - 1][lane] : last_output_[lane];
        const mono_float correction = previous_output - raw_output[start][lane];
        for (int i = start; i < end; ++i) {
          mono_float fade = 1.0f - (i - start + 1.0f) / fade_samples;
          raw_output[i].set(lane, raw_output[i][lane] + correction * fade);
        }
      }
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
