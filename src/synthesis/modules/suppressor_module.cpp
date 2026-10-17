// SPDX-License-Identifier: GPL-3.0-or-later
#include "suppressor_module.h"
#include "../effects/suppressor/suppressor_engine.h"

namespace vital {
  struct SuppressorModule::Engines {
    atlas::suppressor::Engine engine;
    std::array<std::array<float, kMaxBufferSize * kMaxOversample>, 4> scratch {};
  };

  SuppressorModule::SuppressorModule(std::string prefix) :
      SynthModule(0, 1), engines_(std::make_unique<Engines>()), prefix_(std::move(prefix)) {
    static_assert(SuppressorModule::kLatencySamples == atlas::suppressor::Engine::kLatencySamples);
    prepare();
  }
  SuppressorModule::~SuppressorModule() = default;

  void SuppressorModule::init() {
    const char* names[] = { "amount", "smoothing", "release", "frequency", "width", "slope" };
    for (int i = 0; i < 6; ++i)
      controls_[i] = createMonoModControl(prefix_ + "suppressor_" + names[i]);
    delta_ = createBaseControl(prefix_ + "suppressor_delta");
    SynthModule::init();
  }

  void SuppressorModule::prepare() {
    engines_->engine.prepare(getSampleRate());
  }

  void SuppressorModule::processWithInput(const poly_float* input, int samples) {
    if (samples <= 0) return;
    juce::ScopedNoDenormals no_denormals;
    SynthModule::process(samples);
    const auto value = [this](int index, float low, float high) {
      const float v = controls_[index]->buffer[0][0];
      return std::isfinite(v) ? juce::jlimit(low, high, v) : low;
    };
    atlas::suppressor::Parameters parameters;
    parameters.amount = value(0, 0, 100) / 100.0f;
    parameters.smoothing = value(1, 0, 100) / 100.0f;
    parameters.release = value(2, 1, 8);
    parameters.frequency = value(3, 14, 370);
    parameters.width = value(4, 0, 1000);
    parameters.slope = value(5, 0, 100) / 100.0f;
    parameters.delta = delta_->value() > 0.5f;

    auto& engine = engines_->engine;
    engine.setParameters(parameters);
    for (int offset = 0; offset < samples;) {
      const int count = std::min(kMaxBufferSize * kMaxOversample, samples - offset);
      float* in_left = engines_->scratch[0].data();
      float* in_right = engines_->scratch[1].data();
      float* out_left = engines_->scratch[2].data();
      float* out_right = engines_->scratch[3].data();
      for (int i = 0; i < count; ++i) {
        in_left[i] = input[offset + i][0];
        in_right[i] = input[offset + i][1];
      }
      engine.process(in_left, in_right, out_left, out_right, count);
      for (int i = 0; i < count; ++i)
        output()->buffer[offset + i] = poly_float(out_left[i], out_right[i], out_left[i], out_right[i]);
      offset += count;
    }
  }

  void SuppressorModule::hardReset() {
    engines_->engine.reset();
  }
  void SuppressorModule::enable(bool on) {
    if (enabled() != on) hardReset();
    SynthModule::enable(on);
  }
  void SuppressorModule::setSampleRate(int rate) {
    const int previous = getSampleRate();
    SynthModule::setSampleRate(rate);
    if (getSampleRate() != previous) prepare();
  }
  void SuppressorModule::setOversampleAmount(int amount) {
    const int previous = getSampleRate();
    SynthModule::setOversampleAmount(amount);
    if (getSampleRate() != previous) prepare();
  }
}
