// SPDX-License-Identifier: GPL-3.0-or-later
#include "analogue_rotary_module.h"
#include "../effects/analogue/rotary_engine.h"
#include <algorithm>
#include <cmath>

namespace vital {
  namespace {
    namespace rotary = atlas::analogue::rotary;
    enum { kSpeed, kInertia, kDrive, kBalance, kSpread, kMix, kNumControls };
    const char* const kControlNames[kNumControls] = { "speed", "inertia", "drive", "balance", "spread", "mix" };
    static_assert(kNumControls == AnalogueRotaryModule::kNumModControls);

    int choice(const Value* value, int maximum) {
      return std::clamp(static_cast<int>(std::lround(value->value())), 0, maximum);
    }
  }

  AnalogueRotaryModule::AnalogueRotaryModule(std::string prefix) :
      SynthModule(0, 1), engine_(std::make_unique<rotary::Engine>()), prefix_(std::move(prefix)) {
    engine_->prepare(getSampleRate());
  }

  AnalogueRotaryModule::~AnalogueRotaryModule() = default;

  void AnalogueRotaryModule::init() {
    const std::string name = prefix_ + "analoguerotary_";
    for (int i = 0; i < kNumControls; ++i)
      controls_[i] = createMonoModControl(name + kControlNames[i]);
    model_ = createBaseControl(name + "model");
    SynthModule::init();
  }

  void AnalogueRotaryModule::processWithInput(const poly_float* input, int samples) {
    if (samples <= 0) return;
    SynthModule::process(samples);
    const auto value = [this](int index) { return static_cast<double>(controls_[index]->buffer[0][0]); };

    rotary::Parameters p;
    p.model = choice(model_, rotary::kNumModels - 1);
    p.speed = std::clamp(static_cast<int>(std::lround(value(kSpeed))), 0, rotary::kNumSpeeds - 1);
    p.inertia = std::clamp(value(kInertia), 0.0, 1.0);
    p.drive = std::clamp(value(kDrive), 0.0, 1.0);
    p.balance = std::clamp(value(kBalance), -1.0, 1.0);
    p.spread = std::clamp(value(kSpread), 0.0, 1.0);
    p.mix = std::clamp(value(kMix), 0.0, 1.0);
    engine_->setParameters(p);
    if (reset_pending_) {
      // A fresh instance starts at its settings, with the rotors already turning.
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

  void AnalogueRotaryModule::hardReset() {
    engine_->reset();
    reset_pending_ = true;
  }

  void AnalogueRotaryModule::enable(bool on) {
    if (enabled() != on) hardReset();
    SynthModule::enable(on);
  }

  void AnalogueRotaryModule::setSampleRate(int rate) {
    const int previous = getSampleRate();
    SynthModule::setSampleRate(rate);
    if (getSampleRate() != previous) {
      engine_->prepare(getSampleRate());
      hardReset();
    }
  }

  void AnalogueRotaryModule::setOversampleAmount(int amount) {
    const int previous = getSampleRate();
    SynthModule::setOversampleAmount(amount);
    if (getSampleRate() != previous) {
      engine_->prepare(getSampleRate());
      hardReset();
    }
  }
}
