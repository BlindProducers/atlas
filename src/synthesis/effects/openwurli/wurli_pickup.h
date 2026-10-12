// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from OpenWurli 0.9.0, crates/openwurli-dsp/src/pickup.rs (GPL-3.0-or-later).
//
// The Wurlitzer 200A pickup is a capacitive sensor: each reed swings in a slot between two fingers
// of a comb held at +147 V, and its capacitance to the comb rises as it enters the finger band.
// The nonlinearity is in that SOURCE term (one reed's capacitance against its penetration, a 2D
// field solve stored as a cubic spline); the plate node it drives (about 240 pF, of which the
// moving reed is 3 pF) is very nearly linear: a differentiator inside a one-pole lag, i.e. a
// high-pass at kPickupFc. The derivation, the drawn-topology refit of the corner and the
// confidence of each constant are in pickup.rs and docs/research/pickup-overlap-law.md upstream.
#pragma once

#include "wurli_biquad.h"

#include <algorithm>

namespace openwurli {
  // Total capacitance at the pickup plate. LOW confidence: one forum measurement, no primary
  // source; the corner below follows it.
  constexpr double kCTotal = 240.0e-12;
  // C-2, 220 pF at TR-1's base, tied to the plate through C-1 (a short at audio).
  constexpr double kC2Base = 220.0e-12;
  // C-2's return node is drawn broken on the schematics; ground is the settled assumption. Its
  // return to TR-1's emitter would bootstrap C-2 away and raise the corner to about 1436 Hz.
  constexpr bool kC2ReturnsToGround = true;
  // One-pole corners fitted to the whole network response at kCTotalRef (0.23 dB max error).
  constexpr double kFcRefC2Ground = 880.5;
  constexpr double kFcRefC2Emitter = 1436.0;
  constexpr double kCTotalRef = 240.0e-12;
  // Small-signal corner of the pickup network, scaled with the node capacitance.
  constexpr double kPickupFc = (kC2ReturnsToGround ? kFcRefC2Ground : kFcRefC2Emitter) *
                               (kCTotalRef + kC2Base) / (kCTotal + kC2Base);
  // RC time constant the nonlinear core runs on, 1 / (2 pi kPickupFc).
  constexpr double kPickupTau = 1.0 / (2.0 * kPi * kPickupFc);
  // The same with C-2 returned to TR-1's emitter (VoiceSettings::c2_to_emitter).
  constexpr double kPickupFcC2Emitter = kFcRefC2Emitter * (kCTotalRef + kC2Base) / (kCTotal + kC2Base);
  constexpr double kPickupTauC2Emitter = 1.0 / (2.0 * kPi * kPickupFcC2Emitter);
  constexpr double kVPolarizing = 147.0;
  // Rest capacitance of the one moving reed (an estimate with the same confidence as kCTotal).
  constexpr double kCReed0 = 3.0e-12;
  // Share of the plate node the moving reed modulates, 1/80.
  constexpr double kCReedRatio = kCReed0 / kCTotal;
  // High-frequency gain on the normalised source, V_pol * C_reed0 / C_total = 1.8375 V.
  constexpr double kPickupSensitivity = kVPolarizing * kCReed0 / kCTotal;
  // Vertical rest offset between reed and finger band, mm ("a piece of paper, but just barely").
  constexpr double kPickupRestOffsetMm = 0.085;

  // One keyboard section's capacitance against u, the reed's penetration past the near finger
  // face (mm): a natural cubic spline on a uniform grid, so the law and its first two
  // derivatives are continuous (the harmonics come from the curve's bends).
  struct PickupLawTable {
    double u0;              // penetration of the first knot, mm
    double du;              // knot spacing, mm
    double t_r;             // reed thickness the table was solved for, mm
    double t_p;             // finger thickness the table was solved for, mm
    const double* y;        // normalised capacitance at each knot
    const double* m;        // spline second derivative at each knot
    int size;               // number of knots

    // Outside the solved range the value is held at the end knot.
    double eval(double u) const {
      const int last = size - 2;
      double pos = (u - u0) / du;
      if (pos < 0.0)
        pos = 0.0;
      if (pos > static_cast<double>(last + 1))
        pos = static_cast<double>(last + 1);
      // `pos as usize`: NaN truncates to 0.
      const int i = std::min(pos >= 0.0 ? static_cast<int>(pos) : 0, last);
      const double t = pos - static_cast<double>(i);
      const double a = 1.0 - t;
      const double h2 = du * du / 6.0;
      return a * y[i] + t * y[i + 1] + ((a * a * a - a) * m[i] + (t * t * t - t) * m[i + 1]) * h2;
    }
  };

  // wurli_pickup_tables.cpp: one law per measured reed range (reed 1 = MIDI 33).
  extern const PickupLawTable kPickupLawReeds01_14;
  extern const PickupLawTable kPickupLawReeds15_20;
  extern const PickupLawTable kPickupLawReeds21_42;
  extern const PickupLawTable kPickupLawReeds43_64;

  // The law for a note's reed: reed width and side clearance per range (1-14 | 15-20 | 21-42 |
  // 43-64), finger thickness per finger section.
  const PickupLawTable& pickupLawFor(int midi);
  // +1 where the finger band sits above the reed (bass, reeds 1-20), -1 below it (treble): the
  // bass/treble solder flip at reeds 20|21.
  double pickupBandSign(int midi);

  // The time-varying RC network driven by one reed's charge. Input: reed displacement in model
  // units (positive = the hammer's direction). Output: plate voltage in volts.
  class Pickup {
    public:
      // Pickup::new_for_note: `amplitude_mm` is the swing one unit of displacement stands for.
      // `c2_to_emitter` runs the network on the 1436 Hz corner.
      void init(double sample_rate, int midi, double amplitude_mm, bool c2_to_emitter = false);
      void setAmplitudeMm(double amplitude_mm) { amplitude_mm_ = amplitude_mm; }
      // In place: displacement in, volts out.
      void process(double* buffer, int num_samples);
      void reset();

    private:
      double w_ = 0.0;          // plate-node voltage perturbation / kPickupSensitivity
      double s_prev_ = 0.0;     // previous source sample
      double m_prev_ = 1.0;     // previous normalised node capacitance 1 + kCReedRatio * s
      double beta_ = 0.0;       // dt / (2 kPickupTau)
      const PickupLawTable* law_ = &kPickupLawReeds21_42;
      double band_sign_ = 1.0;
      double c_rest_ = 0.0;     // law value at rest, so the source is zero at rest
      double amplitude_mm_ = 0.0;
  };
} // namespace openwurli
