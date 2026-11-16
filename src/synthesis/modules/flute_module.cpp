// SPDX-License-Identifier: GPL-3.0-or-later
#include "flute_module.h"

#include "flute_source.h"

#include <cmath>

namespace vital {
  void FluteRenderer::process(int num_samples) {
    if (num_samples <= 0)
      return;
    // A non-finite control reads as its default.
    const auto control = [this](int index, float low, float high, float default_value) {
      const float value = input(index)->at(0)[0];
      return std::isfinite(value) ? std::max(low, std::min(high, value)) : default_value;
    };
    atlas::flute::Instrument::Params params;
    params.air = control(kAir, 0.0f, 1.0f, 0.5f);
    params.brightness = control(kBrightness, 0.0f, 1.0f, 0.5f);
    params.attack = control(kAttack, 0.0f, 1.0f, 0.5f);
    params.vibratoDepth = control(kVibratoDepth, 0.0f, 1.0f, 0.35f);
    params.vibratoRateHz = control(kVibratoRate, 0.5f, 12.0f, 5.0f);
    params.vibratoDelaySec = control(kVibratoDelay, 0.0f, 10.0f, 0.2f);
    params.humanize = control(kHumanize, 0.0f, 1.0f, 0.5f);
    params.dynamicsSource = static_cast<int>(control(kDynamicsSource, 0.0f, 2.0f, 0.0f) + 0.5f);
    params.dynamicsController = static_cast<int>(control(kDynamicsController, 0.0f, 2.0f, 0.0f) + 0.5f);
    params.dynamicRangeDb = control(kDynamicRange, 0.0f, 40.0f, 18.0f);
    params.releaseSec = control(kRelease, 0.01f, 5.0f, 0.15f);
    params.tuningA4 = control(kTuning, 300.0f, 600.0f, 440.0f);
    params.overlap = control(kOverlap, 0.0f, 1.0f, 1.0f) >= 0.5f;
    params.transpose = static_cast<int>(std::lround(control(kTranspose, -48.0f, 48.0f, 0.0f)));
    instrument_->render(params, num_samples);
  }

  FluteModule::FluteModule(std::string prefix, std::string slot_prefix) :
      SynthModule(kNumInputs, kNumOutputs), prefix_(std::move(prefix)), slot_prefix_(std::move(slot_prefix)),
      instrument_(std::make_shared<atlas::flute::Instrument>()) {
    source_ = new FluteSource(instrument_);
  }

  void FluteModule::init() {
    Value* amp_env = createBaseControl(prefix_ + "amp_env");
    Value* dynamics_source = createBaseControl(prefix_ + "dynamics_source");
    Value* dynamics_cc = createBaseControl(prefix_ + "dynamics_cc");
    Value* overlap = createBaseControl(prefix_ + "overlap");
    // The flute is one instrument per slot, so its controls are mono: they
    // are modulated once per block, ahead of the voices.
    auto* renderer = new FluteRenderer(instrument_);
    renderer->plug(createMonoModControl(prefix_ + "air"), FluteRenderer::kAir);
    renderer->plug(createMonoModControl(prefix_ + "brightness"), FluteRenderer::kBrightness);
    renderer->plug(createMonoModControl(prefix_ + "attack"), FluteRenderer::kAttack);
    renderer->plug(createMonoModControl(prefix_ + "vibrato_depth"), FluteRenderer::kVibratoDepth);
    renderer->plug(createMonoModControl(prefix_ + "vibrato_rate"), FluteRenderer::kVibratoRate);
    renderer->plug(createMonoModControl(prefix_ + "vibrato_delay"), FluteRenderer::kVibratoDelay);
    renderer->plug(createMonoModControl(prefix_ + "humanize"), FluteRenderer::kHumanize);
    renderer->plug(dynamics_source, FluteRenderer::kDynamicsSource);
    renderer->plug(dynamics_cc, FluteRenderer::kDynamicsController);
    renderer->plug(createMonoModControl(prefix_ + "dynamic_range"), FluteRenderer::kDynamicRange);
    renderer->plug(createMonoModControl(prefix_ + "release"), FluteRenderer::kRelease);
    renderer->plug(createMonoModControl(prefix_ + "tuning"), FluteRenderer::kTuning);
    renderer->plug(overlap, FluteRenderer::kOverlap);
    renderer->plug(createMonoModControl(prefix_ + "transpose"), FluteRenderer::kTranspose);
    addMonoProcessor(renderer);

    const std::string mix_prefix = slot_prefix_.empty() ? prefix_ : slot_prefix_;
    Output* level = createPolyModControl(mix_prefix + "level", true, true);
    Output* pan = createPolyModControl(mix_prefix + "pan");

    source_->useInput(input(kReset), FluteSource::kReset);
    source_->useInput(input(kVoiceEvent), FluteSource::kVoiceEvent);
    source_->useInput(input(kNoteCount), FluteSource::kNoteCount);
    source_->useInput(input(kActiveVoices), FluteSource::kActiveVoices);
    source_->plug(amp_env, FluteSource::kAmpEnvMode);
    source_->plug(level, FluteSource::kLevel);
    source_->plug(pan, FluteSource::kPan);
    if (!slot_prefix_.empty()) {
      // Slot-level RM reads the slot's shared distortion controls.
      Value* distortion_type = createBaseControl(slot_prefix_ + "distortion_type");
      Output* distortion_amount = createPolyModControl(slot_prefix_ + "distortion_amount");
      Value* secondary_distortion_type = createBaseControl(slot_prefix_ + "secondary_distortion_type");
      Output* secondary_distortion_amount = createPolyModControl(slot_prefix_ + "secondary_distortion_amount");
      source_->plug(distortion_type, FluteSource::kDistortionType);
      source_->plug(distortion_amount, FluteSource::kDistortionAmount);
      source_->plug(secondary_distortion_type, FluteSource::kSecondaryDistortionType);
      source_->plug(secondary_distortion_amount, FluteSource::kSecondaryDistortionAmount);
    }
    source_->useOutput(output(kRaw), FluteSource::kRaw);
    source_->useOutput(output(kLevelled), FluteSource::kLevelled);
    source_->useOutput(output(kHold), FluteSource::kHold);

    addProcessor(source_);
    SynthModule::init();
  }

  void FluteModule::setModulatorOutput(int position, Output* output) {
    source_->modulation().setModulatorOutput(position, output);
  }
} // namespace vital
