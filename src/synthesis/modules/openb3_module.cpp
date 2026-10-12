// SPDX-License-Identifier: GPL-3.0-or-later
#include "openb3_module.h"

#include "openb3_source.h"

namespace vital {
  const char* const OpenB3Module::kDrawbarSuffixes[] = {
    "drawbar_16", "drawbar_5_1_3", "drawbar_8", "drawbar_4", "drawbar_2_2_3",
    "drawbar_2", "drawbar_1_3_5", "drawbar_1_1_3", "drawbar_1"
  };

  OpenB3Module::OpenB3Module(std::string prefix, std::string slot_prefix) :
      SynthModule(kNumInputs, kNumOutputs), prefix_(std::move(prefix)), slot_prefix_(std::move(slot_prefix)) {
    source_ = new OpenB3Source();
  }

  void OpenB3Module::init() {
    Value* amp_env = createBaseControl(prefix_ + "amp_env");
    for (int d = 0; d < atlas::openb3::kNumDrawbars; ++d)
      source_->plug(createPolyModControl(prefix_ + kDrawbarSuffixes[d]), OpenB3Source::kDrawbar + d);
    Value* percussion = createBaseControl(prefix_ + "percussion");
    Value* percussion_volume = createBaseControl(prefix_ + "percussion_volume");
    Value* percussion_decay = createBaseControl(prefix_ + "percussion_decay");
    Value* percussion_harmonic = createBaseControl(prefix_ + "percussion_harmonic");
    Output* key_click = createPolyModControl(prefix_ + "key_click");
    Value* temperament = createBaseControl(prefix_ + "temperament");
    Output* transpose = createPolyModControl(prefix_ + "transpose");
    Output* tune = createPolyModControl(prefix_ + "tune");
    const std::string mix_prefix = slot_prefix_.empty() ? prefix_ : slot_prefix_;
    Output* level = createPolyModControl(mix_prefix + "level", true, true);
    Output* pan = createPolyModControl(mix_prefix + "pan");

    source_->useInput(input(kReset), OpenB3Source::kReset);
    source_->useInput(input(kVoiceEvent), OpenB3Source::kVoiceEvent);
    source_->useInput(input(kMidi), OpenB3Source::kMidi);
    source_->plug(amp_env, OpenB3Source::kAmpEnvMode);
    source_->plug(percussion, OpenB3Source::kPercussion);
    source_->plug(percussion_volume, OpenB3Source::kPercussionVolume);
    source_->plug(percussion_decay, OpenB3Source::kPercussionDecay);
    source_->plug(percussion_harmonic, OpenB3Source::kPercussionHarmonic);
    source_->plug(key_click, OpenB3Source::kKeyClick);
    source_->plug(temperament, OpenB3Source::kTemperament);
    source_->plug(transpose, OpenB3Source::kTranspose);
    source_->plug(tune, OpenB3Source::kTune);
    source_->plug(level, OpenB3Source::kLevel);
    source_->plug(pan, OpenB3Source::kPan);
    if (!slot_prefix_.empty()) {
      // Slot-level RM reads the slot's shared distortion controls.
      Value* distortion_type = createBaseControl(slot_prefix_ + "distortion_type");
      Output* distortion_amount = createPolyModControl(slot_prefix_ + "distortion_amount");
      Value* secondary_distortion_type = createBaseControl(slot_prefix_ + "secondary_distortion_type");
      Output* secondary_distortion_amount = createPolyModControl(slot_prefix_ + "secondary_distortion_amount");
      source_->plug(distortion_type, OpenB3Source::kDistortionType);
      source_->plug(distortion_amount, OpenB3Source::kDistortionAmount);
      source_->plug(secondary_distortion_type, OpenB3Source::kSecondaryDistortionType);
      source_->plug(secondary_distortion_amount, OpenB3Source::kSecondaryDistortionAmount);
    }
    source_->useOutput(output(kRaw), OpenB3Source::kRaw);
    source_->useOutput(output(kLevelled), OpenB3Source::kLevelled);
    source_->useOutput(output(kHold), OpenB3Source::kHold);

    addProcessor(source_);
    // The organ's clock runs once per block in the mono router, ahead of the voices.
    addMonoProcessor(new OpenB3Clock(source_->organ()));
    SynthModule::init();
  }

  void OpenB3Module::setModulatorOutput(int position, Output* output) {
    source_->modulation().setModulatorOutput(position, output);
  }
} // namespace vital
