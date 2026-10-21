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
  static constexpr int kFirTaps = 384;
  static constexpr int kLowTaps = 32;
  // What tick() derives from the cutoff alone: the delay offsets, the FIR's
  // blended kernel and the gains. The cutoff holds still while a note plays
  // unless it is modulated, so these are worked out again only when it
  // changes, with the same arithmetic as before.
  struct CutoffTerms {
    double cutoff = -1;
    double log_cut = 0;
    std::array<int, 6> allpass_offsets {};
    std::array<int, 16> network_offsets {};
    std::array<int, kLowTaps> low_offsets {};
    int fir_taps = 0;
    double fir_amount = 0, low_amount = 0, gain = 1;
  };
  void updateCutoffTerms(double cutoff_hz);
  int delayOffset(int size, double delay) const;
  double read(int base, int position, int size, int offset) const;
  double allpass(double input, int stage, int offset, double coefficient);
  std::unique_ptr<float[]> memory_;
  int capacity_ = 0, main_size_ = 0, ap_size_ = 0, low_size_ = 0;
  int ap_base_ = 0, low_base_ = 0, valid_ = 0;
  int fdn_pos_ = 0, ap_pos_ = 0, low_pos_ = 0;
  // The FIR's write position: the sub-sample phase and, within that phase's
  // ring, the newest host sample.
  int fir_phase_ = 0, fir_newest_ = 0;
  std::array<double, 16> gains_ {}, gain_steps_ {};
  // Each sub-sample phase has its own feedback low-pass state. Together with
  // host-grid delays, this extends the original transfer function to the
  // oversampled stream without changing its decay or adding buffering latency.
  std::array<std::array<double, 16>, kMaximumOversampling> damping_ {};
  // The FIR's input history, one ring of kFirTaps per sub-sample phase, and
  // its blended kernel stored last tap first, reserved with the delay memory
  // (about 27 KB per channel, so voices without Reverb don't carry them);
  // valid_ guards reads.
  std::unique_ptr<double[]> history_;
  std::unique_ptr<double[]> fir_coefficients_;
  CutoffTerms terms_;
  int oversampling_ = 1, fir_size_ = kFirTaps;
  double rate_ = 44100, damp_ = 1, damp_step_ = 0, boost_ = 1, boost_step_ = 0;
  bool fresh_ = true;
};
}
