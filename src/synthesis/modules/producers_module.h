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
#include "operators.h"
#include "source_slot_module.h"

namespace vital {
  class ModulationConnectionProcessor;
  class VoiceHandler;

  class ProducersModule : public SynthModule {
    public:
      enum {
        kReset,
        kRetrigger,
        kVoiceEvent,
        kMidi,
        kZoneMidi,
        kZoneGlideMidi,
        kVelocity,
        kActiveVoices,
        kNoteCount,
        kNotePressed,
        kChannel,
        // The played MIDI key, for Multi-sample region lookup.
        kMidiKey,
        // Env 1, the voice's amplitude envelope.
        kAmplitudeEnvelope,
        kNumInputs
      };

      enum {
        kToFilter1,
        kToFilter2,
        kRawOut,
        kBus1Out,
        kBus2Out,
        kBus3Out,
        kDirectOut,
        // Env 1, raised to full while a Multi-sample instrument in Instrument
        // envelope mode still sounds in the voice. The voice amplitude follows
        // it, so the voice lives as long as the instrument sounds.
        kGatedAmplitudeEnvelope,
        kNumOutputs
      };

      // Modulator position k of slot `index` is the k-th other slot in ascending order.
      static force_inline int getModulationIndex(int index, int position) {
        for (int i = 0, slot = 0; i < kNumOscillators; ++i) {
          if (i == index)
            continue;
          if (slot == position)
            return i;
          ++slot;
        }
        return 0;
      }

      // The inverse: which modulator position of slot `index` reads slot `source`, or -1.
      static force_inline int getModulationPosition(int index, int source) {
        if (source == index)
          return -1;
        return source < index ? source : source - 1;
      }

      explicit ProducersModule(const Output* beats_per_second = nullptr);
      void correctToTime(double seconds) override;
      virtual ~ProducersModule() { }

      void process(int num_samples) override;
      void init() override;
      virtual Processor* clone() const override { return new ProducersModule(*this); }

      SourceSlotModule* slot(int index) { return slots_[index]; }
      Wavetable* getWavetable(int index) { return slots_[index]->getWavetable(); }
      Sample* getSample(int index) { return slots_[index]->getSample(); }
      Output* slotPhaseOutput(int index) { return slots_[index]->getPhaseOutput(); }
      Output* slotModulationOutput(int index) { return slots_[index]->getModulationOutput(); }

      // Routes from a slot's audio (osc_N_output) run here, in render order: a
      // route into another slot's controls just before that slot renders, any
      // other route just after its source renders. The voice graph skips them.
      // `destination` is the slot whose control the route reaches, or -1.
      // Audio thread, between blocks.
      void addOscillatorRoute(ModulationConnectionProcessor* processor, int source, int destination);
      void removeOscillatorRoute(ModulationConnectionProcessor* processor);
      // Each voice runs its own copies of the routes, found in the voice graph
      // copy this handler is processing.
      void setVoiceHandler(const VoiceHandler* voice_handler) { voice_handler_ = voice_handler; }

      // Message thread with processing paused: build every slot's active type module.
      bool prepareSelectedSources();

      void setFilter1On(const Value* on) { filter1_on_ = on; }
      void setFilter2On(const Value* on) { filter2_on_ = on; }
      void setFilterInputs(const Value* filter1_input, const Value* filter2_input) {
        filter1_input_ = filter1_input;
        filter2_input_ = filter2_input;
      }

    protected:
      // Per-block start/end zone gain used to crossfade a producer in or out of its zone.
      struct ZoneGain {
        poly_float start;
        poly_float end;
      };

      bool isFilter1On() { return filter1_on_ == nullptr || filter1_on_->value() > 0.5f; }
      bool isFilter2On() { return filter2_on_ == nullptr || filter2_on_->value() > 0.5f; }
      bool arpDestinationOn(int arp, int destination) const;
      bool anyArpDestinationOn() const;
      bool sourceHasActiveArp(int destination) const;
      poly_float getSourcePlaybackMask(int destination, poly_float zone_mask) const;
      poly_float getZoneMask(Value* key_start, Value* key_end, Value* velocity_start, Value* velocity_end);
      void plugPitchClamp(cr::ZonePitchClamp* clamp, Value* key_start, Value* key_end);
      ZoneGain advanceZoneGain(poly_float& gain_state, poly_float target, poly_mask reset_mask,
                               bool crossfade_on, mono_float max_move);
      // `ratio`, when given, scales each sample (slots that follow Env 1 while an instrument holds the voice).
      void addZonedBuffer(poly_float* dest, const poly_float* source, const ZoneGain& gain, int num_samples,
                          const poly_float* ratio = nullptr) const;
      // Runs this voice's routes into `slot` (into_slot) or out of it to elsewhere.
      void runOscillatorRoutes(int slot, bool into_slot, int num_samples);

      struct OscillatorRoute {
        ModulationConnectionProcessor* processor = nullptr;
        int source = -1;
        int destination = -1;
      };
      // Shared by every voice copy.
      std::shared_ptr<std::vector<OscillatorRoute>> oscillator_routes_;
      const VoiceHandler* voice_handler_ = nullptr;

      SourceSlotModule* slots_[kNumOscillators];
      Value* slot_destinations_[kNumOscillators];
      Value* slot_solo_[kNumOscillators];
      Value* arp_on_[2];
      Value* arp_destinations_[2][kNumOscillators];
      Value* slot_key_zone_start_[kNumOscillators];
      Value* slot_key_zone_end_[kNumOscillators];
      Value* slot_velocity_zone_start_[kNumOscillators];
      Value* slot_velocity_zone_end_[kNumOscillators];
      cr::ZonePitchClamp* slot_pitch_clamp_[kNumOscillators];
      poly_float slot_zone_gain_[kNumOscillators];

      static constexpr mono_float kInstrumentGateSeconds = 0.005f;
      poly_float envelope_gate_ = 0.0f;

      Value* portamento_glide_zones_;
      Value* zone_crossfade_;

      const Value* filter1_on_;
      const Value* filter2_on_;
      const Value* filter1_input_ = nullptr;
      const Value* filter2_input_ = nullptr;

      JUCE_LEAK_DETECTOR(ProducersModule)
  };
} // namespace vital
