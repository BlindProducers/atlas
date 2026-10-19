/* Copyright 2013-2019 Matt Tytel
 *
 * pylon is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * pylon is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with pylon.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "JuceHeader.h"

#include "synth_base.h"
#include "fx_automation_parameter.h"
#include "undo_history.h"
#include "value_bridge.h"

#include <array>
#include <mutex>

class ValueBridge;

class SynthPlugin : public SynthBase, public AudioProcessor, public ValueBridge::Listener {
  public:
    static constexpr int kSetProgramWaitMilliseconds = 500;
    static constexpr int kNumFxAutomationChains = vital::kNumBuses + 1;
    static constexpr int kFxAutomationSlotsPerChain = 20;
    // Oscillator slots expose their per-type parameters through assignable
    // automation proxies too; chain kNumFxAutomationChains + N is oscillator N.
    static constexpr int kNumOscAutomationChains = vital::kNumOscillators;
    static constexpr int kOscAutomationSlotsPerOscillator = 8;
    static constexpr int kNumAutomationChains = kNumFxAutomationChains + kNumOscAutomationChains;
    static int automationSlotsForChain(int chain) {
      return chain < kNumFxAutomationChains ? kFxAutomationSlotsPerChain : kOscAutomationSlotsPerOscillator;
    }

    SynthPlugin();
    virtual ~SynthPlugin();

    SynthGuiInterface* getGuiInterface() override;
    void beginChangeGesture(const std::string& name) override;
    void endChangeGesture(const std::string& name) override;
    void setValueNotifyHost(const std::string& name, vital::mono_float value) override;
    const CriticalSection& getCriticalSection() override;
    void pauseProcessing(bool pause) override;
    bool isProcessingPaused() const override { return isSuspended(); }
    // Tells the host the state changed where no host parameter shows it (a
    // preset load, an effect, a route, a wavetable or LFO edit), so it marks
    // the project modified, and records the change for undo. Coalesced;
    // message thread; never during a restore.
    void markStateChanged() override;
    // The same for a change in one part of the state, named as
    // UndoHistory::noteStructureEdit names it ("settings/lfos/2"), which
    // undo records at less cost than a change anywhere.
    void markStateChanged(const std::string& undoBlock);
    UndoHistory& undoHistory() { return undo_history_; }
    // Parameter bridges the editor owns tell undo about their gestures here,
    // as the host's parameters do.
    AudioProcessorParameter::Listener& undoGestureListener() { return undo_gesture_listener_; }

    void prepareToPlay(double sample_rate, int buffer_size) override;
    void releaseResources() override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(AudioSampleBuffer&, MidiBuffer&) override;
    // An audio source the editor plays through the output beside the synth
    // (a community preset's preview). It must outlive its time here; null
    // takes it out. Message thread: processBlock reads it only while it can
    // take the lock without waiting, and prepareToPlay prepares it for the
    // host's rate and block size.
    void setAuditionSource(AudioSource* source);

    AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    const String getName() const override;
    bool supportsMPE() const override { return true; }

    const String getInputChannelName(int channel_index) const override;
    const String getOutputChannelName(int channel_index) const override;
    bool isInputChannelStereoPair(int index) const override;
    bool isOutputChannelStereoPair(int index) const override;

    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool silenceInProducesSilenceOut() const override;
    double getTailLengthSeconds() const override;
    double getCpuUsagePercent() const;
    double getFxSampleRate() const { return fx_sample_rate_.load(std::memory_order_relaxed); }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int index) override { }
    const String getProgramName(int index) override;
    void changeProgramName(int index, const String& new_name) override { }

    void getStateInformation(MemoryBlock& destData) override;
    void setStateInformation(const void* data, int size_in_bytes) override;
    AudioProcessorParameter* getBypassParameter() const override { return bypass_parameter_; }

    void parameterChanged(const std::string& name, vital::mono_float value) override;

    int assignFxParameterToAutomationSlot(const std::string& parameterName);
    bool removeFxParameterAutomation(const std::string& parameterName);
    String fxAutomationSlotNameForParameter(const std::string& parameterName) const;
    void beginFxAutomationGesture(const std::string& parameterName);
    void endFxAutomationGesture(const std::string& parameterName);
    void clearFxAutomationForPrefix(const std::string& parameterPrefix);
    void swapFxAutomationPrefixes(const std::string& firstPrefix, const std::string& secondPrefix);
    // True for parameters reachable through an automation proxy (FX slot and oscillator slot parameters).
    bool hasAutomationProxy(const std::string& parameterName) const {
      return automationChainForParameter(parameterName) >= 0;
    }
    // The slot control a legacy fixed-source parameter name (sample_*, granular_*,
    // spectral_*, noise_*, arp destinations) maps to, or empty.
    static std::string legacyAliasTarget(const std::string& parameterName);

    // Bus 0 is the main effects chain; buses 1..kNumBuses are the routed buses.
    void setFxBusEnabled(int bus, bool enabled);
    void clearFxBus(int bus);
    json fxBusPreset(int bus);
    bool loadFxBusPreset(int bus, const json& preset, String& error);

    // Where this instance's editor was left: its section and preset browser
    // filters. Kept here so they outlive the editor window without being shared
    // with the other Atlas instances a host loads into the same process.
    // Message thread only; not saved with the host's project.
    struct EditorMemory {
      String section_name;
      String preset_path;
      String preset_search;
      String preset_library;
      String preset_bank;
      String preset_category;
      String preset_tag;
      String preset_collection;   // All presets, Favourites, Community presets or Community banks
    };
    EditorMemory& getEditorMemory() { return editor_memory_; }

  private:
    struct StateChangeNotifier : public AsyncUpdater {
      explicit StateChangeNotifier(SynthPlugin& plugin) : plugin_(plugin) { }
      void handleAsyncUpdate() override;
      SynthPlugin& plugin_;
    };
    StateChangeNotifier state_change_notifier_ { *this };
    bool restoring_state_ = false;

    EditorMemory editor_memory_;
    // The editor's audition source (setAuditionSource), its scratch block and
    // the lock the audio thread tries for it.
    SpinLock audition_lock_;
    AudioSource* audition_source_ = nullptr;
    AudioBuffer<float> audition_buffer_ { 2, 1024 };
    void mixAudition(AudioSampleBuffer& buffer);
    void updateFxSampleRate();
    std::atomic<double> fx_sample_rate_ { 44100.0 };
    void updateHostLatency();
    void updateLatencyAfterControlChange(const std::string& name);
    AudioProcessLoadMeasurer cpu_load_;
    std::atomic<uint32> cpu_last_render_ms_ { 0 };
    ValueBridge* bypass_parameter_;
    double last_seconds_time_;
    bool was_playing_;
    bool was_bypassed_ = false;

    AudioPlayHead::CurrentPositionInfo position_info_;

    std::map<std::string, ValueBridge*> bridge_lookup_;
    // Made the first time a parameter is assigned to a proxy, then kept:
    // proxies read them from the host's threads without a lock, and map
    // nodes never move. Only assignments insert, under the mutex.
    std::map<std::string, FxAutomationBinding> fx_automation_bindings_;
    std::mutex fx_automation_bindings_mutex_;
    const FxAutomationBinding* automationBinding(const std::string& parameterName);
    std::array<std::array<FxAutomationParameter*, kFxAutomationSlotsPerChain>,
               kNumAutomationChains> fx_automation_parameters_ {};

    FxAutomationParameter* fxAutomationParameterForTarget(const std::string& parameterName) const;
    // The automation chain a proxied parameter belongs to (FX chain or oscillator slot), or -1.
    int automationChainForParameter(const std::string& parameterName) const;
    // Oscillator slot parameters without a direct host parameter; reachable through proxies.
    bool isProxiedSourceParameter(const std::string& parameterName) const;
    static bool legacySourceAliasTargetExists(const std::string& engineName);
    bool setFxAutomationAssignment(int chain, int slot, const std::string& parameterName, bool notifyHost);
    void clearFxAutomationAssignments(bool notifyHost);
    json fxAutomationAssignmentsToJson() const;
    void loadFxAutomationAssignments(const json& data);

    // Undo reads and loads the state, and sets values back as the editor does.
    friend class UndoHistory;
    // markStateChanged's notice to the host, without recording anything for undo.
    void notifyHostStateChanged();
    // Sets a control as undo finds it, telling the host as an editor change does.
    void applyUndoValue(const std::string& name, vital::mono_float value);
    // Gestures on a control the editor changes mark the user's changes for undo.
    struct UndoGestureListener : public AudioProcessorParameter::Listener {
      explicit UndoGestureListener(UndoHistory& history) : history_(history) { }
      void parameterValueChanged(int, float) override { }
      void parameterGestureChanged(int, bool starting) override {
        if (starting)
          history_.beginUserEdit();
        else
          history_.endUserEdit();
      }
      UndoHistory& history_;
    };
    UndoHistory undo_history_ { *this };
    UndoGestureListener undo_gesture_listener_ { undo_history_ };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SynthPlugin)
};
