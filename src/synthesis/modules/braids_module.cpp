// SPDX-License-Identifier: GPL-3.0-or-later
#include "braids_module.h"

namespace vital {
  BraidsModule::BraidsModule(std::string prefix, std::string slot_prefix) :
      SynthModule(kNumInputs, kNumOutputs), prefix_(std::move(prefix)), slot_prefix_(std::move(slot_prefix)) {
    source_ = new BraidsSource();
  }

  void BraidsModule::init() {
    Value* model = createBaseControl(prefix_ + "model");
    Output* timbre = createPolyModControl(prefix_ + "timbre");
    Output* color = createPolyModControl(prefix_ + "color");
    Value* resolution = createBaseControl(prefix_ + "resolution");
    Value* sample_rate = createBaseControl(prefix_ + "sample_rate");
    Output* transpose = createPolyModControl(prefix_ + "transpose");
    Output* tune = createPolyModControl(prefix_ + "tune");
    const std::string mix_prefix = slot_prefix_.empty() ? prefix_ : slot_prefix_;
    Output* level = createPolyModControl(mix_prefix + "level", true, true);
    Output* pan = createPolyModControl(mix_prefix + "pan");

    source_->useInput(input(kReset), BraidsSource::kReset);
    source_->useInput(input(kVoiceEvent), BraidsSource::kVoiceEvent);
    source_->useInput(input(kMidi), BraidsSource::kMidi);
    source_->plug(model, BraidsSource::kModel);
    source_->plug(timbre, BraidsSource::kTimbre);
    source_->plug(color, BraidsSource::kColor);
    source_->plug(resolution, BraidsSource::kResolution);
    source_->plug(sample_rate, BraidsSource::kSampleRate);
    source_->plug(transpose, BraidsSource::kTranspose);
    source_->plug(tune, BraidsSource::kTune);
    source_->plug(level, BraidsSource::kLevel);
    source_->plug(pan, BraidsSource::kPan);
    if (!slot_prefix_.empty()) {
      // Slot-level FM/RM reads the slot's shared distortion controls.
      Value* distortion_type = createBaseControl(slot_prefix_ + "distortion_type");
      Output* distortion_amount = createPolyModControl(slot_prefix_ + "distortion_amount");
      Value* secondary_distortion_type = createBaseControl(slot_prefix_ + "secondary_distortion_type");
      Output* secondary_distortion_amount = createPolyModControl(slot_prefix_ + "secondary_distortion_amount");
      source_->plug(distortion_type, BraidsSource::kDistortionType);
      source_->plug(distortion_amount, BraidsSource::kDistortionAmount);
      source_->plug(secondary_distortion_type, BraidsSource::kSecondaryDistortionType);
      source_->plug(secondary_distortion_amount, BraidsSource::kSecondaryDistortionAmount);
    }
    source_->useOutput(output(kRaw), BraidsSource::kRaw);
    source_->useOutput(output(kLevelled), BraidsSource::kLevelled);

    addProcessor(source_);
    SynthModule::init();
  }
} // namespace vital
