// SPDX-License-Identifier: GPL-3.0-or-later
// The log polynomial, its coefficients and the split of ln 2 come from fdlibm's
// e_log.c and e_exp.c:
//   Copyright (C) 1993 by Sun Microsystems, Inc. All rights reserved.
//   Developed at SunSoft, a Sun Microsystems, Inc. business.
//   Permission to use, copy, modify, and distribute this software is freely
//   granted, provided that this notice is preserved.
// Li2's rational approximation is BP Scream's (Polylogarithm, MIT, Alexander
// Voigt); see third_party/scream/LICENSE.
#include "scream_tanh.h"

namespace vital::scream {
  namespace {
    // Two doubles per register, through intrinsics only so no compiler fuses
    // a multiply and an add into an FMA on one platform and not the other.
#if VITAL_SSE2
    struct D2 { __m128d v; };
    struct M2 { __m128d v; };
    force_inline D2 set(double x) { return { _mm_set1_pd(x) }; }
    force_inline D2 load(const double* p) { return { _mm_load_pd(p) }; }
    force_inline void store(double* p, D2 a) { _mm_store_pd(p, a.v); }
    force_inline D2 operator+(D2 a, D2 b) { return { _mm_add_pd(a.v, b.v) }; }
    force_inline D2 operator-(D2 a, D2 b) { return { _mm_sub_pd(a.v, b.v) }; }
    force_inline D2 operator*(D2 a, D2 b) { return { _mm_mul_pd(a.v, b.v) }; }
    force_inline D2 operator/(D2 a, D2 b) { return { _mm_div_pd(a.v, b.v) }; }
    force_inline D2 abs(D2 a) { return { _mm_andnot_pd(_mm_set1_pd(-0.0), a.v) }; }
    force_inline D2 max(D2 a, D2 b) { return { _mm_max_pd(a.v, b.v) }; }
    // The magnitude of `magnitude` with the sign of `sign`.
    force_inline D2 copySign(D2 magnitude, D2 sign) {
      const __m128d bit = _mm_set1_pd(-0.0);
      return { _mm_or_pd(_mm_andnot_pd(bit, magnitude.v), _mm_and_pd(bit, sign.v)) };
    }
    force_inline M2 lessThan(D2 a, D2 b) { return { _mm_cmplt_pd(a.v, b.v) }; }
    force_inline M2 greaterThan(D2 a, D2 b) { return { _mm_cmpgt_pd(a.v, b.v) }; }
    force_inline bool any(M2 m) { return _mm_movemask_pd(m.v) != 0; }
    force_inline D2 select(M2 m, D2 yes, D2 no) {
      return { _mm_or_pd(_mm_and_pd(m.v, yes.v), _mm_andnot_pd(m.v, no.v)) };
    }
    // 2^k for the integer k held in the low mantissa bits of `shifted` (k + 1.5 * 2^52).
    force_inline D2 powerOfTwo(D2 shifted) {
      const __m128i k = _mm_slli_epi64(_mm_castpd_si128(shifted.v), 52);
      return { _mm_castsi128_pd(_mm_add_epi64(k, _mm_set1_epi64x(int64_t(1023) << 52))) };
    }
#elif VITAL_NEON
    struct D2 { float64x2_t v; };
    struct M2 { uint64x2_t v; };
    force_inline D2 set(double x) { return { vdupq_n_f64(x) }; }
    force_inline D2 load(const double* p) { return { vld1q_f64(p) }; }
    force_inline void store(double* p, D2 a) { vst1q_f64(p, a.v); }
    force_inline D2 operator+(D2 a, D2 b) { return { vaddq_f64(a.v, b.v) }; }
    force_inline D2 operator-(D2 a, D2 b) { return { vsubq_f64(a.v, b.v) }; }
    force_inline D2 operator*(D2 a, D2 b) { return { vmulq_f64(a.v, b.v) }; }
    force_inline D2 operator/(D2 a, D2 b) { return { vdivq_f64(a.v, b.v) }; }
    force_inline D2 abs(D2 a) { return { vabsq_f64(a.v) }; }
    // Like _mm_max_pd: the second operand when either is NaN.
    force_inline D2 max(D2 a, D2 b) { return { vbslq_f64(vcgtq_f64(a.v, b.v), a.v, b.v) }; }
    force_inline D2 copySign(D2 magnitude, D2 sign) {
      const uint64x2_t bit = vdupq_n_u64(uint64_t(1) << 63);
      return { vbslq_f64(bit, sign.v, magnitude.v) };
    }
    force_inline M2 lessThan(D2 a, D2 b) { return { vcltq_f64(a.v, b.v) }; }
    force_inline M2 greaterThan(D2 a, D2 b) { return { vcgtq_f64(a.v, b.v) }; }
    force_inline bool any(M2 m) { return vmaxvq_u32(vreinterpretq_u32_u64(m.v)) != 0; }
    force_inline D2 select(M2 m, D2 yes, D2 no) { return { vbslq_f64(m.v, yes.v, no.v) }; }
    force_inline D2 powerOfTwo(D2 shifted) {
      const int64x2_t k = vshlq_n_s64(vreinterpretq_s64_f64(shifted.v), 52);
      return { vreinterpretq_f64_s64(vaddq_s64(k, vdupq_n_s64(int64_t(1023) << 52))) };
    }
#endif

    constexpr double kPi = 3.1415926535897932;
    constexpr double kLn2Hi = 6.93147180369123816490e-01;   // fdlibm: ln 2 = kLn2Hi + kLn2Lo,
    constexpr double kLn2Lo = 1.90821492927058770002e-10;   // kLn2Hi * k exact for |k| < 2^11
    constexpr double kLn2 = 0.69314718055994530942;
    constexpr double kLog2E = 1.44269504088896338700;
    constexpr double kSqrt2 = 1.41421356237309504880;
    constexpr double kShifter = 6755399441055744.0;           // 1.5 * 2^52: adding it rounds to an integer

    // exp(t) for -700 <= t <= 0: t = k ln 2 + r with |r| <= ln 2 / 2, and
    // exp(r) from its Taylor series to r^13 (the next term is below 2^-57).
    force_inline D2 exp(D2 t) {
      const D2 shifted = t * set(kLog2E) + set(kShifter);
      const D2 k = shifted - set(kShifter);
      const D2 r = (t - k * set(kLn2Hi)) - k * set(kLn2Lo);
      D2 p = set(1.0 / 6227020800.0);                          // 1 / 13!
      p = p * r + set(1.0 / 479001600.0);
      p = p * r + set(1.0 / 39916800.0);
      p = p * r + set(1.0 / 3628800.0);
      p = p * r + set(1.0 / 362880.0);
      p = p * r + set(1.0 / 40320.0);
      p = p * r + set(1.0 / 5040.0);
      p = p * r + set(1.0 / 720.0);
      p = p * r + set(1.0 / 120.0);
      p = p * r + set(1.0 / 24.0);
      p = p * r + set(1.0 / 6.0);
      p = p * r + set(0.5);
      p = p * r + set(1.0);
      p = p * r + set(1.0);
      return p * powerOfTwo(shifted);
    }

    // log1p(u) for 0 <= u <= 1: log(m) of m = 1 + u by fdlibm's e_log
    // polynomial (|s| <= 0.1716 after halving m above sqrt 2), plus the
    // rounding of 1 + u as a first-order correction.
    force_inline D2 log1p(D2 u, D2 m) {
      const D2 one = set(1.0);
      const D2 correction = (u - (m - one)) / m;
      const M2 big = greaterThan(m, set(kSqrt2));
      const D2 f = select(big, m * set(0.5), m) - one;
      const D2 k = select(big, one, set(0.0));
      const D2 s = f / (set(2.0) + f);
      const D2 z = s * s;
      const D2 w = z * z;
      const D2 t1 = w * (set(3.999999999940941908e-01) + w * (set(2.222219843214978396e-01) + w * set(1.531383769920937332e-01)));
      const D2 t2 = z * (set(6.666666666666735130e-01) + w * (set(2.857142874366239149e-01) +
                    w * (set(1.818357216161805012e-01) + w * set(1.479819860511658591e-01))));
      const D2 r = t2 + t1;
      const D2 half_square = set(0.5) * f * f;
      const D2 log = k * set(kLn2Hi) - ((half_square - (s * (half_square + r) + k * set(kLn2Lo))) - f);
      return log + correction;
    }

    struct Kernel { D2 magnitude, u, m, l; };   // |x|, exp(-2|x|), 1 + u, log1p(u)

    force_inline Kernel kernel(D2 x) {
      Kernel result;
      result.magnitude = abs(x);
      // max returns its second operand for NaN, so NaN input stays NaN.
      result.u = exp(max(set(-700.0), set(-2.0) * result.magnitude));
      result.m = set(1.0) + result.u;
      result.l = log1p(result.u, result.m);
      return result;
    }

    force_inline D2 ad1Of(D2, const Kernel& k) { return (k.magnitude - set(kLn2)) + k.l; }

    force_inline D2 tanhOf(D2 x, const Kernel& k) { return copySign((set(1.0) - k.u) / k.m, x); }

    force_inline D2 ad2Of(D2 x, const Kernel& k) {
      // Li2(-u) = -l^2 / 2 - y P(y) / Q(y), y = u / (1 + u) (BP Scream's li2, -1 <= z < 0).
      const D2 y = k.u / k.m;
      const D2 y2 = y * y;
      const D2 y4 = y2 * y2;
      const D2 p = set(0.9999999999999999502e+0) + y * set(-2.6883926818565423430e+0) +
                   y2 * (set(2.6477222699473109692e+0) + y * set(-1.1538559607887416355e+0)) +
                   y4 * (set(2.0886077795020607837e-1) + y * set(-1.0859777134152463084e-2));
      const D2 q = set(1.0000000000000000000e+0) + y * set(-2.9383926818565635485e+0) +
                   y2 * (set(3.2712093293018635389e+0) + y * set(-1.7076702173954289421e+0)) +
                   y4 * (set(4.1596017228400603836e-1) + y * set(-3.9801343754084482956e-2) +
                         y2 * set(8.2743668974466659035e-4));
      const D2 a = k.magnitude;
      const D2 li2_half = set(-0.25) * k.l * k.l - set(0.5) * (y * p / q);
      const D2 g = (set(0.5) * a * a - a * set(kLn2)) + (set(kPi * kPi / 24.0) + li2_half);
      return copySign(g, x);
    }

    double scalar(double x, D2 (*function)(D2, const Kernel&)) {
      alignas(16) double out[2];
      const D2 v = set(x);
      store(out, function(v, kernel(v)));
      return out[0];
    }
  }

  double TanhAdaa2::ad1(double x) { return scalar(x, ad1Of); }
  double TanhAdaa2::ad2(double x) { return scalar(x, ad2Of); }
  double TanhAdaa2::tanh(double x) { return scalar(x, tanhOf); }

  namespace {
    // One step of TanhADAA2 for two lanes, with their state at x1 .. ad2_x1.
    force_inline D2 step(D2 x, double* x1_state, double* x2_state, double* d2_state, double* ad2_state) {
      const D2 tolerance = set(TanhAdaa2::kTolerance), half = set(0.5), two = set(2.0);
      const D2 x1 = load(x1_state), x2 = load(x2_state);
      const D2 d2 = load(d2_state), ad2_x1 = load(ad2_state);
      const D2 ad2_x0 = ad2Of(x, kernel(x));

      D2 d1 = (ad2_x0 - ad2_x1) / (x - x1);
      const M2 ill1 = lessThan(abs(x - x1), tolerance);
      if (any(ill1)) {
        const D2 middle = half * (x + x1);
        d1 = select(ill1, ad1Of(middle, kernel(middle)), d1);
      }

      D2 y = (two / (x - x2)) * (d1 - d2);
      const M2 ill2 = lessThan(abs(x - x2), tolerance);
      if (any(ill2)) {
        const D2 bar = half * (x + x2);
        const D2 delta = bar - x1;
        const Kernel at_bar = kernel(bar);
        const D2 near = (two / delta) * (ad1Of(bar, at_bar) + (ad2_x1 - ad2Of(bar, at_bar)) / delta);
        const D2 centre = half * (bar + x1);
        const D2 flat = tanhOf(centre, kernel(centre));
        y = select(ill2, select(lessThan(abs(delta), tolerance), flat, near), y);
      }

      store(d2_state, d1);
      store(x2_state, x1);
      store(x1_state, x);
      store(ad2_state, ad2_x0);
      return y;
    }

#if VITAL_SSE2
    force_inline D2 lowPair(poly_float value) { return { _mm_cvtps_pd(value.value) }; }
    force_inline D2 highPair(poly_float value) { return { _mm_cvtps_pd(_mm_movehl_ps(value.value, value.value)) }; }
    force_inline poly_float toFloats(D2 low, D2 high) {
      return _mm_movelh_ps(_mm_cvtpd_ps(low.v), _mm_cvtpd_ps(high.v));
    }
#elif VITAL_NEON
    force_inline D2 lowPair(poly_float value) { return { vcvt_f64_f32(vget_low_f32(value.value)) }; }
    force_inline D2 highPair(poly_float value) { return { vcvt_high_f64_f32(value.value) }; }
    force_inline poly_float toFloats(D2 low, D2 high) {
      return vcvt_high_f32_f64(vcvt_f32_f64(low.v), high.v);
    }
#endif
  }

  void TanhAdaa2::process(const double* in, double* out, int pairs) {
    for (int pair = 0; pair < pairs; ++pair) {
      const int lane = 2 * pair;
      store(out + lane, step(load(in + lane), x1_ + lane, x2_ + lane, d2_ + lane, ad2_x1_ + lane));
    }
  }

  poly_float TanhAdaa2::process(poly_float in, int pairs) {
    const D2 low = step(lowPair(in), x1_, x2_, d2_, ad2_x1_);
    const D2 high = pairs > 1 ? step(highPair(in), x1_ + 2, x2_ + 2, d2_ + 2, ad2_x1_ + 2) : low;
    return toFloats(low, high);
  }
} // namespace vital::scream
