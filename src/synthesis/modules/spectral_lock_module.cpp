// SPDX-License-Identifier: GPL-3.0-or-later
#include "spectral_lock_module.h"
#include "../effects/spectral_lock/spectral_lock_engine.h"
#include "../effects/spectral_lock/spectral_retune_engine.h"
namespace vital {
struct SpectralLockModule::Engines {
  // The voice handler sums both voices into both SIMD pairs, so lanes 2-3 repeat
  // lanes 0-1: one stereo engine per mode processes lanes 0-1 and fills both pairs.
  atlas::spectral_lock::Engine filterbank;
  atlas::spectral_lock::retune::Engine retune;
  std::array<std::array<float, kMaxBufferSize * kMaxOversample>, 4> scratch {};
  atlas::spectral_lock::Parameters parameters;  // the latest block's, for resets
};
static_assert(SpectralLockModule::kMaxSpectralLatencySamples == atlas::spectral_lock::retune::Engine::kMaxLatencySamples);
int SpectralLockModule::spectralLatencyForRate(double sample_rate) {
  return atlas::spectral_lock::retune::Engine::latencyForRate(sample_rate);
}
int SpectralLockModule::getLatencySamples() const {
  return mode_ != nullptr && mode_->value() >= 0.5f ? spectralLatencyForRate(getSampleRate()) : 0;
}
SpectralLockModule::SpectralLockModule(std::string prefix) : SynthModule(0, 1),
    engines_(std::make_unique<Engines>()), prefix_(std::move(prefix)) { prepare(); }
SpectralLockModule::~SpectralLockModule() = default;
void SpectralLockModule::init() {
  const char* names[] = { "amount", "isolate", "shimmer", "bend", "bend_range", "range_low",
                          "range_high", "spray", "tilt", "pitch", "mix", "width", "follow",
                          "transients", "reach" };
  for (int i = 0; i < 15; ++i)
    controls_[i] = createMonoModControl(prefix_ + "spectral_lock_" + names[i]);
  root_ = createBaseControl(prefix_ + "spectral_lock_root");
  scale_ = createBaseControl(prefix_ + "spectral_lock_scale");
  mute_low_ = createBaseControl(prefix_ + "spectral_lock_mute_low");
  mute_high_ = createBaseControl(prefix_ + "spectral_lock_mute_high");
  mode_ = createBaseControl(prefix_ + "spectral_lock_mode");
  SynthModule::init();
}
void SpectralLockModule::prepare() {
  engines_->filterbank.prepare(getSampleRate(), kMaxBufferSize * kMaxOversample);
  engines_->retune.prepare(getSampleRate());
  last_mode_ = -1;
}
void SpectralLockModule::resetEngines(int mode) {
  // Nothing of the other mode's past may leak in after a switch: the engine
  // taking over starts from silence, as it would after a hard reset.
  if (mode == 1) {
    engines_->retune.reset();
  }
  else {
    engines_->filterbank.reset(engines_->parameters);
  }
}
void SpectralLockModule::processWithInput(const poly_float* input, int samples) {
  if (samples <= 0) return;
  juce::ScopedNoDenormals no_denormals;
  SynthModule::process(samples);
  const auto value = [this](int i, float low, float high) {
    return juce::jlimit(low, high, controls_[i]->buffer[0][0]);
  };
  atlas::spectral_lock::Parameters p;
  p.amount = value(0, 0, 2); p.isolate = value(1, 0, 1); p.shimmer = value(2, 0, 1);
  p.bend = value(3, -1, 1); p.bendRange = std::round(value(4, 1, 24));
  p.rangeLo = value(5, 60, 4800); p.rangeHi = value(6, 60, 4800);
  p.spray = value(7, 0, 1); p.tilt = value(8, -1, 1);
  p.pitch = std::round(value(9, -24, 24)); p.mix = value(10, 0, 1);
  p.width = value(11, 0, 1); p.follow = value(12, 0, 1);
  p.root = juce::roundToInt(juce::jlimit(0.0f, 11.0f, root_->value()));
  p.scale = juce::roundToInt(juce::jlimit(0.0f, 11.0f, scale_->value()));
  p.muteLo = mute_low_->value(); p.muteHi = mute_high_->value();
  const int mode = mode_->value() >= 0.5f ? 1 : 0;
  engines_->parameters = p;
  if (mode != last_mode_) {
    if (last_mode_ >= 0) resetEngines(mode);
    last_mode_ = mode;
  }
  if (mode == 1) {
    // Spectral mode: polyphonic pitch mapping of the material itself.
    atlas::spectral_lock::retune::Parameters r;
    r.amount = p.amount; r.pitch = p.pitch; r.bend = p.bend; r.bendRange = p.bendRange;
    r.rangeLo = p.rangeLo; r.rangeHi = p.rangeHi; r.mix = p.mix; r.root = p.root; r.scale = p.scale;
    r.transients = value(13, 0, 1); r.reach = std::round(value(14, 1, 12));
    auto& engine = engines_->retune;
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
      engine.process(in_left, in_right, out_left, out_right, count, r);
      for (int i = 0; i < count; ++i)
        output()->buffer[offset + i] = poly_float(out_left[i], out_right[i], out_left[i], out_right[i]);
      offset += count;
    }
    return;
  }
  for (int offset = 0; offset < samples;) {
    const int count = std::min(kMaxBufferSize * kMaxOversample, samples - offset);
    float* channels[] = { engines_->scratch[0].data(), engines_->scratch[1].data() };
    for (int i = 0; i < count; ++i)
      for (int ch = 0; ch < 2; ++ch)
        channels[ch][i] = input[offset + i][ch];
    juce::AudioBuffer<float> buffer(channels, 2, count);
    engines_->filterbank.process(buffer, p);
    for (int i = 0; i < count; ++i)
      output()->buffer[offset + i] = poly_float(channels[0][i], channels[1][i], channels[0][i], channels[1][i]);
    offset += count;
  }
}
void SpectralLockModule::hardReset() {
  // Runs on the audio thread: clear state without reallocating.
  engines_->retune.reset();
  engines_->filterbank.reset(engines_->parameters);
}
void SpectralLockModule::enable(bool on) {
  if (enabled() != on) hardReset();
  SynthModule::enable(on);
}
void SpectralLockModule::setSampleRate(int rate) {
  const int previous = getSampleRate();
  SynthModule::setSampleRate(rate);
  if (getSampleRate() != previous) prepare();
}
void SpectralLockModule::setOversampleAmount(int amount) {
  const int previous = getSampleRate();
  SynthModule::setOversampleAmount(amount);
  if (getSampleRate() != previous) prepare();
}
}
