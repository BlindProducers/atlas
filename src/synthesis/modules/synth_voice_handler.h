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
#include "producers_module.h"
#include "voice_handler.h"
#include "wavetable.h"
#include "synth_types.h"
#include "line_generator.h"

#include <vector>

namespace vital {
  class AudioRateEnvelope;
  class FilterModule;
  class FiltersModule;
  class LegatoFilter;
  class LineMap;
  class LfoModule;
  class EnvelopeModule;
  class RandomLfoModule;
  class TriggerRandom;

  class SynthVoiceHandler : public VoiceHandler {
    public:
      SynthVoiceHandler(Output* beats_per_second);
      virtual ~SynthVoiceHandler() { }

      virtual Processor* clone() const override { VITAL_ASSERT(false); return nullptr; }

      void init() override;
      void prepareDestroy();

      void process(int num_samples) override;
      void noteOn(int note, mono_float velocity, int sample, int channel) override;
      void noteOff(int note, mono_float lift, int sample, int channel) override;
      bool shouldAccumulate(Output* output) override;
      void correctToTime(double seconds) override;
      void disableUnnecessaryModSources();
      void disableModSource(const std::string& source);

      output_map getPolyModulations() override;
      ModulationConnectionBank& getModulationBank() { return modulation_bank_; }
      Wavetable* getWavetable(int index) { return producers_->getWavetable(index); }
      // Filter Table: the table Filter 1 (0) or Filter 2 (1) reads, shared by all voices.
      void setFilterTable(int index, Wavetable* table);
      // The voices' Reverb filter memory (about 330 MB at 48 kHz per filter)
      // exists only while that filter selects the Reverb model. Update and set
      // run on the message thread with processing locked, or offline.
      void updateReverbMemory();
      void setReverbMemory(int index, bool reserve);
      bool takeReverbMemoryRequest();
      Sample* getSample(int index) { return producers_->getSample(index); }
      SourceSlotModule* getSourceSlot(int index) { return producers_->slot(index); }
      // Message thread with processing paused: build every slot's active type module.
      bool prepareSelectedSources() { return producers_->prepareSelectedSources(); }
      LineGenerator* getLfoSource(int index) { return &lfo_sources_[index]; }
      Output* getDirectOutput() { return getAccumulatedOutput(direct_output_->output()); }
      Output* getBusOutput(int index) { return getAccumulatedOutput(bus_outputs_[index]->output()); }
      Output* getIndependentRandomModulationSource(const std::string& source, int connection_index);

      Output* note_retrigger() { return &note_retriggered_; }

      Output* midi_offset_output() { return midi_offset_output_; }

      void enableModulationConnection(ModulationConnectionProcessor* processor);
      void disableModulationConnection(ModulationConnectionProcessor* processor);
      // A route from slot `source`'s audio; see ProducersModule::addOscillatorRoute.
      void addOscillatorRoute(ModulationConnectionProcessor* processor, int source, int destination);
      void removeOscillatorRoute(ModulationConnectionProcessor* processor);
      CircularQueue<ModulationConnectionProcessor*>& enabledModulationConnection() {
        return enabled_modulation_processors_;
      }

    private:
      // Filter 1 (0) or 2 (1); null before init().
      FilterModule* voiceFilter(int index);
      // Tells the producers which slots' playheads the enabled routes read.
      void updatePlayheadRoutes();
      void createNoteArticulation();
      void createProducers();
      void createModulators();
      void createVoiceOutput();
      void createFilters(Output* keytrack);

      void setupPolyModulationReadouts();
      // Mixes Multi-sample CC-triggered regions (pedal noises), which play
      // outside any voice, into the voice outputs.
      void mixMultiSampleTails(int num_samples, bool voices_rendered);

      struct TailControls {
        Value* on = nullptr;
        Value* type = nullptr;
        Value* solo = nullptr;
        Value* level = nullptr;
        Value* pan = nullptr;
        Value* destination = nullptr;
      };
      TailControls tail_controls_[kNumOscillators];
      bool tails_written_ = false;

      ModulationConnectionBank modulation_bank_;
      CircularQueue<ModulationConnectionProcessor*> enabled_modulation_processors_;
      ProducersModule* producers_;
      Output* beats_per_second_;

      Processor* note_from_reference_;
      Output* midi_offset_output_;
      Processor* bent_midi_;
      Processor* current_midi_note_;
      EnvelopeModule* amplitude_envelope_;
      Processor* amplitude_;
      Processor* pitch_wheel_;

      FiltersModule* filters_module_;

      LfoModule* lfos_[kNumLfos];
      EnvelopeModule* envelopes_[kNumEnvelopes];

      Output note_retriggered_;

      LineGenerator lfo_sources_[kNumLfos];

      TriggerRandom* random_;
      RandomLfoModule* random_lfos_[kNumRandomLfos];

      LineMap* note_mapping_;
      LineMap* velocity_mapping_;
      LineMap* aftertouch_mapping_;
      LineMap* slide_mapping_;
      LineMap* lift_mapping_;
      LineMap* mod_wheel_mapping_;
      LineMap* pitch_wheel_mapping_;

      cr::Value* stereo_;
      cr::Multiply* note_percentage_;

      Add* output_;
      Add* direct_output_;
      Add* bus_outputs_[kNumBuses];
      Output num_voices_;

      output_map poly_readouts_;
      poly_mask last_active_voice_mask_;

      JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SynthVoiceHandler)
  };
} // namespace vital
