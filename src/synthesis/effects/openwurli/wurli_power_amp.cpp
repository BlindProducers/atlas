// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from OpenWurli 0.9.0, crates/openwurli-dsp/src/power_amp.rs
// (GPL-3.0-or-later). Floating-point expressions keep the Rust's operand
// order; build without fast-math and with -ffp-contract=off.
#include "wurli_power_amp.h"

#include "wurli_math.h"

#include <cmath>

namespace openwurli {
  namespace {
    // Rail sag (RailDynamics).
    constexpr double kRailVOpen = 24.5;       // idle rail, service manual
    constexpr double kRailDcBias = 22.5;      // V1/V2 bias in wurli-power-amp.cir
    constexpr double kRailREff = 3.5;         // per-rail source impedance, ohms
    constexpr double kSpeakerLoadOhms = 8.0;  // two 16 ohm drivers in parallel
    constexpr double kRailTauAttack = 0.008;  // 3.5 ohm x 2200 uF
    constexpr double kRailTauRelease = 0.015;
    constexpr double kRailTauIAvg = 0.030;

    // Behavioural amp.
    constexpr double kOpenLoopGain = 19'000.0;
    constexpr double kR30 = 220.0;    // feedback shunt leg, ohms
    constexpr double kR31 = 15'000.0; // feedback series leg, ohms
    constexpr double kC10 = 22e-6;    // in series with R-30 to ground, farads
    constexpr double kCrossoverVt = 0.013;
    constexpr double kQuiescentGain = 0.1;
    constexpr int kNrMaxIterations = 8;
    constexpr double kNrTolerance = 1e-6;
  } // namespace

  RailDynamics::RailDynamics(double sample_rate) :
      v_rail_pos_(kRailDcBias), v_rail_neg_(kRailDcBias) {
    setSampleRate(sample_rate);
  }

  void RailDynamics::setSampleRate(double sample_rate) {
    const double dt = 1.0 / sample_rate;
    alpha_attack_ = 1.0 - std::exp(-dt / kRailTauAttack);
    alpha_release_ = 1.0 - std::exp(-dt / kRailTauRelease);
    alpha_i_avg_ = 1.0 - std::exp(-dt / kRailTauIAvg);
  }

  void RailDynamics::reset() {
    v_rail_pos_ = kRailDcBias;
    v_rail_neg_ = kRailDcBias;
    i_avg_pos_ = 0.0;
    i_avg_neg_ = 0.0;
  }

  void RailDynamics::step(double v_out) {
    // Current drawn from each rail (only the conducting one), then its slow
    // envelope: the filter caps integrate out the audio-rate ripple.
    const double i_pos = std::fmax(v_out / kSpeakerLoadOhms, 0.0);
    const double i_neg = std::fmax(-v_out / kSpeakerLoadOhms, 0.0);
    i_avg_pos_ += alpha_i_avg_ * (i_pos - i_avg_pos_);
    i_avg_neg_ += alpha_i_avg_ * (i_neg - i_avg_neg_);

    const double target_pos = kRailVOpen - i_avg_pos_ * kRailREff;
    const double target_neg = kRailVOpen - i_avg_neg_ * kRailREff;

    // Sag with the attack time, recover with the release time.
    const double alpha_p = target_pos < v_rail_pos_ ? alpha_attack_ : alpha_release_;
    const double alpha_n = target_neg < v_rail_neg_ ? alpha_attack_ : alpha_release_;
    v_rail_pos_ += alpha_p * (target_pos - v_rail_pos_);
    v_rail_neg_ += alpha_n * (target_neg - v_rail_neg_);
  }

  double RailDynamics::positiveOffset() const {
    return v_rail_pos_ - kRailDcBias;
  }

  double RailDynamics::negativeOffset() const {
    return v_rail_neg_ - kRailDcBias;
  }

  PowerAmp::PowerAmp(double sample_rate) {
    // beta(s) = (1 + s R30 C10) / (1 + s (R30 + R31) C10), bilinear without
    // prewarping (the knee is at 33 Hz).
    const double k = 2.0 * sample_rate;
    const double tau_zero = kR30 * kC10;           // 4.84 ms, zero at 33 Hz
    const double tau_pole = (kR30 + kR31) * kC10;  // 335 ms, pole at 0.48 Hz
    const double a0 = 1.0 + tau_pole * k;
    open_loop_gain_ = kOpenLoopGain;
    crossover_vt_ = kCrossoverVt;
    rail_limit_ = kHeadroomV;
    quiescent_gain_ = kQuiescentGain;
    fb_b0_ = (1.0 + tau_zero * k) / a0;
    fb_b1_ = (1.0 - tau_zero * k) / a0;
    fb_a1_ = (1.0 - tau_pole * k) / a0;
    fb_y1_ = 0.0;
    fb_prev_ = 0.0;
  }

  double PowerAmp::process(double input) {
    // The history part of the feedback voltage is fixed for this sample.
    const double fb_hist = fb_b1_ * fb_y1_ - fb_a1_ * fb_prev_;
    const double b0 = fb_b0_;

    double y = math::clamp((input - fb_hist) * open_loop_gain_ / (1.0 + open_loop_gain_ * b0),
                               -rail_limit_ + kNrTolerance, rail_limit_ - kNrTolerance);

    for (int iteration = 0; iteration < kNrMaxIterations; ++iteration) {
      const double error = input - (b0 * y + fb_hist);
      const double v = open_loop_gain_ * error;
      const ForwardPath forward = forwardPath(v);
      const double residual = y - forward.value;
      const double jacobian = 1.0 + open_loop_gain_ * b0 * forward.derivative;
      const double delta = residual / jacobian;
      y -= delta;
      if (std::fabs(delta) < kNrTolerance)
        break;
    }

    fb_prev_ = b0 * y + fb_hist;
    fb_y1_ = y;

    return y / rail_limit_;
  }

  // Crossover notch (gain falls to kQuiescentGain around 0 V) into the tanh
  // rail limit, with its derivative.
  PowerAmp::ForwardPath PowerAmp::forwardPath(double v) const {
    const double v_sq = v * v;
    const double vt_sq = crossover_vt_ * crossover_vt_;
    const double exp_term = std::exp(-v_sq / vt_sq);
    const double q = quiescent_gain_;
    const double cross_gain = q + (1.0 - q) * (1.0 - exp_term);
    const double v_cross = v * cross_gain;
    const double dcross_dv = cross_gain + v * (1.0 - q) * (2.0 * v / vt_sq) * exp_term;
    const double tanh_arg = v_cross / rail_limit_;
    const double tanh_val = std::tanh(tanh_arg);
    const double f_val = rail_limit_ * tanh_val;
    const double f_deriv = (1.0 - tanh_val * tanh_val) * dcross_dv;
    return { f_val, f_deriv };
  }

  void PowerAmp::reset() {
    fb_y1_ = 0.0;
    fb_prev_ = 0.0;
  }
} // namespace openwurli
