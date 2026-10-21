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
ReverbFilter::ReverbFilter(const ReverbFilter& other) :
    Processor(other), shared_(other.shared_), fx_mode_(other.fx_mode_) {
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
    core_.reserve(double(prepared_rate_) * kMaxOversample);
    has_memory_ = true;
  }
  catch (const std::bad_alloc&) {
    // Out of memory: the filter passes audio through instead.
    releaseLanes();
  }
}
void ReverbFilter::releaseLanes() {
  core_.release();
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
    if (mask[lane]) { core_.resetLane(lane); bass_.resetLane(lane); }
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
  const int pairs = fx_mode_ ? 1 : poly_float::kSize / 2;
  core_.setSampleRate(getSampleRate(), getOversampleAmount());
  core_.setPairs(pairs);
  bass_.setSampleRate(getSampleRate(), getOversampleAmount());
  int reset_offsets[poly_float::kSize];
  for (int lane = 0; lane < poly_float::kSize; ++lane) {
    core_.beginBlock(lane, resonance[lane], damp[lane], num_samples);
    bass_.beginBlock(lane, bass_amount[lane], num_samples);
    reset_offsets[lane] = reset_mask[lane] ? resets->trigger_offset[lane] : -1;
  }
  // The cutoff only changes when it is modulated or glides. (No NaN marks
  // the first sample: this unit is built with fast math, which assumes no
  // NaNs and may compare them wrongly.)
  bool have_cutoff = false;
  double last_pitch[poly_float::kSize] {};
  alignas(16) double cutoff[poly_float::kSize] {}, dry[poly_float::kSize], wet[poly_float::kSize];
  alignas(16) double mixed[poly_float::kSize];
  const poly_float* audio = input(kAudio)->source->buffer;
  const poly_float* cutoff_log = input(kCutoffLog)->source->buffer;
  const poly_float* keytrack = input(kKeytrack)->source->buffer;
  poly_float* out = output()->buffer;
  for (int i = 0; i < num_samples; ++i) {
    for (int lane = 0; lane < poly_float::kSize; ++lane) {
      if (i == reset_offsets[lane]) {
        core_.resetLane(lane);
        core_.beginBlock(lane, resonance[lane], damp[lane], num_samples - i);
        bass_.resetLane(lane);
        bass_.beginBlock(lane, bass_amount[lane], num_samples - i);
      }
      const double pitch = cutoff_log[i][lane] + keytrack[i][lane] / 12.0;
      if (!have_cutoff || pitch != last_pitch[lane]) {
        last_pitch[lane] = pitch;
        cutoff[lane] = std::exp2(std::clamp(pitch, 3.0, 14.28771237954945));
      }
    }
    have_cutoff = true;
    double_pair low, high;
    double_pair::fromFloats(audio[i], low, high);
    low.store(dry);
    high.store(dry + 2);
    core_.tick(dry, cutoff, wet);
    bass_.tick(dry, wet, mixed, pairs);
    out[i] = double_pair::toFloats(double_pair::load(mixed), double_pair::load(mixed + 2));
  }
}
}
