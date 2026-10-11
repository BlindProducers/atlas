// SPDX-License-Identifier: GPL-3.0-or-later
// A linked-stereo STFT gain stage shared by Atlas's spectral dynamics effects.
// Hann analysis and synthesis windows at 75% overlap; the sum of both channels
// is analysed and one gain curve per frame is applied to both. The input ring
// doubles as a dry path delayed by exactly the FFT size, so the reported
// latency is kFftSize and a dry/wet mix or a delta output stays aligned.
#pragma once
#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <array>
#include <cmath>

namespace atlas::stft {
  inline float powerToDb(float power) { return 10.0f * std::log10(power + 1.0e-14f); }
  inline float dbToGain(float db) { return std::exp(db * 0.11512925465f); }
  inline float smoothingCoefficient(float step_seconds, float tau_seconds) {
    if (tau_seconds <= 0.0f)
      return 1.0f;
    return 1.0f - std::exp(-step_seconds / tau_seconds);
  }

  template<int Order>
  class LinkedGainStft {
    public:
      static constexpr int kFftOrder = Order;
      static constexpr int kFftSize = 1 << Order;
      static constexpr int kHopSize = kFftSize / 4;
      static constexpr int kNumBins = kFftSize / 2 + 1;
      static constexpr int kLatencySamples = kFftSize;

      LinkedGainStft() {
        for (int i = 0; i < kFftSize; ++i)
          window_[i] = 0.5f - 0.5f * std::cos(2.0f * 3.14159265358979323846f * i / kFftSize);
        gain_.fill(1.0f);
        power_.fill(0.0f);
      }
      virtual ~LinkedGainStft() = default;

      void prepare(double sample_rate) {
        sample_rate_ = sample_rate > 1000.0 ? sample_rate : 48000.0;
        for (int bin = 0; bin < kNumBins; ++bin)
          bin_hz_[bin] = float(bin * sample_rate_ / kFftSize);
        resetStft();
      }
      double sampleRate() const { return sample_rate_; }
      float binFrequency(int bin) const { return bin_hz_[std::clamp(bin, 0, kNumBins - 1)]; }
      float hopSeconds() const { return float(kHopSize) / float(sample_rate_); }
      // The gains applied to the most recent frame (linear).
      const std::array<float, kNumBins>& gains() const { return gain_; }
      // Power of the sum in the most recent frame; a full-scale sine reads 1 (0 dBFS).
      const std::array<float, kNumBins>& power() const { return power_; }

    protected:
      void resetStft() {
        in_left_.fill(0.0f); in_right_.fill(0.0f);
        out_left_.fill(0.0f); out_right_.fill(0.0f);
        gain_.fill(1.0f);
        power_.fill(0.0f);
        position_ = 0;
        hop_position_ = 0;
      }

      // Fills `gain` (linear, per bin) from `power` for the frame about to be
      // resynthesised. Called once per hop.
      virtual void updateGains(const std::array<float, kNumBins>& power, std::array<float, kNumBins>& gain) = 0;

      // `combine(dry_l, dry_r, wet_l, wet_r, out_l, out_r)` mixes the aligned
      // dry and processed samples.
      template<class Combine>
      void processStft(const float* in_left, const float* in_right, float* out_left, float* out_right,
                       int count, Combine combine) {
        for (int i = 0; i < count; ++i) {
          const float dry_left = in_left_[position_], dry_right = in_right_[position_];
          const float wet_left = out_left_[position_], wet_right = out_right_[position_];
          in_left_[position_] = in_left[i];
          in_right_[position_] = in_right[i];
          out_left_[position_] = 0.0f;
          out_right_[position_] = 0.0f;
          combine(dry_left, dry_right, wet_left, wet_right, out_left[i], out_right[i]);
          position_ = (position_ + 1) & (kFftSize - 1);
          if (++hop_position_ == kHopSize) {
            hop_position_ = 0;
            processFrame();
          }
        }
      }

    private:
      void processFrame() {
        for (int i = 0; i < kFftSize; ++i) {
          const int index = (position_ + i) & (kFftSize - 1);
          fft_left_[i] = in_left_[index] * window_[i];
          fft_right_[i] = in_right_[index] * window_[i];
        }
        fft_.performRealOnlyForwardTransform(fft_left_.data(), true);
        fft_.performRealOnlyForwardTransform(fft_right_.data(), true);
        // |X| * 2 / N is a Hann-windowed sine's amplitude times 0.5 (the
        // window's coherent gain), so the sum of two identical channels reads 1.
        constexpr float kNorm = 2.0f / kFftSize;
        for (int bin = 0; bin < kNumBins; ++bin) {
          const float real = (fft_left_[2 * bin] + fft_right_[2 * bin]) * kNorm;
          const float imaginary = (fft_left_[2 * bin + 1] + fft_right_[2 * bin + 1]) * kNorm;
          power_[bin] = real * real + imaginary * imaginary;
        }
        updateGains(power_, gain_);
        for (int bin = 0; bin < kNumBins; ++bin) {
          const float gain = gain_[bin];
          fft_left_[2 * bin] *= gain; fft_left_[2 * bin + 1] *= gain;
          fft_right_[2 * bin] *= gain; fft_right_[2 * bin + 1] *= gain;
        }
        fft_.performRealOnlyInverseTransform(fft_left_.data());
        fft_.performRealOnlyInverseTransform(fft_right_.data());
        // Four overlapping squared Hann windows sum to 1.5.
        constexpr float kOverlapScale = 2.0f / 3.0f;
        for (int i = 0; i < kFftSize; ++i) {
          const int index = (position_ + i) & (kFftSize - 1);
          const float scale = window_[i] * kOverlapScale;
          out_left_[index] += fft_left_[i] * scale;
          out_right_[index] += fft_right_[i] * scale;
        }
      }

      double sample_rate_ = 48000.0;
      juce::dsp::FFT fft_ { Order };
      std::array<float, kFftSize> window_ {};
      std::array<float, kNumBins> bin_hz_ {};
      std::array<float, 2 * kFftSize> fft_left_ {}, fft_right_ {};
      std::array<float, kFftSize> in_left_ {}, in_right_ {}, out_left_ {}, out_right_ {};
      std::array<float, kNumBins> gain_ {};
      std::array<float, kNumBins> power_ {};
      int position_ = 0;
      int hop_position_ = 0;
  };

  // Prefix sums over bins so a box average of any width is two lookups.
  template<int Bins>
  struct BoxAverage {
    std::array<float, Bins + 1> cumulative {};
    void load(const float* values) {
      cumulative[0] = 0.0f;
      for (int bin = 0; bin < Bins; ++bin)
        cumulative[bin + 1] = cumulative[bin] + values[bin];
    }
    float mean(int low, int high) const {
      low = std::max(low, 0);
      high = std::min(high, Bins - 1);
      if (high < low)
        return 0.0f;
      return (cumulative[high + 1] - cumulative[low]) / float(high - low + 1);
    }
  };
}
