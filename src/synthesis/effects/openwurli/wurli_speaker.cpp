// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from OpenWurli 0.9.0, crates/openwurli-dsp/src/speaker.rs
// (GPL-3.0-or-later). Floating-point expressions keep the Rust's operand
// order; build without fast-math and with -ffp-contract=off.
#include "wurli_speaker.h"

#include "wurli_math.h"

#include <cmath>

namespace openwurli {
  namespace {
    // At full character. 30 Hz is subsonic protection only: the circuit's own
    // bass roll-off is the power amp's feedback shelf, and the cabinet's
    // ~95 Hz roll-off is left to a downstream cabinet model.
    constexpr double kHpfAuthenticHz = 30.0;
    constexpr double kHpfQ = 0.75;
    // Cone breakup, from A/B treble centroids against recordings.
    constexpr double kLpfAuthenticHz = 5500.0;
    constexpr double kLpfQ = 0.707;
    // At zero character (where the filters are skipped anyway).
    constexpr double kHpfBypassHz = 20.0;
    constexpr double kLpfBypassHz = 20000.0;
    constexpr double kThermalTau = 5.0;
  } // namespace

  Speaker::Speaker(double sample_rate) :
      hpf_(Biquad::highpass(kHpfAuthenticHz, kHpfQ, sample_rate)),
      lpf_(Biquad::lowpass(kLpfAuthenticHz, kLpfQ, sample_rate)), character_(1.0),
      sample_rate_(sample_rate), thermal_alpha_(1.0 / (kThermalTau * sample_rate)) {
    updateCoefficients();
  }

  void Speaker::setCharacter(double character) {
    const double c = math::clamp(character, 0.0, 1.0);
    if (std::fabs(c - character_) > 0.002) {
      character_ = c;
      updateCoefficients();
    }
  }

  void Speaker::updateCoefficients() {
    const double c = character_;
    // Cutoffs interpolated logarithmically.
    const double hpf_hz = kHpfBypassHz * std::pow(kHpfAuthenticHz / kHpfBypassHz, c);
    const double lpf_hz = kLpfBypassHz * std::pow(kLpfAuthenticHz / kLpfBypassHz, c);
    hpf_.setHighpass(hpf_hz, kHpfQ, sample_rate_);
    lpf_.setLowpass(lpf_hz, kLpfQ, sample_rate_);

    a2_ = 0.2 * c;
    a3_ = 0.6 * c;
    thermal_coeff_ = 2.0 * c;
  }

  double Speaker::process(double input) {
    // Polynomial normalized so y(1) = 1; the x^2 term makes it asymmetric.
    const double x2 = input * input;
    const double x3 = x2 * input;
    const double shaped = (input + a2_ * x2 + a3_ * x3) / (1.0 + a2_ + a3_);

    // Cone excursion limit (soft mechanical stop).
    const double limited = character_ < 0.001 ? shaped : std::tanh(shaped);

    // Voice-coil heating: a slow envelope of the input power.
    const double power = x2;
    thermal_state_ += (power - thermal_state_) * thermal_alpha_;
    const double thermal_gain = 1.0 / (1.0 + thermal_coeff_ * std::sqrt(thermal_state_));

    const double filtered = hpf_.process(limited * thermal_gain);
    const double out = lpf_.process(filtered);
    return character_ < 0.001 ? input : out;
  }

  void Speaker::reset() {
    hpf_.reset();
    lpf_.reset();
    thermal_state_ = 0.0;
  }
} // namespace openwurli
