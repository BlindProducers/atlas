// SPDX-License-Identifier: GPL-3.0-or-later
#include "braids_source.h"

#include "futils.h"
#include "synth_constants.h"

#include <algorithm>
#include <cmath>
#include <mutex>

namespace vital {
  namespace {
    constexpr int kBraidsHistoryMask = BraidsSource::kHistorySize - 1;
    static_assert((BraidsSource::kHistorySize & kBraidsHistoryMask) == 0, "history size is a power of two");
    static_assert(BraidsSource::kHistorySize >= BraidsSource::kTaps + 2 * braids_engine::kBlockSize,
                  "history holds the kernel and a block ahead");
    // Rebase positions long before doubles lose sub-sample precision.
    constexpr double kBraidsRebaseThreshold = 1 << 24;
    constexpr double kBraidsKernelPi = 3.14159265358979323846;

    std::shared_ptr<const BraidsSource::Kernel> buildBraidsKernel(double sample_rate) {
      auto kernel = std::make_shared<BraidsSource::Kernel>();
      kernel->sample_rate = sample_rate;
      // Below 96 kHz the kernel low-passes at 90% of the output's Nyquist;
      // at 96 kHz and above it only interpolates.
      const double ratio = sample_rate / braids_engine::kNativeSampleRate;
      const double cutoff = ratio >= 1.0 ? 1.0 : ratio * 0.9;
      const int points = BraidsSource::kTaps * BraidsSource::kKernelPhases + 2;
      kernel->table.resize(static_cast<size_t>(points));
      for (int i = 0; i < points; ++i) {
        const double t = static_cast<double>(i) / BraidsSource::kKernelPhases - BraidsSource::kHalfTaps;
        const double x = cutoff * t;
        const double sinc = std::abs(x) < 1.0e-9 ? 1.0 : std::sin(kBraidsKernelPi * x) / (kBraidsKernelPi * x);
        // Blackman window across the kernel's span.
        const double w = std::max(0.0, std::min(1.0, (t + BraidsSource::kHalfTaps) / BraidsSource::kTaps));
        const double window = 0.42 - 0.5 * std::cos(2.0 * kBraidsKernelPi * w) + 0.08 * std::cos(4.0 * kBraidsKernelPi * w);
        kernel->table[static_cast<size_t>(i)] = static_cast<float>(cutoff * sinc * window);
      }
      // Unity gain at DC.
      double sum = 0.0;
      for (int k = 1; k <= BraidsSource::kTaps; ++k)
        sum += kernel->table[static_cast<size_t>(k * BraidsSource::kKernelPhases)];
      if (sum > 0.0) {
        for (float& value : kernel->table)
          value = static_cast<float>(value / sum);
      }
      return kernel;
    }
  } // namespace

  std::shared_ptr<const BraidsSource::Kernel> BraidsSource::kernelFor(double sample_rate) {
    static std::mutex mutex;
    static std::vector<std::shared_ptr<const Kernel>> cache;
    std::lock_guard<std::mutex> lock(mutex);
    for (const auto& kernel : cache) {
      if (kernel->sample_rate == sample_rate)
        return kernel;
    }
    cache.push_back(buildBraidsKernel(sample_rate));
    return cache.back();
  }

  BraidsSource::BraidsSource() : Processor(kNumInputs, kNumOutputs) { }

  void BraidsSource::resetVoice(VoiceState& state, float random_phase) {
    // As the Wavetable type does: up to half a cycle either side of zero, so
    // 100% is anywhere in the cycle. Each oscillator of the voice draws its own.
    uint32_t phases[braids_engine::kNumPhases] = {};
    if (random_phase > 0.0f) {
      for (uint32_t& phase : phases) {
        const double offset = static_cast<double>(random_generator_.next()) * random_phase * 2147483647.0;
        phase = static_cast<uint32_t>(static_cast<int32_t>(offset));
      }
    }
    state.voice.reset(phases);
    state.history.fill(0.0f);
    state.written = 0;
    state.position = 0.0;
  }

  void BraidsSource::renderBlock(VoiceState& state, float note, float timbre, float color) {
    state.voice.setNote(note);
    state.voice.setParameters(timbre, color);
    float block[braids_engine::kBlockSize];
    state.voice.render(block);
    for (float sample : block)
      state.history[static_cast<size_t>(state.written++ & kBraidsHistoryMask)] = sample;
  }

  float BraidsSource::readSample(const VoiceState& state) const {
    const double base = std::floor(state.position);
    const double fraction = state.position - base;
    const double offset = (1.0 - fraction) * kKernelPhases;
    int index = static_cast<int>(offset);
    const float blend = static_cast<float>(offset - index);
    index = std::min(index, kKernelPhases);
    const float* table = kernel_->table.data();
    const int64_t first = static_cast<int64_t>(base) - kHalfTaps + 1;
    float sum = 0.0f;
    for (int tap = 0; tap < kTaps; ++tap) {
      const int point = index + tap * kKernelPhases;
      const float coefficient = table[point] + blend * (table[point + 1] - table[point]);
      sum += coefficient * state.history[static_cast<size_t>((first + tap) & kBraidsHistoryMask)];
    }
    return sum;
  }

  void BraidsSource::process(int num_samples) {
    if (num_samples <= 0)
      return;

    const double sample_rate = getSampleRate();
    if (kernel_ == nullptr || kernel_->sample_rate != sample_rate)
      kernel_ = kernelFor(sample_rate);
    const double step = braids_engine::kNativeSampleRate / sample_rate;

    const int model = static_cast<int>(input(kModel)->at(0)[0] + 0.5f);
    const int resolution = static_cast<int>(input(kResolution)->at(0)[0] + 0.5f);
    const int decimation = static_cast<int>(input(kSampleRate)->at(0)[0] + 0.5f);
    const poly_float note_input = input(kMidi)->at(0) + input(kTranspose)->at(0) + input(kTune)->at(0);
    const poly_float timbre_input = utils::clamp(input(kTimbre)->at(0), 0.0f, 1.0f);
    const poly_float color_input = utils::clamp(input(kColor)->at(0), 0.0f, 1.0f);
    const poly_float random_phase_input = utils::clamp(input(kRandomPhase)->at(0), 0.0f, 1.0f);

    // A voice starting from silence drops whatever its lane played before.
    const poly_mask reset_mask = getResetMask(kReset);
    const Output* events = input(kVoiceEvent)->source;
    const SlotModulation::Block modulation = modulation_.resolve(*this, kDistortionType, kDistortionAmount,
                                                                 kSecondaryDistortionType,
                                                                 kSecondaryDistortionAmount, true);
    const bool fm_active = modulation.anyFm();

    poly_float* raw = output(kRaw)->buffer;
    float mono[kMaxBufferSize * kMaxOversample];
    const int count = std::min(num_samples, kMaxBufferSize * kMaxOversample);
    const float inverse_count = 1.0f / count;

    for (int voice = 0; voice < kVoicesPerProcessor; ++voice) {
      const int lane = 2 * voice;
      VoiceState& state = voices_[static_cast<size_t>(voice)];
      const float note = note_input[lane];
      const float timbre = timbre_input[lane];
      const float color = color_input[lane];
      if (reset_mask[lane]) {
        resetVoice(state, random_phase_input[lane]);
        state.note = note;
        state.timbre = timbre;
        state.color = color;
      }
      state.voice.setModel(model);
      state.voice.setResolution(resolution);
      state.voice.setSampleRate(decimation);

      int strike_at = -1;
      if (events->trigger_mask[lane] && static_cast<int>(events->trigger_value[lane]) == kVoiceOn)
        strike_at = std::max(0, std::min(count - 1, static_cast<int>(events->trigger_offset[lane])));

      for (int i = 0; i < count; ++i) {
        if (i == strike_at)
          state.voice.strike();
        // Render far enough ahead for the kernel around the read position.
        const int64_t needed = static_cast<int64_t>(std::floor(state.position)) + kHalfTaps;
        while (state.written <= needed) {
          const float t = i * inverse_count;
          renderBlock(state, state.note + (note - state.note) * t, state.timbre + (timbre - state.timbre) * t,
                      state.color + (color - state.color) * t);
        }
        mono[i] = readSample(state);
        double advance = step;
        if (fm_active)
          advance *= std::max(0.0f, modulation.rateScale(i)[lane]);
        state.position += advance;
      }
      state.note = note;
      state.timbre = timbre;
      state.color = color;

      if (state.position > kBraidsRebaseThreshold) {
        // Keep ring indices: shift by a whole number of history lengths.
        const int64_t shift = (static_cast<int64_t>(state.position) / kHistorySize - 1) * kHistorySize;
        state.position -= static_cast<double>(shift);
        state.written -= shift;
      }

      for (int i = 0; i < count; ++i) {
        // Braids renders 16-bit integers, so the resampled signal is always finite.
        raw[i].set(lane, mono[i]);
        raw[i].set(lane + 1, mono[i]);
      }
    }
    modulation.applyRingMod(raw, count);

    // Slot level and pan, as the Sample type applies them.
    const poly_float target_pan = futils::panAmplitude(utils::clamp(input(kPan)->at(0), -1.0f, 1.0f));
    if (!pan_initialized_) {
      pan_amplitude_ = target_pan;
      pan_initialized_ = true;
    }
    poly_float current_pan = utils::maskLoad(pan_amplitude_, target_pan, reset_mask);
    const poly_float delta_pan = (target_pan - current_pan) * inverse_count;
    pan_amplitude_ = target_pan;

    const Output* level_source = input(kLevel)->source;
    const bool audio_rate_level = level_source->buffer_size > 1;
    poly_float* levelled = output(kLevelled)->buffer;
    for (int i = 0; i < count; ++i) {
      current_pan += delta_pan;
      const poly_float level = utils::clamp(level_source->buffer[audio_rate_level ? i : 0], 0.0f, kMaxAmplitude);
      levelled[i] = current_pan * level * level * raw[i];
    }
  }
} // namespace vital
