// SPDX-License-Identifier: GPL-3.0-or-later
#include "resampler.h"
#include <algorithm>
#include <cmath>
#include <numeric>

namespace atlas::maim {
namespace {
  constexpr double kPi = 3.14159265358979323846;
  // The pass band stops a little below the lower Nyquist, where the window's
  // transition band would otherwise let images and aliases through.
  constexpr double kCutoff = 0.9;
  constexpr double kKaiserBeta = 7.0;

  double besselI0(double x) {
    double sum = 1.0, term = 1.0;
    const double half = x * 0.5;
    for (int k = 1; k < 64; ++k) {
      term *= (half / k) * (half / k);
      sum += term;
      if (term < 1.0e-12 * sum) break;
    }
    return sum;
  }
}

ResamplerKernel::ResamplerKernel(int in_rate, int out_rate) :
    in_rate_(in_rate), out_rate_(out_rate), p_(1), q_(1), half_(0) {
  if (in_rate <= 0 || out_rate <= 0 || in_rate == out_rate) {
    coefficients_.assign(1, 1.0f);
    return;
  }
  const int divisor = std::gcd(in_rate, out_rate);
  p_ = in_rate / divisor;
  q_ = out_rate / divisor;
  const double step = static_cast<double>(in_rate) / out_rate;
  const double stretch = std::max(1.0, step);
  half_ = static_cast<int>(std::ceil(kHalfWidth * stretch));
  const int taps = 2 * half_ + 1;
  coefficients_.assign(static_cast<size_t>(q_) * static_cast<size_t>(taps), 0.0f);
  const double window_scale = 1.0 / besselI0(kKaiserBeta);
  for (int phase = 0; phase < q_; ++phase) {
    float* c = coefficients_.data() + static_cast<size_t>(phase) * static_cast<size_t>(taps);
    double sum = 0.0;
    for (int t = 0; t < taps; ++t) {
      // The output sits phase / q_ input samples past integer position 0;
      // tap t reads the input (t - half_) samples before it.
      const double x = (t - half_) + static_cast<double>(phase) / q_;
      const double u = x / stretch;
      double value = 0.0;
      if (std::abs(u) < kHalfWidth) {
        const double argument = kPi * kCutoff * u;
        const double sinc = std::abs(argument) < 1.0e-9 ? 1.0 : std::sin(argument) / argument;
        const double r = u / kHalfWidth;
        const double window = besselI0(kKaiserBeta * std::sqrt(std::max(0.0, 1.0 - r * r))) * window_scale;
        value = (kCutoff / stretch) * sinc * window;
      }
      c[t] = static_cast<float>(value);
      sum += value;
    }
    // Unity gain at DC for every phase.
    if (sum > 0.0) {
      const float scale = static_cast<float>(1.0 / sum);
      for (int t = 0; t < taps; ++t) c[t] *= scale;
    }
  }
}

void Resampler::reserve(int max_half, int max_step) {
  uint32_t size = 64;
  const uint32_t needed = static_cast<uint32_t>(2 * std::max(0, max_half) + std::max(1, max_step) + 8);
  while (size < needed) size *= 2;
  history_.assign(size, 0.0f);
  mask_ = size - 1;
  reset();
}

void Resampler::bind(const ResamplerKernel* kernel) {
  kernel_ = kernel;
  reset();
}

void Resampler::reset() {
  std::fill(history_.begin(), history_.end(), 0.0f);
  written_ = 0;
  position_ = 0;
  phase_ = 0;
}

bool Resampler::pull(float& out) {
  if (kernel_ == nullptr || history_.empty()) return false;
  const int half = kernel_->half();
  if (position_ + half > written_ - 1) return false;
  const float* c = kernel_->coefficients(phase_);
  const int taps = kernel_->taps();
  int64_t index = position_ + half;
  float sum = 0.0f;
  for (int t = 0; t < taps; ++t, --index)
    sum += c[t] * history_[static_cast<uint32_t>(index) & mask_];
  out = sum;
  phase_ += kernel_->stepNumerator();
  position_ += phase_ / kernel_->phases();
  phase_ %= kernel_->phases();
  return true;
}
}
