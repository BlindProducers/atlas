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

#include "spectral_analysis.h"

#include "futils.h"

#include <algorithm>
#include <cmath>

namespace vital {

  SpectralAnalysis::SpectralAnalysis() = default;

  SpectralAnalysis::SpectralAnalysis(const SpectralAnalysis& other) :
      frames_(other.frames_), source_samples_(other.source_samples_),
      source_sample_rate_(other.source_sample_rate_), global_max_(other.global_max_) { }

  void SpectralAnalysis::clear() noexcept {
    frames_.clear();
    source_samples_ = 0;
    global_max_ = 0.0f;
  }

  bool SpectralAnalysis::analyzeChannel(const float* source, int total_samples, double sample_rate,
                                        const std::function<bool()>& should_cancel) {
    clear();
    source_sample_rate_ = sample_rate;

    if (source == nullptr || total_samples <= 0 || !futils::isFinite(sample_rate) || sample_rate <= 0)
      return false;

    const int num_frames = 1 + (std::max(0, total_samples - kFftSize) + kHop - 1) / kHop;
    if (num_frames <= 0)
      return false;

    source_samples_ = total_samples;
    frames_.reserve(static_cast<size_t>(num_frames));

    std::vector<float> window(static_cast<size_t>(kFftSize));
    for (int i = 0; i < kFftSize; ++i) {
      window[static_cast<size_t>(i)] = 0.5f - 0.5f * std::cos(juce::MathConstants<float>::twoPi *
                                                              i / kFftSize);
    }

    std::vector<float> work(static_cast<size_t>(kFftSize * 2), 0.0f);
    std::vector<float> previous_phase(static_cast<size_t>(kNumBins), 0.0f);
    const float expected_delta_bin_1 = juce::MathConstants<float>::twoPi * kHop / kFftSize;

    for (int frame_index = 0; frame_index < num_frames; ++frame_index) {
      if ((frame_index & 31) == 0 && should_cancel && should_cancel()) {
        clear();
        return false;
      }
      std::fill(work.begin(), work.end(), 0.0f);
      const int start = frame_index * kHop;
      // Centre the window at FFT time zero. Fractional-bin interpolation then
      // combines aligned lobes instead of cancelling alternating complex bins.
      for (int i = 0; i < kFftSize && start + i < total_samples; ++i)
        work[static_cast<size_t>((i + kFftSize / 2) % kFftSize)] =
            source[start + i] * window[static_cast<size_t>(i)];

      fft_.performRealOnlyForwardTransform(work.data());

      Frame frame;
      frame.magnitude.resize(static_cast<size_t>(kNumBins));
      frame.true_freq.resize(static_cast<size_t>(kNumBins));
      frame.phase.resize(static_cast<size_t>(kNumBins));

      for (int bin = 0; bin < kNumBins; ++bin) {
        const float real = work[static_cast<size_t>(2 * bin)];
        const float imag = work[static_cast<size_t>(2 * bin + 1)];
        const float magnitude = std::sqrt(real * real + imag * imag);
        const float phase = std::atan2(imag, real);

        const float expected = bin * expected_delta_bin_1;
        float delta = phase - previous_phase[static_cast<size_t>(bin)] - expected;
        delta -= juce::MathConstants<float>::twoPi *
                 std::round(delta / juce::MathConstants<float>::twoPi);

        frame.magnitude[static_cast<size_t>(bin)] = magnitude;
        frame.true_freq[static_cast<size_t>(bin)] =
            juce::MathConstants<float>::twoPi * bin / kFftSize + delta / kHop;
        frame.phase[static_cast<size_t>(bin)] = phase;
        previous_phase[static_cast<size_t>(bin)] = phase;
      }
      frame.magnitude[0] = 0.0f;
      frames_.push_back(std::move(frame));
    }

    // There is no preceding phase observation for frame zero. Its arbitrary
    // phase relative to zero is not a frequency measurement.
    if (frames_.size() > 1)
      frames_[0].true_freq = frames_[1].true_freq;
    else {
      for (int bin = 0; bin < kNumBins; ++bin)
        frames_[0].true_freq[static_cast<size_t>(bin)] =
            juce::MathConstants<float>::twoPi * bin / kFftSize;
    }

    float max_magnitude = 1.0e-9f;
    for (const Frame& frame : frames_) {
      for (float magnitude : frame.magnitude)
        max_magnitude = std::max(max_magnitude, magnitude);
    }

    const float ceiling = kFftSize / 2.0f;
    if (max_magnitude > ceiling) {
      const float scale = ceiling / max_magnitude;
      for (Frame& frame : frames_) {
        for (float& magnitude : frame.magnitude)
          magnitude *= scale;
      }
      max_magnitude = ceiling;
    }
    global_max_ = max_magnitude;

    if (frames_.size() < 3)
      return true;

    std::vector<float> flux(frames_.size(), 0.0f);
    for (size_t i = 1; i < frames_.size(); ++i) {
      const std::vector<float>& current = frames_[i].magnitude;
      const std::vector<float>& previous = frames_[i - 1].magnitude;
      for (size_t bin = 1; bin < current.size(); ++bin) {
        const float delta = current[bin] - previous[bin];
        if (delta > 0.0f)
          flux[i] += delta;
      }
    }

    float mean = 0.0f;
    for (float value : flux)
      mean += value;
    mean /= static_cast<float>(flux.size());

    const float threshold = mean * 1.6f + 1.0e-3f;
    for (size_t i = 1; i + 1 < frames_.size(); ++i) {
      if (flux[i] > threshold && flux[i] > flux[i - 1] && flux[i] >= flux[i + 1])
        frames_[i].transient = true;
    }
    return true;
  }

  void SpectralAnalysis::readFrameInterpolated(double position, float* out_mag, float* out_freq,
                                               float* out_phase, bool* transient) const noexcept {
    const int num_frames = static_cast<int>(frames_.size());
    if (num_frames == 0) {
      for (int bin = 0; bin < kNumBins; ++bin) {
        out_mag[bin] = 0.0f;
        out_freq[bin] = 0.0f;
        if (out_phase)
          out_phase[bin] = 0.0f;
      }
      if (transient)
        *transient = false;
      return;
    }

    if (num_frames == 1) {
      const Frame& frame = frames_[0];
      for (int bin = 0; bin < kNumBins; ++bin) {
        out_mag[bin] = frame.magnitude[static_cast<size_t>(bin)];
        out_freq[bin] = frame.true_freq[static_cast<size_t>(bin)];
        if (out_phase)
          out_phase[bin] = frame.phase[static_cast<size_t>(bin)];
      }
      if (transient)
        *transient = frame.transient;
      return;
    }

    const double clamped = juce::jlimit(0.0, num_frames - 1.0, position);
    const int frame_0_index = static_cast<int>(std::floor(clamped));
    const int frame_1_index = std::min(frame_0_index + 1, num_frames - 1);
    const float t = static_cast<float>(clamped - frame_0_index);
    const int closest_index = t < 0.5f ? frame_0_index : frame_1_index;

    const Frame& frame_0 = frames_[static_cast<size_t>(frame_0_index)];
    const Frame& frame_1 = frames_[static_cast<size_t>(frame_1_index)];
    const Frame& closest = frames_[static_cast<size_t>(closest_index)];

    for (int bin = 0; bin < kNumBins; ++bin) {
      const float magnitude_0 = frame_0.magnitude[static_cast<size_t>(bin)];
      const float magnitude_1 = frame_1.magnitude[static_cast<size_t>(bin)];
      out_mag[bin] = magnitude_0 + t * (magnitude_1 - magnitude_0);
      const float freq_0 = frame_0.true_freq[static_cast<size_t>(bin)];
      const float freq_1 = frame_1.true_freq[static_cast<size_t>(bin)];
      out_freq[bin] = freq_0 + t * (freq_1 - freq_0);
      if (out_phase) {
        const float expected = freq_1 * kHop;
        const float delta = expected + std::remainder(frame_1.phase[static_cast<size_t>(bin)] -
            frame_0.phase[static_cast<size_t>(bin)] - expected, juce::MathConstants<float>::twoPi);
        out_phase[bin] = std::remainder(frame_0.phase[static_cast<size_t>(bin)] + t * delta,
                                      juce::MathConstants<float>::twoPi);
      }
    }
    if (transient)
      *transient = closest.transient;
  }
} // namespace vital
