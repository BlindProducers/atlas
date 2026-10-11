// SPDX-License-Identifier: GPL-3.0-or-later
#include "filter_table_kernel.h"
#include <algorithm>
#include <cmath>

namespace atlas::filter_table {
  namespace {
    constexpr float kPi = 3.14159265358979323846f;
    constexpr float kSmoothSide = 0.1f, kSmoothCentre = 0.8f;
    constexpr int kFadeSamples = 128;
    constexpr float kBandLimitStart = 0.44f, kBandLimitEnd = 0.46f;     // of the sample rate (zero-phase modes)
    constexpr float kFloor = 1.0e-6f;                                  // -120 dB magnitude floor
    constexpr float kMaxResonancePeakBoost = 2.2387f;                  // +7 dB
    // Measured sub-sample offsets of each mode's kernel against the exact design
    // (positive is later).
    // Minimum sits between the best fits at 1 kHz (1.25) and 5 kHz (0.75).
    constexpr float kMinimumOffset = 1.0f, kLinearOffset = 0.0f, kOriginalOffset = -0.5f, kRawOffset = -0.5f;

    float interpolate(const float* data, int length, float position) {
      if (position <= -1.0f || position >= float(length)) return 0.0f;
      const int index = int(std::floor(position));
      const float fraction = position - index;
      const float a = index >= 0 && index < length ? data[index] : 0.0f;
      const float b = index + 1 >= 0 && index + 1 < length ? data[index + 1] : 0.0f;
      return a + (b - a) * fraction;
    }
  }

  FrameAnalyser::FrameAnalyser() : buffer_(2 * kFrameSize) {}

  void FrameAnalyser::analyse(const float* waveform, Frame& out) {
    std::copy_n(waveform, kFrameSize, buffer_.begin());
    std::fill(buffer_.begin() + kFrameSize, buffer_.end(), 0.0f);
    fft_.performRealOnlyForwardTransform(buffer_.data(), true);
    const float scale = 2.0f / kFrameSize;
    out.harmonics[0] = 0.0f;
    for (int n = 1; n <= kNumHarmonics; ++n)
      out.harmonics[n] = std::complex<float>(buffer_[2 * n] * scale, buffer_[2 * n + 1] * scale);
  }

  void FrameAnalyser::interpolate(const Frame& a, const Frame& b, float t, Frame& out) {
    t = std::clamp(t, 0.0f, 1.0f);
    for (int n = 0; n <= kNumHarmonics; ++n)
      out.harmonics[n] = a.harmonics[n] + (b.harmonics[n] - a.harmonics[n]) * t;
  }

  KernelBuilder::KernelBuilder(int order, int design_order) :
      order_(order), size_(1 << order), design_order_(std::max(order, design_order)),
      design_size_(1 << design_order_), fft_(design_order_), grid_(design_size_ / 2 + 1),
      complex_a_(design_size_), complex_b_(design_size_), period_(2 * kFrameSize) {}

  void KernelBuilder::tiltedMagnitudes(const Frame& frame, const Parameters& p) {
    for (int n = 1; n <= kNumHarmonics; ++n)
      magnitude_[n] = kTilt * n * std::abs(frame.harmonics[n]);
    const float r = std::clamp(p.resonance, kMinResonance, kMaxResonance);
    if (std::abs(r - 1.0f) >= 1.0e-4f) {
      // |H_r| = c (|H| / c)^r about the energy-weighted mean level of the
      // response (within 0.1 dB of the measured pivot on both reference
      // frames, and it stays where the sound is on bright tables).
      double weight_sum = 0.0, weighted_log = 0.0;
      for (int n = 1; n <= kNumHarmonics; ++n) {
        const double value = magnitude_[n];
        if (value <= kFloor) continue;
        const double weight = value * value;
        weight_sum += weight;
        weighted_log += weight * std::log(value);
      }
      float reference = weight_sum > 0.0 ? float(std::exp(weighted_log / weight_sum)) : 0.0f;
      if (reference < kFloor) reference = *std::max_element(magnitude_.begin() + 1, magnitude_.end());
      if (reference >= kFloor) {
        const float log_reference = std::log(reference);
        const float peak_before = *std::max_element(magnitude_.begin() + 1, magnitude_.end());
        for (int n = 1; n <= kNumHarmonics; ++n) {
          const float value = std::max(magnitude_[n], reference * kFloor);
          magnitude_[n] = std::exp(log_reference + r * (std::log(value) - log_reference));
        }
        // A strong peak raised to the fourth power would climb by tens of dB; the
        // measured reference frame's peak rose 6.7 dB at 400%, so allow that much.
        const float peak_after = *std::max_element(magnitude_.begin() + 1, magnitude_.end());
        const float allowed = peak_before * kMaxResonancePeakBoost;
        if (peak_after > allowed && peak_after > 0.0f) {
          const float scale = allowed / peak_after;
          for (int n = 1; n <= kNumHarmonics; ++n) magnitude_[n] *= scale;
        }
      }
    }
    magnitude_[0] = magnitude_[1];
    // The output stage band-limits dry and wet with a zero-phase brickwall at 0.458 fs
    // (measured on reference renders). Linear and Original absorb it in
    // their zero-phase design; Minimum and Raw are designed full band so their
    // phase stays minimum / raw, and harmonics above Nyquist are dropped.
    const float cutoff = std::max(p.cutoff_hz, 1.0f);
    const bool zero_phase = p.mode == kLinear || p.mode == kOriginal;
    const float limit_start = std::isfinite(p.band_limit_start) ? p.band_limit_start : (zero_phase ? kBandLimitStart : 0.5f);
    const float limit_end = std::isfinite(p.band_limit_end) ? p.band_limit_end : (zero_phase ? kBandLimitEnd : 0.5f);
    const float start = limit_start * p.sample_rate * kCutoffHarmonic / cutoff;
    const float end = limit_end * p.sample_rate * kCutoffHarmonic / cutoff;
    for (int n = 1; n <= kNumHarmonics; ++n) {
      if (n <= start) continue;
      if (n >= end) { magnitude_[n] = 0.0f; continue; }
      magnitude_[n] *= 0.5f + 0.5f * std::cos(kPi * (n - start) / (end - start));
    }
  }

  float KernelBuilder::interpolatedMagnitude(float q) const {
    if (q <= 1.0f) return magnitude_[1];
    if (q >= float(kNumHarmonics)) return 0.0f;
    const int low = int(q);
    return magnitude_[low] + (magnitude_[low + 1] - magnitude_[low]) * (q - low);
  }

  float KernelBuilder::magnitudeAt(const Frame& frame, const Parameters& p, float hz) {
    tiltedMagnitudes(frame, p);
    return interpolatedMagnitude(hz * kCutoffHarmonic / std::max(p.cutoff_hz, 1.0f));
  }

  void KernelBuilder::fillGrid(const Parameters& p) {
    const float cutoff = std::max(p.cutoff_hz, 1.0f);
    for (int k = 0; k <= design_size_ / 2; ++k)
      grid_[k] = interpolatedMagnitude(float(k) * p.sample_rate / float(design_size_) * kCutoffHarmonic / cutoff);
  }

  // complex_a_ holds the designed spectrum on the design grid; applies the
  // sub-sample offset as a phase ramp, transforms, and writes size_ taps with
  // time zero at `zero` (the design grid is at least as long as the kernel).
  void KernelBuilder::finishKernel(float sub_sample_offset, int zero, float* kernel) {
    const int n = design_size_, half = n / 2;
    if (sub_sample_offset != 0.0f) {
      for (int k = 1; k < half; ++k) {
        const std::complex<float> ramp = std::polar(1.0f, -2.0f * kPi * float(k) * sub_sample_offset / float(n));
        complex_a_[k] *= ramp;
        complex_a_[n - k] = std::conj(complex_a_[k]);
      }
      complex_a_[half] = std::complex<float>(complex_a_[half].real() * std::cos(kPi * sub_sample_offset), 0.0f);
    }
    fft_.perform(complex_a_.data(), complex_b_.data(), true);
    for (int t = 0; t < size_; ++t) kernel[t] = complex_b_[(t - zero) & (n - 1)].real();
  }

  void KernelBuilder::designMinimum(const Parameters& p, float* kernel) {
    fillGrid(p);
    const int n = design_size_, half = n / 2;
    float peak = 0.0f;
    for (int k = 0; k <= half; ++k) peak = std::max(peak, grid_[k]);
    const float floor = std::max(peak * kFloor, 1.0e-20f);
    for (int k = 0; k <= half; ++k) {
      const float value = std::log(std::max(grid_[k], floor));
      complex_a_[k] = value;
      if (k > 0 && k < half) complex_a_[n - k] = value;
    }
    fft_.perform(complex_a_.data(), complex_b_.data(), true);          // real cepstrum
    for (int i = 0; i < n; ++i) {
      const float c = complex_b_[i].real();
      complex_a_[i] = i == 0 || i == half ? c : (i < half ? 2.0f * c : 0.0f);
    }
    fft_.perform(complex_a_.data(), complex_b_.data(), false);
    for (int i = 0; i < n; ++i) complex_a_[i] = std::exp(complex_b_[i]);
    finishKernel(std::isfinite(p.offset_override) ? p.offset_override : kMinimumOffset, 0, kernel);
    smooth(kernel);
    fade(kernel, false, true);
  }

  void KernelBuilder::designLinear(const Parameters& p, float* kernel) {
    fillGrid(p);
    const int n = design_size_, half = n / 2;
    for (int k = 0; k <= half; ++k) {
      complex_a_[k] = grid_[k];
      if (k > 0 && k < half) complex_a_[n - k] = grid_[k];
    }
    finishKernel(std::isfinite(p.offset_override) ? p.offset_override : kLinearOffset, size_ / 2, kernel);
    smooth(kernel);
    fade(kernel, true, true);
  }

  void KernelBuilder::designOriginal(const Frame& frame, const Parameters& p, float* kernel) {
    fillGrid(p);
    const int n = design_size_, half = n / 2;
    const float cutoff = std::max(p.cutoff_hz, 1.0f);
    for (int k = 0; k <= half; ++k) {
      // Harmonic position q: the frame's phase (quadrature) advanced by an
      // eighth of a period, as measured.
      const float q = float(k) * p.sample_rate / float(n) * kCutoffHarmonic / cutoff;
      const float clamped = std::clamp(q, 1.0f, float(kNumHarmonics));
      const int low = int(clamped), high = std::min(low + 1, kNumHarmonics);
      std::complex<float> a = frame.harmonics[low], b = frame.harmonics[high];
      if (std::abs(a) > 0.0f) a /= std::abs(a);
      if (std::abs(b) > 0.0f) b /= std::abs(b);
      const std::complex<float> z = a + (b - a) * (clamped - low);
      const float phase = (std::abs(z) > 0.0f ? std::arg(z) : 0.0f) + 2.0f * kPi * q / 8.0f - kPi / 2.0f;
      complex_a_[k] = k == 0 || k == half ? std::complex<float>(grid_[k]) : std::polar(grid_[k], phase);
      if (k > 0 && k < half) complex_a_[n - k] = std::conj(complex_a_[k]);
    }
    finishKernel(std::isfinite(p.offset_override) ? p.offset_override : kOriginalOffset, size_ / 2, kernel);
    smooth(kernel);
    fade(kernel, true, true);
  }

  void KernelBuilder::designRaw(const Frame& frame, const Parameters& p, float* kernel) {
    // One period of the tilted waveform with its phases, plus the DC term that
    // keeps the response below harmonic 1 at the harmonic-1 level, time-scaled
    // from the native period by linear interpolation.
    std::fill(period_.begin(), period_.end(), 0.0f);
    period_[0] = magnitude_[1];
    for (int n = 1; n <= kNumHarmonics; ++n) {
      const float amplitude = std::abs(frame.harmonics[n]);
      if (magnitude_[n] <= 0.0f || amplitude <= 0.0f) continue;
      const std::complex<float> value = frame.harmonics[n] * (magnitude_[n] / amplitude);
      period_[2 * n] = value.real(); period_[2 * n + 1] = value.imag();
    }
    // JUCE's real inverse divides by N; cosine amplitudes need N / 2 to come
    // back as written, and one period of a kernel carries 2 / period.
    frame_fft_.performRealOnlyInverseTransform(period_.data());
    const float ratio = std::max(p.cutoff_hz, 1.0f) / referenceCutoff(p.sample_rate);   // native period / period
    const float period = float(kReferencePeriod) / ratio;
    const float scale = float(kFrameSize) / 2.0f * 2.0f / period;
    const int taps = std::min(size_, int(std::ceil(period)));
    for (int t = 0; t < size_; ++t) {
      if (t >= taps) { kernel[t] = 0.0f; continue; }
      const float position = (float(t) - (std::isfinite(p.offset_override) ? p.offset_override : kRawOffset)) * ratio;
      const int index = int(std::floor(position));
      const float fraction = position - index;
      const float a = period_[index & (kFrameSize - 1)], b = period_[(index + 1) & (kFrameSize - 1)];
      kernel[t] = (a + (b - a) * fraction) * scale;
    }
    if (taps >= size_) fade(kernel, false, true);
  }

  void KernelBuilder::smooth(float* kernel) const {
    // [0.1 0.8 0.1] centred: the measured high-frequency roll-off.
    float before = 0.0f;
    for (int t = 0; t < size_; ++t) {
      const float current = kernel[t];
      const float next = t + 1 < size_ ? kernel[t + 1] : 0.0f;
      kernel[t] = kSmoothSide * before + kSmoothCentre * current + kSmoothSide * next;
      before = current;
    }
  }

  void KernelBuilder::fade(float* kernel, bool start, bool end) const {
    const int n = std::min(kFadeSamples, size_ / 4);
    for (int i = 0; i < n; ++i) {
      const float gain = 0.5f - 0.5f * std::cos(kPi * (i + 0.5f) / n);
      if (start) kernel[i] *= gain;
      if (end) kernel[size_ - 1 - i] *= gain;
    }
  }

  int KernelBuilder::build(const Frame& frame, const Parameters& p, float* kernel) {
    tiltedMagnitudes(frame, p);
    switch (p.mode) {
      case kLinear: designLinear(p, kernel); break;
      case kOriginal: designOriginal(frame, p, kernel); break;
      case kRaw: designRaw(frame, p, kernel); break;
      case kMinimum: default: designMinimum(p, kernel); break;
    }
    return latencyForMode(p.mode, size_);
  }
}
