// SPDX-License-Identifier: GPL-3.0-or-later
// BP Scream's second-order antiderivative-antialiased tanh (TanhADAA2), for
// four lanes at once in double precision.
//
// The original evaluates log(cosh x), exp, log and Li2 through libm for every
// lane and sample. Here everything comes from u = exp(-2|x|) and
// l = log1p(u), which are exact identities of the original's functions:
//   log cosh x = |x| - ln 2 + l
//   tanh x     = sign(x) (1 - u) / (1 + u)
//   AD2(x)     = sign(x) [x^2 / 2 - |x| ln 2 + pi^2 / 24 + Li2(-u) / 2]
// (AD2 is odd; the original's x (x + 2 log(e^-2x + 1) - 2 log cosh x) is
// x (2 ln 2 - x)). Li2(-u) for u in (0, 1] uses the original's rational
// approximation on its -1 <= z < 0 branch. exp and log1p are fdlibm-style
// polynomials built from IEEE add, multiply and divide only, so SSE2 and NEON
// give bit-identical results (libm differs between platforms).
#pragma once
#include "poly_values.h"

namespace vital::scream {
  class TanhAdaa2 {
    public:
      static constexpr int kLanes = 4;
      static constexpr double kTolerance = 1.0e-5;   // BP Scream's ADAA_TOL

      void reset() { for (int lane = 0; lane < kLanes; ++lane) resetLane(lane); }
      void resetLane(int lane) { x1_[lane] = x2_[lane] = d2_[lane] = ad2_x1_[lane] = 0.0; }

      // Processes lanes 0 .. 2 * pairs - 1 of `in` into `out`.
      void process(const double* in, double* out, int pairs);
      // The same for float lanes, rounding the result to float as BP Scream
      // does. With one pair, lanes 2-3 of the result repeat lanes 0-1.
      poly_float process(poly_float in, int pairs);

      // Scalar forms of the same arithmetic, for tests.
      static double ad1(double x);
      static double ad2(double x);
      static double tanh(double x);

    private:
      alignas(16) double x1_[kLanes] {};
      alignas(16) double x2_[kLanes] {};
      alignas(16) double d2_[kLanes] {};
      alignas(16) double ad2_x1_[kLanes] {};
  };
} // namespace vital::scream
