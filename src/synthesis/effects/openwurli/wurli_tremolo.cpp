// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from OpenWurli 0.9.0, crates/openwurli-dsp/src/tremolo.rs
// (GPL-3.0-or-later). Floating-point expressions keep the Rust's operand
// order; build without fast-math and with -ffp-contract=off.
#include "wurli_tremolo.h"

#include "wurli_math.h"

#include <cmath>
#include <cstdint>

namespace openwurli {
  namespace {
    // CdS cell of the LG-1 (VTL5C-class) opto: rise ~2.5 ms, fall ~35 ms,
    // power-law exponent 0.9.
    constexpr double kAttackTau = 0.0025;
    constexpr double kReleaseTau = 0.035;
    constexpr double kGamma = 0.9;
    // The cell under the 200A's weak LED drive: ~9K lit, ~1M dark.
    constexpr double kRLdrMin = 9'000.0;
    constexpr double kRLdrMax = 1'000'000.0;

    // Depth network (schematic #203720-S-3): the 50K VIBRATO pot divides the
    // feedback-junction shunt, top to the junction, bottom to ground, the LDR
    // on the wiper, with 18K bridging top to wiper:
    //   Z = (R_upper || 18K) + (R_lower || R_ldr), R_lower = 50K * depth.
    constexpr double kRVibBridge = 18'000.0;
    constexpr double kRVibPot = 50'000.0;

    // LED drive path: I_LED = (V_RAIL - v(led_anode)) / R-18.
    constexpr double kVRail = 14.5;
    constexpr double kR18LedSeries = 680.0;

    // TIL209A relative luminous intensity against forward current (TI
    // bulletin DL-S 12024, Fig. 4) as a piecewise power law: (mA, local
    // log-log exponent from there up). Below 0.35 mA it is extrapolated.
    struct LawSegment {
      double current_ma;
      double exponent;
    };
    constexpr LawSegment kLedIntensityLaw[5] = {
      { 0.10, 1.36 }, { 1.00, 1.22 }, { 3.00, 1.02 }, { 10.00, 0.88 }, { 40.00, 0.88 }
    };

    // The oscillator's measured peak LED current (R-17 at 4.7K): the cell is
    // at kRLdrMin here.
    constexpr double kLedIFullMa = 2.279;

    // ln of the relative intensity at `i` (mA), integrating the piecewise
    // exponent in log-current space.
    double lnIntensity(double i) {
      double acc = 0.0;
      double prev_i = kLedIntensityLaw[0].current_ma;
      double prev_n = kLedIntensityLaw[0].exponent;
      // Below the first breakpoint: continue the first segment's slope.
      if (i <= prev_i)
        return prev_n * std::log(i / prev_i);
      for (int segment = 1; segment < 5; ++segment) {
        const double bp_i = kLedIntensityLaw[segment].current_ma;
        const double bp_n = kLedIntensityLaw[segment].exponent;
        const double hi = std::fmin(i, bp_i);
        acc += prev_n * std::log(hi / prev_i);
        if (i <= bp_i)
          return acc;
        prev_i = bp_i;
        prev_n = bp_n;
      }
      return acc + prev_n * std::log(i / prev_i);
    }

    // Relative intensity at `i_ma`, normalized to 1 at kLedIFullMa.
    double ledIntensity(double i_ma) {
      if (i_ma <= 0.0)
        return 0.0;
      return std::exp(lnIntensity(i_ma) - lnIntensity(kLedIFullMa));
    }
  } // namespace

  tremolo_circuit::CircuitState Tremolo::settledOscillator(double sample_rate) {
    tremolo_circuit::CircuitState state = tremolo_circuit::CircuitState::makeDefault();
    if (std::fabs(sample_rate - tremolo_circuit::kSampleRate) > 0.5)
      state.setSampleRate(sample_rate);
    double taps[tremolo_circuit::kNumTaps];
    const uint64_t settle = math::toU64(sample_rate * 2.0);
    for (uint64_t i = 0; i < settle; ++i)
      tremolo_circuit::processSample(0.0, state, taps);
    return state;
  }

  Tremolo::Tremolo(double depth, double sample_rate) :
      osc_state_(settledOscillator(sample_rate)), sample_rate_(sample_rate), depth_(depth),
      r_ldr_(kRLdrMax), ldr_envelope_(0.0),
      ldr_attack_(std::exp(-1.0 / (kAttackTau * sample_rate))),
      ldr_release_(std::exp(-1.0 / (kReleaseTau * sample_rate))), r_ldr_max_(kRLdrMax),
      gamma_(kGamma), ln_r_max_(std::log(kRLdrMax)),
      ln_min_minus_max_(std::log(kRLdrMin) - std::log(kRLdrMax)) {
    for (int k = 0; k < kLedLutSize; ++k) {
      const double i_ma = kLedIFullMa * static_cast<double>(k) / static_cast<double>(kLedLutSize - 1);
      led_lut_[k] = math::clamp(ledIntensity(i_ma), 0.0, 1.0);
    }
  }

  void Tremolo::setDepth(double depth) {
    depth_ = math::clamp(depth, 0.0, 1.0);
  }

  double Tremolo::process() {
    // 1. LED drive, 0..1. Depth does not scale the LED: it lives in the
    // shunt divider (step 4).
    const double led_drive = oscillatorDrive();

    // 2. The cell's asymmetric envelope.
    const double coeff = led_drive > ldr_envelope_ ? ldr_attack_ : ldr_release_;
    ldr_envelope_ = led_drive + coeff * (ldr_envelope_ - led_drive);

    // 3. Power law between dark and lit resistance.
    const double drive = math::clamp(ldr_envelope_, 0.0, 1.0);
    if (drive < 1e-6)
      r_ldr_ = r_ldr_max_;
    else {
      const double log_r = ln_r_max_ + ln_min_minus_max_ * std::pow(drive, gamma_);
      r_ldr_ = std::exp(log_r);
    }

    // 4. The depth divider.
    return shuntImpedance();
  }

  double Tremolo::shuntImpedance() const {
    const double r_upper = kRVibPot * (1.0 - depth_);
    const double r_lower = kRVibPot * depth_;
    const double top = r_upper > 0.0 ? r_upper * kRVibBridge / (r_upper + kRVibBridge) : 0.0;
    const double branch = r_ldr_;
    const double low = r_lower > 0.0 ? r_lower * branch / (r_lower + branch) : 0.0;
    return top + low;
  }

  // The light from the oscillator's real LED current, read from its anode
  // tap and mapped through the TIL209A law (interpolated table).
  double Tremolo::oscillatorDrive() {
    double taps[tremolo_circuit::kNumTaps];
    tremolo_circuit::processSample(0.0, osc_state_, taps);
    // The deck runs without oversampling: one inner sample per call, so the
    // generated mean over the inner samples is over one.
    constexpr int kInnerSamples = 1;
    double light = 0.0;
    const double i_ma = ((kVRail - taps[0]) / kR18LedSeries) * 1000.0;
    const double x = math::clamp(i_ma / kLedIFullMa, 0.0, 1.0) * static_cast<double>(kLedLutSize - 1);
    const uint64_t k = math::toU64(x);
    if (k + 1 < static_cast<uint64_t>(kLedLutSize)) {
      const double f = x - static_cast<double>(k);
      light += led_lut_[k] * (1.0 - f) + led_lut_[k + 1] * f;
    }
    else
      light += led_lut_[kLedLutSize - 1];
    return light / static_cast<double>(kInnerSamples);
  }

  double Tremolo::currentResistance() const {
    return shuntImpedance();
  }

  void Tremolo::reset() {
    // Rebuilt rather than reset to the DC point: the oscillator would never
    // leave that unstable equilibrium.
    osc_state_ = settledOscillator(sample_rate_);
    ldr_envelope_ = 0.0;
    r_ldr_ = r_ldr_max_;
  }
} // namespace openwurli
