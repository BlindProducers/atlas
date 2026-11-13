// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "band_reject_core.h"
#include "processor.h"

namespace vital {
  // The Band Reject filter model: BandRejectCore on every lane. Cutoff is the
  // MIDI note the two sections spread around, Resonance their damping on
  // Atlas's law, Drive the gain into their soft clip and Width (the filter's
  // Blend, 0 to 2 as the script's Var from 0 to 100%) how far apart they sit.
  // Resonance, Drive and Width move linearly over a block, as Vital's filters
  // do; the cutoff is followed sample by sample, and a lane's coefficients
  // are worked out again only when its cutoff or width moves. The dry/wet
  // balance comes from FilterModule's Mix.
  class BandRejectFilter : public Processor {
    public:
      enum { kAudio, kReset, kMidiCutoff, kResonance, kDriveGain, kWidth, kNumInputs };

      // Drive in dB, clamped as the other models clamp theirs.
      static constexpr mono_float kMinDriveDb = 0.0f;
      static constexpr mono_float kMaxDriveDb = 36.0f;

      BandRejectFilter();
      Processor* clone() const override { return new BandRejectFilter(*this); }
      void process(int num_samples) override;
      void reset(poly_mask reset_mask) override;
      void hardReset() override;
      // A Filter FX slot's lanes 2-3 repeat lanes 0-1: process lanes 0-1 and
      // copy them.
      void setFxMode(bool fx) { fx_mode_ = fx; }

    private:
      struct Lane {
        BandRejectCore core;
        // What the core's coefficients were set for.
        double set_midi = 0.0, set_width = 0.0, set_rate = 0.0;
        // Where the next block's ramps start: the last block's targets.
        double damping = 2.0, drive = 1.0, width = 0.0;
        // A lane that starts afresh takes its targets at once instead.
        bool fresh = true;
      };
      static void resetLane(Lane& lane);

      Lane lanes_[poly_float::kSize];
      bool fx_mode_ = false;

      JUCE_LEAK_DETECTOR(BandRejectFilter)
  };
} // namespace vital
