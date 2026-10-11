// SPDX-License-Identifier: GPL-3.0-or-later
// The Opus tape drive curve, shared by the Tape effect and Analogue Echo so
// both saturate identically.
#pragma once

#include <cmath>

namespace atlas::analogue::tape_saturation {
  // Drive amount 0-1 (0-100%) maps to 0-30 dB of input gain.
  inline double driveGain(double amount) { return std::pow(10.0, amount * 30.0 / 20.0); }

  // Slightly biased tanh for even harmonics plus a small cubic lift.
  inline double shape(double x) {
    constexpr double kBias = 0.035;
    double wet = std::tanh(x + kBias) - std::tanh(kBias);
    return wet + 0.06 * wet * wet * wet;
  }
}
