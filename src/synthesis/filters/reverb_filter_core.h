// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <memory>

namespace vital {
// The supplied Reference Fit JSFX for four channels at once: a voice pair's
// lanes (two voices, left and right), each with its own cutoff, resonance,
// damping and history. The host sample grid defines its integer delays and
// measured FIR taps, even when Atlas oversamples audio.
//
// Every delay position holds the four lanes side by side, so lanes whose
// cutoffs agree (the usual case) read and write all four with one access, and
// the arithmetic runs two lanes per SIMD register in double precision. The
// write positions are shared: a lane reset only forgets that lane's history,
// since every read is guarded by how many samples the lane has written since.
//
// A pair of lanes whose input, delay reads and damping have all been exactly
// zero for longer than the longest delay (the empty half of a voice pair with
// one voice playing) holds nothing but zeros. Its arithmetic is skipped, and
// zeros written, until its input is non-zero again, giving the same output.
class ReverbFilterCore {
public:
  static constexpr int kMaximumOversampling = 8;
  static constexpr int kLanes = 4;
  void reserve(double maximum_rate);
  // Frees the delay memory; reserve() must run again before tick().
  void release();
  void setSampleRate(double rate, int oversampling = 1);
  // 1 processes lanes 0-1 only (Filter FX, whose lanes 2-3 repeat them) and
  // copies them into lanes 2-3; 2 processes all four.
  void setPairs(int pairs);
  void reset();
  void resetLane(int lane);
  void beginBlock(int lane, double resonance, double damp, int samples);
  // One sample of every lane.
  void tick(const double* input, const double* cutoff_hz, double* output);
private:
  static constexpr int kFirTaps = 384;
  static constexpr int kLowTaps = 32;
  static constexpr int kLines = 16;
  // What tick() derives from a lane's cutoff alone: the delay offsets, the
  // FIR's blended kernel and the gains. The cutoff holds still while a note
  // plays unless it is modulated, so these are worked out again only when it
  // changes, with the same arithmetic as before.
  struct CutoffTerms {
    double cutoff = -1;
    double log_cut = 0;
    std::array<int, 6> allpass_offsets {};
    std::array<int, kLines> network_offsets {};
    std::array<int, kLowTaps> low_offsets {};
    int fir_taps = 0;
    double fir_amount = 0, low_amount = 0, gain = 1;
  };
  struct Active;
  void updateCutoffTerms(int lane, double cutoff_hz);
  void compareLanes();
  int delayOffset(int size, double delay) const;
  float* at(int base, int position, int size, int offset) const;
  template<typename Offset>
  void readLanes(const Active& active, int base, int position, int size, Offset offset, double* values) const;
  void allpass(const Active& active, double* values, int stage, double coefficient);
  double fir(int lane, int count) const;

  std::unique_ptr<float[]> memory_;
  int capacity_ = 0, main_size_ = 0, ap_size_ = 0, low_size_ = 0;
  int ap_base_ = 0, low_base_ = 0;
  int fdn_pos_ = 0, ap_pos_ = 0, low_pos_ = 0;
  // The FIR's write position: the sub-sample phase and, within that phase's
  // ring, the newest host sample.
  int fir_phase_ = 0, fir_newest_ = 0;
  // Per lane: samples written since its reset (guarding its reads), and
  // whether its offsets match lane 0's or its pair partner's.
  std::array<int, kLanes> valid_ {};
  bool uniform_ = false;
  std::array<bool, kLanes / 2> pair_uniform_ {};
  std::array<CutoffTerms, kLanes> terms_;
  // [line][lane] so a pair of lanes is two adjacent doubles.
  alignas(16) double gains_[kLines][kLanes] {}, gain_steps_[kLines][kLanes] {};
  // Each sub-sample phase has its own feedback low-pass state. Together with
  // host-grid delays, this extends the original transfer function to the
  // oversampled stream without changing its decay or adding buffering latency.
  alignas(16) double damping_[kMaximumOversampling][kLines][kLanes] {};
  alignas(16) double damp_[kLanes] {}, damp_step_[kLanes] {}, boost_[kLanes] {}, boost_step_[kLanes] {};
  alignas(16) double gain_[kLanes] {}, fir_amount_[kLanes] {}, low_amount_[kLanes] {};
  std::array<bool, kLanes> fresh_ {};
  // Whether a pair's gains are ramping this block (a still gain needs no step).
  std::array<bool, kLanes / 2> gains_moving_ {};
  bool gains_checked_ = false;
  // Per lane: consecutive samples with exactly zero input, delay reads and
  // damping, saturating at the longest delay, when the lane is idle.
  std::array<int, kLanes> silent_ {};
  int pairs_ = kLanes / 2;
  // The FIR's input history, one ring of kFirTaps per sub-sample phase, and
  // its blended kernels stored last tap first, [tap][lane] in both, reserved
  // with the delay memory (about 110 KB, so voices without Reverb don't carry
  // them); valid_ guards reads.
  std::unique_ptr<double[]> history_;
  std::unique_ptr<double[]> fir_coefficients_;
  int oversampling_ = 1, fir_size_ = kFirTaps;
  double rate_ = 44100;
};
}
