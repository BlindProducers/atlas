// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "reverb_filter_core.h"
#include <algorithm>
#include <array>
#include <cmath>

namespace vital {
// A fixed Linkwitz-Riley crossover replaces the wet bass with dry bass. The
// matching low/high branches avoid doubling the lows when the JSFX boosts them.
// This sits outside the reference core, which remains exact at amount zero.
class ReverbFilterBass {
public:
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
    phases_ = {};
    phase_ = 0;
    fresh_ = true;
  }
  void beginBlock(double amount, int samples) {
    amount = std::clamp(amount, 0.0, 1.0);
    if (fresh_) amount_ = amount;
    step_ = (amount - amount_) / std::max(1, samples);
    fresh_ = false;
  }
  double tick(double dry, double wet) {
    auto& state = phases_[phase_];
    double low = dry, high = wet;
    for (int i = 0; i < 2; ++i) {
      low = state[i].tick(low, low_, 2.0 * low_, a1_, a2_);
      high = state[i + 2].tick(high, high_, -2.0 * high_, a1_, a2_);
    }
    if (++phase_ == oversampling_) phase_ = 0;
    amount_ += step_;
    return wet + std::clamp(amount_, 0.0, 1.0) * (low + high - wet);
  }
private:
  struct Biquad {
    double z1 = 0, z2 = 0;
    double tick(double x, double b0, double b1, double a1, double a2) {
      const double y = b0 * x + z1;
      z1 = b1 * x - a1 * y + z2;
      z2 = b0 * x - a2 * y;
      if (std::abs(z1) < 1e-20) z1 = 0;
      if (std::abs(z2) < 1e-20) z2 = 0;
      return y;
    }
  };
  std::array<std::array<Biquad, 4>, ReverbFilterCore::kMaximumOversampling> phases_ {};
  double rate_ = 0, low_ = 0, high_ = 0, a1_ = 0, a2_ = 0;
  double amount_ = 0, step_ = 0;
  int oversampling_ = 1, phase_ = 0;
  bool fresh_ = true;
};
}
