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

#include "circular_queue.h"
#include "synth_constants.h"
#include "synth_module.h"
#include "note_handler.h"
#include "trance_gate_clock.h"
#include "note_monitor.h"

#include <atomic>
#include <memory>

class LineGenerator;
class Tuning;

namespace vital {
  class ConvolutionReverbModule;
  class ConvolutionIrWorker;
  class EffectPreparationWorker;
  class Decimator;
  class PeakMeter;
  class MultiSampleSlot;
  class SpeechSlot;
  struct MultiSampleMidiState;
  class Sample;
  class SourceSlotModule;
  class BusRouter;
  class LatencyCompensatedSum;
  class ReorderableEffectChain;
  class StereoMemory;
  class SynthVoiceHandler;
  class SynthLfo;
  class Value;
  class ValueSwitch;
  class WaveFrame;
  class Wavetable;

  class SoundEngine : public SynthModule, public NoteHandler {
    public:
      static constexpr int kDefaultOversamplingAmount = 2;
      static constexpr int kDefaultSampleRate = 44100;

      SoundEngine();
      virtual ~SoundEngine();

      void init() override;
      void process(int num_samples) override;
      void correctToTime(double seconds) override;
      // Voice and modulation clocks run from this origin: 0 follows host
      // time, otherwise the time of the first note played from silence while
      // the transport is stopped, so synced LFOs start with that note.
      void setVoiceClockOrigin(double seconds) { voice_clock_origin_ = seconds; }
      double voiceClockOrigin() const { return voice_clock_origin_; }
      void setTransportPlaying(bool playing) { trance_gate_clock_.setPlaying(playing); }

      int getNumPressedNotes();
      void connectModulation(const modulation_change& change);
      void disconnectModulation(const modulation_change& change);
      int getNumActiveVoices();
      ModulationConnectionBank& getModulationBank();
      Output* getIndependentRandomModulationSource(const std::string& source, int connection_index);
      mono_float getLastActiveNote() const;

      void setTuning(const Tuning* tuning);

      void allSoundsOff() override;
      void allNotesOff(int sample) override;
      void allNotesOff(int sample, int channel) override;
      void allNotesOffRange(int sample, int from_channel, int to_channel);

      void noteOn(int note, mono_float velocity, int sample, int channel) override;
      void noteOff(int note, mono_float lift, int sample, int channel) override;
      void setModWheel(mono_float value, int channel);
      // Any MIDI controller (all channels): the Multi-sample CC state.
      void controlChange(int controller, int value, int sample);
      void setModWheelAllChannels(mono_float value);
      void setPitchWheel(mono_float value, int channel);
      void setZonedPitchWheel(mono_float value, int from_channel, int to_channel);
      void disableUnnecessaryModSources();
      void enableModSource(const std::string& source);
      void disableModSource(const std::string& source);
      bool isModSourceEnabled(const std::string& source);
      const StereoMemory* getEqualizerMemory();
      std::vector<std::pair<std::string, ConvolutionReverbModule*>> getConvolutionReverbModules();
      // Filter Table: gives the filter named by `prefix` ("filter_1", "filter_2",
      // "filter_fx", "fx_slot_3_filter_fx", "bus_2_fx_slot_1_filter_fx") its table.
      void setFilterTable(const std::string& prefix, Wavetable* table);
      // Allocates a Filter FX slot's Filter Table buffers (message thread).
      void prepareFilterTable(const std::string& prefix);

      void setBpm(mono_float bpm);
      void setAftertouch(mono_float note, mono_float value, int sample, int channel);
      void setChannelAftertouch(int channel, mono_float value, int sample);
      void setChannelRangeAftertouch(int from_channel, int to_channel, mono_float value, int sample);
      void setChannelSlide(int channel, mono_float value, int sample);
      void setChannelRangeSlide(int from_channel, int to_channel, mono_float value, int sample);
      Wavetable* getWavetable(int index);
      Sample* getSample(int index);
      SourceSlotModule* getSourceSlot(int index);
      MultiSampleSlot* getMultiSample(int index);
      SpeechSlot* getSpeech(int index);
      // Message thread with processing paused: build every oscillator slot's active type module.
      bool prepareSelectedSources();
      // Message thread with processing paused: bring every voice copy of the
      // graph up to date with the slots' type modules.
      void syncVoiceGraphs();
      LineGenerator* getLfoSource(int index);

      void sustainOn(int channel);
      void sustainOff(int sample, int channel);
      void sostenutoOn(int channel);
      void sostenutoOff(int sample, int channel);

      void sustainOnRange(int from_channel, int to_channel);
      void sustainOffRange(int sample, int from_channel, int to_channel);
      void sostenutoOnRange(int from_channel, int to_channel);
      void sostenutoOffRange(int sample, int from_channel, int to_channel);
      force_inline int getOversamplingAmount() const { return last_oversampling_amount_; }

      void checkOversampling();
      // Both also give the voice filters' Reverb model its memory, or free it,
      // as their models stand (see SynthVoiceHandler::updateReverbMemory).
      void prepareSelectedEffects();
      void reserveSelectedEffectControls();
      // See ReorderableEffectChain: parks the effects no slot selects, and
      // wires a parked effect's parameter for a route. Message thread, with
      // processing stopped or the callback lock held.
      void parkUnselectedEffects();
      bool wireModulationDestination(const std::string& name);
      // Whether the audio thread met a selected effect whose controls are not
      // reserved, a voice Reverb filter without its memory, or a latency
      // compensation line without the history its delay needs.
      bool takeEffectControlReservationRequest();
      // Voice filter 0 or 1's Reverb memory; message thread, callback lock held.
      void setVoiceReverbMemory(int filter, bool reserve);
      // Aligns parallel paths for the current effect latencies. Off the audio
      // thread it also gives compensation lines the history their delays need.
      void updateEffectLatency();
      // Host-rate samples, after parallel routing has been compensated.
      int getLatencySamples() const override { return effect_latency_samples_; }

    private:
      void setOversamplingAmount(int oversampling_amount, int sample_rate);
      void applyEffectLatency();
      bool latencyHistoryNeeded() const;
      // Allocates; processing stopped or the callback lock held, or offline.
      void reserveLatencyHistory();

      std::shared_ptr<ConvolutionIrWorker> convolution_worker_;
      std::unique_ptr<MultiSampleMidiState> multisample_midi_state_;
      std::shared_ptr<EffectPreparationWorker> preparation_worker_;
      SynthVoiceHandler* voice_handler_;
      ReorderableEffectChain* effect_chain_;
      Value scream_midi_note_ { -1.0f };
      TranceGateClock trance_gate_clock_;
      double voice_clock_origin_ = 0.0;
      NoteMonitor note_monitor_;
      ReorderableEffectChain* bus_effect_chains_[kNumBuses];
      BusRouter* bus_routers_[kNumBuses];
      LatencyCompensatedSum* bus_chain_inputs_[kNumBuses];
      LatencyCompensatedSum* main_chain_input_;
      LatencyCompensatedSum* output_total_;
      int effect_latency_samples_ = 0;
      std::atomic<bool> latency_history_requested_ { false };

      int last_oversampling_amount_;
      int last_sample_rate_;
      Value* oversampling_;
      Value* bps_;
      Value* legato_;
      Decimator* decimator_;
      PeakMeter* peak_meter_;

      CircularQueue<Processor*> modulation_processors_;
      FixedProcessorSet refreshed_idle_sources_;

      JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SoundEngine)
  };
} // namespace vital
