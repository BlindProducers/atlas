// SPDX-License-Identifier: GPL-3.0-or-later
// Rate conversion between Atlas's processing rate and a codec's own rate.
// Maim feeds its codecs the host's samples whatever the rate; Atlas runs its
// effects oversampled, so the Maim effect converts to each codec's rate and
// back with a windowed sinc. Atlas's own code.
#pragma once
#include <cstdint>
#include <vector>

namespace atlas::maim {

// The coefficients for one rate pair: a Kaiser-windowed sinc, stretched to
// the lower rate's band when decimating, tabulated for every phase of the
// rational step. Built off the audio thread; shared by the streams.
class ResamplerKernel {
 public:
  // Half the kernel's width, in samples at the lower of the two rates.
  static constexpr int kHalfWidth = 16;

  ResamplerKernel(int in_rate, int out_rate);
  int inRate() const { return in_rate_; }
  int outRate() const { return out_rate_; }
  bool identity() const { return in_rate_ == out_rate_; }
  // Input samples each side of an output's position.
  int half() const { return half_; }
  int taps() const { return 2 * half_ + 1; }
  // The step from one output to the next is stepNumerator() / phases() input samples.
  int phases() const { return q_; }
  int stepNumerator() const { return p_; }
  const float* coefficients(int phase) const {
    return coefficients_.data() + static_cast<size_t>(phase) * static_cast<size_t>(taps());
  }

 private:
  int in_rate_, out_rate_, p_, q_, half_;
  std::vector<float> coefficients_;
};

// One channel's stream through a kernel: push input samples, pull every
// output that can be computed. The delay is the kernel's half width.
class Resampler {
 public:
  // Room for the widest kernel and longest step the stream will see. Allocates.
  void reserve(int max_half, int max_step);
  void bind(const ResamplerKernel* kernel);
  void reset();
  void push(float sample) {
    history_[static_cast<uint32_t>(written_) & mask_] = sample;
    ++written_;
  }
  bool pull(float& out);
  int delayInputSamples() const { return kernel_ ? kernel_->half() : 0; }
  const ResamplerKernel* kernel() const { return kernel_; }

 private:
  const ResamplerKernel* kernel_ = nullptr;
  std::vector<float> history_;
  uint32_t mask_ = 0;
  int64_t written_ = 0;   // the newest input sample's index plus one
  int64_t position_ = 0;  // integer part of the next output's input position
  int phase_ = 0;         // its fractional part, in units of 1 / phases()
};
}
