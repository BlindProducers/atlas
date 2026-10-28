/* Copyright 2013-2019 Matt Tytel
 *
 * vital is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * vital is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with vital.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "JuceHeader.h"
#include "spectral_common/shared_fft.h"

#include <functional>
#include <vector>

namespace vital {

  class SpectralAnalysis {
    public:
      static constexpr int kFftOrder = 11;
      static constexpr int kFftSize = 1 << kFftOrder;
      static constexpr int kHop = kFftSize / 4;
      static constexpr int kNumBins = kFftSize / 2;

      struct Frame {
        std::vector<float> magnitude;
        std::vector<float> true_freq;
        std::vector<float> phase;
        bool transient = false;
      };

      SpectralAnalysis();
      SpectralAnalysis(const SpectralAnalysis& other);

      bool analyzeChannel(const float* data, int num_samples, double sample_rate,
                          const std::function<bool()>& should_cancel = {});
      void clear() noexcept;
      void readFrameInterpolated(double position, float* out_mag, float* out_freq,
                                 float* out_phase = nullptr, bool* transient = nullptr) const noexcept;

      // The frames readFrameInterpolated blends at a position, so a caller can
      // read the phases of only the bins it needs, as readFrameInterpolated
      // would give them.
      struct FramePair {
        const Frame* frame_0 = nullptr;
        const Frame* frame_1 = nullptr;
        float t = 0.0f;
      };
      FramePair framePair(double position) const noexcept;

      static float interpolatedPhase(const FramePair& frames, int bin) noexcept {
        if (frames.frame_0 == nullptr)
          return 0.0f;
        if (frames.frame_1 == nullptr)
          return frames.frame_0->phase[static_cast<size_t>(bin)];
        const float phase_0 = frames.frame_0->phase[static_cast<size_t>(bin)];
        const float expected = frames.frame_1->true_freq[static_cast<size_t>(bin)] * kHop;
        const float delta = expected + std::remainder(frames.frame_1->phase[static_cast<size_t>(bin)] -
            phase_0 - expected, juce::MathConstants<float>::twoPi);
        return std::remainder(phase_0 + frames.t * delta, juce::MathConstants<float>::twoPi);
      }

      int numFrames() const noexcept { return static_cast<int>(frames_.size()); }
      int numSourceSamples() const noexcept { return source_samples_; }
      double sourceSampleRate() const noexcept { return source_sample_rate_; }
      float globalMaxMagnitude() const noexcept { return global_max_; }

    private:
      std::vector<Frame> frames_;
      int source_samples_ = 0;
      double source_sample_rate_ = 48000.0;
      float global_max_ = 0.0f;
      std::shared_ptr<const juce::dsp::FFT> fft_;

      JUCE_LEAK_DETECTOR(SpectralAnalysis)
  };
} // namespace vital
