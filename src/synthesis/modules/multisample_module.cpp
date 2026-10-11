// SPDX-License-Identifier: GPL-3.0-or-later
#include "multisample_module.h"

#include "multisample_slot.h"
#include "multisample_source.h"

namespace vital {
  MultiSampleModule::MultiSampleModule(std::string prefix, std::shared_ptr<MultiSampleSlot> slot,
                                       std::string slot_prefix) :
      SynthModule(kNumInputs, kNumOutputs), prefix_(std::move(prefix)), slot_prefix_(std::move(slot_prefix)) {
    // The type probe builds a module without a slot; it still needs a holder.
    source_ = new MultiSampleSource(slot ? std::move(slot) : std::make_shared<MultiSampleSlot>());
  }

  void MultiSampleModule::init() {
    Value* amp_env = createBaseControl(prefix_ + "amp_env");
    Output* transpose = createPolyModControl(prefix_ + "transpose");
    Output* tune = createPolyModControl(prefix_ + "tune");
    const std::string mix_prefix = slot_prefix_.empty() ? prefix_ : slot_prefix_;
    Output* level = createPolyModControl(mix_prefix + "level", true, true);
    Output* pan = createPolyModControl(mix_prefix + "pan");

    source_->useInput(input(kReset), MultiSampleSource::kReset);
    source_->useInput(input(kRetrigger), MultiSampleSource::kRetrigger);
    source_->useInput(input(kVoiceEvent), MultiSampleSource::kVoiceEvent);
    source_->useInput(input(kMidi), MultiSampleSource::kMidi);
    source_->useInput(input(kNote), MultiSampleSource::kNote);
    source_->useInput(input(kMidiKey), MultiSampleSource::kMidiKey);
    source_->useInput(input(kVelocity), MultiSampleSource::kVelocity);
    source_->plug(amp_env, MultiSampleSource::kAmpEnvMode);
    source_->plug(transpose, MultiSampleSource::kTranspose);
    source_->plug(tune, MultiSampleSource::kTune);
    source_->plug(level, MultiSampleSource::kLevel);
    source_->plug(pan, MultiSampleSource::kPan);
    source_->useOutput(output(kRaw), MultiSampleSource::kRaw);
    source_->useOutput(output(kLevelled), MultiSampleSource::kLevelled);
    source_->useOutput(output(kHold), MultiSampleSource::kHold);

    addProcessor(source_);
    SynthModule::init();
  }
} // namespace vital
