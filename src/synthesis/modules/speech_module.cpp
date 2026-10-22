// SPDX-License-Identifier: GPL-3.0-or-later
#include "speech_module.h"

#include "speech_slot.h"
#include "speech_source.h"

namespace vital {
  SpeechModule::SpeechModule(std::string prefix, std::shared_ptr<SpeechSlot> slot, std::string slot_prefix) :
      SynthModule(kNumInputs, kNumOutputs), prefix_(std::move(prefix)), slot_prefix_(std::move(slot_prefix)) {
    // The type probe builds a module without a slot; it still needs a holder.
    source_ = new SpeechSource(slot ? std::move(slot) : std::make_shared<SpeechSlot>());
  }

  void SpeechModule::init() {
    // Read on the message thread, which renders the text with them.
    createBaseControl(prefix_ + "language");
    createBaseControl(prefix_ + "variant");
    createBaseControl(prefix_ + "mode");
    Value* amp_env = createBaseControl(prefix_ + "amp_env");
    Value* split = createBaseControl(prefix_ + "split");
    Output* formant = createPolyModControl(prefix_ + "formant");
    Output* breath = createPolyModControl(prefix_ + "breath");
    Output* speed = createPolyModControl(prefix_ + "speed");
    Output* vibrato = createPolyModControl(prefix_ + "vibrato");
    Output* vibrato_rate = createPolyModControl(prefix_ + "vibrato_rate");
    Output* transpose = createPolyModControl(prefix_ + "transpose");
    Output* tune = createPolyModControl(prefix_ + "tune");
    const std::string mix_prefix = slot_prefix_.empty() ? prefix_ : slot_prefix_;
    Output* level = createPolyModControl(mix_prefix + "level", true, true);
    Output* pan = createPolyModControl(mix_prefix + "pan");

    source_->useInput(input(kReset), SpeechSource::kReset);
    source_->useInput(input(kRetrigger), SpeechSource::kRetrigger);
    source_->useInput(input(kMidi), SpeechSource::kMidi);
    source_->useInput(input(kActiveVoices), SpeechSource::kActiveVoices);
    source_->plug(amp_env, SpeechSource::kAmpEnvMode);
    source_->plug(level, SpeechSource::kLevel);
    source_->plug(pan, SpeechSource::kPan);
    source_->plug(transpose, SpeechSource::kTranspose);
    source_->plug(tune, SpeechSource::kTune);
    source_->plug(split, SpeechSource::kSplit);
    source_->plug(formant, SpeechSource::kFormant);
    source_->plug(breath, SpeechSource::kBreath);
    source_->plug(speed, SpeechSource::kSpeed);
    source_->plug(vibrato, SpeechSource::kVibrato);
    source_->plug(vibrato_rate, SpeechSource::kVibratoRate);
    if (!slot_prefix_.empty()) {
      // Slot-level FM/RM reads the slot's shared distortion controls.
      Value* distortion_type = createBaseControl(slot_prefix_ + "distortion_type");
      Output* distortion_amount = createPolyModControl(slot_prefix_ + "distortion_amount");
      Value* secondary_distortion_type = createBaseControl(slot_prefix_ + "secondary_distortion_type");
      Output* secondary_distortion_amount = createPolyModControl(slot_prefix_ + "secondary_distortion_amount");
      source_->plug(distortion_type, SpeechSource::kDistortionType);
      source_->plug(distortion_amount, SpeechSource::kDistortionAmount);
      source_->plug(secondary_distortion_type, SpeechSource::kSecondaryDistortionType);
      source_->plug(secondary_distortion_amount, SpeechSource::kSecondaryDistortionAmount);
    }
    source_->useOutput(output(kRaw), SpeechSource::kRaw);
    source_->useOutput(output(kLevelled), SpeechSource::kLevelled);
    source_->useOutput(output(kHold), SpeechSource::kHold);

    addProcessor(source_);
    SynthModule::init();
  }

  void SpeechModule::setModulatorOutput(int position, Output* output) {
    source_->modulation().setModulatorOutput(position, output);
  }
} // namespace vital
