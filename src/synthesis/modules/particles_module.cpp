// SPDX-License-Identifier: GPL-3.0-or-later
#include "particles_module.h"
#include "particles/particles_engine.h"
#include "note_monitor.h"
#include "prepared_effect_module.h"
#include <algorithm>
#include <cmath>

namespace vital {
  namespace {
    namespace particles = atlas::particles;
    const char* const kModSuffixes[ParticlesModule::kNumModControls] = {
      "input_gain", "position", "size", "pitch", "density", "texture", "mix", "spread", "feedback", "reverb",
      "freeze", "trigger"
    };
    const char* const kBaseSuffixes[ParticlesModule::kNumBaseControls] = { "mode", "quality" };

    int roundedChoice(float value, int low, int high) {
      if (!std::isfinite(value)) return low;
      return std::clamp(static_cast<int>(std::lround(value)), low, high);
    }
  }

  const char* ParticlesModule::modControlSuffix(int control) { return kModSuffixes[control]; }
  const char* ParticlesModule::baseControlSuffix(int control) { return kBaseSuffixes[control]; }

  ParticlesModule::ParticlesModule(const NoteMonitor* notes, std::string prefix) :
      SynthModule(0, 1), engine_(std::make_unique<particles::Engine>()), notes_(notes), prefix_(std::move(prefix)) { }

  ParticlesModule::~ParticlesModule() = default;

  void ParticlesModule::init() {
    const std::string name = prefix_ + "particles_";
    for (int i = 0; i < kNumModControls; ++i)
      mod_controls_[static_cast<size_t>(i)] = createMonoModControl(name + kModSuffixes[i]);
    for (int i = 0; i < kNumBaseControls; ++i)
      base_controls_[static_cast<size_t>(i)] = createBaseControl(name + kBaseSuffixes[i]);
    SynthModule::init();
  }

  int ParticlesModule::getLatencySamples() const {
    return std::min(kMaxLatencySamples, particles::Engine::latencyFor(getSampleRate()));
  }

  void ParticlesModule::prepareEngine() {
    prepared_rate_ = getSampleRate();
    engine_->prepare(prepared_rate_);
    reset_pending_ = false;
    trigger_high_ = false;
    note_on_count_ = notes_ ? notes_->noteOnCount() : 0;
  }

  void ParticlesModule::processWithInput(const poly_float* input, int samples) {
    if (samples <= 0) return;
    SynthModule::process(samples);
    if (!engine_->prepared() || prepared_rate_ != getSampleRate()) {
      if (RealtimeEffectScope::active()) {
        // Built by the preparation worker before any realtime block; until
        // then the audio passes through.
        utils::copyBuffer(output()->buffer, input, samples);
        return;
      }
      prepareEngine();
    }

    const auto mod = [this](int index) { return mod_controls_[static_cast<size_t>(index)]->buffer[0][0]; };
    const auto base = [this](int index) { return base_controls_[static_cast<size_t>(index)]->value(); };
    particles::Parameters p;
    p.mode = roundedChoice(base(kMode), 0, particles::kNumModes - 1);
    p.quality = roundedChoice(base(kQuality), 0, particles::kNumQualities - 1);
    p.input_gain = std::pow(10.0f, std::clamp(mod(kInputGain), -18.0f, 6.0f) / 20.0f);
    p.position = mod(kPosition);
    p.size = mod(kSize);
    p.pitch = mod(kPitch);
    // Density is stored around the module's centre detent.
    p.density = 0.5f * (std::clamp(mod(kDensity), -1.0f, 1.0f) + 1.0f);
    p.texture = mod(kTexture);
    p.dry_wet = mod(kMix);
    p.stereo_spread = mod(kSpread);
    p.feedback = mod(kFeedback);
    p.reverb = mod(kReverb);
    p.freeze = mod(kFreeze) >= 0.5f;
    engine_->setParameters(p);
    if (reset_pending_) {
      engine_->reset();
      reset_pending_ = false;
    }

    // A grain for every note-on since the last block, and for Trigger turning
    // on (above one half when modulated): the module's TRIGGER input.
    const bool trigger_high = mod(kTrigger) >= 0.5f;
    bool fire = trigger_high && !trigger_high_;
    trigger_high_ = trigger_high;
    if (notes_) {
      const uint32_t count = notes_->noteOnCount();
      fire = fire || count != note_on_count_;
      note_on_count_ = count;
    }
    if (fire) engine_->trigger();

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

  void ParticlesModule::hardReset() {
    reset_pending_ = true;
  }

  void ParticlesModule::enable(bool on) {
    if (enabled() != on) hardReset();
    SynthModule::enable(on);
  }

  void ParticlesModule::setSampleRate(int rate) {
    SynthModule::setSampleRate(rate);
    if (getSampleRate() != prepared_rate_) prepareEngine();
  }

  void ParticlesModule::setOversampleAmount(int amount) {
    SynthModule::setOversampleAmount(amount);
    if (getSampleRate() != prepared_rate_) prepareEngine();
  }
}
