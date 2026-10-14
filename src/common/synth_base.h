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

#include "JuceHeader.h"
#include "concurrentqueue/concurrentqueue.h"
#include "atlas_arpeggiator.h"
#include "line_generator.h"
#include "synth_constants.h"
#include "synth_types.h"
#include "midi_manager.h"
#include "tuning.h"
#include "wavetable_creator.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <set>
#include <string>

namespace vital {
  class ConvolutionReverbModule;
  class SoundEngine;
  struct Output;
  class StatusOutput;
  class StereoMemory;
  class Sample;
  class WaveFrame;
  class Wavetable;
}

class SynthGuiInterface;

namespace vital {
namespace multisample {
  class Manager;
}
}

class SynthBase : public MidiManager::Listener {
  public:
    static constexpr float kOutputWindowMinNote = 16.0f;
    static constexpr float kOutputWindowMaxNote = 128.0f;

    SynthBase();
    virtual ~SynthBase();

    void valueChanged(const std::string& name, vital::mono_float value);
    void valueChangedThroughMidi(const std::string& name, vital::mono_float value) override;
    void valueMidiLearned(const std::string& name, int midi_id) override;
    void pitchWheelMidiChanged(vital::mono_float value) override;
    void modWheelMidiChanged(vital::mono_float value) override;
    void pitchWheelGuiChanged(vital::mono_float value);
    void modWheelGuiChanged(vital::mono_float value);
    void presetChangedThroughMidi(File preset) override;
    void valueChangedExternal(const std::string& name, vital::mono_float value);
    void valueChangedInternal(const std::string& name, vital::mono_float value);
    bool connectModulation(const std::string& source, const std::string& destination);
    // Builds the modulation wiring of a parked effect's parameter so a route
    // can reach it. Takes the callback lock (the graph changes).
    void prepareModulationDestination(const std::string& destination);
    // Whether oscillator `slot`'s audio (osc_N_output) can modulate `destination`:
    // a per-voice, audio-rate control of another oscillator or of a voice filter.
    bool acceptsOscillatorModulation(int slot, const std::string& destination);
    void connectModulation(vital::ModulationConnection* connection);
    void disconnectModulation(const std::string& source, const std::string& destination);
    void disconnectModulation(vital::ModulationConnection* connection);
    void clearModulations();
    void forceShowModulation(const std::string& source, bool force);
    bool isModSourceEnabled(const std::string& source);
    int getNumModulations(const std::string& destination);
    int getConnectionIndex(const std::string& source, const std::string& destination);
    vital::CircularQueue<vital::ModulationConnection*> getModulationConnections() { return mod_connections_; }
    std::vector<vital::ModulationConnection*> getSourceConnections(const std::string& source);
    bool isSourceConnected(const std::string& source);
    std::vector<vital::ModulationConnection*> getDestinationConnections(const std::string& destination);

    const vital::StatusOutput* getStatusOutput(const std::string& name);

    vital::Wavetable* getWavetable(int index);
    WavetableCreator* getWavetableCreator(int index);
    // Filter Table wavetables, keyed by the filter's prefix ("filter_1",
    // "fx_slot_3_filter_fx", ...). Created on demand; a filter without one uses
    // the built-in default low-pass table.
    WavetableCreator* getFilterTableCreator(const std::string& prefix, bool create);
    std::vector<std::string> filterTablePrefixes() const;
    void resetFilterTable(const std::string& prefix);
    void clearFilterTables();
    // "fx_slot_3_filter_fx_model" -> "fx_slot_3_filter_fx"; empty when `name` is no filter model control.
    static std::string filterPrefixForModelControl(const std::string& name);
    void prepareFilterTable(const std::string& prefix);
    vital::Sample* getSample(int index);
    // Loads and tracks each oscillator slot's SFZ/SF2 instrument.
    vital::multisample::Manager* getMultiSampleManager() { return multisample_manager_.get(); }
    // The container preset being read, while its state is applied (its
    // instruments' files are inside it).
    const File& multiSampleContainer() const { return multisample_container_; }
    // Builds the engine for each oscillator slot's selected type and drops
    // modulation routes into parameters of the types it left. Message thread;
    // callers pause processing around it.
    void prepareSelectedSources();
    // Makes each effect slot's selected effect live and parks the others, then
    // prepares the enabled effects' DSP. Processing stopped or the callback
    // lock held.
    void prepareSelectedEffects();
    static bool isSourceTypeControl(const std::string& name);
    // An effect slot's type ("fx_slot_3_type", "bus_1_fx_slot_3_type").
    static bool isEffectSlotTypeControl(const std::string& name);
    LineGenerator* getLfoSource(int index);
    std::vector<std::pair<std::string, vital::ConvolutionReverbModule*>> getConvolutionReverbModules();

    int getSampleRate();
    void initEngine();
    void loadTuningFile(const File& file);
    void loadInitPreset();
    bool loadFromFile(File preset, std::string& error);
    // Renders slot `slot`'s wavetable creator now if the slot plays a wavetable
    // (Wavetable or Analog type); otherwise the creator keeps its state and the
    // table renders when the slot comes back to one of those types.
    void renderWavetable(int slot);
    bool slotUsesWavetable(int slot) const;
    bool isWavetableRenderPending(int slot) const;
    void renderAudioToFile(File file, float seconds, float bpm, std::vector<int> notes, bool render_images);
    void renderAudioForResynthesis(float* data, int samples, int note);
    bool saveCopyToFile(File preset);
    // Writes the current state: plain JSON, or a container holding the
    // instruments' files when an oscillator plays an SFZ/SF2 instrument.
    bool writePresetFile(const File& preset, std::string* error = nullptr);
    bool saveToFile(File preset);
    bool saveToActiveFile();
    void clearActiveFile() { active_file_ = File(); }
    void setActiveFile(const File& file) { active_file_ = file; }
    File getActiveFile() { return active_file_; }

    int takeDownloadedPresetsImported() {
      int count = downloaded_presets_imported_;
      downloaded_presets_imported_ = 0;
      return count;
    }

    void setMpeEnabled(bool enabled);
    virtual void beginChangeGesture(const std::string& name) { }
    virtual void endChangeGesture(const std::string& name) { }
    virtual void setValueNotifyHost(const std::string& name, vital::mono_float value) { }

    void armMidiLearn(const std::string& name);
    void cancelMidiLearn();
    void clearMidiLearn(const std::string& name);
    bool isMidiMapped(const std::string& name);
    // MIDI learn mappings are global (saved in the Atlas settings, not presets).
    std::vector<std::pair<int, std::string>> midiLearnMappings();
    std::string midiLearnArmedName() const;
    void clearAllMidiLearn();

    void setAuthor(const String& author);
    void setComments(const String& comments);
    void setStyle(const String& comments);
    void setTags(const String& tags);
    void setPresetName(const String& preset_name);
    void setMacroName(int index, const String& macro_name);
    String getAuthor();
    String getComments();
    String getStyle();
    String getTags();
    String getPresetName();
    String getMacroName(int index);

    vital::control_map& getControls() { return controls_; }
    vital::SoundEngine* getEngine() { return engine_.get(); }
    MidiKeyboardState* getKeyboardState() { return keyboard_state_.get(); }
    const vital::poly_float* getOscilloscopeMemory() { return oscilloscope_memory_; }
    const vital::StereoMemory* getAudioMemory() { return audio_memory_.get(); }
    void setAudioHistoryEnabled(bool enabled) { audio_history_enabled_.store(enabled, std::memory_order_relaxed); }
    const vital::StereoMemory* getEqualizerMemory();
    vital::ModulationConnectionBank& getModulationBank();
    void notifyOversamplingChanged();
    void checkOversampling();
    void clearArpeggiators();
    virtual const CriticalSection& getCriticalSection() = 0;
    virtual void pauseProcessing(bool pause) = 0;
    Tuning* getTuning() { return &tuning_; }
    uint64_t getScheduledMidiOverflowCount() const { return scheduled_midi_overflow_count_; }

    // Called by the editor's message-thread timer. Audio/MIDI producers only
    // publish into the preallocated slots; they never allocate or post events.
    void dispatchPendingGuiUpdates(SynthGuiInterface& gui_interface);
    void clearPendingGuiUpdates();

  protected:
    vital::modulation_change createModulationChange(vital::ModulationConnection* connection);
    bool isInvalidConnection(const vital::modulation_change& change);
    virtual SynthGuiInterface* getGuiInterface() = 0;
    json saveToJson();
    bool loadFromJson(const json& state);
    vital::ModulationConnection* getConnection(const std::string& source, const std::string& destination);

    inline bool getNextModulationChange(vital::modulation_change& change) {
      return modulation_change_queue_.try_dequeue_non_interleaved(change);
    }
  
    inline void clearModulationQueue() {
      vital::modulation_change change;
      while (modulation_change_queue_.try_dequeue_non_interleaved(change))
        ;
    }

    void processAudio(AudioSampleBuffer* buffer, int channels, int samples, int offset);
    void processAudioWithInput(AudioSampleBuffer* buffer, const vital::poly_float* input_buffer,
                               int channels, int samples, int offset);
    void processAudioAndMidi(AudioSampleBuffer* buffer, int channels, MidiBuffer& midi_messages,
                             int start_sample, int num_samples, double host_bpm,
                             double ppq_position, bool has_ppq_position, double start_seconds);
    void writeAudio(AudioSampleBuffer* buffer, int channels, int samples, int offset);
    void processMidi(MidiBuffer& buffer, int start_sample = 0, int end_sample = 0,
                     double host_bpm = 0.0, double ppq_position = 0.0, bool has_ppq_position = false);
    void processKeyboardEvents(MidiBuffer& buffer, int num_samples);
    void processModulationChanges();
    void updateMemoryOutput(int samples, const vital::poly_float* audio);
    void queueGuiControlUpdate(const std::string& name, vital::mono_float value);

    static constexpr int kMaxScheduledMidiEvents = 2048;
    static constexpr int kMidiBufferReserveBytes = 16384;

    struct ScheduledMidiEvent {
      uint8 data[3] = { 0, 0, 0 };
      uint8 size = 0;
      int sample_offset = 0;
      int engine_channel = -1;
      uint32_t sequence = 0;
    };

    struct MidiSchedulingBuffers {
      MidiBuffer input;
      MidiBuffer merged_output;
      MidiBuffer arp_input[2][8];
      MidiBuffer arp_segment_input;
      MidiBuffer arp_output;
    };

    void prepareMidiSchedulingBuffers();
    void clearMidiSchedulingBuffers();
    void buildScheduledMidiEvents(MidiBuffer& midi_messages, int start_sample, int end_sample,
                                  double host_bpm, double ppq_position, bool has_ppq_position);
    void addScheduledMidiEvent(const MidiMessage& message, int sample_offset, int engine_channel = -1);
    void dispatchScheduledMidiEvent(const ScheduledMidiEvent& event);
    static int scheduledMidiEventPriority(const ScheduledMidiEvent& event);

    std::unique_ptr<vital::SoundEngine> engine_;
    // Declared after the engine so it is destroyed first.
    std::unique_ptr<vital::multisample::Manager> multisample_manager_;
    File multisample_container_;
    std::unique_ptr<MidiManager> midi_manager_;
    std::unique_ptr<MidiKeyboardState> keyboard_state_;
    std::array<std::array<AtlasArpeggiator, 8>, 2> arpeggiators_;
    std::array<std::array<vital::Value*, 19>, 2> arpeggiator_controls_ {};
    MidiSchedulingBuffers midi_scheduling_buffers_;
    std::array<ScheduledMidiEvent, kMaxScheduledMidiEvents> scheduled_midi_events_;
    int scheduled_midi_event_count_ = 0;
    uint32_t scheduled_midi_sequence_ = 0;
    uint64_t scheduled_midi_overflow_count_ = 0;
    bool arpeggiators_were_enabled_ = false;

    std::unique_ptr<WavetableCreator> wavetable_creators_[vital::kNumOscillators];
    bool wavetable_render_pending_[vital::kNumOscillators] = {};
    std::map<std::string, std::unique_ptr<WavetableCreator>> filter_table_creators_;
    std::map<std::string, std::shared_ptr<vital::Wavetable>> filter_tables_;
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
    bool source_preparation_queued_ = false;
    void scheduleSourcePreparation();
    std::atomic<bool> effect_reservation_queued_ { false };
    void scheduleEffectControlReservation();
    // Live and parked effects as the slots select them. Queued route changes
    // point at wiring that parking may remove, so they are applied first.
    // Processing stopped or the callback lock held.
    void updateEffectShells();

    File active_file_;
    vital::poly_float oscilloscope_memory_[2 * vital::kOscilloscopeMemoryResolution];
    vital::poly_float oscilloscope_memory_write_[2 * vital::kOscilloscopeMemoryResolution];
    std::unique_ptr<vital::StereoMemory> audio_memory_;
#if VITAL_ACCESSIBLE_UI
    std::atomic<bool> audio_history_enabled_ { false };
#else
    std::atomic<bool> audio_history_enabled_ { true };
#endif
    vital::mono_float last_played_note_;
    int last_num_pressed_;
    vital::mono_float memory_reset_period_;
    vital::mono_float memory_input_offset_;
    int memory_index_;
    bool expired_;
    int downloaded_presets_imported_ = 0;

    std::map<std::string, String> save_info_;
    vital::control_map controls_;
    // Resolve the source Compression macro's targets before host automation
    // begins, so macro changes do not allocate parameter-name strings.
    std::map<std::string, std::array<std::string, 2>> spectral_compression_targets_;
    // Value changes the GUI shows, for every control except effects'
    // parameters (the accessible editor reads those itself, and they are
    // most of the controls).
    struct PendingGuiUpdate {
      std::atomic<vital::mono_float> value { 0.0f };
      std::atomic<bool> dirty { false };
    };
    std::map<std::string, PendingGuiUpdate> pending_gui_updates_;
    // MIDI learn arms one parameter at a time; the learned parameter (its name
    // in the static parameter table) and controller wait here for the GUI.
    std::atomic<const std::string*> midi_learned_name_ { nullptr };
    std::atomic<int> midi_learned_id_ { -1 };
    std::atomic<bool> gui_updates_pending_ { false };
    static bool isEffectParameter(const std::string& name);
    vital::CircularQueue<vital::ModulationConnection*> mod_connections_;
    moodycamel::ConcurrentQueue<vital::control_change> value_change_queue_;
    moodycamel::ConcurrentQueue<vital::modulation_change> modulation_change_queue_;
    Tuning tuning_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SynthBase)
};

class HeadlessSynth : public SynthBase {
  public:
    virtual const CriticalSection& getCriticalSection() override {
      return critical_section_;
    }

    virtual void pauseProcessing(bool pause) override {
      if (pause)
        critical_section_.enter();
      else
        critical_section_.exit();
    }

  protected:
    virtual SynthGuiInterface* getGuiInterface() override { return nullptr; }

  private:
    CriticalSection critical_section_;
};
