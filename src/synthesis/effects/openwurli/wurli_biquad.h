// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from OpenWurli 0.9.0 (crates/openwurli-dsp/src/filters.rs), whose
// biquad is melange-primitives' (rev 7ecb36c, crates/melange-primitives/src/
// filters.rs): Audio EQ Cookbook coefficients, transposed direct form II.
// Both GPL-3.0-or-later. See README.atlas.md. Like the Rust build, each filter
// takes the sine and cosine of its angle in one sincos call.
#pragma once

#include "wurli_math.h"

#include <algorithm>
#include <cmath>

namespace openwurli {
  // core::f64::consts::TAU and PI, so expressions round as the Rust does.
  constexpr double kTau = 6.283185307179586476925286766559;
  constexpr double kPi = 3.14159265358979323846264338327950288;

  class Biquad {
    public:
      static Biquad bandpass(double center_hz, double q, double sample_rate) {
        Biquad biquad;
        biquad.setBandpass(center_hz, q, sample_rate);
        return biquad;
      }

      static Biquad lowpass(double cutoff_hz, double q, double sample_rate) {
        Biquad biquad;
        biquad.setLowpass(cutoff_hz, q, sample_rate);
        return biquad;
      }

      static Biquad highpass(double cutoff_hz, double q, double sample_rate) {
        Biquad biquad;
        biquad.setHighpass(cutoff_hz, q, sample_rate);
        return biquad;
      }

      // The setters change coefficients and keep the filter state.
      void setLowpass(double fc, double q, double fs) {
        const double w0 = kTau * std::fmin(fc, fs * 0.499) / fs;
        double sin_w0, cos_w0;
        math::sinCos(w0, sin_w0, cos_w0);
        const double alpha = sin_w0 / (2.0 * q);
        const double a0 = 1.0 + alpha;
        b0_ = ((1.0 - cos_w0) / 2.0) / a0;
        b1_ = (1.0 - cos_w0) / a0;
        b2_ = ((1.0 - cos_w0) / 2.0) / a0;
        a1_ = (-2.0 * cos_w0) / a0;
        a2_ = (1.0 - alpha) / a0;
      }

      void setHighpass(double fc, double q, double fs) {
        const double w0 = kTau * std::fmin(fc, fs * 0.499) / fs;
        double sin_w0, cos_w0;
        math::sinCos(w0, sin_w0, cos_w0);
        const double alpha = sin_w0 / (2.0 * q);
        const double a0 = 1.0 + alpha;
        b0_ = ((1.0 + cos_w0) / 2.0) / a0;
        b1_ = -(1.0 + cos_w0) / a0;
        b2_ = ((1.0 + cos_w0) / 2.0) / a0;
        a1_ = (-2.0 * cos_w0) / a0;
        a2_ = (1.0 - alpha) / a0;
      }

      void setBandpass(double fc, double q, double fs) {
        const double w0 = kTau * std::fmin(fc, fs * 0.499) / fs;
        double sin_w0, cos_w0;
        math::sinCos(w0, sin_w0, cos_w0);
        const double alpha = sin_w0 / (2.0 * q);
        const double a0 = 1.0 + alpha;
        b0_ = alpha / a0;
        b1_ = 0.0;
        b2_ = -alpha / a0;
        a1_ = (-2.0 * cos_w0) / a0;
        a2_ = (1.0 - alpha) / a0;
      }

      double process(double input) {
        const double output = b0_ * input + s1_;
        s1_ = b1_ * input - a1_ * output + s2_;
        s2_ = b2_ * input - a2_ * output;
        return output;
      }

      void reset() {
        s1_ = 0.0;
        s2_ = 0.0;
      }

    private:
      double s1_ = 0.0;
      double s2_ = 0.0;
      double b0_ = 0.0;
      double b1_ = 0.0;
      double b2_ = 0.0;
      double a1_ = 0.0;
      double a2_ = 0.0;
  };
} // namespace openwurli
