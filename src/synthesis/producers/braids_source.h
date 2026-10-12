// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "processor.h"
#include "braids_engine.h"
#include "slot_modulation.h"
#include "utils.h"

#include <array>
#include <memory>
#include <vector>

namespace vital {
  // The per-voice engine of the Braids oscillator type. Each voice runs a
  // Braids macro-oscillator at its native 96 kHz and a windowed-sinc
  // resampler brings it to the engine's rate. A note-on strikes the voice, so
  // the plucked, struck and percussion models sound on every note. A voice
  // that starts from silence can start its oscillators at random phases.
  class BraidsSource : public Processor {
    public:
      static constexpr int kVoicesPerProcessor = poly_float::kSize / 2;
      static constexpr mono_float kMaxAmplitude = 1.41421356237f;
      // Resampler: 32 taps, 256 kernel phases interpolated linearly.
      static constexpr int kHalfTaps = 16;
      static constexpr int kTaps = 2 * kHalfTaps;
      static constexpr int kKernelPhases = 256;
      static constexpr int kHistorySize = 256;

      enum {
        kReset,
        kVoiceEvent,
        kMidi,
        kModel,
        kTimbre,
        kColor,
        kResolution,
        kSampleRate,
        kTranspose,
        kTune,
        kRandomPhase,
        kLevel,
        kPan,
        kDistortionType,
        kDistortionAmount,
        kSecondaryDistortionType,
        kSecondaryDistortionAmount,
        kNumInputs
      };

      enum {
        kRaw,
        kLevelled,
        kNumOutputs
      };

      struct Kernel {
        double sample_rate = 0.0;
        // kTaps * kKernelPhases + 2 points of the kernel, from -kHalfTaps to kHalfTaps.
        std::vector<float> table;
      };
      // Shared by every voice and slot running at `sample_rate`.
      static std::shared_ptr<const Kernel> kernelFor(double sample_rate);

      BraidsSource();

      void process(int num_samples) override;
      Processor* clone() const override { return new BraidsSource(*this); }
      SlotModulation& modulation() { return modulation_; }

    private:
      struct VoiceState {
        braids_engine::Voice voice;
        std::array<float, kHistorySize> history {};
        // 96 kHz samples written, and the read position in the same units.
        int64_t written = 0;
        double position = 0.0;
        // The previous block's settings, for a ramp across this block.
        float note = 60.0f;
        float timbre = 0.5f;
        float color = 0.5f;
      };

      void resetVoice(VoiceState& state, float random_phase);
      void renderBlock(VoiceState& state, float note, float timbre, float color);
      float readSample(const VoiceState& state) const;

      std::array<VoiceState, kVoicesPerProcessor> voices_;
      std::shared_ptr<const Kernel> kernel_;
      utils::RandomGenerator random_generator_ { -1.0f, 1.0f };
      poly_float pan_amplitude_ = 0.0f;
      bool pan_initialized_ = false;
      SlotModulation modulation_;

      JUCE_LEAK_DETECTOR(BraidsSource)
  };
} // namespace vital
