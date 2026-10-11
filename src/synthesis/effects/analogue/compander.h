// SPDX-License-Identifier: GPL-3.0-or-later
// NE570/SA571-style compander around a BBD: a 2:1 compressor before the chip
// and a 1:2 expander after it.
//
// Both halves use the NE570's full-wave averaging rectifier, whose time
// constant is the internal 10 kOhm times the external rectifier capacitor.
// The compressor's gain cell sits in its op-amp's feedback, so its gain
// follows the average of its own output; the expander's gain follows the
// average of its input, which is the BBD output. Whatever the BBD changes
// between them (bandwidth, delay, noise) makes the expander mistrack, which
// is the breathing and pumping heard on companded BBD units.
#pragma once

#include <algorithm>
#include <cmath>

namespace atlas::analogue {
  class Compander {
   public:
    // reference: level (digital RMS-like average) at which both halves have
    // unity gain. max_gain_db: the compressor's low-level gain limit.
    void prepare(double sample_rate, double rectifier_seconds, double reference = 0.25, double max_gain_db = 24.0) {
      coefficient_ = -std::expm1(-1.0 / (rectifier_seconds * sample_rate));
      reference_ = reference;
      floor_ = reference * std::pow(10.0, -max_gain_db / 20.0);
      reset();
    }

    void reset() { compressor_average_ = expander_average_ = reference_; }

    // Mistracking from wear: the expander's reference drifts from the compressor's.
    void setExpanderOffset(double ratio) { expander_reference_scale_ = ratio; }

    double compress(double x) {
      const double y = x * reference_ / std::max(compressor_average_, floor_);
      compressor_average_ += (std::abs(y) - compressor_average_) * coefficient_;
      return y;
    }

    double expand(double x) {
      expander_average_ += (std::abs(x) - expander_average_) * coefficient_;
      return x * expander_average_ / (reference_ * expander_reference_scale_);
    }

   private:
    double coefficient_ = 0.001, reference_ = 0.25, floor_ = 0.016;
    double compressor_average_ = 0.25, expander_average_ = 0.25;
    double expander_reference_scale_ = 1.0;
  };
}
