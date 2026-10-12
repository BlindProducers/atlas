// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from OpenWurli 0.9.0, crates/openwurli-dsp/src/speaker.rs
// (GPL-3.0-or-later).
#pragma once

#include "wurli_biquad.h"

namespace openwurli {
  // The 200A's two 4x8" ceramic speakers in the open-backed lid, as a
  // Hammerstein-style chain: memoryless polynomial (x^2 for BL asymmetry, x^3
  // for suspension hardening), tanh excursion limit, slow (5 s) voice-coil
  // thermal compression, then a 30 Hz subsonic high-pass and a 5.5 kHz cone
  // breakup low-pass. "Speaker character" blends from a true passthrough (0;
  // the filters keep running so their state is warm) to all of it (1).
  // Runs at the base rate.
  class Speaker {
    public:
      // Unusable until assigned from Speaker(sample_rate).
      Speaker() = default;
      // Starts at character 1.0.
      explicit Speaker(double sample_rate);

      // 0 = bypass, 1 = authentic; changes under 0.002 are ignored.
      void setCharacter(double character);
      double process(double input);
      void reset();

    private:
      void updateCoefficients();

      Biquad hpf_;
      Biquad lpf_;
      double character_ = 1.0;
      double sample_rate_ = 0.0;
      double a2_ = 0.0;  // x^2 coefficient: even harmonics
      double a3_ = 0.0;  // x^3 coefficient: odd harmonics
      double thermal_coeff_ = 0.0;
      double thermal_alpha_ = 0.0;
      double thermal_state_ = 0.0;
  };
} // namespace openwurli
