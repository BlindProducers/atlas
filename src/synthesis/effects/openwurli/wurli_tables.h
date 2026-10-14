// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from OpenWurli 0.9.0, crates/openwurli-dsp/src/tables.rs and variation.rs
// (GPL-3.0-or-later): the per-note parameters the voice needs. Of OpenWurli's CalibrationConfig,
// the swing scale, air-drag scale, voicing slope and register trim are VoiceSettings; the swing
// pick stays Central and the level target -29.55 dB. The amplifier-side volume network, the
// intermodulation report and the test-only closed-form proxy are not ported.
//
// The reed physics (Euler-Bernoulli cantilever with tip mass, the provisional swing prior, the
// air-drag and decay fits) is documented upstream in tables.rs and
// docs/research/reed-and-hammer-physics.md.
#pragma once

#include <array>

namespace openwurli {
  constexpr int kNumModes = 7;
  constexpr int kMidiLo = 33;   // A1, reed 1
  constexpr int kMidiHi = 96;   // C7, reed 64
  constexpr int kNumKeys = kMidiHi - kMidiLo + 1;

  using ModeArray = std::array<double, kNumModes>;

  // How a note is voiced: the parts of OpenWurli's CalibrationConfig a player can change, plus
  // two Atlas additions. The defaults are OpenWurli's shipping calibration, and with them every
  // result is bit-identical to OpenWurli's.
  struct VoiceSettings {
    // CalibrationConfig::swing_scale: the ff swing at the pickup relative to the provisional
    // prior. A larger swing takes the reed further into the finger band, where the pickup's
    // nonlinearity makes the bark, and is louder: the keyboard is levelled against C4 at the same
    // swing. Atlas holds each note's swing within the pickup tables' reach (kSwingReachMm*).
    double swing_scale = 1.0;
    // CalibrationConfig::air_drag_scale: 1 = drag coefficient 4.47, 0.447 and 2.236 the 2 and 10
    // corners of its plausible band, 0 = no air drag.
    double air_drag_scale = 1.0;
    // CalibrationConfig::voicing_slope: level tilt in dB per semitone above C4.
    double voicing_slope = -0.04;
    // The register trim (CalibrationConfig::zero_trim off).
    bool register_trim = true;
    // Atlas: the per-reed tuning spread relative to OpenWurli's +-3 cents, the "Condition" that
    // variation.rs leaves as a TODO (up to +-14 cents for an unserviced instrument).
    double detune_scale = 1.0;
    // Atlas: C-2 returned to TR-1's emitter instead of ground, pickup.rs's open fork: the
    // pickup's corner moves from 880 to 1436 Hz.
    bool c2_to_emitter = false;
  };

  // The swing each pickup table can take, mm (tables.rs: the solved tables' reach): reeds 1-20
  // and reeds 21-64.
  constexpr double kSwingReachMmBass = 4.29;
  constexpr double kSwingReachMmTreble = 2.89;

  // Mode amplitudes calibrated against recordings of a 200A: physical mode 2 is 20-37 dB weaker
  // than the bare-beam 1/omega_n prediction (solder tip mass, hammer coupling), and was lowered a
  // further 6 dB to remove a metallic "plink". The low harmonics come from the pickup, not from
  // these modes.
  constexpr ModeArray kBaseModeAmplitudes = { 1.0, 0.005, 0.0035, 0.0018, 0.0011, 0.0007, 0.0005 };

  struct NoteParams {
    double fundamental_hz;
    ModeArray mode_ratios;
    ModeArray mode_amplitudes;
    ModeArray mode_decay_rates;   // dB/s
  };

  // A440 equal temperament.
  double midiToFreq(int midi);
  // Tip (solder) mass ratio mu, interpolated between anchor reeds.
  double tipMassRatio(int midi);
  // f_n / f_1 of a cantilever with tip mass ratio mu: (beta_n / beta_1)^2.
  ModeArray modeRatios(double mu);
  // Reed length in mm (two-segment formula, reed n = midi - 32).
  double reedLengthMm(int midi);
  // PROVISIONAL ff reed swing at the pickup, mm: the median of a stated prior over the unmeasured
  // hammer, linear in log(swing) between knots, never across the reed 20|21 break.
  double provisionalFfSwingMm(int midi);
  // pickup_swing_mm: the provisional swing, floored at 0.02 mm.
  double pickupSwingMm(int midi);
  // pickup_swing_mm_with_config, held within the pickup table's reach.
  double pickupSwingMm(int midi, const VoiceSettings& settings);
  // PROVISIONAL quadratic air drag on mode 1, dB/s per mm of finger-region swing, at air-drag
  // scale 1.
  double airDragDbPerMm(int midi);
  // Pickup coupling of each mode relative to mode 1: the finite plate (6 mm) integrates the mode
  // shape over its length, so higher bending modes partly cancel (Simpson's rule, 32 intervals).
  ModeArray spatialCouplingCoefficients(double mu, double reed_len_mm);
  // 0.005 f^1.22 dB/s, floored at 3 dB/s (frequency-independent losses dominate the bass).
  double fundamentalDecayRate(int midi);
  // decay_n = decay_1 * ratio_n^2 (thermoelastic loss grows with omega^2).
  ModeArray modeDecayRates(int midi, const ModeArray& ratios);
  // RMS of a note's pickup source over H1..H8 through the pickup's one-pole high-pass, with the
  // harmonic magnitudes from a 64-point DFT of one cycle of the field-solved law.
  double pickupRmsProxyLaw(int midi, double swing_mm, double f0, double fc);
  // Empirical per-register level trim, dB (0 at C4).
  double registerTrimDb(int midi);
  // Post-pickup gain that levels the keyboard: velocity-aware pickup RMS proxy, voicing slope and
  // register trim, anchored at C4 (output_scale_with_config). `midi` in [kMidiLo, kMidiHi].
  double outputScale(int midi, double velocity, const VoiceSettings& settings = VoiceSettings());
  // Register-dependent velocity exponent: expanded dynamics mid-keyboard, compressed bass.
  double velocityExponent(int midi);
  // Sigmoid velocity curve (neoprene hammer pad compression), 0 -> 0 and 1 -> 1.
  double velocityScurve(double velocity);
  // The full parameter set for one note, computed directly.
  NoteParams computeNoteParams(int midi);

  // variation.rs: fixed per-reed offsets from a hash of the note, so a key always sounds the
  // same. Tuning within +-3 cents (factory tolerance), mode amplitudes within +-8 %.
  double freqDetune(int midi);
  // The key's tuning offset in [-1, 1] that freqDetune scales.
  double detuneUnit(int midi);
  ModeArray modeAmplitudeOffsets(int midi);

  // Everything the voice derives from the key alone, built once for every key so a note-on does
  // no beam-shape integrals.
  struct KeyParams {
    NoteParams note;
    double freq_detune;
    double detune_unit;
    ModeArray amplitude_offsets;
    double velocity_exponent;
    double provisional_swing_mm;
    double swing_mm;
    double air_drag_db_per_mm;
    double register_trim_db;
  };
  KeyParams computeKeyParams(int midi);
  // The cached KeyParams of `midi`, clamped to [kMidiLo, kMidiHi]. Built on first use.
  const KeyParams& keyParams(int midi);
  // Builds the static tables behind keyParams and pickupRmsProxyLaw.
  void prepareKeyTables();
} // namespace openwurli
