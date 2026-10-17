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
    Value* amp_env = createBaseControl(prefix_ + "amp_env");
    const std::string mix_prefix = slot_prefix_.empty() ? prefix_ : slot_prefix_;
    Output* level = createPolyModControl(mix_prefix + "level", true, true);
    Output* pan = createPolyModControl(mix_prefix + "pan");

    source_->useInput(input(kReset), SpeechSource::kReset);
    source_->useInput(input(kRetrigger), SpeechSource::kRetrigger);
    source_->useInput(input(kMidi), SpeechSource::kMidi);
    source_->plug(amp_env, SpeechSource::kAmpEnvMode);
    source_->plug(level, SpeechSource::kLevel);
    source_->plug(pan, SpeechSource::kPan);
    source_->useOutput(output(kRaw), SpeechSource::kRaw);
    source_->useOutput(output(kLevelled), SpeechSource::kLevelled);
    source_->useOutput(output(kHold), SpeechSource::kHold);

    addProcessor(source_);
    SynthModule::init();
  }
} // namespace vital
