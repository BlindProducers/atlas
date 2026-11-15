// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The four-lane float operations the piano engine runs its resonator banks
// with. The original engine is written with NEON intrinsics; on arm64 these
// wrappers are those intrinsics, so the output is bit-identical to it. On
// x64 they are SSE2, where there is no fused multiply-add: the sums round
// twice, which the resonators do not mind.

#if defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(_M_ARM64)
  #include <arm_neon.h>
  #define PIANO_SIMD_NEON 1
#else
  #include <emmintrin.h>
  #define PIANO_SIMD_NEON 0
#endif

namespace pm::simd
{

#if PIANO_SIMD_NEON

using f4 = float32x4_t;

inline f4 load (const float* p) { return vld1q_f32 (p); }
inline void store (float* p, f4 v) { vst1q_f32 (p, v); }
inline f4 set (float x) { return vdupq_n_f32 (x); }
inline f4 add (f4 a, f4 b) { return vaddq_f32 (a, b); }
inline f4 mul (f4 a, f4 b) { return vmulq_f32 (a, b); }
/** a + b * c */
inline f4 fma (f4 a, f4 b, f4 c) { return vfmaq_f32 (a, b, c); }
/** a - b * c */
inline f4 fms (f4 a, f4 b, f4 c) { return vfmsq_f32 (a, b, c); }
/** a + b * s */
inline f4 fmaScalar (f4 a, f4 b, float s) { return vfmaq_n_f32 (a, b, s); }
inline float maxLane (f4 v) { return vmaxvq_f32 (v); }
inline float sum (f4 v) { return vaddvq_f32 (v); }

#else

using f4 = __m128;

inline f4 load (const float* p) { return _mm_loadu_ps (p); }
inline void store (float* p, f4 v) { _mm_storeu_ps (p, v); }
inline f4 set (float x) { return _mm_set1_ps (x); }
inline f4 add (f4 a, f4 b) { return _mm_add_ps (a, b); }
inline f4 mul (f4 a, f4 b) { return _mm_mul_ps (a, b); }
inline f4 fma (f4 a, f4 b, f4 c) { return _mm_add_ps (a, _mm_mul_ps (b, c)); }
inline f4 fms (f4 a, f4 b, f4 c) { return _mm_sub_ps (a, _mm_mul_ps (b, c)); }
inline f4 fmaScalar (f4 a, f4 b, float s) { return _mm_add_ps (a, _mm_mul_ps (b, _mm_set1_ps (s))); }

inline float maxLane (f4 v)
{
    const f4 m1 = _mm_max_ps (v, _mm_shuffle_ps (v, v, _MM_SHUFFLE (2, 3, 0, 1)));
    const f4 m2 = _mm_max_ps (m1, _mm_shuffle_ps (m1, m1, _MM_SHUFFLE (1, 0, 3, 2)));
    return _mm_cvtss_f32 (m2);
}

inline float sum (f4 v)
{
    const f4 s1 = _mm_add_ps (v, _mm_shuffle_ps (v, v, _MM_SHUFFLE (2, 3, 0, 1)));
    const f4 s2 = _mm_add_ps (s1, _mm_shuffle_ps (s1, s1, _MM_SHUFFLE (1, 0, 3, 2)));
    return _mm_cvtss_f32 (s2);
}

#endif

} // namespace pm::simd
