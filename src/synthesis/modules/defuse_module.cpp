// SPDX-License-Identifier: GPL-3.0-or-later
// Diopser DSP copyright (C) 2021-2024 Robbert van der Helm.
// Port of plugins/diopser/{lib,filter,params}.rs and NIH-plug's smoothing.rs.
// Keep this outside the fast-math unity build, with FP contraction disabled.
#include "defuse_module.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace vital {
  namespace {
    // Rust f32::powi lowers to this integer-power operation, not powf(x, n).
    float defuseIntegerPower(float value, unsigned exponent) {
      float result = 1.0f;
      while (true) {
        if (exponent & 1) result *= value;
        exponent >>= 1;
        if (!exponent) return result;
        value *= value;
      }
    }
  }

  void DefuseModule::Smoother::reset(float value) {
    current = target = value;
    remaining = 0;
  }

  void DefuseModule::Smoother::setTarget(float value, float rate, bool logarithmic) {
    if (value == target) return;
    target = value;
    remaining = static_cast<int>(std::round(rate * 100.0f / 1000.0f));
    step = remaining <= 0 ? 0.0f : logarithmic
        ? static_cast<float>(std::pow(static_cast<double>(target / current), 1.0 / remaining))
        : (target - current) / static_cast<float>(remaining);
  }

  float DefuseModule::Smoother::next(int steps, bool logarithmic) {
    if (remaining <= 0) return target;
    remaining -= steps;
    if (remaining <= 0) {
      remaining = 0;
      current = target;
    }
    else if (logarithmic)
      current *= defuseIntegerPower(step, static_cast<unsigned>(steps));
    else
      current += step * static_cast<float>(steps);
    return current;
  }

  void DefuseModule::Biquad::coefficients(float rate, float frequency, float q) {
    constexpr float tau = 6.28318530717958647692f;
    const float omega = tau * (frequency / rate);
    const float cosine = std::cos(omega);
    const float alpha = std::sin(omega) / (2.0f * q);
    const float a0 = 1.0f + alpha;
    b0 = (1.0f - alpha) / a0;
    b1 = (-2.0f * cosine) / a0;
    b2 = (1.0f + alpha) / a0;
    a1 = (-2.0f * cosine) / a0;
    a2 = (1.0f - alpha) / a0;
  }

  poly_float DefuseModule::Biquad::process(poly_float input) {
    const poly_float result = b0 * input + s1;
    s1 = b1 * input - a1 * result + s2;
    s2 = b2 * input - a2 * result;
    return result;
  }

  DefuseModule::DefuseModule(std::string prefix) : SynthModule(0, 1), prefix_(std::move(prefix)) { }

  void DefuseModule::init() {
    const std::string name = prefix_ + "defuse_";
    // No Atlas smoothing: the original NIH-plug smoothers below own it.
    frequency_ = createMonoModControl(name + "frequency");
    resonance_ = createMonoModControl(name + "resonance");
    spread_ = createMonoModControl(name + "spread");
    precision_ = createMonoModControl(name + "automation_precision");
    stages_ = createBaseControl(name + "stages");
    style_ = createBaseControl(name + "spread_style");
    SynthModule::init();
  }

  void DefuseModule::hardReset() { reset_pending_ = true; }

  void DefuseModule::enable(bool on) {
    if (enabled() != on) hardReset();
    SynthModule::enable(on);
  }

  void DefuseModule::setSampleRate(int rate) {
    const int previous = getSampleRate();
    SynthModule::setSampleRate(rate);
    if (getSampleRate() != previous) hardReset();
  }

  void DefuseModule::setOversampleAmount(int amount) {
    const int previous = getSampleRate();
    SynthModule::setOversampleAmount(amount);
    if (getSampleRate() != previous) hardReset();
  }

  void DefuseModule::updateFilters(int interval, bool reset) {
    const float rate = static_cast<float>(getSampleRate());
    const float frequency = frequency_smoother_.next(interval, true);
    const float resonance = resonance_smoother_.next(interval, true);
    const float spread = spread_smoother_.next(interval, false);
    const float max_octave_spread = spread >= 0.0f
        ? frequency - (frequency * std::pow(2.0f, -spread))
        : (frequency * std::pow(2.0f, spread)) - frequency;
    for (int index = 0; index < stage_count_; ++index) {
      // The source divides by N, deliberately NOT N-1.
      const float proportion = (static_cast<float>(index) / static_cast<float>(stage_count_)) * 2.0f - 1.0f;
      const float cutoff = spread_style_ == 0
          ? frequency * std::pow(2.0f, spread * proportion)
          : frequency + (max_octave_spread * proportion);
      filters_[index].coefficients(rate, std::clamp(cutoff, kMinimumFrequency, rate / 2.05f), resonance);
      if (reset) filters_[index].s1 = filters_[index].s2 = poly_float(0.0f);
    }
  }

  void DefuseModule::processWithInput(const poly_float* input, int samples) {
    if (samples <= 0) return;
    SynthModule::process(samples);
    const float rate = static_cast<float>(getSampleRate());
    const float frequency = std::clamp(frequency_->buffer[0][0], kMinimumFrequency, 20000.0f);
    const float resonance = std::clamp(resonance_->buffer[0][0], 0.01f, 30.0f);
    const float spread = std::clamp(spread_->buffer[0][0], -5.0f, 5.0f);
    const int stages = std::clamp(static_cast<int>(stages_->value()), 0, 512);
    const int style = std::clamp(static_cast<int>(style_->value()), 0, 1);
    const int interval = 512 - static_cast<int>(std::round(
        std::clamp(precision_->buffer[0][0], 0.0f, 1.0f) * 511.0f));
    bool reset_filters = reset_pending_ || stages != stage_count_ || style != spread_style_;
    stage_count_ = stages;
    spread_style_ = style;
    if (reset_pending_) {
      frequency_smoother_.reset(frequency);
      resonance_smoother_.reset(resonance);
      spread_smoother_.reset(spread);
    }
    else {
      frequency_smoother_.setTarget(frequency, rate, true);
      resonance_smoother_.setTarget(resonance, rate, true);
      spread_smoother_.setTarget(spread, rate, false);
    }
    reset_pending_ = false;
    // Atlas owns bypass through the slot's Enabled switch. Process only the
    // wet cascade here; notes must not reset, crossfade, or reweight its tail.
    poly_float* output_audio = output()->buffer;
    for (int sample = 0; sample < samples; ++sample) {
      const bool smoothing = frequency_smoother_.remaining > 0 || resonance_smoother_.remaining > 0 ||
                             spread_smoother_.remaining > 0;
      if (reset_filters || (smoothing && next_filter_smoothing_in_ <= 1)) {
        updateFilters(interval, reset_filters);
        reset_filters = false;
        next_filter_smoothing_in_ = interval;
      }
      else {
        // Match Rust release-mode i32 wrapping without C++ signed overflow.
        next_filter_smoothing_in_ = next_filter_smoothing_in_ == std::numeric_limits<int>::min()
            ? std::numeric_limits<int>::max() : next_filter_smoothing_in_ - 1;
      }
      poly_float wet = input[sample];
      for (int stage = 0; stage < stage_count_; ++stage)
        wet = filters_[stage].process(wet);
      output_audio[sample] = wet;
    }
  }
}
