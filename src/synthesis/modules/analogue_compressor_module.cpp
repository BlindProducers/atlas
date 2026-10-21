// SPDX-License-Identifier: GPL-3.0-or-later
#include "analogue_compressor_module.h"
#include "zl_compressor/zl_compressor_engine.h"
#include <algorithm>
#include <cmath>

namespace vital {
  namespace {
    namespace zl = atlas::zl;
    enum { kThreshold, kRatio, kKnee, kAttack, kRelease, kMakeup, kMix, kNumControls };
    const char* const kControlNames[kNumControls] = {
      "threshold", "ratio", "knee", "attack", "release", "makeup", "mix"
    };
    static_assert(kNumControls == AnalogueCompressorModule::kNumModControls);

    // A modulated control can leave its range for a moment; a non-finite one
    // falls back to the default.
    double clamped(double value, double low, double high, double fallback) {
      return std::isfinite(value) ? std::clamp(value, low, high) : fallback;
    }
  }

  AnalogueCompressorModule::AnalogueCompressorModule(std::string prefix) :
      SynthModule(0, 1), engine_(std::make_unique<zl::Engine>()), prefix_(std::move(prefix)) {
    engine_->prepare(getSampleRate());
  }

  AnalogueCompressorModule::~AnalogueCompressorModule() = default;

  void AnalogueCompressorModule::init() {
    const std::string name = prefix_ + "analoguecompressor_";
    for (int i = 0; i < kNumControls; ++i)
      controls_[i] = createMonoModControl(name + kControlNames[i]);
    mode_ = createBaseControl(name + "mode");
    SynthModule::init();
  }

  void AnalogueCompressorModule::processWithInput(const poly_float* input, int samples) {
    if (samples <= 0) return;
    SynthModule::process(samples);
    // The mod controls apply each parameter's scale: Ratio arrives as the
    // ratio (stored as its log2), Attack and Release in ZL's milliseconds
    // (stored as their cube roots).
    const auto value = [this](int index) { return static_cast<double>(controls_[index]->buffer[0][0]); };

    const zl::Parameters defaults;
    zl::Parameters p;
    const double mode = mode_->value();
    p.style = std::isfinite(mode) ? std::clamp(static_cast<int>(std::lround(mode)), 0, zl::kNumStyles - 1)
                                  : zl::kClean;
    p.threshold = clamped(value(kThreshold), -60.0, 0.0, defaults.threshold);
    p.ratio = clamped(value(kRatio), 1.0, 100.0, defaults.ratio);
    p.knee = clamped(value(kKnee), 0.0, 32.0, defaults.knee);
    p.attack = clamped(value(kAttack), 0.0, 1000.0, defaults.attack);
    p.release = clamped(value(kRelease), 0.0, 5000.0, defaults.release);
    p.makeup = clamped(value(kMakeup), -30.0, 30.0, defaults.makeup);
    p.wet = clamped(value(kMix), 0.0, 1.0, defaults.wet);
    engine_->setParameters(p);
    if (reset_pending_) {
      engine_->reset();
      reset_pending_ = false;
    }

    poly_float* output_buffer = output()->buffer;
    for (int offset = 0; offset < samples;) {
      const int count = std::min(kMaxBufferSize, samples - offset);
      float left[kMaxBufferSize], right[kMaxBufferSize];
      for (int i = 0; i < count; ++i) {
        left[i] = input[offset + i][0];
        right[i] = input[offset + i][1];
      }
      engine_->process(left, right, left, right, count);
      for (int i = 0; i < count; ++i)
        output_buffer[offset + i] = poly_float(left[i], right[i], left[i], right[i]);
      offset += count;
    }
  }

  void AnalogueCompressorModule::hardReset() {
    engine_->reset();
    reset_pending_ = true;
  }

  void AnalogueCompressorModule::enable(bool on) {
    if (enabled() != on) hardReset();
    SynthModule::enable(on);
  }

  void AnalogueCompressorModule::setSampleRate(int rate) {
    const int previous = getSampleRate();
    SynthModule::setSampleRate(rate);
    if (getSampleRate() != previous) {
      engine_->prepare(getSampleRate());
      hardReset();
    }
  }

  void AnalogueCompressorModule::setOversampleAmount(int amount) {
    const int previous = getSampleRate();
    SynthModule::setOversampleAmount(amount);
    if (getSampleRate() != previous) {
      engine_->prepare(getSampleRate());
      hardReset();
    }
  }
}
