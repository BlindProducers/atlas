// SPDX-License-Identifier: GPL-3.0-or-later
// Rust f64 semantics for the OpenWurli port, so the C++ rounds and propagates
// NaN exactly as OpenWurli 0.9.0 built by rustc does on the same libm. Every
// file using these must be built with -ffp-contract=off and without fast-math.
//
// rustc's LLVM rewrites some calls before they reach libm, and macOS's libm
// does not give bit-identical results across those rewrites, so the port calls
// what the Rust binary calls:
//  - x.sin() and x.cos() of the same x become one __sincos_stret call (sinCos);
//  - f64::powf(2.0, x) becomes exp2(x), f64::powf(10.0, x) becomes __exp10(x);
//  - f64::powi(n) is LLVM's powi (repeated squaring), never pow.
// f64::max / f64::min return the non-NaN operand, like std::fmax / std::fmin.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

namespace openwurli {
namespace math {
  // x.sin() and x.cos() together, as rustc emits them on Apple platforms.
  inline void sinCos(double x, double& sin_x, double& cos_x) {
#if defined(__APPLE__)
    const __double2 result = __sincos_stret(x);
    sin_x = result.__sinval;
    cos_x = result.__cosval;
#else
    sin_x = std::sin(x);
    cos_x = std::cos(x);
#endif
  }

  // f64::powf(10.0, x).
  inline double exp10(double x) {
#if defined(__APPLE__)
    return __exp10(x);
#else
    return std::pow(10.0, x);
#endif
  }

  // f64::powi: compiler-rt's __powidf2.
  inline double powi(double a, int b) {
    const bool recip = b < 0;
    double r = 1.0;
    while (true) {
      if (b & 1)
        r *= a;
      b /= 2;
      if (b == 0)
        break;
      a *= a;
    }
    return recip ? 1.0 / r : r;
  }

  // f64::clamp: a NaN input stays NaN, -0.0 stays -0.0.
  inline double clamp(double value, double min, double max) {
    if (value < min)
      value = min;
    if (value > max)
      value = max;
    return value;
  }

  // `value as u64` / `as u32` / `as usize`: truncate toward zero, saturate, NaN to 0.
  inline uint64_t toU64(double value) {
    if (!(value > 0.0))
      return 0;
    if (value >= 18446744073709551616.0)
      return UINT64_MAX;
    return static_cast<uint64_t>(value);
  }

  inline uint32_t toU32(double value) {
    if (!(value > 0.0))
      return 0;
    if (value >= 4294967296.0)
      return UINT32_MAX;
    return static_cast<uint32_t>(value);
  }

  inline uint64_t toBits(double value) {
    uint64_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
  }

  inline double fromBits(uint64_t bits) {
    double value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
  }

  // f32::is_finite of `value as f32`: doubles from 0x1.ffffffp127 up round to
  // infinity in single precision.
  inline bool isFiniteAsFloat(double value) {
    return value < 0x1.ffffffp127 && value > -0x1.ffffffp127;
  }
} // namespace math
} // namespace openwurli
