/* Copyright 2013-2019 Matt Tytel
 *
 * vital is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * vital is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with vital.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "synth_module.h"
#include "source_types.h"
#include "synth_oscillator.h"
#include "slot_modulation.h"

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace vital {
  class MultiSampleSlot;
  class OscillatorModule;
  class Sample;
  class Wavetable;

  // One oscillator slot. The slot permanently owns its controls (slot-level
  // switches plus every oscillator type's parameters), its wavetable and its
  // sample. The sound engine for the selected type is built on demand on the
  // message thread and destroyed when the type changes, so an oscillator costs
  // only what its active type needs. Inactive types are just control values.
  class SourceSlotModule : public SynthModule {
    public:
      enum {
        kReset,
        kRetrigger,
        kVoiceEvent,
        kMidi,
        kActiveVoices,
        kNoteCount,
        kNotePressed,
        kVelocity,
        // The tuned note and the played MIDI key (Multi-sample region lookup).
        kNote,
        kMidiKey,
        kNumInputs
      };

      enum {
        kRaw,
        kLevelled,
        // 1 per voice while a Multi-sample instrument or an OpenWurli reed still
        // sounds (holds the voice open).
        kHold,
        kNumOutputs
      };

      static constexpr int kNumModulatorPositions = SynthOscillator::kNumModulatorPositions;

      SourceSlotModule(int slot, const Output* beats_per_second = nullptr);
      virtual ~SourceSlotModule() { }

      void init() override;
      void process(int num_samples) override;
      Processor* clone() const override { return new SourceSlotModule(*this); }

      // Message thread, with processing paused. Builds the module for the type
      // the `type` control requests if it differs from the active one. Returns
      // true when the active module was replaced.
      bool prepareActiveType();
      int requestedType() const;
      int activeType() const { return state_->active_type; }
      // A Multi-sample type in Instrument envelope mode, or OpenWurli in Reed
      // mode: its own envelopes shape it, not Env 1.
      bool holdsVoiceOpen() const;
      bool hasActiveModule() const { return state_->module != nullptr; }
      int slot() const { return slot_; }
      const std::string& prefix() const { return prefix_; }

      Wavetable* getWavetable() { return wavetable_.get(); }
      Sample* getSample() { return sample_.get(); }
      MultiSampleSlot* getMultiSample() { return multisample_.get(); }
      Output* getPhaseOutput() const { return playhead_->output(); }
      // The slot's audio as an audio-rate modulation source (osc_N_output).
      Output* getModulationOutput() const { return modulation_output_->output(); }
      SynthModule* activeModule() const { return state_->module; }
      OscillatorModule* oscillatorModule() const { return state_->oscillator; }

      // Position k is the k-th other slot in ascending order; see SynthOscillator.
      void setModulatorOutput(int position, Output* output);
      // The modulator position the active type reads for its primary or
      // secondary distortion (FM or RM from another slot), or -1 when it reads none.
      int modulatorPosition(bool secondary) const;

      // The permanent control values of one type group, by full parameter name.
      const control_map& typeControls(int type) const;

    private:
      struct TypeControls {
        control_map controls;
        std::vector<Processor*> smoothers;
        bool created = false;
      };

      struct ActiveState {
        SynthModule* module = nullptr;
        OscillatorModule* oscillator = nullptr;
        Output* module_phase = nullptr;
        // Slot-level FM/RM target for the non-oscillator types.
        SlotModulationTarget* modulation_target = nullptr;
        int active_type = -1;
        std::array<Output*, kNumModulatorPositions> modulators {};
      };

      static const std::vector<ControlSpec>& describeType(int type);
      static SynthModule* createTypeModuleFor(int slot, int type, std::shared_ptr<Wavetable> wavetable,
                                              std::shared_ptr<Sample> sample, const Output* beats_per_second,
                                              std::shared_ptr<MultiSampleSlot> multisample = nullptr);
      SynthModule* createTypeModule(int type) const;
      void connectTypeModule(SynthModule* module, int type);
      void destroyActiveModule();
      void createPermanentControls(int type);
      void setTypeControlsEnabled(int type, bool enabled);

      int slot_;
      std::string prefix_;
      const Output* beats_per_second_;
      std::shared_ptr<Wavetable> wavetable_;
      std::shared_ptr<Sample> sample_;
      std::shared_ptr<MultiSampleSlot> multisample_;
      // Owns the playhead output so it can serve as a modulation source: the
      // engine enables and queries a source's owner when a route connects.
      class PlayheadOutput : public Processor {
        public:
          PlayheadOutput() : Processor(0, 1, true) { }
          void process(int) override { }
          Processor* clone() const override { return new PlayheadOutput(*this); }
      };
      std::shared_ptr<PlayheadOutput> playhead_;
      // Owns the audio modulation output, one signal per voice. No router owns
      // it, so connecting a route never reorders the voice graph around it:
      // ProducersModule runs every route from a slot's audio itself.
      class ModulationOutput : public Processor {
        public:
          ModulationOutput() : Processor(0, 1, false, kMaxOversample) { }
          void process(int) override { }
          bool isPolyphonic() const override { return true; }
          Processor* clone() const override { return new ModulationOutput(*this); }
      };
      std::shared_ptr<ModulationOutput> modulation_output_;
      // Writes the slot's raw audio as a modulation value, 0 to 1 around 0.5.
      void writeModulationOutput(int num_samples);
      Value* on_ = nullptr;
      Value* type_ = nullptr;
      // The shared distortion controls, read for render ordering whatever the type.
      Value* distortion_type_ = nullptr;
      Value* multisample_amp_env_ = nullptr;
      Value* wurli_amp_env_ = nullptr;
      Value* secondary_distortion_type_ = nullptr;
      // The output buffers are shared by every voice copy, so the "clear once
      // after switching off" flag must be shared too: the copy that last wrote
      // them may never run again (its voice was killed), and another copy has
      // to do the clearing or every voice keeps mixing the stale block.
      std::shared_ptr<bool> was_on_;
      // Per copy: the spectral engine hard-resets its own voice state when switched on.
      bool clone_was_on_ = false;
      std::shared_ptr<ActiveState> state_;
      std::shared_ptr<std::array<TypeControls, source_types::kNumTypes>> type_controls_;

      JUCE_LEAK_DETECTOR(SourceSlotModule)
  };
} // namespace vital
