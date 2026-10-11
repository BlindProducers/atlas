// SPDX-License-Identifier: GPL-3.0-or-later
#include "reverb_filter.h"
#include <algorithm>
#include <cmath>

namespace vital {
ReverbFilter::ReverbFilter() : Processor(kNumInputs, 1) { prepare(kDefaultSampleRate); }
ReverbFilter::ReverbFilter(const ReverbFilter& other) : Processor(other) { prepare(other.prepared_rate_); }
void ReverbFilter::prepare(int sample_rate) {
  prepared_rate_ = std::max(sample_rate, prepared_rate_);
  for (auto& lane : lanes_) lane.reserve(double(prepared_rate_) * kMaxOversample);
}
void ReverbFilter::setSampleRate(int sample_rate) {
  Processor::setSampleRate(sample_rate);
  prepare(sample_rate);
}
void ReverbFilter::reset(poly_mask mask) {
  for (int lane = 0; lane < poly_float::kSize; ++lane)
    if (mask[lane]) { lanes_[lane].reset(); bass_[lane].reset(); }
}
void ReverbFilter::process(int num_samples) {
  if (num_samples <= 0) return;
  const auto* resets = input(kReset)->source;
  const poly_mask reset_mask = getResetMask(kReset) & resets->trigger_mask;
  const poly_float resonance = input(kResonance)->at(0), damp = input(kDamp)->at(0);
  const poly_float bass_amount = input(kBassPreserve)->at(0);
  for (int lane = 0; lane < poly_float::kSize; ++lane) {
    auto& core = lanes_[lane];
    auto& bass = bass_[lane];
    core.setSampleRate(getSampleRate(), getOversampleAmount());
    bass.setSampleRate(getSampleRate(), getOversampleAmount());
    core.beginBlock(resonance[lane], damp[lane], num_samples);
    bass.beginBlock(bass_amount[lane], num_samples);
    for (int i = 0; i < num_samples; ++i) {
      if (reset_mask[lane] && i == resets->trigger_offset[lane]) {
        core.reset();
        core.beginBlock(resonance[lane], damp[lane], num_samples - i);
        bass.reset();
        bass.beginBlock(bass_amount[lane], num_samples - i);
      }
      const double pitch = input(kCutoffLog)->at(i)[lane] + input(kKeytrack)->at(i)[lane] / 12.0;
      const double cutoff = std::exp2(std::clamp(pitch, 3.0, 14.28771237954945));
      const double dry = input(kAudio)->at(i)[lane];
      output()->buffer[i].set(lane, static_cast<float>(bass.tick(dry, core.tick(dry, cutoff))));
    }
  }
}
}
