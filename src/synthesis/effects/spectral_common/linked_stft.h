// SPDX-License-Identifier: GPL-3.0-or-later
// A linked-stereo STFT gain stage shared by Atlas's spectral dynamics effects.
// Hann analysis and synthesis windows at 75% overlap; the sum of both channels
// is analysed and one gain curve per frame is applied to both. The input ring
// doubles as a dry path delayed by exactly the FFT size, so the reported
// latency is kFftSize and a dry/wet mix or a delta output stays aligned.
#pragma once
#include "atlas_fft.h"
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
        processStftRuns(in_left, in_right, out_left, out_right, count, [&](auto run) { run(combine); });
      }

      // Like processStft, but the samples go in runs up to the next hop and
      // `begin_run(run)` is called before each one; it calls `run(combine)`
      // once, so it can pick a cheaper combine while its controls hold still.
      template<class BeginRun>
      void processStftRuns(const float* in_left, const float* in_right, float* out_left, float* out_right,
                           int count, BeginRun begin_run) {
        for (int offset = 0; offset < count;) {
          // position_ and hop_position_ wrap together, so up to the next hop
          // the rings are contiguous. The input ring doubles as the dry path.
          const int span = std::min(count - offset, kHopSize - hop_position_);
          float* const dry_left = in_left_.data() + position_;
          float* const dry_right = in_right_.data() + position_;
          float* const wet_left = out_left_.data() + position_;
          float* const wet_right = out_right_.data() + position_;
          const float* const input_left = in_left + offset;
          const float* const input_right = in_right + offset;
          float* const output_left = out_left + offset;
          float* const output_right = out_right + offset;
          begin_run([&](auto combine) {
            for (int i = 0; i < span; ++i) {
              const float dl = dry_left[i], dr = dry_right[i], wl = wet_left[i], wr = wet_right[i];
              dry_left[i] = input_left[i];
              dry_right[i] = input_right[i];
              wet_left[i] = 0.0f;
              wet_right[i] = 0.0f;
              combine(dl, dr, wl, wr, output_left[i], output_right[i]);
            }
          });
          offset += span;
          position_ = (position_ + span) & (kFftSize - 1);
          hop_position_ += span;
          if (hop_position_ == kHopSize) {
            hop_position_ = 0;
            processFrame();
          }
        }
      }

    private:
      void processFrame() {
        // The ring from position_ to its end, then from its start.
        const int tail = kFftSize - position_;
        for (int i = 0; i < tail; ++i) {
          fft_left_[i] = in_left_[position_ + i] * window_[i];
          fft_right_[i] = in_right_[position_ + i] * window_[i];
        }
        for (int i = tail; i < kFftSize; ++i) {
          fft_left_[i] = in_left_[i - tail] * window_[i];
          fft_right_[i] = in_right_[i - tail] * window_[i];
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
        for (int i = 0; i < tail; ++i) {
          const float scale = window_[i] * kOverlapScale;
          out_left_[position_ + i] += fft_left_[i] * scale;
          out_right_[position_ + i] += fft_right_[i] * scale;
        }
        for (int i = tail; i < kFftSize; ++i) {
          const float scale = window_[i] * kOverlapScale;
          out_left_[i - tail] += fft_left_[i] * scale;
          out_right_[i - tail] += fft_right_[i] * scale;
        }
      }

      double sample_rate_ = 48000.0;
      atlas::Fft fft_ { Order };
      std::array<float, kFftSize> window_ {};
      std::array<float, kNumBins> bin_hz_ {};
      alignas(16) std::array<float, 2 * kFftSize> fft_left_ {};
      alignas(16) std::array<float, 2 * kFftSize> fft_right_ {};
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
