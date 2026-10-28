// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "reverb_filter_core.h"
#include "double_pair.h"
#include <algorithm>
#include <cmath>

namespace vital {
// A fixed Linkwitz-Riley crossover replaces the wet bass with dry bass. The
// matching low/high branches avoid doubling the lows when the JSFX boosts them.
// This sits outside the reference core, which remains exact at amount zero.
// Like the core, it runs a voice pair's four lanes at once.
class ReverbFilterBass {
public:
  static constexpr int kLanes = ReverbFilterCore::kLanes;
  void setSampleRate(double rate, int oversampling) {
    if (rate == rate_ && oversampling == oversampling_) return;
    rate_ = rate;
    oversampling_ = oversampling;
    const double g = std::tan(3.141592653589793 * 350.0 / (rate / oversampling));
    const double norm = 1.0 / (1.0 + std::sqrt(2.0) * g + g * g);
    low_ = g * g * norm;
    high_ = norm;
    a1_ = 2.0 * (g * g - 1.0) * norm;
    a2_ = (1.0 - std::sqrt(2.0) * g + g * g) * norm;
    reset();
  }
  void reset() {
    phase_ = 0;
    for (int lane = 0; lane < kLanes; ++lane) resetLane(lane);
  }
  void resetLane(int lane) {
    for (auto& phase : state_)
      for (auto& biquad : phase)
        for (auto& delay : biquad) delay[lane] = 0;
    fresh_[lane] = true;
  }
  void beginBlock(int lane, double amount, int samples) {
    amount = std::clamp(amount, 0.0, 1.0);
    if (fresh_[lane]) amount_[lane] = amount;
    step_[lane] = (amount - amount_[lane]) / std::max(1, samples);
    fresh_[lane] = false;
  }
  // Lanes 0 .. 2 * pairs - 1; with one pair, lanes 2-3 copy lanes 0-1.
  void tick(const double* dry, const double* wet, double* output, int pairs = kLanes / 2) {
    auto& state = state_[phase_];
    const double_pair low_gain = double_pair::splat(low_), high_gain = double_pair::splat(high_);
    const double_pair low_b1 = double_pair::splat(2.0 * low_), high_b1 = double_pair::splat(-2.0 * high_);
    const double_pair a1 = double_pair::splat(a1_), a2 = double_pair::splat(a2_);
    const double_pair zero = double_pair::splat(0.0), one = double_pair::splat(1.0);
    for (int pair = 0; pair < pairs; ++pair) {
      const int lane = 2 * pair;
      const double_pair wet_pair = double_pair::load(wet + lane);
      double_pair low = double_pair::load(dry + lane), high = wet_pair;
      for (int i = 0; i < 2; ++i) {
        low = biquad(state[i], lane, low, low_gain, low_b1, a1, a2);
        high = biquad(state[i + 2], lane, high, high_gain, high_b1, a1, a2);
      }
      const double_pair amount = double_pair::load(amount_ + lane) + double_pair::load(step_ + lane);
      amount.store(amount_ + lane);
      const double_pair clamped = select(lessThan(amount, zero), zero, select(lessThan(one, amount), one, amount));
      (wet_pair + clamped * ((low + high) - wet_pair)).store(output + lane);
    }
    if (pairs == 1) {
      output[2] = output[0];
      output[3] = output[1];
    }
    if (++phase_ == oversampling_) phase_ = 0;
  }
private:
  using Delays = double[2][kLanes];   // z1 and z2 of each lane
  static force_inline double_pair biquad(Delays& delays, int lane, double_pair x, double_pair b0, double_pair b1,
                                         double_pair a1, double_pair a2) {
    const double_pair tiny = double_pair::splat(1e-20), zero = double_pair::splat(0.0);
    const double_pair z1 = double_pair::load(delays[0] + lane), z2 = double_pair::load(delays[1] + lane);
    const double_pair y = b0 * x + z1;
    const double_pair next_z1 = b1 * x - a1 * y + z2;
    const double_pair next_z2 = b0 * x - a2 * y;
    select(lessThan(abs(next_z1), tiny), zero, next_z1).store(delays[0] + lane);
    select(lessThan(abs(next_z2), tiny), zero, next_z2).store(delays[1] + lane);
    return y;
  }
  alignas(16) double state_[ReverbFilterCore::kMaximumOversampling][4][2][kLanes] {};
  alignas(16) double amount_[kLanes] {}, step_[kLanes] {};
  double rate_ = 0, low_ = 0, high_ = 0, a1_ = 0, a2_ = 0;
  int oversampling_ = 1, phase_ = 0;
  bool fresh_[kLanes] {};
};
}
