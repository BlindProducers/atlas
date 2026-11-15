// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "slot_modulation.h"
#include "synth_module.h"
#include "piano_instrument.h"

#include <memory>
#include <string>

namespace vital {
  struct MultiSampleMidiState;
  class PianoSource;

  // The Piano oscillator type: a modelled concert grand struck on every note.
  // `prefix` names its controls ("osc_3_piano_"); level, pan and the RM
  // distortion pairs are the slot's shared controls. Tuning, Octave stretch,
  // Unison width and Sustain length rebuild the model, in the background
  // (pm::ModelService); the rest are read as the notes play. The slot's
  // Instrument is shared by every voice and renders the sympathetic strings
  // once per block from the voice handler.
  class PianoModule : public SynthModule, public SlotModulationTarget {
    public:
      enum {
        kReset,
        kVoiceEvent,
        kMidi,
        kVelocity,
        kActiveVoices,
        kKeyLifted,
        kNumInputs
      };

      enum {
        kRaw,
        kLevelled,
        kHold,
        kNumOutputs
      };

      PianoModule(std::string prefix, std::string slot_prefix);
      virtual ~PianoModule();

      void init() override;
      void setModulatorOutput(int position, Output* output) override;
      Processor* clone() const override { return new PianoModule(*this); }

      const std::shared_ptr<pm::Instrument>& instrument() const { return instrument_; }
      // Audio thread, before the voices render a block: the model the voicing
      // controls ask for at this rate, and the block's pedal moves from its
      // MIDI (`oversample` times the controllers' host-rate offsets).
      void wantModel(float sample_rate) const;
      void pedalEvents(const MultiSampleMidiState* midi, int oversample) const;
      // The engine's note (0 to 87, or outside) a voice at this tuned note strikes.
      int noteFor(float tuned_note) const;

    protected:
      std::string prefix_;
      std::string slot_prefix_;
      std::shared_ptr<pm::Instrument> instrument_;
      PianoSource* source_;
      Output* transpose_ = nullptr;
      Value* tuning_ = nullptr;
      Value* stretch_ = nullptr;
      Value* unison_ = nullptr;
      Value* sustain_ = nullptr;

      JUCE_LEAK_DETECTOR(PianoModule)
  };
} // namespace vital
