// SPDX-License-Identifier: GPL-3.0-or-later
#include "spectral_compressor_module.h"
#include "../effects/spectral_compressor/spectral_compressor_engine.h"

namespace vital {
  struct SpectralCompressorModule::Engines {
    atlas::spectral_compressor::Engine engine;
    std::array<std::array<float, kMaxBufferSize * kMaxOversample>, 4> scratch {};
  };

  SpectralCompressorModule::SpectralCompressorModule(std::string prefix) :
      SynthModule(0, 1), engines_(std::make_unique<Engines>()), prefix_(std::move(prefix)) {
    static_assert(SpectralCompressorModule::kLatencySamples == atlas::spectral_compressor::Engine::kLatencySamples);
    prepare();
  }

  SpectralCompressorModule::~SpectralCompressorModule() = default;

  void SpectralCompressorModule::init() {
    // Compression is a UI macro that moves Downward and Upward together.
    createBaseControl(prefix_ + "spectral_compressor_compression");
    curve_ = createBaseControl(prefix_ + "spectral_compressor_curve");
    const char* names[] = { "downward", "upward", "target", "attack", "release",
                            "smooth", "tilt", "makeup", "mix" };
    for (int i = 0; i < 9; ++i)
      controls_[i] = createMonoModControl(prefix_ + "spectral_compressor_" + names[i]);
    SynthModule::init();
  }

  void SpectralCompressorModule::prepare() {
    engines_->engine.prepare(getSampleRate());
  }

  void SpectralCompressorModule::processWithInput(const poly_float* input, int samples) {
    if (samples <= 0) return;
    juce::ScopedNoDenormals no_denormals;
    SynthModule::process(samples);
    const auto value = [this](int index, float minimum, float maximum) {
      const float v = controls_[index]->buffer[0][0];
      return std::isfinite(v) ? juce::jlimit(minimum, maximum, v) : minimum;
    };
    atlas::spectral_compressor::Parameters parameters;
    parameters.downward = value(0, 0, 100) / 100.0f;
    parameters.upward = value(1, 0, 100) / 100.0f;
    parameters.target_db = value(2, -64, 0);
    parameters.attack = value(3, 0, 100) / 100.0f;
    parameters.release = value(4, 0, 100) / 100.0f;
    parameters.smooth = value(5, 0, 100) / 100.0f;
    parameters.curve = juce::roundToInt(juce::jlimit(0.0f, 2.0f, curve_->value()));
    parameters.tilt = value(6, -6, 0);
    parameters.makeup_db = value(7, -6, 18);
    parameters.mix = value(8, 0, 100) / 100.0f;

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

  void SpectralCompressorModule::hardReset() {
    engines_->engine.reset();
  }

  void SpectralCompressorModule::enable(bool on) {
    if (enabled() != on) hardReset();
    SynthModule::enable(on);
  }

  void SpectralCompressorModule::setSampleRate(int rate) {
    const int previous = getSampleRate();
    SynthModule::setSampleRate(rate);
    if (getSampleRate() != previous) prepare();
  }

  void SpectralCompressorModule::setOversampleAmount(int amount) {
    const int previous = getSampleRate();
    SynthModule::setOversampleAmount(amount);
    if (getSampleRate() != previous) prepare();
  }
}
