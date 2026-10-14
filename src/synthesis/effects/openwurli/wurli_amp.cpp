// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from OpenWurli 0.9.0, crates/openwurli-dsp/src/engine.rs and
// tables.rs (GPL-3.0-or-later). Floating-point expressions keep the Rust's
// operand order; build without fast-math and with -ffp-contract=off.
#include "wurli_amp.h"

#include "wurli_math.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace openwurli {
  namespace {
    // OpenWurli's plugin holds R-11 at its default (a service trimmer).
    constexpr double kR11 = VolumeNetwork::kR11ReedBarVolumeDefault;

    // Up to this host rate the preamp and power amp run at twice the rate.
    constexpr double kOversampleBelowRate = 88'200.0;
    constexpr double kWarmUpSeconds = 0.6;

    // 5 ms at any rate.
    uint32_t rampSamplesForRate(double sample_rate) {
      return std::max(math::toU32(sample_rate * 0.005), uint32_t { 1 });
    }

    // One-pole low-pass coefficient for corner `fc` at rate `sr` (exact decay
    // form); a corner at or above Nyquist passes straight through.
    double onePoleAlpha(double fc, double sr) {
      if (!std::isfinite(fc) || fc >= 0.5 * sr)
        return 1.0;
      return 1.0 - std::exp(-2.0 * kPi * fc / sr);
    }
  } // namespace

  double VolumeNetwork::audioTaper(double pos) {
    const double p = math::clamp(pos, 0.0, 1.0);
    if (p <= 0.5)
      return 0.15 * (p / 0.5);
    return 0.15 + 0.85 * ((p - 0.5) / 0.5);
  }

  //   R_lower = 10K * taper(volume), R_upper = 10K - R_lower
  //   gain = (R_lower || R-27) / (R9 + R11 + R_upper + (R_lower || R-27))
  double VolumeNetwork::potGain(double volume, double r11) {
    const double trim = math::clamp(r11, 0.0, kR11ReedBarVolumeMax);
    const double r_lower = kVolumePotR * audioTaper(volume);
    const double r_upper = kVolumePotR - r_lower;
    const double r_low_eff = r_lower <= 0.0 ? 0.0 : r_lower * kPowerAmpRIn / (r_lower + kPowerAmpRIn);
    return r_low_eff / (kPreampOutputR9 + trim + r_upper + r_low_eff);
  }

  // C-9 sees (R9 + R11 + R_upper) || R_lower || R-27.
  double VolumeNetwork::potPoleHz(double volume, double r11) {
    const double trim = math::clamp(r11, 0.0, kR11ReedBarVolumeMax);
    const double r_lower = kVolumePotR * audioTaper(volume);
    const double r_upper = kVolumePotR - r_lower;
    const double r_src = kPreampOutputR9 + trim + r_upper;
    auto parallel = [](double a, double b) { return a * b / (a + b); };
    const double r_th = r_lower <= 0.0 ? 0.0 : parallel(parallel(r_src, r_lower), kPowerAmpRIn);
    if (r_th <= 0.0)
      return std::numeric_limits<double>::infinity();
    return 1.0 / (2.0 * kPi * r_th * kPowerAmpC9);
  }

  AmpChain::LinearSmoother::LinearSmoother(double initial, uint32_t ramp_samples) :
      current_(initial), target_(initial), ramp_samples_(ramp_samples) { }

  void AmpChain::LinearSmoother::setTarget(double target) {
    if (std::fabs(target - target_) < 1e-9)
      return;
    target_ = target;
    const double delta = target - current_;
    if (ramp_samples_ == 0) {
      current_ = target;
      samples_remaining_ = 0;
      return;
    }
    step_ = delta / static_cast<double>(ramp_samples_);
    samples_remaining_ = ramp_samples_;
  }

  void AmpChain::LinearSmoother::setRampSamples(uint32_t ramp_samples) {
    ramp_samples_ = ramp_samples;
    if (samples_remaining_ > 0) {
      step_ = (target_ - current_) / static_cast<double>(std::max(ramp_samples, uint32_t { 1 }));
      samples_remaining_ = ramp_samples;
    }
  }

  void AmpChain::LinearSmoother::snapTo(double value) {
    current_ = value;
    target_ = value;
    step_ = 0.0;
    samples_remaining_ = 0;
  }

  AmpChain::AmpChain() :
      volume_(kDefaultVolume, rampSamplesForRate(44'100.0)),
      tremolo_depth_(kDefaultTremoloDepth, rampSamplesForRate(44'100.0)),
      speaker_character_(kDefaultSpeakerCharacter, rampSamplesForRate(44'100.0)) { }

  void AmpChain::prepare(double sample_rate) {
    sample_rate_ = sample_rate;
    // Upstream panics (singular matrices) or never finishes settling here.
    if (!(sample_rate > 0.0 && std::isfinite(sample_rate))) {
      prepared_valid_ = false;
      return;
    }

    oversample_ = sample_rate < kOversampleBelowRate;
    os_sample_rate_ = oversample_ ? sample_rate * 2.0 : sample_rate;

    const uint32_t ramp = rampSamplesForRate(sample_rate);
    volume_.setRampSamples(ramp);
    tremolo_depth_.setRampSamples(ramp);
    speaker_character_.setRampSamples(ramp);
    snapSmoothers();

    // The preamp and power amp run at the oversampled rate, so the nonlinear
    // solvers see a small enough step not to make harmonics of their own; the
    // speaker stays at the base rate, as upstream.
    stages_.preamp = DkPreamp(os_sample_rate_);
    stages_.tremolo = Tremolo(tremolo_depth_.target(), os_sample_rate_);
    stages_.oversampler = Oversampler();
    stages_.power_amp = PowerAmp(os_sample_rate_);
    stages_.speaker = Speaker(sample_rate);
    stages_.c9_state = 0.0;
    stages_.c9_alpha = 1.0;
    stages_.volume_cached = false;

    warmUp();
    prepared_ = stages_;
    prepared_valid_ = true;
  }

  void AmpChain::reset() {
    if (prepared_valid_)
      stages_ = prepared_;
    snapSmoothers();
  }

  void AmpChain::resetAndWarmUp() {
    if (!prepared_valid_)
      return;
    stages_.preamp.reset();
    stages_.c9_state = 0.0;
    stages_.tremolo.reset();
    stages_.oversampler.reset();
    stages_.power_amp.reset();
    stages_.speaker.reset();
    snapSmoothers();
    warmUp();
  }

  void AmpChain::setVolume(double volume) {
    volume_.setTarget(volume);
  }

  void AmpChain::setTremoloDepth(double depth) {
    tremolo_depth_.setTarget(depth);
  }

  void AmpChain::setSpeakerCharacter(double character) {
    speaker_character_.setTarget(character);
  }

  void AmpChain::process(const double* input, double* output, int num_samples) {
    if (!prepared_valid_) {
      for (int i = 0; i < num_samples; ++i)
        output[i] = 0.0;
      return;
    }
    for (int i = 0; i < num_samples; ++i)
      output[i] = processSample(input[i]);
  }

  void AmpChain::snapSmoothers() {
    volume_.snapTo(volume_.target());
    tremolo_depth_.snapTo(tremolo_depth_.target());
    speaker_character_.snapTo(speaker_character_.target());
  }

  // WurliEngine::warm_up: settle the preamp's operating point, the tremolo
  // oscillator and the cell's envelope on silence, so the first note does not
  // ride the start-up excursion.
  void AmpChain::warmUp() {
    const uint64_t total = math::toU64(sample_rate_ * kWarmUpSeconds);
    for (uint64_t i = 0; i < total; ++i)
      processSample(0.0);
  }

  // The preamp's open-circuit output through R-11 and the pot into the amp
  // (loaded), with C-9 against the wiper's source resistance as a one-pole.
  void AmpChain::updateVolumeNetwork(double volume) {
    Stages& stages = stages_;
    const uint64_t bits = math::toBits(volume);
    if (stages.volume_cached && bits == stages.cached_volume_bits)
      return;
    stages.drive_gain = stages.preamp.openCircuitOutputFactor() * VolumeNetwork::potGain(volume, kR11);
    stages.c9_alpha = onePoleAlpha(VolumeNetwork::potPoleHz(volume, kR11), os_sample_rate_);
    stages.volume_cached = true;
    stages.cached_volume_bits = bits;
  }

  // engine.rs render + render_voices_to_preamp_out, one base-rate sample at a
  // time (every stage is streaming, so this equals the engine's block order).
  double AmpChain::processSample(double input) {
    Stages& stages = stages_;
    // WurliEngine zeroes a block whose voice sum is not finite: per sample here.
    if (!std::isfinite(input))
      input = 0.0;

    double amp_out;
    if (oversample_) {
      double up[2];
      stages.oversampler.upsample(input, up[0], up[1]);

      // Per base-rate sample: the depth and volume smoothers advance once,
      // the preamp and power amp run twice.
      stages.tremolo.setDepth(tremolo_depth_.next());
      updateVolumeNetwork(volume_.next());

      double amp[2];
      for (int j = 0; j < 2; ++j) {
        stages.preamp.setLdrResistance(stages.tremolo.process());
        const double preamp_out = stages.preamp.processSample(up[j]);
        stages.c9_state += stages.c9_alpha * (preamp_out * stages.drive_gain - stages.c9_state);
        amp[j] = stages.power_amp.process(stages.c9_state);
      }
      amp_out = stages.oversampler.downsample(amp[0], amp[1]);
    }
    else {
      stages.tremolo.setDepth(tremolo_depth_.next());
      stages.preamp.setLdrResistance(stages.tremolo.process());
      const double preamp_out = stages.preamp.processSample(input);
      updateVolumeNetwork(volume_.next());
      stages.c9_state += stages.c9_alpha * (preamp_out * stages.drive_gain - stages.c9_state);
      amp_out = stages.power_amp.process(stages.c9_state);
    }

    stages.speaker.setCharacter(speaker_character_.next());
    // The user volume was the pot ahead of the power amp; this only maps the
    // rail to full scale. No ceiling: the amp's clipping is the limit.
    const double output = stages.speaker.process(amp_out) * VolumeNetwork::kPostSpeakerGain;

    // WurliEngine's guard on its f32 output.
    if (math::isFiniteAsFloat(output))
      return output;
    stages.preamp.reset();
    stages.c9_state = 0.0;
    stages.oversampler.reset();
    stages.power_amp.reset();
    stages.speaker.reset();
    return 0.0;
  }
} // namespace openwurli
