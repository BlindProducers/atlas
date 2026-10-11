// SPDX-License-Identifier: GPL-3.0-or-later
#include "spectral_balance_module.h"
#include "../effects/spectral_balance/spectral_balance_engine.h"
#include <algorithm>
#include <cmath>

namespace vital {
  namespace {
    namespace balance = atlas::spectral_balance;
    enum { kAmount, kBias, kSpeed, kRangeLow, kRangeHigh, kTameRecover, kMix, kNumControls };
    const char* const kControlNames[kNumControls] = {
      "amount", "bias", "speed", "range_low", "range_high", "tame_recover", "mix"
    };
    static_assert(kNumControls == SpectralBalanceModule::kNumModControls);
    static_assert(SpectralBalanceModule::kLatencySamples == balance::Engine::kLatencySamples);
  }

  SpectralBalanceModule::SpectralBalanceModule(std::string prefix) :
      SynthModule(0, 1), engine_(std::make_unique<balance::Engine>()), prefix_(std::move(prefix)) {
    engine_->prepare(getSampleRate());
  }

  SpectralBalanceModule::~SpectralBalanceModule() = default;

  void SpectralBalanceModule::init() {
    const std::string name = prefix_ + "spectral_balance_";
    for (int i = 0; i < kNumControls; ++i)
      controls_[i] = createMonoModControl(name + kControlNames[i]);
    target_ = createBaseControl(name + "target");
    SynthModule::init();
  }

  void SpectralBalanceModule::processWithInput(const poly_float* input, int samples) {
    if (samples <= 0) return;
    juce::ScopedNoDenormals no_denormals;
    SynthModule::process(samples);
    const auto value = [this](int index, float low, float high) {
      const float v = controls_[index]->buffer[0][0];
      return std::isfinite(v) ? std::clamp(v, low, high) : low;
    };
    balance::Parameters p;
    p.amount = value(kAmount, 0.0f, 1.0f);
    p.target = std::clamp(int(std::lround(target_->value())), 0, int(balance::kNumTargets) - 1);
    p.bias = value(kBias, -3.0f, 3.0f);
    p.speed = value(kSpeed, 0.0f, 1.0f);
    // Range edges are stored as log2 Hz, like the delay rates; the
    // exponential-scaled controls already deliver Hz.
    p.range_low = value(kRangeLow, 20.0f, 2000.0f);
    p.range_high = value(kRangeHigh, 1000.0f, 20000.0f);
    p.tame_recover = value(kTameRecover, -1.0f, 1.0f);
    p.mix = value(kMix, 0.0f, 1.0f);
    engine_->setParameters(p);

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

  void SpectralBalanceModule::hardReset() { engine_->reset(); }

  void SpectralBalanceModule::enable(bool on) {
    if (enabled() != on) hardReset();
    SynthModule::enable(on);
  }

  void SpectralBalanceModule::setSampleRate(int rate) {
    const int previous = getSampleRate();
    SynthModule::setSampleRate(rate);
    if (getSampleRate() != previous) engine_->prepare(getSampleRate());
  }

  void SpectralBalanceModule::setOversampleAmount(int amount) {
    const int previous = getSampleRate();
    SynthModule::setOversampleAmount(amount);
    if (getSampleRate() != previous) engine_->prepare(getSampleRate());
  }
}
