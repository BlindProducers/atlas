// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <memory>

namespace vital {
// One channel of the supplied Reference Fit JSFX. The host sample grid defines
// its integer delays and measured FIR taps, even when Atlas oversamples audio.
class ReverbFilterCore {
public:
  static constexpr int kMaximumOversampling = 8;
  void reserve(double maximum_rate);
  // Frees the delay memory; reserve() must run again before tick().
  void release();
  void setSampleRate(double rate, int oversampling = 1);
  void reset();
  void beginBlock(double resonance, double damp, int samples);
  double tick(double input, double cutoff_hz);
private:
  double read(int base, int position, int size, double delay) const;
  double allpass(double input, int stage, double delay, double coefficient);
  std::unique_ptr<float[]> memory_;
  int capacity_ = 0, main_size_ = 0, ap_size_ = 0, low_size_ = 0;
  int ap_base_ = 0, low_base_ = 0, valid_ = 0;
  int fdn_pos_ = 0, ap_pos_ = 0, low_pos_ = 0, fir_pos_ = 0;
  std::array<double, 16> gains_ {}, gain_steps_ {};
  // Each sub-sample phase has its own feedback low-pass state. Together with
  // host-grid delays, this extends the original transfer function to the
  // oversampled stream without changing its decay or adding buffering latency.
  std::array<std::array<double, 16>, kMaximumOversampling> damping_ {};
  // The FIR's input history, reserved with the delay memory (about 24 KB per
  // channel, so voices without Reverb don't carry it); valid_ guards reads.
  std::unique_ptr<double[]> history_;
  int oversampling_ = 1, fir_size_ = 384;
  double rate_ = 44100, damp_ = 1, damp_step_ = 0, boost_ = 1, boost_step_ = 0;
  bool fresh_ = true;
};
}
