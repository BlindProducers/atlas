// SPDX-License-Identifier: GPL-3.0-or-later
#include "reverb_filter.h"
#include "prepared_effect_module.h"
#include <algorithm>
#include <cmath>
#include <new>

namespace vital {
ReverbFilter::ReverbFilter() : Processor(kNumInputs, 1), shared_(std::make_shared<Shared>()) {
  std::lock_guard<std::mutex> lock(shared_->mutex);
  shared_->filters.push_back(this);
  prepare(kDefaultSampleRate);
}
ReverbFilter::ReverbFilter(const ReverbFilter& other) : Processor(other), shared_(other.shared_) {
  std::lock_guard<std::mutex> lock(shared_->mutex);
  shared_->filters.push_back(this);
  prepare(other.prepared_rate_);
}
ReverbFilter::~ReverbFilter() {
  std::lock_guard<std::mutex> lock(shared_->mutex);
  auto& filters = shared_->filters;
  filters.erase(std::remove(filters.begin(), filters.end(), this), filters.end());
}
void ReverbFilter::prepare(int sample_rate) {
  prepared_rate_ = std::max(sample_rate, prepared_rate_);
  if (shared_->reserved) reserveLanes();
}
void ReverbFilter::reserveLanes() {
  try {
    for (auto& lane : lanes_) lane.reserve(double(prepared_rate_) * kMaxOversample);
    has_memory_ = true;
  }
  catch (const std::bad_alloc&) {
    // Out of memory: the filter passes audio through instead.
    releaseLanes();
  }
}
void ReverbFilter::releaseLanes() {
  for (auto& lane : lanes_) lane.release();
  has_memory_ = false;
}
void ReverbFilter::reserveMemory() {
  std::lock_guard<std::mutex> lock(shared_->mutex);
  shared_->reserved = true;
  for (ReverbFilter* filter : shared_->filters) filter->reserveLanes();
}
void ReverbFilter::releaseMemory() {
  std::lock_guard<std::mutex> lock(shared_->mutex);
  shared_->reserved = false;
  for (ReverbFilter* filter : shared_->filters) filter->releaseLanes();
}
void ReverbFilter::setSampleRate(int sample_rate) {
  Processor::setSampleRate(sample_rate);
  std::lock_guard<std::mutex> lock(shared_->mutex);
  prepare(sample_rate);
}
void ReverbFilter::reset(poly_mask mask) {
  for (int lane = 0; lane < poly_float::kSize; ++lane)
    if (mask[lane]) { lanes_[lane].reset(); bass_[lane].reset(); }
}
void ReverbFilter::process(int num_samples) {
  if (num_samples <= 0) return;
  if (!has_memory_) {
    // Offline, reserve now, as the filter used to at construction. In
    // realtime, pass the audio through until the message thread has.
    if (!RealtimeEffectScope::active()) reserveMemory();
    if (!has_memory_) {
      shared_->requested.store(true, std::memory_order_release);
      utils::copyBuffer(output()->buffer, input(kAudio)->source->buffer, num_samples);
      return;
    }
  }
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
    // The cutoff only changes when it is modulated or glides. (No NaN marks
    // the first sample: this unit is built with fast math, which assumes no
    // NaNs and may compare them wrongly.)
    bool have_cutoff = false;
    double last_pitch = 0, cutoff = 0;
    for (int i = 0; i < num_samples; ++i) {
      if (reset_mask[lane] && i == resets->trigger_offset[lane]) {
        core.reset();
        core.beginBlock(resonance[lane], damp[lane], num_samples - i);
        bass.reset();
        bass.beginBlock(bass_amount[lane], num_samples - i);
      }
      const double pitch = input(kCutoffLog)->at(i)[lane] + input(kKeytrack)->at(i)[lane] / 12.0;
      if (!have_cutoff || pitch != last_pitch) {
        have_cutoff = true;
        last_pitch = pitch;
        cutoff = std::exp2(std::clamp(pitch, 3.0, 14.28771237954945));
      }
      const double dry = input(kAudio)->at(i)[lane];
      output()->buffer[i].set(lane, static_cast<float>(bass.tick(dry, core.tick(dry, cutoff))));
    }
  }
}
}
