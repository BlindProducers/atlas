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

#include "spectral_source.h"

#include "futils.h"
#include "poly_utils.h"
#include "synth_constants.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#if JUCE_MAC
#include <pthread/qos.h>
#endif

namespace vital {
  struct SpectralSource::AnalysisData {
    std::array<SpectralAnalysis, 2> analyses;
    int sample_length = 0;
    int sample_rate = 0;
    bool stereo = false;
  };

  struct SpectralSource::SharedAnalysisCache {
    struct Request {
      std::vector<float> left;
      std::vector<float> right;
      int sample_length = 0;
      int sample_rate = 0;
      int generation = 0;
    };

    SharedAnalysisCache() : worker_([this] { run(); }) { }

    ~SharedAnalysisCache() {
      stopping_.store(true, std::memory_order_release);
      generation_.fetch_add(1, std::memory_order_acq_rel);
      condition_.notify_one();
      if (worker_.joinable())
        worker_.join();
    }

    void request(const mono_float* left, const mono_float* right, int length, int sample_rate) {
      if (left == nullptr || length <= 0 || sample_rate <= 0)
        return;

      Request request;
      request.left.assign(left, left + length);
      if (right != nullptr && right != left)
        request.right.assign(right, right + length);
      request.sample_length = length;
      request.sample_rate = sample_rate;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        request.generation = generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
        // Serialize invalidation with publication, so a cancelled job cannot
        // publish between the final generation check and the snapshot store.
        auto retired = std::atomic_exchange_explicit(&snapshot_, std::shared_ptr<const AnalysisData>(),
                                                     std::memory_order_acq_rel);
        revision_.fetch_add(1, std::memory_order_acq_rel);
        if (retired)
          retired_.push_back(std::move(retired));
        pending_ = std::move(request);
        has_pending_ = true;
      }
      condition_.notify_one();
    }

    int revision() const noexcept { return revision_.load(std::memory_order_acquire); }

    std::shared_ptr<const AnalysisData> snapshot() const {
      return std::atomic_load_explicit(&snapshot_, std::memory_order_acquire);
    }

    private:
      bool cancelled(int generation) const noexcept {
        return stopping_.load(std::memory_order_acquire) ||
               generation_.load(std::memory_order_acquire) != generation;
      }

      void run() {
#if JUCE_MAC
        pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
#endif
        while (!stopping_.load(std::memory_order_acquire)) {
          Request request;
          {
            std::unique_lock<std::mutex> lock(mutex_);
            condition_.wait(lock, [this] {
              return stopping_.load(std::memory_order_acquire) || has_pending_;
            });
            if (stopping_.load(std::memory_order_acquire))
              return;
            request = std::move(pending_);
            has_pending_ = false;
          }

          auto analysis = std::make_shared<AnalysisData>();
          const auto should_cancel = [this, generation = request.generation] {
            return cancelled(generation);
          };
          if (!analysis->analyses[0].analyzeChannel(request.left.data(), request.sample_length,
                                                    request.sample_rate, should_cancel))
            continue;
          analysis->stereo = !request.right.empty();
          if (analysis->stereo &&
              !analysis->analyses[1].analyzeChannel(request.right.data(), request.sample_length,
                                                    request.sample_rate, should_cancel))
            continue;
          analysis->sample_length = request.sample_length;
          analysis->sample_rate = request.sample_rate;
          std::shared_ptr<const AnalysisData> published = std::move(analysis);
          std::lock_guard<std::mutex> lock(mutex_);
          if (cancelled(request.generation))
            continue;
          std::atomic_store_explicit(&snapshot_, published, std::memory_order_release);
          revision_.fetch_add(1, std::memory_order_acq_rel);

          // Keep retired analyses alive until every audio voice has switched
          // snapshots, then reclaim them here rather than on the real-time
          // thread that happens to release the final reference.
          retired_.erase(std::remove_if(retired_.begin(), retired_.end(), [](const auto& item) {
            return item.use_count() == 1;
          }), retired_.end());
        }
      }

      mutable std::shared_ptr<const AnalysisData> snapshot_;
      std::atomic<int> revision_ { 0 };
      std::atomic<int> generation_ { 0 };
      std::atomic<bool> stopping_ { false };
      std::mutex mutex_;
      std::condition_variable condition_;
      Request pending_;
      std::vector<std::shared_ptr<const AnalysisData>> retired_;
      bool has_pending_ = false;
      std::thread worker_;
  };

  namespace {
    constexpr int kSpectralTransposeQuantizeCustomScale = 14;
    constexpr int kSpectralTransposeQuantizeScaleMasks[] = {
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

    int rotateSpectralMask(int mask, int key) {
      int result = 0;
      key = std::max(0, std::min(kNotesPerOctave - 1, key));
      for (int i = 0; i < kNotesPerOctave; ++i) {
        if ((mask >> i) & 1)
          result |= 1 << ((i + key) % kNotesPerOctave);
      }
      return result;
    }

    bool finite(float value) {
      // This translation unit also builds with fast-math; std::isfinite may
      // otherwise be optimized away in the real-time safety checks.
      uint32_t bits;
      std::memcpy(&bits, &value, sizeof(bits));
      return (bits & 0x7f800000u) != 0x7f800000u;
    }

    float spectralBin(float value) {
      // Equal positions mean equal edges. Log spacing gives bass partials
      // useful travel without making a one-percent move remove whole octaves.
      return std::exp2(10.0f * juce::jlimit(0.0f, 1.0f, value)) - 1.0f;
    }

    float smoothEdge(float value) {
      value = juce::jlimit(0.0f, 1.0f, value);
      return value * value * (3.0f - 2.0f * value);
    }

    float curvedSpectralAmount(float value) {
      value = juce::jlimit(0.0f, 1.0f, value);
      return value * value * (3.0f - 2.0f * value);
    }

    float curvedGateThreshold(float value) {
      value = juce::jlimit(0.0f, 1.0f, value);
      return std::pow(value, 3.0f) * 0.75f;
    }

    float stableSourceFrequency(int bin, float analyzed_frequency, double source_sample_rate) {
      const float sample_rate = static_cast<float>(source_sample_rate);
      const float nominal_hz = bin * sample_rate / SpectralAnalysis::kFftSize;
      const float bin_width = sample_rate / SpectralAnalysis::kFftSize;
      const float analyzed_hz = analyzed_frequency * sample_rate / juce::MathConstants<float>::twoPi;

      if (!finite(analyzed_hz) || analyzed_hz <= 0.0f ||
          std::abs(analyzed_hz - nominal_hz) > bin_width * 2.01f)
        return nominal_hz;

      return analyzed_hz;
    }
  } // namespace

  SpectralSource::SpectralSource() : SpectralSource(nullptr) { }

  SpectralSource::SpectralSource(std::shared_ptr<Sample> sample) :
      Processor(kNumInputs, kNumOutputs), sample_(sample ? std::move(sample) : std::make_shared<Sample>()),
      analysis_cache_(std::make_shared<SharedAnalysisCache>()),
      fft_(atlas::sharedFft(SpectralAnalysis::kFftOrder)) {
    for (int i = 0; i < SpectralAnalysis::kFftSize; ++i) {
      window_[static_cast<size_t>(i)] = 0.5f - 0.5f * std::cos(juce::MathConstants<float>::twoPi *
                                                               i / SpectralAnalysis::kFftSize);
    }

    std::weak_ptr<SharedAnalysisCache> weak_cache = analysis_cache_;
    sample_->setLoadCallback([weak_cache](const mono_float* left, const mono_float* right,
                                          int length, int sample_rate) {
      if (auto cache = weak_cache.lock())
        cache->request(left, right, length, sample_rate);
    });

    // The Sample constructor has already installed its default data before the
    // callback exists. Queue that first analysis here, off the audio thread.
    sample_->markUsed();
    const int initial_length = sample_->activeLength() >> Sample::kUpsampleTimes;
    const mono_float* initial_left =
        sample_->getActiveLeftBuffer(Sample::kUpsampleTimes) + Sample::kBufferSamples;
    const mono_float* initial_right =
        sample_->getActiveRightBuffer(Sample::kUpsampleTimes) + Sample::kBufferSamples;
    analysis_cache_->request(initial_left, initial_right, initial_length, sample_->activeSampleRate());
    sample_->markUnused();
  }

  SpectralSource::SpectralSource(const SpectralSource& other) :
      Processor(other), sample_(other.sample_), analysis_cache_(other.analysis_cache_),
      window_(other.window_), fft_(atlas::sharedFft(SpectralAnalysis::kFftOrder)),
      modulation_(other.modulation_) {
    for (int lane = 0; lane < poly_float::kSize; ++lane) {
      lanes_[lane].analysis_valid = false;
      resetLane(lane);
    }
  }

  SpectralSource::~SpectralSource() = default;

  bool SpectralSource::isAnalysisReady() const {
    return analysis_cache_->snapshot() != nullptr;
  }

  int SpectralSource::quantizeScaleMask(int scale, int key) {
    scale = utils::iclamp(scale, 0, kSpectralTransposeQuantizeCustomScale);
    if (scale == kSpectralTransposeQuantizeCustomScale)
      return 0;
    return rotateSpectralMask(kSpectralTransposeQuantizeScaleMasks[scale], key);
  }

  poly_float SpectralSource::snapTranspose(poly_float input_midi, poly_float transpose, int quantize) {
    if (quantize == 0)
      return input_midi + transpose;

    bool global_transpose = utils::isTransposeQuantizeGlobal(quantize);
    poly_float pre_add = 0.0f;
    poly_float post_add = input_midi;
    if (global_transpose) {
      pre_add = input_midi;
      post_add = 0.0f;
    }

    return post_add + utils::snapTranspose(pre_add + transpose, quantize);
  }

  float SpectralSource::noteToFrequency(float note) {
    return 440.0f * std::pow(2.0f, (note - 69.0f) / 12.0f);
  }

  void SpectralSource::refreshAnalysisIfNeeded() {
    const int length = sample_->activeLength() >> Sample::kUpsampleTimes;
    const int sample_rate = sample_->activeSampleRate();
    const int revision = analysis_cache_->revision();
    if (analysis_revision_ == revision)
      return;

    analysis_revision_ = revision;
    active_analysis_ = analysis_cache_->snapshot();
    const bool valid = active_analysis_ != nullptr &&
                       active_analysis_->sample_length == length &&
                       active_analysis_->sample_rate == sample_rate &&
                       active_analysis_->analyses[0].numFrames() > 0;
    for (int lane = 0; lane < poly_float::kSize; ++lane) {
      const float previous_output = lanes_[lane].last_output;
      lanes_[lane].analysis_valid = valid;
      resetLane(lane);
      lanes_[lane].last_output = previous_output;
    }
  }

  void SpectralSource::resetLane(int lane_index) {
    LaneState& lane = lanes_[static_cast<size_t>(lane_index)];
    lane.position = 0.0;
    lane.bounce_forward = true;
    lane.write_pos = 0;
    lane.read_pos = 0;
    lane.samples_until_frame = 0;
    lane.last_play_mode = -1;
    lane.mode_handoff_samples = 0;
    lane.random_seed = 1u + static_cast<unsigned int>(lane_index) * 7919u;
    lane.last_output = 0.0f;
    lane.mode_handoff_correction = 0.0f;
    lane.started = false;
    lane.phases_initialized = false;
    lane.last_transient_frame = -1;
    lane.overlap.fill(0.0f);
    lane.synth_phase.fill(0.0f);
  }

  void SpectralSource::reset(poly_mask reset_mask) {
    for (int lane = 0; lane < poly_float::kSize; ++lane) {
      if (reset_mask[lane]) resetLane(lane);
    }
  }

  float SpectralSource::applyModeHandoff(LaneState& lane, int play_mode, float value) const {
    static constexpr float kModeHandoffSeconds = 0.005f;
    const int handoff_length = std::max(1, static_cast<int>(kModeHandoffSeconds * getSampleRate()));
    if (lane.last_play_mode != play_mode) {
      lane.last_play_mode = play_mode;
      lane.mode_handoff_samples = handoff_length;
      lane.mode_handoff_correction = lane.last_output - value;
    }

    if (lane.mode_handoff_samples > 0) {
      const float amount = lane.mode_handoff_samples / static_cast<float>(handoff_length);
      value += lane.mode_handoff_correction * amount;
      --lane.mode_handoff_samples;
    }
    lane.last_output = value;
    return value;
  }

  void SpectralSource::advancePosition(LaneState& lane, int play_mode, float position, float speed,
                                       float loop_start, float loop_end) const {
    if (active_analysis_ == nullptr)
      return;
    const int num_frames = active_analysis_->analyses[0].numFrames();
    if (num_frames <= 1) {
      lane.position = 0.0;
      return;
    }

    auto loopBounds = [&](double& low, double& high) {
      low = juce::jlimit(0.0, static_cast<double>(num_frames - 1), loop_start * (num_frames - 1.0));
      high = juce::jlimit(0.0, static_cast<double>(num_frames - 1), loop_end * (num_frames - 1.0));
      if (low > high)
        std::swap(low, high);
      if (high - low < 1.0)
        high = std::min<double>(num_frames - 1, low + 1.0);
    };

    const double source_to_host = active_analysis_->analyses[0].sourceSampleRate() /
                                  std::max(1, getSampleRate());
    const double delta = speed * source_to_host;
    switch (play_mode) {
      case kManual:
        lane.position = juce::jlimit(0.0, static_cast<double>(num_frames - 1), position * (num_frames - 1.0));
        break;
      case kForward:
        lane.position = juce::jlimit(0.0, static_cast<double>(num_frames - 1), lane.position + delta);
        break;
      case kReverse:
        lane.position = juce::jlimit(0.0, static_cast<double>(num_frames - 1), lane.position - std::abs(delta));
        break;
      case kLoop: {
        double low = 0.0, high = 0.0;
        loopBounds(low, high);
        const double span = high - low;
        if (span <= 0.0) lane.position = low;
        else {
          const double wrapped = std::fmod(lane.position + delta - low, span);
          lane.position = low + (wrapped < 0.0 ? wrapped + span : wrapped);
        }
        break;
      }
      case kBounce: {
        double low = 0.0, high = 0.0;
        loopBounds(low, high);
        const double span = high - low;
        if (span <= 0.0) lane.position = low;
        else {
          const double distance = juce::jlimit(0.0, span, lane.position - low);
          const double phase = lane.bounce_forward ? distance : 2.0 * span - distance;
          const double wrapped = std::fmod(phase + std::abs(delta), 2.0 * span);
          lane.bounce_forward = wrapped < span;
          lane.position = low + (lane.bounce_forward ? wrapped : 2.0 * span - wrapped);
        }
        break;
      }
      default:
        lane.position = juce::jlimit(0.0, static_cast<double>(num_frames - 1), position * (num_frames - 1.0));
        break;
    }
  }

  float SpectralSource::control(int index, int sample, int lane, float fallback) const {
    const Output* source = input(index)->source;
    const float value = source->buffer[source->isControlRate() ? 0 : sample][lane];
    return finite(value) ? value : fallback;
  }

  void SpectralSource::synthesizeFrame(LaneState& lane, int lane_index, int sample,
                                       float pitch_ratio, float root_note,
                                       int phase_mode, int spectral_effect) {
    if (active_analysis_ == nullptr)
      return;
    const int analysis_channel = active_analysis_->stereo ? (lane_index & 1) : 0;
    const SpectralAnalysis& analysis = active_analysis_->analyses[static_cast<size_t>(analysis_channel)];
    if (analysis.numFrames() == 0)
      return;

    static constexpr int size = SpectralAnalysis::kFftSize;
    static constexpr int bins = SpectralAnalysis::kNumBins;
    static constexpr int hop = SpectralAnalysis::kHop;
    auto value = [&](int index, float fallback = 0.0f) { return control(index, sample, lane_index, fallback); };
    auto& synth_phase = lane.synth_phase;
    auto& overlap = lane.overlap;

    bool transient = false;
    // The analysed phases are read below, only for the bins that use them.
    analysis.readFrameInterpolated(lane.position, lane.magnitude.data(), lane.frequency.data(),
                                   nullptr, &transient);
    lane.original_magnitude = lane.magnitude;

    const float root_frequency = noteToFrequency(root_note);
    const int nominal_fundamental = juce::jlimit(1, bins - 1,
        static_cast<int>(std::round(root_frequency * size / analysis.sourceSampleRate())));
    int fundamental_bin = nominal_fundamental;
    const int search_radius = juce::jmax(5, nominal_fundamental / 4);
    float best_magnitude = -1.0f;
    for (int offset = -search_radius; offset <= search_radius; ++offset) {
      const int bin = nominal_fundamental + offset;
      if (bin < 1 || bin >= bins)
        continue;
      if (lane.magnitude[static_cast<size_t>(bin)] > best_magnitude) {
        best_magnitude = lane.magnitude[static_cast<size_t>(bin)];
        fundamental_bin = bin;
      }
    }

    const float fundamental_magnitude = lane.magnitude[static_cast<size_t>(fundamental_bin)];
    int fundamental_low = fundamental_bin;
    int fundamental_high = fundamental_bin;
    if (fundamental_magnitude > 1.0e-6f) {
      const float threshold = fundamental_magnitude * 0.35f;
      while (fundamental_low > 1 && (fundamental_bin - fundamental_low) < 8 &&
             lane.magnitude[static_cast<size_t>(fundamental_low - 1)] >= threshold)
        --fundamental_low;
      while (fundamental_high < bins - 1 && (fundamental_high - fundamental_bin) < 8 &&
             lane.magnitude[static_cast<size_t>(fundamental_high + 1)] >= threshold)
        ++fundamental_high;
    }

    const bool keep_fundamental = value(kKeepFundamental) >= 0.5f;
    const float low_control = juce::jlimit(0.0f, 1.0f, value(kBinLow));
    const float high_control = juce::jlimit(0.0f, 1.0f, value(kBinHigh, 1.0f));
    const float low = spectralBin(low_control);
    const float high = spectralBin(high_control);
    const float band_gain = smoothEdge((high - low) * 0.5f);
    for (int bin = 0; bin < bins; ++bin) {
      const float low_gain = low_control <= 0 ? 1.0f : smoothEdge((bin - low) * 0.5f + 0.5f);
      const float high_gain = high_control >= 1 ? 1.0f : smoothEdge((high - bin) * 0.5f + 0.5f);
      lane.magnitude[static_cast<size_t>(bin)] *= band_gain * low_gain * high_gain;
    }

    const float tilt = juce::jlimit(-24.0f, 24.0f, value(kSpectralTilt));
    if (std::abs(tilt) > 0.0001f) {
      // A log and a power per bin cost more than the rest of the frame, and
      // tilt rarely moves between hops.
      if (!lane.tilt_gain_valid || tilt != lane.tilt_gain_tilt) {
        const float reference_bin = bins / 8.0f;
        for (int bin = 1; bin < bins; ++bin) {
          const float octaves = std::log2(bin / reference_bin);
          lane.tilt_gain[static_cast<size_t>(bin)] = std::pow(10.0f, tilt * octaves / 20.0f);
        }
        lane.tilt_gain_valid = true;
        lane.tilt_gain_tilt = tilt;
      }
      for (int bin = 1; bin < bins; ++bin)
        lane.magnitude[static_cast<size_t>(bin)] *= lane.tilt_gain[static_cast<size_t>(bin)];
    }

    const float raw_effect_amount = juce::jlimit(0.0f, 1.0f, value(kEffectAmount));
    const float effect_amount = curvedSpectralAmount(raw_effect_amount);
    if (spectral_effect != kNoEffect) {
      float max_magnitude = 1.0e-9f;
      for (int bin = 1; bin < bins; ++bin)
        max_magnitude = std::max(max_magnitude, lane.magnitude[static_cast<size_t>(bin)]);
      const float global_reference = std::max(analysis.globalMaxMagnitude(), max_magnitude);
      std::copy(lane.magnitude.begin(), lane.magnitude.end(), lane.effect_original.begin());
      switch (spectral_effect) {
        case kGate: {
          const float threshold = curvedGateThreshold(raw_effect_amount) * global_reference;
          for (int bin = 1; bin < bins; ++bin) {
            if (lane.magnitude[static_cast<size_t>(bin)] < threshold)
              lane.magnitude[static_cast<size_t>(bin)] = 0.0f;
          }
          break;
        }
        case kCompress: {
          const float power = 1.0f - 0.7f * effect_amount;
          for (int bin = 1; bin < bins; ++bin) {
            const float normalized = lane.magnitude[static_cast<size_t>(bin)] / max_magnitude;
            lane.magnitude[static_cast<size_t>(bin)] = std::pow(normalized, power) * max_magnitude;
          }
          break;
        }
        case kExpand: {
          const float power = 1.0f + 3.0f * effect_amount;
          for (int bin = 1; bin < bins; ++bin) {
            const float normalized = lane.magnitude[static_cast<size_t>(bin)] / max_magnitude;
            lane.magnitude[static_cast<size_t>(bin)] = std::pow(normalized, power) * max_magnitude;
          }
          break;
        }
        case kComb: {
          if (effect_amount <= 0.001f)
            break;
          const int period = 2 + static_cast<int>(std::round(effect_amount * 14.0f));
          for (int bin = 1; bin < bins; ++bin) {
            if ((bin % period) != 0)
              lane.magnitude[static_cast<size_t>(bin)] = 0.0f;
          }
          break;
        }
        case kDetune: {
          const float peak_floor = 0.05f * max_magnitude;
          const float offset_bins = effect_amount * 1.8f;
          const float duplicate_gain = 0.5f + 0.4f * effect_amount;
          std::copy(lane.magnitude.begin(), lane.magnitude.end(), lane.scratch.begin());
          for (int bin = 2; bin < bins - 2; ++bin) {
            const float magnitude = lane.scratch[static_cast<size_t>(bin)];
            if (magnitude < peak_floor)
              continue;
            if (magnitude <= lane.scratch[static_cast<size_t>(bin - 1)] ||
                magnitude < lane.scratch[static_cast<size_t>(bin + 1)] ||
                magnitude < lane.scratch[static_cast<size_t>(bin - 2)] ||
                magnitude < lane.scratch[static_cast<size_t>(bin + 2)])
              continue;

            const float target_bin = bin + offset_bins;
            const int target_index = static_cast<int>(std::floor(target_bin));
            const float fraction = target_bin - target_index;
            const float add_magnitude = magnitude * duplicate_gain;
            if (target_index >= 1 && target_index < bins)
              lane.magnitude[static_cast<size_t>(target_index)] += add_magnitude * (1.0f - fraction);
            if (target_index + 1 >= 1 && target_index + 1 < bins)
              lane.magnitude[static_cast<size_t>(target_index + 1)] += add_magnitude * fraction;
          }
          break;
        }
        case kSpread: {
          const int width = 1 + static_cast<int>(std::round(effect_amount * 6.0f));
          const float peak_floor = 0.05f * max_magnitude;
          std::copy(lane.magnitude.begin(), lane.magnitude.end(), lane.scratch.begin());
          for (int bin = 2; bin < bins - 2; ++bin) {
            const float magnitude = lane.scratch[static_cast<size_t>(bin)];
            if (magnitude < peak_floor ||
                magnitude <= lane.scratch[static_cast<size_t>(bin - 1)] ||
                magnitude < lane.scratch[static_cast<size_t>(bin + 1)])
              continue;
            for (int spread = 1; spread <= width; ++spread) {
              const float spread_magnitude = magnitude * (1.0f - spread / (width + 1.0f));
              if (bin - spread >= 1)
                lane.magnitude[static_cast<size_t>(bin - spread)] =
                    std::max(lane.magnitude[static_cast<size_t>(bin - spread)], spread_magnitude);
              if (bin + spread < bins)
                lane.magnitude[static_cast<size_t>(bin + spread)] =
                    std::max(lane.magnitude[static_cast<size_t>(bin + spread)], spread_magnitude);
            }
          }
          break;
        }
        case kShapedTone: {
          if (fundamental_bin >= 1) {
            const float base_bin = static_cast<float>(fundamental_bin);
            for (int bin = 1; bin < bins; ++bin) {
              const float harmonic_index = bin / base_bin;
              const float distance = std::abs(harmonic_index - std::round(harmonic_index));
              const float harmonic_gain = 1.0f - std::min(1.0f, distance * 2.0f);
              lane.magnitude[static_cast<size_t>(bin)] *= (1.0f - effect_amount) + effect_amount * harmonic_gain;
            }
          }
          break;
        }
        default:
          break;
      }

      for (int bin = 1; bin < bins; ++bin) {
        lane.magnitude[static_cast<size_t>(bin)] =
            lane.effect_original[static_cast<size_t>(bin)] * (1.0f - effect_amount) +
            lane.magnitude[static_cast<size_t>(bin)] * effect_amount;
      }
    }

    float cleanup_reference = 1.0e-9f;
    for (int bin = 1; bin < bins; ++bin)
      cleanup_reference = std::max(cleanup_reference, lane.magnitude[static_cast<size_t>(bin)]);
    const float cleanup_floor = cleanup_reference * 2.5e-5f;
    for (int bin = 1; bin < bins; ++bin) {
      if (lane.magnitude[static_cast<size_t>(bin)] < cleanup_floor)
        lane.magnitude[static_cast<size_t>(bin)] = 0.0f;
    }

    const float blur = juce::jlimit(0.0f, 1.0f, value(kSpectralBlur));
    // Fractional diffusion passes make Blur continuous all the way from zero.
    const float diffusion = blur * 12.0f;
    for (int pass = 0; pass < static_cast<int>(std::ceil(diffusion)); ++pass) {
      const float mix = std::min(1.0f, diffusion - pass);
      lane.scratch[0] = lane.magnitude[0];
      lane.scratch[static_cast<size_t>(bins - 1)] = lane.magnitude[static_cast<size_t>(bins - 1)];
      for (int bin = 1; bin < bins - 1; ++bin)
        lane.scratch[bin] = lane.magnitude[bin] + mix * 0.25f *
            (lane.magnitude[bin - 1] - 2.0f * lane.magnitude[bin] + lane.magnitude[bin + 1]);
      lane.magnitude = lane.scratch;
    }

    // Preserve the recorded fundamental lobe, including its tuning and phase,
    // rather than replacing it with a sine at the declared MIDI root.
    if (keep_fundamental) {
      for (int bin = fundamental_low; bin <= fundamental_high; ++bin)
        lane.magnitude[bin] = lane.original_magnitude[bin];
    }

    // Find peak regions before advancing phases. Updating peaks and followers
    // in one ascending loop made the left half use the preceding frame's phase.
    lane.peak_of.fill(-1);
    for (int bin = 1; bin < bins - 1; ++bin) {
      if (lane.magnitude[bin] > 1.0e-9f && lane.magnitude[bin] > lane.magnitude[bin - 1] &&
          lane.magnitude[bin] >= lane.magnitude[bin + 1])
        lane.peak_of[bin] = bin;
    }
    int last = -1;
    for (int bin = 1; bin < bins; ++bin) {
      if (lane.peak_of[bin] == bin) last = bin;
      else lane.peak_of[bin] = last;
    }
    int next = -1;
    for (int bin = bins - 1; bin >= 1; --bin) {
      if (lane.peak_of[bin] == bin) next = bin;
      else if (next >= 0 && (lane.peak_of[bin] < 0 || next - bin < bin - lane.peak_of[bin]))
        lane.peak_of[bin] = next;
    }

    lane.work.fill(0.0f);
    const float stretch = juce::jlimit(0.25f, 4.0f, value(kSpectralStretch, 1.0f));
    const float shift = juce::jlimit(-1.0f, 1.0f, value(kSpectralShift));
    // Hz, independent of host sample rate and oversampling. Fine at the centre,
    // still reaches the original 48 kHz engine's +/-6 kHz at the extremes.
    const float shift_hz = 6000.0f * shift * std::abs(shift);
    const float source_rate = static_cast<float>(analysis.sourceSampleRate());
    const float output_rate = static_cast<float>(getSampleRate());
    const float random_phase = juce::jlimit(0.0f, 1.0f, value(kPhaseRandomize));
    const int analysis_frame = static_cast<int>(std::floor(lane.position + 0.5));
    const bool reset_phases = !lane.phases_initialized ||
        (value(kPreserveTransients) >= 0.5f && transient && analysis_frame != lane.last_transient_frame);
    // A frozen/slow transient must only reset once, not once per synthesis hop.
    lane.last_transient_frame = transient ? analysis_frame : -1;

    // Each analysed phase costs two remainders. A reset takes every bin's; after
    // that only the bins locked to a peak read theirs (and their peak's), and
    // vocoder phases read none.
    const SpectralAnalysis::FramePair frames = analysis.framePair(lane.position);
    if (reset_phases) {
      for (int bin = 0; bin < bins; ++bin)
        lane.phase[bin] = SpectralAnalysis::interpolatedPhase(frames, bin);
    }
    else if (phase_mode != kVocoder) {
      std::array<bool, bins> needed {};
      for (int bin = 1; bin < bins; ++bin) {
        if (lane.magnitude[bin] > 1.0e-9f && lane.peak_of[bin] >= 1) {
          needed[bin] = true;
          needed[lane.peak_of[bin]] = true;
        }
      }
      for (int bin = 1; bin < bins; ++bin) {
        if (needed[bin])
          lane.phase[bin] = SpectralAnalysis::interpolatedPhase(frames, bin);
      }
    }

    for (int bin = 1; bin < bins; ++bin) {
      const int peak = lane.peak_of[bin] < 0 ? bin : lane.peak_of[bin];
      const bool preserve = keep_fundamental && peak == fundamental_bin;
      const float scale = pitch_ratio * (preserve ? 1.0f : stretch);
      const float peak_hz = stableSourceFrequency(peak, lane.frequency[peak], source_rate);
      const float source_hz = stableSourceFrequency(bin, lane.frequency[bin], source_rate);
      const float frequency_shift = peak_hz * (scale - 1.0f) + (preserve ? 0.0f : shift_hz);
      const float target_hz = source_hz + frequency_shift;
      // Keep the entire lobe around each peak. Collapsing all its bins onto
      // their instantaneous frequency destroys both timbre and amplitude.
      lane.scratch[bin] = bin + peak_hz * (scale * source_rate / output_rate - 1.0f) * size / source_rate +
                          (preserve ? 0.0f : shift_hz * size / output_rate);
      if (reset_phases)
        synth_phase[bin] = lane.phase[bin];
      else
        synth_phase[bin] = std::remainder(synth_phase[bin] +
            juce::MathConstants<float>::twoPi * target_hz * hop / output_rate,
            juce::MathConstants<float>::twoPi);
    }
    lane.phases_initialized = true;

    for (int bin = 1; bin < bins; ++bin) {
      const float magnitude = lane.magnitude[bin];
      if (magnitude <= 1.0e-9f) continue;
      const float target_bin = lane.scratch[bin];
      if (!finite(target_bin) || target_bin <= 0.0f || target_bin >= bins - 1.0f) continue;
      const int output_bin = static_cast<int>(std::floor(target_bin));
      const float fraction = target_bin - output_bin;
      const int peak = lane.peak_of[bin];
      const bool lock = peak >= 1 && (phase_mode == kPeakLocked ||
                       (phase_mode == kLocked && std::abs(peak - bin) <= 2));
      float phase = lock ? synth_phase[peak] + lane.phase[bin] - lane.phase[peak] : synth_phase[bin];
      if (random_phase > 0.0f) {
        // Stable per-bin offsets; do not inject fresh phase noise or a channel
        // gain imbalance every hop. Mono samples stay mono at Randomize=0.
        uint32_t seed = lane.random_seed ^ (static_cast<uint32_t>(bin) * 2654435761u);
        seed ^= seed >> 16; seed *= 2246822519u; seed ^= seed >> 13;
        phase += random_phase * (static_cast<float>(seed >> 8) / 16777216.0f - 0.5f) *
                 juce::MathConstants<float>::twoPi;
      }
      const float real = magnitude * std::cos(phase);
      const float imag = magnitude * std::sin(phase);
      lane.work[2 * output_bin] += real * (1.0f - fraction);
      lane.work[2 * output_bin + 1] += imag * (1.0f - fraction);
      lane.work[2 * (output_bin + 1)] += real * fraction;
      lane.work[2 * (output_bin + 1) + 1] += imag * fraction;
    }
    lane.work[1] = 0.0f;
    fft_->performRealOnlyInverseTransform(lane.work.data());
    // Overlap-add in runs where neither ring index wraps, so the loop vectorises.
    for (int start = 0; start < size;) {
      const int overlap_start = (lane.write_pos + start) % size;
      const int work_start = (start + size / 2) % size;
      const int run = std::min({ size - start, size - overlap_start, size - work_start });
      float* out = overlap.data() + overlap_start;
      const float* in = lane.work.data() + work_start;
      const float* window = window_.data() + start;
      for (int i = 0; i < run; ++i) {
        const float sample_value = in[i] * window[i];
        out[i] += finite(sample_value) ? sample_value : 0.0f;
      }
      start += run;
    }
  }

  void SpectralSource::process(int num_samples) {
    if (num_samples <= 0) return;
    sample_->markUsed();
    refreshAnalysisIfNeeded();

    const poly_mask reset_mask = getResetMask(kReset);
    const poly_mask retrigger_mask = getResetMask(kRetrigger);
    const poly_int reset_offset = input(kReset)->source->trigger_offset;
    const poly_int retrigger_offset = input(kRetrigger)->source->trigger_offset;
    const int scale = utils::iclamp(static_cast<int>(control(kTransposeQuantizeScale, 0, 0)), 0, 14);
    int quantize = static_cast<int>(control(kTransposeQuantize, 0, 0));
    if (scale != kSpectralTransposeQuantizeCustomScale) {
      quantize = quantizeScaleMask(scale, static_cast<int>(control(kTransposeQuantizeKey, 0, 0)));
      if (control(kTransposeQuantizeMode, 0, 0) >= 0.5f && quantize != 0)
        quantize |= 1 << kNotesPerOctave;
    }
    const int play_mode = utils::iclamp(static_cast<int>(control(kPlayMode, 0, 0)), 0, kNumPlayModes - 1);
    const int phase_mode = utils::iclamp(static_cast<int>(control(kPhaseMode, 0, 0)), 0, kNumPhaseModes - 1);
    const int spectral_effect = utils::iclamp(static_cast<int>(control(kSpectralEffect, 0, 0)), 0, kNumSpectralEffects - 1);
    const bool freeze = control(kFreeze, 0, 0) >= 0.5f;
    const SlotModulation::Block modulation = modulation_.resolve(*this, kDistortionType, kDistortionAmount,
                                                                 kSecondaryDistortionType,
                                                                 kSecondaryDistortionAmount, false);
    const bool rm_active = modulation.anyRm();

    for (int frame = 0; frame < num_samples; ++frame) {
      poly_float raw = 0.0f;
      poly_float pan = 0.0f;
      poly_float level = 0.0f;
      for (int lane_index = 0; lane_index < poly_float::kSize; ++lane_index) {
        LaneState& lane = lanes_[static_cast<size_t>(lane_index)];
        pan.set(lane_index, juce::jlimit(-1.0f, 1.0f, control(kPan, frame, lane_index)));
        level.set(lane_index, juce::jlimit(0.0f, kMaxAmplitude, control(kLevel, frame, lane_index)));
        if (control(kActiveVoices, frame, lane_index) < 0.5f) continue;
        // A new voice must not consume any of its FFT frame before note-on.
        if (reset_mask[lane_index] && frame < reset_offset[lane_index]) continue;
        const bool hard_reset = reset_mask[lane_index] && frame == reset_offset[lane_index];
        const bool retrigger = retrigger_mask[lane_index] && frame == retrigger_offset[lane_index];
        if (hard_reset || retrigger) {
          const float previous_output = lane.last_output;
          resetLane(lane_index);
          if (!hard_reset) lane.last_output = previous_output;
        }
        // Never play unrelated raw PCM while spectral analysis is pending.
        // In particular a frozen raw PCM sample would be a DC signal.
        if (!lane.analysis_valid) continue;

        if (lane.samples_until_frame <= 0) {
          if (!lane.started) {
            const float start = juce::jlimit(0.0f, 1.0f, control(kPosition, frame, lane_index));
            const double last = active_analysis_->analyses[0].numFrames() - 1.0;
            lane.position = (play_mode == kReverse ? 1.0f - start : start) * last;
            lane.started = true;
          }
          else if (!freeze)
            advancePosition(lane, play_mode, control(kPosition, frame, lane_index),
                            juce::jlimit(-2.0f, 2.0f, control(kSpeed, frame, lane_index, 1.0f)),
                            control(kLoopStart, frame, lane_index), control(kLoopEnd, frame, lane_index, 1.0f));

          const float root = juce::jlimit(0.0f, 127.0f, control(kRootKey, frame, lane_index, 60.0f));
          const float midi = (control(kMidi, frame, lane_index, root) - root) *
                             juce::jlimit(0.0f, 1.0f, control(kKeytrack, frame, lane_index));
          const float transpose = juce::jlimit(-96.0f, 96.0f, control(kTranspose, frame, lane_index));
          const float offset = snapTranspose(poly_float(midi), poly_float(transpose), quantize)[0] +
                               control(kTune, frame, lane_index);
          const float ratio = std::exp2(juce::jlimit(-96.0f, 96.0f, offset) / 12.0f);
          synthesizeFrame(lane, lane_index, frame, ratio, root, phase_mode, spectral_effect);
          lane.write_pos = (lane.write_pos + SpectralAnalysis::kHop) % SpectralAnalysis::kFftSize;
          lane.samples_until_frame = SpectralAnalysis::kHop;
        }

        // Periodic Hann analysis/synthesis windows sum to exactly 3/2 at 4x overlap.
        const float synthesized = lane.overlap[lane.read_pos] * (1.0f / 1.5f);
        lane.overlap[lane.read_pos] = 0.0f;
        lane.read_pos = (lane.read_pos + 1) % SpectralAnalysis::kFftSize;
        --lane.samples_until_frame;
        const float sample_value = applyModeHandoff(lane, play_mode, synthesized);
        raw.set(lane_index, finite(sample_value) ? juce::jlimit(-4.0f, 4.0f, sample_value * 0.75f) : 0.0f);
      }
      if (rm_active)
        raw *= modulation.ringGain(frame);
      output(kRaw)->buffer[frame] = raw;
      output(kLevelled)->buffer[frame] = futils::panAmplitude(pan) * level * level * raw;
    }
    sample_->markUnused();
  }
} // namespace vital
