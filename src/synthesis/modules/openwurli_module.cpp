// SPDX-License-Identifier: GPL-3.0-or-later
#include "openwurli_module.h"

#include "openwurli_source.h"

namespace vital {
  OpenWurliModule::OpenWurliModule(std::string prefix, std::string slot_prefix) :
      SynthModule(kNumInputs, kNumOutputs), prefix_(std::move(prefix)), slot_prefix_(std::move(slot_prefix)) {
    source_ = new OpenWurliSource();
  }

  void OpenWurliModule::init() {
    Value* amp_env = createBaseControl(prefix_ + "amp_env");
    Value* mlp = createBaseControl(prefix_ + "mlp");
    Output* bark = createPolyModControl(prefix_ + "bark");
    Output* damping = createPolyModControl(prefix_ + "damping");
    Output* condition = createPolyModControl(prefix_ + "condition");
    Output* treble_voicing = createPolyModControl(prefix_ + "treble_voicing");
    Value* register_balance = createBaseControl(prefix_ + "register_balance");
    Value* pickup_low_cut = createBaseControl(prefix_ + "pickup_low_cut");
    Output* transpose = createPolyModControl(prefix_ + "transpose");
    Output* tune = createPolyModControl(prefix_ + "tune");
    const std::string mix_prefix = slot_prefix_.empty() ? prefix_ : slot_prefix_;
    Output* level = createPolyModControl(mix_prefix + "level", true, true);
    Output* pan = createPolyModControl(mix_prefix + "pan");

    source_->useInput(input(kReset), OpenWurliSource::kReset);
    source_->useInput(input(kVoiceEvent), OpenWurliSource::kVoiceEvent);
    source_->useInput(input(kMidi), OpenWurliSource::kMidi);
    source_->useInput(input(kVelocity), OpenWurliSource::kVelocity);
    source_->plug(amp_env, OpenWurliSource::kAmpEnvMode);
    source_->plug(mlp, OpenWurliSource::kMlp);
    source_->plug(bark, OpenWurliSource::kBark);
    source_->plug(damping, OpenWurliSource::kDamping);
    source_->plug(condition, OpenWurliSource::kCondition);
    source_->plug(treble_voicing, OpenWurliSource::kTrebleVoicing);
    source_->plug(register_balance, OpenWurliSource::kRegisterBalance);
    source_->plug(pickup_low_cut, OpenWurliSource::kPickupLowCut);
    source_->plug(transpose, OpenWurliSource::kTranspose);
    source_->plug(tune, OpenWurliSource::kTune);
    source_->plug(level, OpenWurliSource::kLevel);
    source_->plug(pan, OpenWurliSource::kPan);
    if (!slot_prefix_.empty()) {
      // Slot-level RM reads the slot's shared distortion controls.
      Value* distortion_type = createBaseControl(slot_prefix_ + "distortion_type");
      Output* distortion_amount = createPolyModControl(slot_prefix_ + "distortion_amount");
      Value* secondary_distortion_type = createBaseControl(slot_prefix_ + "secondary_distortion_type");
      Output* secondary_distortion_amount = createPolyModControl(slot_prefix_ + "secondary_distortion_amount");
      source_->plug(distortion_type, OpenWurliSource::kDistortionType);
      source_->plug(distortion_amount, OpenWurliSource::kDistortionAmount);
      source_->plug(secondary_distortion_type, OpenWurliSource::kSecondaryDistortionType);
      source_->plug(secondary_distortion_amount, OpenWurliSource::kSecondaryDistortionAmount);
    }
    source_->useOutput(output(kRaw), OpenWurliSource::kRaw);
    source_->useOutput(output(kLevelled), OpenWurliSource::kLevelled);
    source_->useOutput(output(kHold), OpenWurliSource::kHold);

    addProcessor(source_);
    SynthModule::init();
  }

  void OpenWurliModule::setModulatorOutput(int position, Output* output) {
    source_->modulation().setModulatorOutput(position, output);
  }
} // namespace vital
