// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from OpenWurli 0.9.0, crates/openwurli-dsp/src/voice.rs, reed.rs and hammer.rs
// (GPL-3.0-or-later).
//
// One struck Wurlitzer 200A reed: a modal reed (seven damped modes of a cantilever), the
// hammer's attack noise, and the electrostatic pickup, which supplies the bark. The output is the
// pickup voltage in volts (millivolts at mf), before the preamp. Ported in double precision with
// every expression in the Rust's order, so the output is bit-identical to OpenWurli's on the same
// libm (tests/openwurli/voice_reference_tests.h).
#pragma once

#include "wurli_biquad.h"
#include "wurli_pickup.h"
#include "wurli_tables.h"

#include <cstdint>

namespace openwurli {
  // Builds the static per-key and DFT tables the note-on reads, so the audio thread never builds
  // them. Thread-safe; call it once from the main thread before the first note.
  void prepareVoiceTables();

  // hammer.rs: contact time of the felt hammer, 0.75 (ff) to 1 (pp) fundamental cycles (Miessner,
  // US 2,932,231), clamped 0.3-20 ms.
  double dwellTime(double velocity, double fundamental_hz);
  // Onset ramp (the reed's inertia): 1 period at ff, 2 at pp, at least 2 ms.
  double onsetRampTime(double velocity, double fundamental_hz);
  // Gaussian low-pass of the finite hammer contact on each mode's excitation (sigma 8 in f*T
  // units), normalised to mode 1.
  ModeArray dwellAttenuation(double velocity, double fundamental_hz, const ModeArray& mode_ratios);

  // reed.rs: seven damped modes, each a quadrature oscillator rotated once per sample and
  // renormalised every 1024 samples. Each mode holds its frequency exactly (no pitch jitter).
  class ModalReed {
    public:
      // `amplitudes` post dwell filter and variation, `decay_rates_db` in dB/s.
      void init(double fundamental_hz, const ModeArray& mode_ratios, const ModeArray& amplitudes,
                const ModeArray& decay_rates_db, double onset_time_s, double velocity, double sample_rate);
      // Quadratic air drag on mode 1, dA/dt = -beta |A| A: `beta_db_per_mm` extra dB/s per mm of
      // swing at the pickup, `swing_mm` the swing one unit of displacement stands for.
      void setAirDrag(double beta_db_per_mm, double swing_mm, double sample_rate);
      // The damper felt: rates growing 3x per mode (higher modes die first), ramped in over
      // 50 / 25 / 8 ms by register. The top five keys (MIDI 92 up) have no damper.
      void startDamper(int midi_note, double sample_rate);
      // Adds the reed's displacement to `output`.
      void render(double* output, int num_samples);
      bool isSilent(double threshold_db) const;
      bool isDamping() const { return damper_active_; }
      double releaseSeconds(double sample_rate) const;
      // Atlas: every mode's frequency times `ratio`, keeping phases and envelopes.
      void setPitchRatio(double ratio);

    private:
      struct Mode {
        double s;            // quadrature sine state (the output)
        double c;            // quadrature cosine state
        double cos_inc;      // rotation per sample
        double sin_inc;
        double amplitude;
        double decay_mult;   // natural decay per sample
        double envelope;
        double damper_rate;  // nepers per sample, set by the damper
        double damper_mult;  // exp(-damper_rate)
        double phase_inc;    // radians per sample as struck (Atlas: the base for setPitchRatio)
      };

      Mode modes_[kNumModes] = {};
      uint64_t sample_ = 0;
      uint64_t onset_ramp_samples_ = 0;
      double onset_ramp_inc_ = 0.0;
      double onset_shape_exp_ = 1.0;
      bool damper_active_ = false;
      double damper_ramp_samples_ = 0.0;
      double damper_release_count_ = 0.0;
      bool damper_ramp_done_ = false;
      // Air drag per sample per unit of mode-1 envelope; 0 = off.
      double drag_k_ = 0.0;
      double pitch_ratio_ = 1.0;
  };

  // hammer.rs: the impact transient, band-passed noise (5x the fundamental, 200-2000 Hz, Q 0.7)
  // decaying with a 3 ms time constant over 15 ms, faded in over 16 samples so simultaneous
  // attacks do not step.
  class AttackNoise {
    public:
      void init(double velocity, double fundamental_hz, double sample_rate, uint32_t seed);
      // Adds the burst to `output`.
      void render(double* output, int num_samples);
      bool isDone() const { return remaining_ == 0; }

    private:
      double nextNoise();

      double amplitude_ = 0.0;
      double decay_per_sample_ = 0.0;
      uint32_t remaining_ = 0;
      uint32_t fade_in_remaining_ = 0;
      Biquad bpf_;
      uint32_t rng_state_ = 0;
  };

  // One struck reed: modal reed, hammer noise and electrostatic pickup (voice.rs). Output in volts
  // at the pickup. Allocation-free, and copyable (a copy carries on exactly as the original).
  class Voice {
    public:
      // Voice::note_on_with_config. midi_note is clamped to [kMidiLo, kMidiHi] here; velocity
      // 0..1; noise_seed decorrelates simultaneous notes.
      void noteOn(int midi_note, double velocity, double sample_rate, uint32_t noise_seed, bool mlp_enabled,
                  const VoiceSettings& settings = VoiceSettings());
      // Voice::note_off: starts the damper.
      void noteOff();
      // Voice::render: overwrites `output`.
      void render(double* output, int num_samples);
      // Voice::is_silent: every mode 80 dB down, or 10 s after the damper started.
      bool isSilent() const;
      // Atlas extension, not in OpenWurli: scale every mode's frequency by `ratio` relative to how
      // the note was struck (pitch bend, glide), keeping each mode's phase and envelope. 1.0
      // leaves the output bit-identical to never calling it; noteOn resets it to 1.
      void setPitchRatio(double ratio);
      bool active() const { return active_; }
      void clear() { active_ = false; }

    private:
      ModalReed reed_;
      Pickup pickup_;
      AttackNoise noise_;
      double post_pickup_gain_ = 0.0;
      double sample_rate_ = 44100.0;
      int midi_note_ = kMidiLo;
      bool active_ = false;
  };
} // namespace openwurli
