// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "processor.h"

namespace vital {
  // Serum-style sample and hold filter: the input is resampled at a rate set
  // by Cutoff (including keytrack), so aliasing follows the played note.
  // Style chooses how held values are output; Blend reduces bit depth.
  // The dry/wet balance comes from FilterModule's Mix.
  class SampleHoldFilter : public Processor {
    public:
      enum { kAudio, kReset, kMidiCutoff, kStyle, kCrush, kNumInputs };
      // Hold: classic steps. Smooth: ramps between held values (one hold
      // period later). Inverted: negated steps, which cancel against the dry
      // signal at 50% Mix to leave only the aliasing residue.
      enum Style { kHold, kSmooth, kInverted, kNumStyles };

      static constexpr mono_float kMaxCrushBits = 16.0f;
      static constexpr mono_float kMinCrushBits = 1.0f;

      SampleHoldFilter();
      Processor* clone() const override { return new SampleHoldFilter(*this); }
      void process(int num_samples) override;
      void reset(poly_mask reset_mask) override;

      // Blend (0 to 2) as bit depth; 0 disables quantization.
      static mono_float crushBits(mono_float blend);
      static mono_float quantize(mono_float value, mono_float bits);

    private:
      poly_float phase_;
      poly_float held_;
      poly_float previous_;

      JUCE_LEAK_DETECTOR(SampleHoldFilter)
  };
} // namespace vital
