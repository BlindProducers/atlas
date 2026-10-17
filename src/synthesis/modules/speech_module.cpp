// SPDX-License-Identifier: GPL-3.0-or-later
#include "speech_module.h"

#include "speech_source.h"

namespace vital {
  const char* const SpeechModule::kRenderSuffixes[] = {
    "language", "gender", "head_size", "pitch_baseline", "pitch_fluctuation", "roughness", "breathiness", "speed",
    "volume", "annotations"
  };
  const int SpeechModule::kNumRenderSuffixes =
      static_cast<int>(sizeof(SpeechModule::kRenderSuffixes) / sizeof(SpeechModule::kRenderSuffixes[0]));

  SpeechModule::SpeechModule(std::string prefix, std::shared_ptr<SpeechSlot> slot, std::string slot_prefix) :
      SynthModule(kNumInputs, kNumOutputs), prefix_(std::move(prefix)), slot_prefix_(std::move(slot_prefix)),
      slot_(slot ? std::move(slot) : std::make_shared<SpeechSlot>()) {
    source_ = new SpeechSource(slot_);
  }

  void SpeechModule::init() {
    // Read by SpeechManager, which renders the text again when one changes.
    for (int i = 0; i < kNumRenderSuffixes; ++i)
      createBaseControl(prefix_ + kRenderSuffixes[i]);

    Value* amp_env = createBaseControl(prefix_ + "amp_env");
    Value* split = createBaseControl(prefix_ + "split");
    Value* sustain = createBaseControl(prefix_ + "sustain");
    Value* loop = createBaseControl(prefix_ + "loop");
    Value* restart_on_release = createBaseControl(prefix_ + "restart_on_release");
    Value* pitch_mode = createBaseControl(prefix_ + "pitch_mode");
    Output* intonation = createPolyModControl(prefix_ + "intonation");
    Output* rate = createPolyModControl(prefix_ + "rate");
    Value* quality = createBaseControl(prefix_ + "quality");
    Output* transpose = createPolyModControl(prefix_ + "transpose");
    Output* tune = createPolyModControl(prefix_ + "tune");
    const std::string mix_prefix = slot_prefix_.empty() ? prefix_ : slot_prefix_;
    Output* level = createPolyModControl(mix_prefix + "level", true, true);
    Output* pan = createPolyModControl(mix_prefix + "pan");

    source_->useInput(input(kReset), SpeechSource::kReset);
    source_->useInput(input(kVoiceEvent), SpeechSource::kVoiceEvent);
    source_->useInput(input(kMidi), SpeechSource::kMidi);
    source_->useInput(input(kRetrigger), SpeechSource::kRetrigger);
    source_->plug(amp_env, SpeechSource::kAmpEnvMode);
    source_->plug(split, SpeechSource::kSplit);
    source_->plug(sustain, SpeechSource::kSustain);
    source_->plug(loop, SpeechSource::kLoop);
    source_->plug(restart_on_release, SpeechSource::kRestartOnRelease);
    source_->plug(pitch_mode, SpeechSource::kPitchMode);
    source_->plug(intonation, SpeechSource::kIntonation);
    source_->plug(rate, SpeechSource::kRate);
    source_->plug(quality, SpeechSource::kQuality);
    source_->plug(transpose, SpeechSource::kTranspose);
    source_->plug(tune, SpeechSource::kTune);
    source_->plug(level, SpeechSource::kLevel);
    source_->plug(pan, SpeechSource::kPan);
    if (!slot_prefix_.empty()) {
      // Slot-level RM reads the slot's shared distortion controls.
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
    // The slot's clock runs once per block in the mono router, ahead of the voices.
    addMonoProcessor(new SpeechClock(slot_));
    SynthModule::init();
  }

  void SpeechModule::setModulatorOutput(int position, Output* output) {
    source_->modulation().setModulatorOutput(position, output);
  }
} // namespace vital
