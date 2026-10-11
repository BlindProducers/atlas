// SPDX-License-Identifier: GPL-3.0-or-later
#include "analogue_spring_module.h"
#include "../effects/analogue/spring_engine.h"
#include <algorithm>
#include <cmath>

namespace vital {
  namespace {
    namespace spring = atlas::analogue::spring;
    enum { kDwell, kTone, kPan, kNoise, kMix, kNumControls };
    const char* const kControlNames[kNumControls] = { "dwell", "tone", "pan", "noise", "mix" };
    static_assert(kNumControls == AnalogueSpringModule::kNumModControls);

    int choice(const Value* value, int maximum) {
      return std::clamp(static_cast<int>(std::lround(value->value())), 0, maximum);
    }
  }

  AnalogueSpringModule::AnalogueSpringModule(std::string prefix) :
      SynthModule(0, 1), engine_(std::make_unique<spring::Engine>()), prefix_(std::move(prefix)) {
    engine_->prepare(getSampleRate());
  }

  AnalogueSpringModule::~AnalogueSpringModule() = default;

  void AnalogueSpringModule::init() {
    const std::string name = prefix_ + "analoguespring_";
    for (int i = 0; i < kNumControls; ++i)
      controls_[i] = createMonoModControl(name + kControlNames[i]);
    tank_ = createBaseControl(name + "tank");
    mono_ = createBaseControl(name + "mono");
    SynthModule::init();
  }

  void AnalogueSpringModule::processWithInput(const poly_float* input, int samples) {
    if (samples <= 0) return;
    SynthModule::process(samples);
    const auto value = [this](int index) { return static_cast<double>(controls_[index]->buffer[0][0]); };

    spring::Parameters p;
    p.tank = choice(tank_, spring::kNumTanks - 1);
    p.mono = mono_->value() >= 0.5f;
    p.dwell = std::clamp(value(kDwell), 0.0, 1.0);
    p.tone = std::clamp(value(kTone), 0.0, 1.0);
    p.pan = std::clamp(value(kPan), -1.0, 1.0);
    p.noise = std::clamp(value(kNoise), 0.0, 1.0);
    p.mix = std::clamp(value(kMix), 0.0, 1.0);
    engine_->setParameters(p);
    if (reset_pending_) {
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

  void AnalogueSpringModule::hardReset() {
    engine_->reset();
    reset_pending_ = true;
  }

  void AnalogueSpringModule::enable(bool on) {
    if (enabled() != on) hardReset();
    SynthModule::enable(on);
  }

  void AnalogueSpringModule::setSampleRate(int rate) {
    const int previous = getSampleRate();
    SynthModule::setSampleRate(rate);
    if (getSampleRate() != previous) {
      engine_->prepare(getSampleRate());
      hardReset();
    }
  }

  void AnalogueSpringModule::setOversampleAmount(int amount) {
    const int previous = getSampleRate();
    SynthModule::setOversampleAmount(amount);
    if (getSampleRate() != previous) {
      engine_->prepare(getSampleRate());
      hardReset();
    }
  }
}
