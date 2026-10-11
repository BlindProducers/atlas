// SPDX-License-Identifier: GPL-3.0-or-later
#include "analogue_wah_module.h"
#include "../effects/analogue/wah_engine.h"
#include <algorithm>
#include <cmath>

namespace vital {
  namespace {
    namespace wah = atlas::analogue::wah;
    enum { kPosition, kEnvelope, kDecay, kPeak, kMix, kNumControls };
    const char* const kControlNames[kNumControls] = { "position", "envelope", "decay", "peak", "mix" };
    static_assert(kNumControls == AnalogueWahModule::kNumModControls);

    int choice(const Value* value, int maximum) {
      return std::clamp(static_cast<int>(std::lround(value->value())), 0, maximum);
    }
  }

  AnalogueWahModule::AnalogueWahModule(std::string prefix) :
      SynthModule(0, 1), engine_(std::make_unique<wah::Engine>()), prefix_(std::move(prefix)) {
    engine_->prepare(getSampleRate());
  }

  AnalogueWahModule::~AnalogueWahModule() = default;

  void AnalogueWahModule::init() {
    const std::string name = prefix_ + "analoguewah_";
    for (int i = 0; i < kNumControls; ++i)
      controls_[i] = createMonoModControl(name + kControlNames[i]);
    model_ = createBaseControl(name + "model");
    mode_ = createBaseControl(name + "mode");
    range_ = createBaseControl(name + "range");
    SynthModule::init();
  }

  void AnalogueWahModule::processWithInput(const poly_float* input, int samples) {
    if (samples <= 0) return;
    SynthModule::process(samples);
    const auto value = [this](int index) { return static_cast<double>(controls_[index]->buffer[0][0]); };

    wah::Parameters p;
    p.model = choice(model_, wah::kNumModels - 1);
    p.mode = choice(mode_, wah::kNumModes - 1);
    p.range = choice(range_, wah::kNumRanges - 1);
    p.position = std::clamp(value(kPosition), 0.0, 1.0);
    p.envelope = std::clamp(value(kEnvelope), -1.0, 1.0);
    p.decay = std::clamp(value(kDecay), 0.0, 1.0);
    p.peak = std::clamp(value(kPeak), 0.0, 1.0);
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

  void AnalogueWahModule::hardReset() {
    engine_->reset();
    reset_pending_ = true;
  }

  void AnalogueWahModule::enable(bool on) {
    if (enabled() != on) hardReset();
    SynthModule::enable(on);
  }

  void AnalogueWahModule::setSampleRate(int rate) {
    const int previous = getSampleRate();
    SynthModule::setSampleRate(rate);
    if (getSampleRate() != previous) {
      engine_->prepare(getSampleRate());
      hardReset();
    }
  }

  void AnalogueWahModule::setOversampleAmount(int amount) {
    const int previous = getSampleRate();
    SynthModule::setOversampleAmount(amount);
    if (getSampleRate() != previous) {
      engine_->prepare(getSampleRate());
      hardReset();
    }
  }
}
