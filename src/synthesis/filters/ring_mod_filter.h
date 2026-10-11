// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "processor.h"

namespace vital {
  // Serum-style ring modulation filter: the input is multiplied by a carrier
  // whose frequency follows Cutoff (including keytrack). Style chooses the
  // carrier waveform. The dry/wet balance comes from FilterModule's Mix.
  class RingModFilter : public Processor {
    public:
      enum { kAudio, kReset, kMidiCutoff, kStyle, kNumInputs };
      enum Waveform { kSine, kSquare, kSaw, kTriangle, kNumWaveforms };

      RingModFilter();
      Processor* clone() const override { return new RingModFilter(*this); }
      void process(int num_samples) override;
      void reset(poly_mask reset_mask) override;

      // Bipolar carrier in [-1, 1]. Every shape starts at phase 0; square and
      // saw use PolyBLEP so bright carriers do not alias badly.
      static mono_float carrier(int waveform, mono_float phase, mono_float phase_delta);

    private:
      poly_float phase_;

      JUCE_LEAK_DETECTOR(RingModFilter)
  };
} // namespace vital
