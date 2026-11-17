// SPDX-License-Identifier: GPL-3.0-or-later
#include "sample_slices.h"
#include "sample_source.h"

#include <algorithm>
#include <cmath>

namespace vital::sample_slices {
  namespace {
    constexpr int kHopSamples = 256;          // one level measurement per hop
    constexpr float kMinGapSeconds = 0.05f;   // two onsets closer than this are one
    constexpr float kMinRiseDb = 3.0f;        // the rise that counts at full sensitivity
    constexpr float kMaxRiseDb = 18.0f;       // ... and at none
    constexpr float kLeadInSeconds = 0.05f;   // an onset this close to the start is the start

    float toDb(float energy) { return 10.0f * std::log10(energy + 1.0e-10f); }
  }

  std::vector<int> grid(int length, int divisions) {
    std::vector<int> starts;
    divisions = std::max(1, std::min({ divisions, Sample::kMaxSlices, length / Sample::kMinSliceLength }));
    starts.reserve(static_cast<size_t>(divisions));
    for (int i = 0; i < divisions; ++i)
      starts.push_back(static_cast<int>(static_cast<int64_t>(length) * i / divisions));
    return starts;
  }

  std::vector<int> transients(const mono_float* left, const mono_float* right, int length, int sample_rate,
                              float sensitivity) {
    std::vector<int> starts { 0 };
    if (left == nullptr || length < 2 * kHopSamples || sample_rate <= 0)
      return starts;

    // The level of each hop, in dB.
    const int hops = length / kHopSamples;
    std::vector<float> level(static_cast<size_t>(hops));
    for (int hop = 0; hop < hops; ++hop) {
      float energy = 0.0f;
      const int begin = hop * kHopSamples;
      for (int i = begin; i < begin + kHopSamples; ++i) {
        const float sample = right != nullptr ? 0.5f * (left[i] + right[i]) : left[i];
        energy += sample * sample;
      }
      level[static_cast<size_t>(hop)] = toDb(energy / kHopSamples);
    }

    // An onset is a hop whose level rises over the quietest of the two before
    // it by more than the threshold, and more than its neighbours' rise.
    const float clamped = std::max(0.0f, std::min(1.0f, sensitivity));
    const float threshold = kMaxRiseDb - (kMaxRiseDb - kMinRiseDb) * clamped;
    const int min_gap = static_cast<int>(kMinGapSeconds * sample_rate);
    const int lead_in = static_cast<int>(kLeadInSeconds * sample_rate);
    std::vector<float> rise(static_cast<size_t>(hops), 0.0f);
    for (int hop = 2; hop < hops; ++hop) {
      const float before = std::min(level[static_cast<size_t>(hop - 1)], level[static_cast<size_t>(hop - 2)]);
      rise[static_cast<size_t>(hop)] = level[static_cast<size_t>(hop)] - before;
    }
    int last_onset = -min_gap;
    for (int hop = 2; hop < hops && static_cast<int>(starts.size()) < Sample::kMaxSlices; ++hop) {
      const float value = rise[static_cast<size_t>(hop)];
      if (value < threshold)
        continue;
      if (value < rise[static_cast<size_t>(hop - 1)] || (hop + 1 < hops && value <= rise[static_cast<size_t>(hop + 1)]))
        continue;
      // The slice starts where this hop's attack begins: the first sample of
      // the hop before it that reaches a tenth of the peak around the onset.
      const int window_start = (hop - 1) * kHopSamples;
      const int window_end = std::min(length, (hop + 1) * kHopSamples);
      float peak = 0.0f;
      for (int i = window_start; i < window_end; ++i)
        peak = std::max(peak, std::fabs(right != nullptr ? 0.5f * (left[i] + right[i]) : left[i]));
      int onset = hop * kHopSamples;
      for (int i = window_start; i < window_end; ++i) {
        const float sample = std::fabs(right != nullptr ? 0.5f * (left[i] + right[i]) : left[i]);
        if (sample >= 0.1f * peak) {
          onset = i;
          break;
        }
      }
      if (onset - last_onset < min_gap)
        continue;
      last_onset = onset;
      if (onset >= lead_in)
        starts.push_back(onset);
    }
    return starts;
  }
} // namespace vital::sample_slices
