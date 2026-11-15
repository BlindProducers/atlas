// SPDX-License-Identifier: GPL-3.0-or-later
#include "piano_module.h"

#include "multisample_midi_state.h"
#include "piano_model_service.h"
#include "piano_source.h"

#include <cmath>

namespace vital {
  PianoModule::PianoModule(std::string prefix, std::string slot_prefix) :
      SynthModule(kNumInputs, kNumOutputs), prefix_(std::move(prefix)), slot_prefix_(std::move(slot_prefix)),
      instrument_(std::make_shared<pm::Instrument>()) {
    source_ = new PianoSource(instrument_);
    pm::ModelService::add(instrument_);
  }

  PianoModule::~PianoModule() {
    pm::ModelService::remove(instrument_.get());
  }

  void PianoModule::init() {
    Value* amp_env = createBaseControl(prefix_ + "amp_env");
    Output* dynamics = createPolyModControl(prefix_ + "dynamics");
    Output* hardness = createPolyModControl(prefix_ + "hardness");
    Output* action_noise = createPolyModControl(prefix_ + "action_noise");
    Output* sympathetic = createPolyModControl(prefix_ + "sympathetic");
    Output* width = createPolyModControl(prefix_ + "width");
    // Read on the audio thread when a block starts; a change rebuilds the model.
    tuning_ = createBaseControl(prefix_ + "tuning");
    stretch_ = createBaseControl(prefix_ + "stretch");
    unison_ = createBaseControl(prefix_ + "unison");
    sustain_ = createBaseControl(prefix_ + "sustain");
    transpose_ = createPolyModControl(prefix_ + "transpose");
    Output* transpose = transpose_;
    const std::string mix_prefix = slot_prefix_.empty() ? prefix_ : slot_prefix_;
    Output* level = createPolyModControl(mix_prefix + "level", true, true);
    Output* pan = createPolyModControl(mix_prefix + "pan");

    source_->useInput(input(kReset), PianoSource::kReset);
    source_->useInput(input(kVoiceEvent), PianoSource::kVoiceEvent);
    source_->useInput(input(kMidi), PianoSource::kMidi);
    source_->useInput(input(kVelocity), PianoSource::kVelocity);
    source_->useInput(input(kActiveVoices), PianoSource::kActiveVoices);
    source_->useInput(input(kKeyLifted), PianoSource::kKeyLifted);
    source_->plug(amp_env, PianoSource::kAmpEnvMode);
    source_->plug(dynamics, PianoSource::kDynamics);
    source_->plug(hardness, PianoSource::kHardness);
    source_->plug(action_noise, PianoSource::kActionNoise);
    source_->plug(sympathetic, PianoSource::kSympathetic);
    source_->plug(width, PianoSource::kWidth);
    source_->plug(transpose, PianoSource::kTranspose);
    source_->plug(level, PianoSource::kLevel);
    source_->plug(pan, PianoSource::kPan);
    if (!slot_prefix_.empty()) {
      // Slot-level RM reads the slot's shared distortion controls.
      Value* distortion_type = createBaseControl(slot_prefix_ + "distortion_type");
      Output* distortion_amount = createPolyModControl(slot_prefix_ + "distortion_amount");
      Value* secondary_distortion_type = createBaseControl(slot_prefix_ + "secondary_distortion_type");
      Output* secondary_distortion_amount = createPolyModControl(slot_prefix_ + "secondary_distortion_amount");
      source_->plug(distortion_type, PianoSource::kDistortionType);
      source_->plug(distortion_amount, PianoSource::kDistortionAmount);
      source_->plug(secondary_distortion_type, PianoSource::kSecondaryDistortionType);
      source_->plug(secondary_distortion_amount, PianoSource::kSecondaryDistortionAmount);
    }
    source_->useOutput(output(kRaw), PianoSource::kRaw);
    source_->useOutput(output(kLevelled), PianoSource::kLevelled);
    source_->useOutput(output(kHold), PianoSource::kHold);

    addProcessor(source_);
    SynthModule::init();
  }

  void PianoModule::setModulatorOutput(int position, Output* output) {
    source_->modulation().setModulatorOutput(position, output);
  }

  int PianoModule::noteFor(float tuned_note) const {
    const float transpose = transpose_ != nullptr ? transpose_->buffer[0][0] : 0.0f;
    return static_cast<int>(std::lround(tuned_note + transpose)) - pm::kLowestMidi;
  }

  void PianoModule::pedalEvents(const MultiSampleMidiState* midi, int oversample) const {
    if (midi == nullptr)
      return;
    pm::Instrument& instrument = *instrument_;
    static constexpr int kSustain = 64, kSostenuto = 66, kSoft = 67;
    const auto pedal = [](int cc) {
      return cc == kSustain ? pm::Instrument::kSustainPedal
                            : cc == kSostenuto ? pm::Instrument::kSostenutoPedal : pm::Instrument::kSoftPedal;
    };
    // All Controllers Off: a pedal the block starts without is up.
    for (int cc : { kSustain, kSostenuto, kSoft }) {
      if (!midi->received[static_cast<size_t>(cc)])
        instrument.pedalEvent(pedal(cc), 0.0f, 0);
    }
    for (int i = 0; i < midi->num_events; ++i) {
      const MultiSampleMidiState::Event& event = midi->events[static_cast<size_t>(i)];
      if (event.cc == kSustain || event.cc == kSostenuto || event.cc == kSoft)
        instrument.pedalEvent(pedal(event.cc), event.value / 127.0f, event.offset * oversample);
    }
  }

  void PianoModule::wantModel(float sample_rate) const {
    pm::Instrument& instrument = *instrument_;
    pm::Instrument::Wanted wanted;
    wanted.sampleRate = sample_rate;
    wanted.a4 = tuning_->value();
    wanted.stretch = stretch_->value();
    wanted.unison = unison_->value();
    wanted.sustain = sustain_->value();
    instrument.want(wanted);
  }
} // namespace vital
