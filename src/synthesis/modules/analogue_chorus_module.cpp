// SPDX-License-Identifier: GPL-3.0-or-later
#include "analogue_chorus_module.h"
#include "../effects/analogue/chorus_engine.h"
#include <algorithm>
#include <cmath>

namespace vital {
  namespace {
    namespace chorus = atlas::analogue::chorus;
    enum { kRate, kDepth, kNoise, kWidth, kMix, kNumControls };
    const char* const kControlNames[kNumControls] = { "rate", "depth", "noise", "width", "mix" };
    static_assert(kNumControls == AnalogueChorusModule::kNumModControls);

    int choice(const Value* value, int maximum) {
      return std::clamp(static_cast<int>(std::lround(value->value())), 0, maximum);
    }
  }

  AnalogueChorusModule::AnalogueChorusModule(std::string prefix) :
      SynthModule(0, 1), engine_(std::make_unique<chorus::Engine>()), prefix_(std::move(prefix)) {
    engine_->prepare(getSampleRate());
  }

  AnalogueChorusModule::~AnalogueChorusModule() = default;

  void AnalogueChorusModule::init() {
    const std::string name = prefix_ + "analoguechorus_";
    for (int i = 0; i < kNumControls; ++i)
      controls_[i] = createMonoModControl(name + kControlNames[i]);
    model_ = createBaseControl(name + "model");
    juno_mode_ = createBaseControl(name + "juno_mode");
    dimension_mode_ = createBaseControl(name + "dimension_mode");
    SynthModule::init();
  }

  void AnalogueChorusModule::processWithInput(const poly_float* input, int samples) {
    if (samples <= 0) return;
    SynthModule::process(samples);
    const auto value = [this](int index) { return static_cast<double>(controls_[index]->buffer[0][0]); };

    chorus::Parameters p;
    p.model = choice(model_, chorus::kNumModels - 1);
    p.juno_mode = choice(juno_mode_, chorus::kNumJunoModes - 1);
    p.dimension_mode = choice(dimension_mode_, chorus::kNumDimensionModes - 1);
    p.rate = std::clamp(value(kRate), 0.0, 4.0);
    p.depth = std::clamp(value(kDepth), 0.0, 2.0);
    p.noise = std::clamp(value(kNoise), 0.0, 1.0);
    p.width = std::clamp(value(kWidth), 0.0, 1.0);
    p.mix = std::clamp(value(kMix), 0.0, 1.0);
    engine_->setParameters(p);
    if (reset_pending_) {
      // A fresh instance starts at its settings, without smoothing from stale ones.
      engine_->reset();
      reset_pending_ = false;
    }

    poly_float* output_buffer = output()->buffer;
    for (int offset = 0; offset < samples;) {
      const int count = std::min(kMaxBufferSize, samples - offset);
      float in_left[kMaxBufferSize], in_right[kMaxBufferSize];
      float out_left[kMaxBufferSize], out_right[kMaxBufferSize];
      for (int i = 0; i < count; ++i) {
        in_left[i] = input[offset + i][0];
        in_right[i] = input[offset + i][1];
      }
      engine_->process(in_left, in_right, out_left, out_right, count);
      for (int i = 0; i < count; ++i)
        output_buffer[offset + i] = poly_float(out_left[i], out_right[i], out_left[i], out_right[i]);
      offset += count;
    }
  }

  void AnalogueChorusModule::hardReset() {
    engine_->reset();
    reset_pending_ = true;
  }

  void AnalogueChorusModule::enable(bool on) {
    if (enabled() != on) hardReset();
    SynthModule::enable(on);
  }

  void AnalogueChorusModule::setSampleRate(int rate) {
    const int previous = getSampleRate();
    SynthModule::setSampleRate(rate);
    if (getSampleRate() != previous) {
      engine_->prepare(getSampleRate());
      hardReset();
    }
  }

  void AnalogueChorusModule::setOversampleAmount(int amount) {
    const int previous = getSampleRate();
    SynthModule::setOversampleAmount(amount);
    if (getSampleRate() != previous) {
      engine_->prepare(getSampleRate());
      hardReset();
    }
  }
}
