// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from OpenWurli 0.9.0, crates/openwurli-dsp/src/tremolo.rs
// (GPL-3.0-or-later), default build (the circuit oscillator, not the
// `legacy-tremolo` sine LFO).
#pragma once

#include "wurli_tremolo_circuit.h"

namespace openwurli {
  // The 200A's vibrato: the Twin-T oscillator drives the LG-1 opto's LED
  // (+14.5 V, R-18 680R, LED, R-17 4.7K, oscillator collector: about
  // 0.37-2.28 mA), the CdS cell follows the light with a 2.5 ms attack and a
  // 35 ms release and a power law between ~9K (lit) and 1M (dark), and the
  // front-panel VIBRATO pot divides that into the shunt the preamp's
  // feedback junction sees. process() returns that shunt for
  // DkPreamp::setLdrResistance. Runs at the oversampled rate.
  class Tremolo {
    public:
      static constexpr int kLedLutSize = 256;

      // Unusable until assigned from Tremolo(depth, sample_rate).
      Tremolo() = default;
      // Builds the oscillator for `sample_rate` and lets it settle for 2 s
      // (it takes that long to build up). Slow; allocation-free.
      Tremolo(double depth, double sample_rate);

      // Vibrato pot position, clamped to 0..1.
      void setDepth(double depth);
      // One sample: the shunt impedance from the feedback junction to ground, ohms.
      double process();
      double currentResistance() const;
      // Rebuilds and re-settles the oscillator (2 s of samples) and darkens the cell.
      void reset();

    private:
      double oscillatorDrive();
      double shuntImpedance() const;
      static tremolo_circuit::CircuitState settledOscillator(double sample_rate);

      tremolo_circuit::CircuitState osc_state_ {};
      double sample_rate_ = 0.0;
      double depth_ = 0.0;
      double r_ldr_ = 0.0;
      double ldr_envelope_ = 0.0;
      double ldr_attack_ = 0.0;
      double ldr_release_ = 0.0;
      double r_ldr_max_ = 0.0;
      double gamma_ = 0.0;
      double ln_r_max_ = 0.0;
      double ln_min_minus_max_ = 0.0;
      // Normalized light against LED current (index proportional to current).
      double led_lut_[kLedLutSize] = {};
  };
} // namespace openwurli
