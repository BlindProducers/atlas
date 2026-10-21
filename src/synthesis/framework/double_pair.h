// SPDX-License-Identifier: GPL-3.0-or-later
// Two doubles in one SIMD register (SSE2 or NEON), for double-precision DSP
// that runs two lanes of a poly_float at once. Every operation is a single
// IEEE intrinsic, so no compiler fuses a multiply and an add into an FMA on
// one platform and not the other, and both platforms round alike.
#pragma once
#include "poly_values.h"

namespace vital {
  struct double_mask {
#if VITAL_SSE2
    __m128d value;
    force_inline bool any() const { return _mm_movemask_pd(value) != 0; }
    force_inline bool all() const { return _mm_movemask_pd(value) == 3; }
#elif VITAL_NEON
    uint64x2_t value;
    force_inline bool any() const { return vmaxvq_u32(vreinterpretq_u32_u64(value)) != 0; }
    force_inline bool all() const { return vminvq_u32(vreinterpretq_u32_u64(value)) != 0; }
#endif
  };

  struct double_pair {
#if VITAL_SSE2
    __m128d value;

    static force_inline double_pair splat(double x) { return { _mm_set1_pd(x) }; }
    static force_inline double_pair make(double first, double second) { return { _mm_set_pd(second, first) }; }
    static force_inline double_pair load(const double* memory) { return { _mm_loadu_pd(memory) }; }
    // Two floats, widened.
    static force_inline double_pair loadFloats(const float* memory) {
      return { _mm_cvtps_pd(_mm_loadl_pi(_mm_setzero_ps(), reinterpret_cast<const __m64*>(memory))) };
    }
    // Four floats, widened into two pairs.
    static force_inline void loadFloats(const float* memory, double_pair& low, double_pair& high) {
      const __m128 floats = _mm_loadu_ps(memory);
      low.value = _mm_cvtps_pd(floats);
      high.value = _mm_cvtps_pd(_mm_movehl_ps(floats, floats));
    }
    // Two pairs, rounded to four floats.
    static force_inline void storeFloats(float* memory, double_pair low, double_pair high) {
      _mm_storeu_ps(memory, _mm_movelh_ps(_mm_cvtpd_ps(low.value), _mm_cvtpd_ps(high.value)));
    }
    static force_inline poly_float toFloats(double_pair low, double_pair high) {
      return _mm_movelh_ps(_mm_cvtpd_ps(low.value), _mm_cvtpd_ps(high.value));
    }
    static force_inline void fromFloats(poly_float floats, double_pair& low, double_pair& high) {
      low.value = _mm_cvtps_pd(floats.value);
      high.value = _mm_cvtps_pd(_mm_movehl_ps(floats.value, floats.value));
    }
    force_inline void store(double* memory) const { _mm_storeu_pd(memory, value); }
    force_inline double first() const { return _mm_cvtsd_f64(value); }
    force_inline double second() const { return _mm_cvtsd_f64(_mm_unpackhi_pd(value, value)); }

    friend force_inline double_pair operator+(double_pair a, double_pair b) { return { _mm_add_pd(a.value, b.value) }; }
    friend force_inline double_pair operator-(double_pair a, double_pair b) { return { _mm_sub_pd(a.value, b.value) }; }
    friend force_inline double_pair operator*(double_pair a, double_pair b) { return { _mm_mul_pd(a.value, b.value) }; }
    friend force_inline double_pair operator/(double_pair a, double_pair b) { return { _mm_div_pd(a.value, b.value) }; }
    friend force_inline double_pair abs(double_pair a) { return { _mm_andnot_pd(_mm_set1_pd(-0.0), a.value) }; }
    // The second operand when either is NaN.
    friend force_inline double_pair max(double_pair a, double_pair b) { return { _mm_max_pd(a.value, b.value) }; }
    friend force_inline double_pair copySign(double_pair magnitude, double_pair sign) {
      const __m128d bit = _mm_set1_pd(-0.0);
      return { _mm_or_pd(_mm_andnot_pd(bit, magnitude.value), _mm_and_pd(bit, sign.value)) };
    }
    friend force_inline double_mask lessThan(double_pair a, double_pair b) { return { _mm_cmplt_pd(a.value, b.value) }; }
    friend force_inline double_mask greaterThan(double_pair a, double_pair b) { return { _mm_cmpgt_pd(a.value, b.value) }; }
    friend force_inline double_pair select(double_mask mask, double_pair yes, double_pair no) {
      return { _mm_or_pd(_mm_and_pd(mask.value, yes.value), _mm_andnot_pd(mask.value, no.value)) };
    }
    // The bits of both, for tracking whether any value was other than +0.
    friend force_inline double_pair bitOr(double_pair a, double_pair b) { return { _mm_or_pd(a.value, b.value) }; }
    // Whether each lane's bits are all zero (+0).
    force_inline bool firstIsZeroBits() const { return _mm_cvtsi128_si64(_mm_castpd_si128(value)) == 0; }
    force_inline bool secondIsZeroBits() const {
      return _mm_cvtsi128_si64(_mm_castpd_si128(_mm_unpackhi_pd(value, value))) == 0;
    }
#elif VITAL_NEON
    float64x2_t value;

    static force_inline double_pair splat(double x) { return { vdupq_n_f64(x) }; }
    static force_inline double_pair make(double first, double second) {
      return { vsetq_lane_f64(second, vdupq_n_f64(first), 1) };
    }
    static force_inline double_pair load(const double* memory) { return { vld1q_f64(memory) }; }
    static force_inline double_pair loadFloats(const float* memory) { return { vcvt_f64_f32(vld1_f32(memory)) }; }
    static force_inline void loadFloats(const float* memory, double_pair& low, double_pair& high) {
      const float32x4_t floats = vld1q_f32(memory);
      low.value = vcvt_f64_f32(vget_low_f32(floats));
      high.value = vcvt_high_f64_f32(floats);
    }
    static force_inline void storeFloats(float* memory, double_pair low, double_pair high) {
      vst1q_f32(memory, vcvt_high_f32_f64(vcvt_f32_f64(low.value), high.value));
    }
    static force_inline poly_float toFloats(double_pair low, double_pair high) {
      return vcvt_high_f32_f64(vcvt_f32_f64(low.value), high.value);
    }
    static force_inline void fromFloats(poly_float floats, double_pair& low, double_pair& high) {
      low.value = vcvt_f64_f32(vget_low_f32(floats.value));
      high.value = vcvt_high_f64_f32(floats.value);
    }
    force_inline void store(double* memory) const { vst1q_f64(memory, value); }
    force_inline double first() const { return vgetq_lane_f64(value, 0); }
    force_inline double second() const { return vgetq_lane_f64(value, 1); }

    friend force_inline double_pair operator+(double_pair a, double_pair b) { return { vaddq_f64(a.value, b.value) }; }
    friend force_inline double_pair operator-(double_pair a, double_pair b) { return { vsubq_f64(a.value, b.value) }; }
    friend force_inline double_pair operator*(double_pair a, double_pair b) { return { vmulq_f64(a.value, b.value) }; }
    friend force_inline double_pair operator/(double_pair a, double_pair b) { return { vdivq_f64(a.value, b.value) }; }
    friend force_inline double_pair abs(double_pair a) { return { vabsq_f64(a.value) }; }
    friend force_inline double_pair max(double_pair a, double_pair b) {
      return { vbslq_f64(vcgtq_f64(a.value, b.value), a.value, b.value) };
    }
    friend force_inline double_pair copySign(double_pair magnitude, double_pair sign) {
      return { vbslq_f64(vdupq_n_u64(uint64_t(1) << 63), sign.value, magnitude.value) };
    }
    friend force_inline double_mask lessThan(double_pair a, double_pair b) { return { vcltq_f64(a.value, b.value) }; }
    friend force_inline double_mask greaterThan(double_pair a, double_pair b) { return { vcgtq_f64(a.value, b.value) }; }
    friend force_inline double_pair select(double_mask mask, double_pair yes, double_pair no) {
      return { vbslq_f64(mask.value, yes.value, no.value) };
    }
    friend force_inline double_pair bitOr(double_pair a, double_pair b) {
      return { vreinterpretq_f64_u64(vorrq_u64(vreinterpretq_u64_f64(a.value), vreinterpretq_u64_f64(b.value))) };
    }
    force_inline bool firstIsZeroBits() const { return vgetq_lane_u64(vreinterpretq_u64_f64(value), 0) == 0; }
    force_inline bool secondIsZeroBits() const { return vgetq_lane_u64(vreinterpretq_u64_f64(value), 1) == 0; }
#endif
  };
} // namespace vital
