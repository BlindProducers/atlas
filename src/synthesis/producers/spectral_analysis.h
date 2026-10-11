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

      int numFrames() const noexcept { return static_cast<int>(frames_.size()); }
      int numSourceSamples() const noexcept { return source_samples_; }
      double sourceSampleRate() const noexcept { return source_sample_rate_; }
      float globalMaxMagnitude() const noexcept { return global_max_; }

    private:
      std::vector<Frame> frames_;
      int source_samples_ = 0;
      double source_sample_rate_ = 48000.0;
      float global_max_ = 0.0f;
      juce::dsp::FFT fft_;

      JUCE_LEAK_DETECTOR(SpectralAnalysis)
  };
} // namespace vital
