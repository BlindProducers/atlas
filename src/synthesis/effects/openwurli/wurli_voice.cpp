// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from OpenWurli 0.9.0, crates/openwurli-dsp/src/voice.rs, reed.rs and hammer.rs
// (GPL-3.0-or-later). Every expression keeps the Rust's operation order; see wurli_math.h.
#include "wurli_voice.h"

#include "wurli_mlp.h"
#include "wurli_math.h"

#include <algorithm>
#include <cmath>

namespace openwurli {
  namespace {
    using math::clamp;

    // Quadrature renormalisation interval (a power of two): corrects each (s, c) radius against
    // accumulated rounding.
    constexpr uint64_t kRenormInterval = 1024;
    // dB per neper.
    constexpr double kDbPerNeper = 8.686;
    // Samples over which the attack noise fades in from 0 (raised cosine).
    constexpr uint32_t kNoiseFadeInSamples = 16;
    // Voice::is_silent's threshold and safety timeout.
    constexpr double kSilenceThresholdDb = -80.0;
    constexpr double kReleaseTimeoutSeconds = 10.0;
  } // namespace

  void prepareVoiceTables() {
    prepareKeyTables();
  }

  double dwellTime(double velocity, double fundamental_hz) {
    const double cycles = 0.75 + 0.25 * (1.0 - velocity);
    return clamp(cycles / fundamental_hz, 0.0003, 0.020);
  }

  double onsetRampTime(double velocity, double fundamental_hz) {
    const double period_s = 1.0 / fundamental_hz;
    const double periods = 1.0 + 1.0 * (1.0 - velocity);
    return std::fmax(periods * period_s, 0.002);
  }

  ModeArray dwellAttenuation(double velocity, double fundamental_hz, const ModeArray& mode_ratios) {
    const double t_dwell = dwellTime(velocity, fundamental_hz);
    const double sigma_sq = 8.0 * 8.0;

    ModeArray atten;
    for (int i = 0; i < kNumModes; ++i) {
      const double ft = fundamental_hz * mode_ratios[i] * t_dwell;
      atten[i] = std::exp(-ft * ft / (2.0 * sigma_sq));
    }

    const double a0 = atten[0];
    if (a0 > 1e-30) {
      for (double& a : atten)
        a /= a0;
    }
    return atten;
  }

  void ModalReed::init(double fundamental_hz, const ModeArray& mode_ratios, const ModeArray& amplitudes,
                       const ModeArray& decay_rates_db, double onset_time_s, double velocity, double sample_rate) {
    for (int i = 0; i < kNumModes; ++i) {
      Mode& mode = modes_[i];
      const double freq = fundamental_hz * mode_ratios[i];
      const double phase_inc = kTau * freq / sample_rate;
      const double alpha_nepers = decay_rates_db[i] / kDbPerNeper;
      const double decay_per_sample = alpha_nepers / sample_rate;

      mode.s = 0.0;
      mode.c = 1.0;
      math::sinCos(phase_inc, mode.sin_inc, mode.cos_inc);
      mode.amplitude = amplitudes[i];
      mode.decay_mult = std::exp(-decay_per_sample);
      mode.envelope = 1.0;
      mode.damper_rate = 0.0;
      mode.damper_mult = 1.0;
      mode.phase_inc = phase_inc;
    }

    // Onset ramp: all modes ramp in together during hammer contact.
    const uint64_t ramp_samples = math::toU64(std::round(onset_time_s * sample_rate));
    onset_ramp_samples_ = ramp_samples;
    onset_ramp_inc_ = ramp_samples > 0 ? kPi / static_cast<double>(ramp_samples) : 0.0;
    onset_shape_exp_ = 1.0 + (1.0 - velocity);

    sample_ = 0;
    damper_active_ = false;
    damper_ramp_samples_ = 0.0;
    damper_release_count_ = 0.0;
    damper_ramp_done_ = false;
    drag_k_ = 0.0;
    pitch_ratio_ = 1.0;
  }

  void ModalReed::setAirDrag(double beta_db_per_mm, double swing_mm, double sample_rate) {
    const double beta_nepers = beta_db_per_mm / kDbPerNeper;
    drag_k_ = beta_nepers * std::abs(modes_[0].amplitude) * swing_mm / sample_rate;
  }

  void ModalReed::startDamper(int midi_note, double sample_rate) {
    if (midi_note >= 92)
      return;

    const double base_rate = std::fmax(55.0 * std::exp2((static_cast<double>(midi_note) - 60.0) / 24.0), 0.5);
    for (int m = 0; m < kNumModes; ++m) {
      Mode& mode = modes_[m];
      const double factor = std::fmin(base_rate * math::powi(3.0, m), 2000.0);
      mode.damper_rate = factor / sample_rate;
      mode.damper_mult = std::exp(-mode.damper_rate);
    }

    double ramp_time = 0.008;
    if (midi_note < 48)
      ramp_time = 0.050;
    else if (midi_note < 72)
      ramp_time = 0.025;

    damper_ramp_samples_ = ramp_time * sample_rate;
    damper_active_ = true;
    damper_release_count_ = 0.0;
    damper_ramp_done_ = false;
  }

  void ModalReed::render(double* output, int num_samples) {
    for (int i = 0; i < num_samples; ++i) {
      double sum = 0.0;

      if (damper_active_) {
        damper_release_count_ += 1.0;
        const double t = damper_release_count_;
        const double ramp = damper_ramp_samples_;
        if (!damper_ramp_done_) {
          if (t > ramp) {
            damper_ramp_done_ = true;
          }
          else {
            // During the ramp the felt's contact grows: the instantaneous rate needs an exp.
            for (Mode& mode : modes_) {
              const double inst_rate = mode.damper_rate * t / ramp;
              mode.envelope *= std::exp(-inst_rate);
            }
          }
        }
        if (damper_ramp_done_) {
          for (Mode& mode : modes_)
            mode.envelope *= mode.damper_mult;
        }
      }

      // Raised-cosine onset, cosine^1 at ff to cosine^2 at pp.
      double onset = 1.0;
      if (sample_ < onset_ramp_samples_) {
        const double n = static_cast<double>(sample_);
        const double cosine = 0.5 * (1.0 - std::cos(n * onset_ramp_inc_));
        if (onset_shape_exp_ <= 1.001)
          onset = cosine;
        else if (onset_shape_exp_ >= 1.999)
          onset = cosine * cosine;
        else
          onset = std::pow(cosine, onset_shape_exp_);
      }

      for (Mode& mode : modes_) {
        sum += mode.amplitude * mode.s * onset * mode.envelope;

        const double s_new = mode.s * mode.cos_inc + mode.c * mode.sin_inc;
        const double c_new = mode.c * mode.cos_inc - mode.s * mode.sin_inc;
        mode.s = s_new;
        mode.c = c_new;

        mode.envelope *= mode.decay_mult;
      }

      // Air drag: the decay rate of mode 1 grows with its current amplitude (linearised update).
      if (drag_k_ > 0.0) {
        const double env = modes_[0].envelope;
        modes_[0].envelope = env * (1.0 - drag_k_ * env);
      }

      if ((sample_ & (kRenormInterval - 1)) == 0 && sample_ > 0) {
        for (Mode& mode : modes_) {
          const double r_sq = mode.s * mode.s + mode.c * mode.c;
          const double r_inv = 1.0 / std::sqrt(r_sq);
          mode.s *= r_inv;
          mode.c *= r_inv;
        }
      }

      output[i] += sum;
      sample_ += 1;
    }
  }

  bool ModalReed::isSilent(double threshold_db) const {
    const double threshold_linear = math::exp10(threshold_db / 20.0);
    for (const Mode& mode : modes_) {
      if (!(std::abs(mode.amplitude * mode.envelope) <= threshold_linear))
        return false;
    }
    return true;
  }

  double ModalReed::releaseSeconds(double sample_rate) const {
    return damper_active_ ? damper_release_count_ / sample_rate : 0.0;
  }

  void ModalReed::setPitchRatio(double ratio) {
    if (ratio == pitch_ratio_)
      return;
    pitch_ratio_ = ratio;
    for (Mode& mode : modes_)
      math::sinCos(mode.phase_inc * ratio, mode.sin_inc, mode.cos_inc);
  }

  void AttackNoise::init(double velocity, double fundamental_hz, double sample_rate, uint32_t seed) {
    const double tau = 0.003;
    amplitude_ = 0.025 * velocity * velocity;
    decay_per_sample_ = std::exp(-1.0 / (tau * sample_rate));
    remaining_ = math::toU32(0.015 * sample_rate);
    fade_in_remaining_ = kNoiseFadeInSamples;
    // 5x the fundamental keeps the noise in the reed's harmonic neighbourhood.
    const double center = clamp(fundamental_hz * 5.0, 200.0, 2000.0);
    bpf_ = Biquad::bandpass(center, 0.7, sample_rate);
    rng_state_ = seed;
  }

  void AttackNoise::render(double* output, int num_samples) {
    const uint32_t count = std::min(remaining_, static_cast<uint32_t>(std::max(num_samples, 0)));
    double amp = amplitude_;
    uint32_t fade_in = fade_in_remaining_;

    for (uint32_t i = 0; i < count; ++i) {
      double env = 1.0;
      if (fade_in > 0) {
        const uint32_t pos = kNoiseFadeInSamples - fade_in;
        const double t = static_cast<double>(pos) / static_cast<double>(kNoiseFadeInSamples);
        fade_in -= 1;
        env = 0.5 * (1.0 - std::cos(kPi * t));
      }
      const double noise = nextNoise();
      const double filtered = bpf_.process(noise);
      output[i] += amp * env * filtered;
      amp *= decay_per_sample_;
    }

    amplitude_ = amp;
    fade_in_remaining_ = fade_in;
    remaining_ -= count;
  }

  double AttackNoise::nextNoise() {
    rng_state_ = rng_state_ * 1664525u + 1013904223u;
    // `rng_state as i32 as f64 / i32::MAX as f64`: two's complement reinterpretation.
    return static_cast<double>(static_cast<int32_t>(rng_state_)) / 2147483647.0;
  }

  void Voice::noteOn(int midi_note, double velocity, double sample_rate, uint32_t noise_seed, bool mlp_enabled,
                     const VoiceSettings& settings) {
    const int midi = std::min(std::max(midi_note, kMidiLo), kMidiHi);
    const KeyParams& key = keyParams(midi);
    const NoteParams& params = key.note;

    // Voice::build_reed. Atlas's Condition widens the reed's tuning offset.
    const double detune = settings.detune_scale == 1.0 ? key.freq_detune
                                                        : 1.0 + key.detune_unit * (0.00173 * settings.detune_scale);
    const double detuned_fundamental = params.fundamental_hz * detune;
    const ModeArray dwell = dwellAttenuation(velocity, detuned_fundamental, params.mode_ratios);
    const double onset_time = onsetRampTime(velocity, detuned_fundamental);

    ModeArray amplitudes;
    for (int i = 0; i < kNumModes; ++i)
      amplitudes[i] = params.mode_amplitudes[i] * dwell[i] * key.amplitude_offsets[i];

    // Hammer force (pre-pickup): the S-curve models the pad's progressive stiffness, the
    // register-dependent power law the dynamic range. The level is applied after the pickup.
    const double vel_scale = std::pow(velocityScurve(velocity), key.velocity_exponent);
    for (double& a : amplitudes)
      a *= vel_scale;

    // MLP corrections to modes 2-6. Without the MLP they are identity (2^0 = 1, x / 1 = x), so
    // the loops are skipped.
    ModeArray corrected_ratios = params.mode_ratios;
    ModeArray corrected_decay = params.mode_decay_rates;
    if (mlp_enabled) {
      const MlpCorrections corrections = MlpCorrections::infer(midi, velocity);
      for (int i = 1; i < 6; ++i)
        corrected_ratios[i] *= std::exp2(corrections.freq_offsets_cents[i - 1] / 1200.0);
      for (int i = 1; i < 6; ++i)
        corrected_decay[i] /= corrections.decay_offsets[i - 1];
    }

    // The note's ff swing at the pickup, mm. The MLP's ds_correction is deliberately not applied
    // (its training target assumed the retired 1/(1-y) pickup law).
    const double swing_mm = pickupSwingMm(midi, settings);

    reed_.init(detuned_fundamental, corrected_ratios, amplitudes, corrected_decay, onset_time, velocity,
               sample_rate);
    reed_.setAirDrag(key.air_drag_db_per_mm * settings.air_drag_scale, swing_mm, sample_rate);

    pickup_.init(sample_rate, midi, swing_mm, settings.c2_to_emitter);
    noise_.init(velocity, detuned_fundamental, sample_rate, noise_seed);

    // Technician voicing: levels the keyboard after the pickup, without changing the reed's
    // swing (and so the bark).
    post_pickup_gain_ = outputScale(midi, velocity, settings);
    sample_rate_ = sample_rate;
    midi_note_ = midi;
    active_ = true;
  }

  void Voice::noteOff() {
    if (active_)
      reed_.startDamper(midi_note_, sample_rate_);
  }

  void Voice::render(double* output, int num_samples) {
    std::fill(output, output + std::max(num_samples, 0), 0.0);
    if (!active_)
      return;

    reed_.render(output, num_samples);
    if (!noise_.isDone())
      noise_.render(output, num_samples);
    pickup_.process(output, num_samples);

    const double gain = post_pickup_gain_;
    for (int i = 0; i < num_samples; ++i)
      output[i] *= gain;
  }

  bool Voice::isSilent() const {
    if (!active_)
      return true;
    if (reed_.isDamping() && reed_.releaseSeconds(sample_rate_) > kReleaseTimeoutSeconds)
      return true;
    return reed_.isSilent(kSilenceThresholdDb);
  }

  void Voice::setPitchRatio(double ratio) {
    reed_.setPitchRatio(ratio);
  }
} // namespace openwurli
