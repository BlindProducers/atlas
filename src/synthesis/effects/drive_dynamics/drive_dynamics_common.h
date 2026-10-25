// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cmath>

// What the four ports share with EEL2, the language their scripts are written in.
namespace atlas::drive_dynamics {
  // $pi
  inline constexpr double kPi = 3.14159265358979323846;

  // EEL2's == and != compare within NSEEL_CLOSEFACTOR.
  inline bool eelEqual(double a, double b) { return std::fabs(a - b) < 0.00001; }

  // Saturator's and Dynamic Tube's ap_chain: three 1st-order allpass sections over
  // coefficients hb[first], hb[first + 2], hb[first + 4], with state pairs (x, y).
  inline double allpassChain(const double* hb, double x, int first, std::array<double, 6>& sb) {
    for (int i = 0; i < 3; ++i) {
      const double c = hb[first + 2 * i];
      const double y = c * (x - sb[2 * i + 1]) + sb[2 * i];
      sb[2 * i] = x; sb[2 * i + 1] = y; x = y;
    }
    return x;
  }
}
