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

#include "synth_plugin.h"
#include "source_types.h"
#include "legacy_source_aliases.h"
#include "synth_editor.h"
#include "library_layout.h"
#include "sound_engine.h"
#include "prepared_effect_module.h"
#include "load_save.h"
#include "convolution_reverb_module.h"
#include "synth_strings.h"
#include "multisample_manager.h"
#include "speech_manager.h"
#include "speech_slot.h"

#include <limits>
#include <optional>

namespace {
  constexpr const char* kFxAutomationStateKey = "fx_automation_slots";
  // Where in the song each Speech slot's notes start and which stretches
  // have played (SpeechSlot), saved with the host's project rather than
  // with presets.
  constexpr const char* kSpeechSongKey = "speech_song";

  std::string fxBusPrefix(int bus) {
    return bus == 0 ? "" : "bus_" + std::to_string(bus) + "_";
  }

  int fxChainForParameter(const std::string& name, std::string* unprefixedName = nullptr) {
    std::string rest = name;
    int chain = 0;
    for (int bus = 1; bus <= vital::kNumBuses; ++bus) {
      const std::string prefix = "bus_" + std::to_string(bus) + "_";
      if (name.rfind(prefix, 0) == 0) {
        chain = bus;
        rest = name.substr(prefix.size());
        break;
      }
    }

    if (unprefixedName != nullptr)
      *unprefixedName = rest;
    return chain;
  }

  bool isLegacyFxModuleParameter(const std::string& name) {
    static const std::string prefixes[] = {
      "chorus_", "compressor_", "convolution_reverb_", "delay_", "dimension_expander_",
      "distortion_", "eq_", "filter_fx_", "flanger_", "frequency_shifter_", "limiter_",
      "mid_side_eq_", "phase_shift_", "phaser_", "reverb_", "utility_", "waveshaper_", "spectral_gate_",
      "erosion_", "floaty_delay_", "spectral_compressor_", "scream_", "tremolo_pan_", "tape_", "defuse_", "suppressor_", "trance_gate_", "spectral_lock_", "resonator_",
      "analoguechorus_", "analogueflanger_", "analogueecho_",
      "analoguerotary_", "analoguespring_", "analoguewah_", "spectral_balance_", "glitch_delay_",
      "openwurli_amp_", "scanner_vibrato_", "analoguecompressor_", "maim_"
    };
    for (const auto& prefix : prefixes) {
      if (name.rfind(prefix, 0) == 0)
        return true;
    }
    return false;
  }

  bool isFxHostParameter(const std::string& name) {
    std::string rest;
    fxChainForParameter(name, &rest);
    return isLegacyFxModuleParameter(rest) || rest == "effect_chain_order" ||
           rest == "post_effect_order" || rest.rfind("effect_chain_slot_", 0) == 0;
  }

  int dynamicFxChainForParameter(const std::string& name) {
    std::string rest;
    const int chain = fxChainForParameter(name, &rest);
    return rest.rfind("fx_slot_", 0) == 0 ? chain : -1;
  }

  String fxAutomationParameterId(int chain, int slot) {
    if (chain >= SynthPlugin::kNumFxAutomationChains)
      return "osc_automation_" + String(chain - SynthPlugin::kNumFxAutomationChains + 1) + "_param_" + String(slot + 1);
    const String chain_id = chain == 0 ? String("main") : "bus_" + String(chain);
    return "fx_automation_" + chain_id + "_param_" + String(slot + 1);
  }

  String fxAutomationParameterName(int chain, int slot) {
    if (chain >= SynthPlugin::kNumFxAutomationChains)
      return "Osc " + String(chain - SynthPlugin::kNumFxAutomationChains + 1) + " Param " + String(slot + 1);
    const String chain_name = chain == 0 ? String("Main") : "Bus " + String(chain);
    return "FX " + chain_name + " Param " + String(slot + 1);
  }

  std::string fxAutomationChainStateName(int chain) {
    static const char* names[] = { "main", "bus_1", "bus_2", "bus_3" };
    if (chain >= SynthPlugin::kNumFxAutomationChains)
      return "osc_" + std::to_string(chain - SynthPlugin::kNumFxAutomationChains + 1);
    return names[jlimit(0, vital::kNumBuses, chain)];
  }

  // Oscillator slot a slot-prefixed parameter name belongs to, or -1.
  int sourceSlotForParameter(const std::string& name, std::string* suffix = nullptr) {
    for (int slot = 0; slot < vital::kNumOscillators; ++slot) {
      const std::string prefix = vital::source_types::slotPrefix(slot);
      if (name.rfind(prefix, 0) == 0) {
        if (suffix != nullptr)
          *suffix = name.substr(prefix.size());
        return slot;
      }
    }
    return -1;
  }

  // Old fixed-source host parameters drive their slot controls
  // (legacy_source_aliases.h; MIDI-learn mappings use the same table).
  using vital::source_types::legacySourceAliasTarget;

  // New arp destinations for slots 5-8 are reached through the legacy aliases above.
  bool isAliasedArpDestination(const std::string& name) {
    for (int arp = 1; arp <= 2; ++arp) {
      for (int slot = vital::source_types::kFirstLegacySourceSlot; slot < vital::kNumOscillators; ++slot) {
        if (name == "arp_" + std::to_string(arp) + "_destination_osc_" + std::to_string(slot + 1))
          return true;
      }
    }
    return false;
  }
}

SynthPlugin::SynthPlugin() {
  last_seconds_time_ = 0.0;
  was_playing_ = false;

  int num_params = vital::Parameters::getNumParameters();
  int legacy_host_index = 0;
  for (int i = 0; i < num_params; ++i) {
    const vital::ValueDetails* details = vital::Parameters::getDetails(i);
    std::string engine_name = details->name;
    if (controls_.count(engine_name) == 0) {
      // Legacy fixed-source aliases keep their host parameter and drive the slot control.
      engine_name = legacySourceAliasTarget(details->name);
      if (engine_name.empty() || controls_.count(engine_name) == 0)
        continue;
    }

    // Before FX proxying, ValueBridge parameters had implicit numeric IDs.
    // Retain each surviving parameter's original number so old non-FX host
    // automation cannot be shifted onto a different control.
    const String stable_legacy_id(legacy_host_index++);
    if (isFxHostParameter(details->name))
      continue;
    // A slot control already reached through its legacy alias keeps that single host parameter.
    if (bridge_lookup_.count(engine_name) != 0)
      continue;
    // Oscillator slot type parameters beyond the original set are automated
    // through the oscillator automation proxies, not as individual host parameters.
    if (engine_name == details->name &&
        (isProxiedSourceParameter(details->name) || isAliasedArpDestination(details->name)))
      continue;

    ValueBridge* bridge = new ValueBridge(engine_name, controls_[engine_name], stable_legacy_id);
    if (vital::envelope_time::isTimeParameter(engine_name)) {
      const auto prefix = engine_name.substr(0, engine_name.find_last_of('_'));
      bridge->setEnvelopeSyncContext(controls_.at(prefix + "_tempo_sync"), controls_.at("beats_per_minute"));
    }
    bridge->setListener(this);
    // The editor's gestures on it mark the user's changes for undo.
    bridge->addListener(&undo_gesture_listener_);
    bridge_lookup_[engine_name] = bridge;
    addParameter(bridge);
  }

  // Dynamic FX controls deliberately stay out of the host parameter list;
  // stable proxy slots are assigned to them instead (automationBinding).

  for (int chain = 0; chain < kNumAutomationChains; ++chain) {
    for (int slot = 0; slot < automationSlotsForChain(chain); ++slot) {
      auto* parameter = new FxAutomationParameter(fxAutomationParameterId(chain, slot),
                                                  fxAutomationParameterName(chain, slot), this);
      fx_automation_parameters_[chain][slot] = parameter;
      addParameter(parameter);
    }
  }

  auto bypass = bridge_lookup_.find("bypass");
  bypass_parameter_ = bypass == bridge_lookup_.end() ? nullptr : bypass->second;
}

SynthPlugin::~SynthPlugin() {
  clearFxAutomationAssignments(false);
  // A MIDI learn completed while the editor was closed is saved now.
  midi_manager_->commitPendingMidiLearn();
  midi_manager_ = nullptr;
  keyboard_state_ = nullptr;
}

SynthGuiInterface* SynthPlugin::getGuiInterface() {
  AudioProcessorEditor* editor = getActiveEditor();
  if (editor)
    return dynamic_cast<SynthGuiInterface*>(editor);
  return nullptr;
}

void SynthPlugin::beginChangeGesture(const std::string& name) {
  const auto direct = bridge_lookup_.find(name);
  if (direct != bridge_lookup_.end())
    direct->second->beginChangeGesture();
  else
    beginFxAutomationGesture(name);
}

void SynthPlugin::endChangeGesture(const std::string& name) {
  const auto direct = bridge_lookup_.find(name);
  if (direct != bridge_lookup_.end())
    direct->second->endChangeGesture();
  else
    endFxAutomationGesture(name);
}

void SynthPlugin::setValueNotifyHost(const std::string& name, vital::mono_float value) {
  // A value MIDI learn or the audio thread set is where undo starts from.
  if (!MessageManager::existsAndIsCurrentThread())
    undo_history_.absorbExternalChange(name);
  const auto direct = bridge_lookup_.find(name);
  if (direct != bridge_lookup_.end()) {
    vital::mono_float plugin_value = direct->second->convertToPluginValue(value);
    direct->second->setValueNotifyHost(plugin_value);
  }
  else if (auto* proxy = fxAutomationParameterForTarget(name)) {
    if (!proxy->isApplyingHostValue())
      proxy->setEngineValueNotifyHost(value);
  }
  updateLatencyAfterControlChange(name);
}

const CriticalSection& SynthPlugin::getCriticalSection() {
  return getCallbackLock();
}

void SynthPlugin::StateChangeNotifier::handleAsyncUpdate() {
  // JUCE's VST3 wrapper turns this into IComponentHandler2::setDirty.
  plugin_.updateHostDisplay(AudioProcessorListener::ChangeDetails().withNonParameterStateChanged(true));
}

void SynthPlugin::markStateChanged() {
  markStateChanged(std::string());
}

void SynthPlugin::markStateChanged(const std::string& undoBlock) {
  if (restoring_state_ || !MessageManager::existsAndIsCurrentThread())
    return;
  undo_history_.noteStructureEdit(undoBlock);
  state_change_notifier_.triggerAsyncUpdate();
}

void SynthPlugin::notifyHostStateChanged() {
  if (restoring_state_ || !MessageManager::existsAndIsCurrentThread())
    return;
  state_change_notifier_.triggerAsyncUpdate();
}

void SynthPlugin::applyUndoValue(const std::string& name, vital::mono_float value) {
  beginChangeGesture(name);
  valueChanged(name, value);
  setValueNotifyHost(name, value);
  endChangeGesture(name);
}

void SynthPlugin::pauseProcessing(bool pause) {
  if (!pause) {
    if (isSuspended()) {
      // JUCE's wrappers check isSuspended() under the callback lock and skip
      // processBlock, so the effects build without it. Holding the lock made
      // the host's audio thread wait for the whole build.
      prepareSelectedEffects();
    }
    else {
      const ScopedLock lock(getCallbackLock());
      prepareSelectedEffects();
    }
  }
  suspendProcessing(pause);
}

const String SynthPlugin::getName() const {
  return JucePlugin_Name;
}

const String SynthPlugin::getInputChannelName(int channel_index) const {
  return String(channel_index + 1);
}

const String SynthPlugin::getOutputChannelName(int channel_index) const {
  return String(channel_index + 1);
}

bool SynthPlugin::isInputChannelStereoPair(int index) const {
  return true;
}

bool SynthPlugin::isOutputChannelStereoPair(int index) const {
  return true;
}

bool SynthPlugin::acceptsMidi() const {
#if JucePlugin_WantsMidiInput
  return true;
#else
  return false;
#endif
}

bool SynthPlugin::producesMidi() const {
#if JucePlugin_ProducesMidiOutput
  return true;
#else
  return false;
#endif
}

bool SynthPlugin::silenceInProducesSilenceOut() const {
  return false;
}

double SynthPlugin::getTailLengthSeconds() const {
  // Feedback can reach unity and modulation can sustain it indefinitely.
  // There is no finite upper bound valid for every Atlas patch. JUCE maps
  // infinity to VST3's kInfiniteTail so hosts do not truncate these tails.
  return std::numeric_limits<double>::infinity();
}

const String SynthPlugin::getProgramName(int index) {
  SynthGuiInterface* editor = getGuiInterface();
  if (editor == nullptr || editor->getSynth() == nullptr)
    return "";

  return editor->getSynth()->getPresetName();
}

void SynthPlugin::prepareToPlay(double sample_rate, int buffer_size) {
  cpu_load_.reset(sample_rate, jmax(1, buffer_size));
  cpu_last_render_ms_.store(0, std::memory_order_relaxed);
  engine_->setSampleRate(sample_rate);
  engine_->checkOversampling();
  engine_->updateAllModulationSwitches();
  prepareSelectedEffects();
  midi_manager_->setSampleRate(sample_rate);
  updateHostLatency();
  {
    const SpinLock::ScopedLockType lock(audition_lock_);
    audition_buffer_.setSize(2, jmax(1, buffer_size));
    if (audition_source_ != nullptr)
      audition_source_->prepareToPlay(jmax(1, buffer_size), sample_rate);
  }
}

void SynthPlugin::setAuditionSource(AudioSource* source) {
  const SpinLock::ScopedLockType lock(audition_lock_);
  audition_source_ = source;
  audition_active_.store(source != nullptr, std::memory_order_release);
  const double sample_rate = AudioProcessor::getSampleRate();
  if (source != nullptr && sample_rate > 0.0)
    source->prepareToPlay(jmax(1, getBlockSize()), sample_rate);
}

void SynthPlugin::mixAudition(AudioSampleBuffer& buffer) {
  if (!audition_active_.load(std::memory_order_acquire))
    return;
  // Never waits: the message thread holds the lock only to swap the source
  // or prepare it, and a block that meets that goes out without the preview.
  const SpinLock::ScopedTryLockType lock(audition_lock_);
  if (!lock.isLocked() || audition_source_ == nullptr)
    return;
  const int channels = jmin(buffer.getNumChannels(), audition_buffer_.getNumChannels());
  const int chunk = audition_buffer_.getNumSamples();
  if (channels <= 0 || chunk <= 0)
    return;
  for (int offset = 0; offset < buffer.getNumSamples();) {
    const int samples = jmin(buffer.getNumSamples() - offset, chunk);
    AudioSourceChannelInfo info(&audition_buffer_, 0, samples);
    audition_source_->getNextAudioBlock(info);
    for (int channel = 0; channel < channels; ++channel)
      buffer.addFrom(channel, offset, audition_buffer_, channel, 0, samples);
    offset += samples;
  }
}

void SynthPlugin::updateFxSampleRate() {
  int fx_factor = engine_->getOversamplingAmount();
  for (int multiple = engine_->getSampleRate() / 44100; multiple > 1 && fx_factor > 1; multiple >>= 1)
    fx_factor >>= 1;
  fx_sample_rate_.store(engine_->getSampleRate() * fx_factor, std::memory_order_relaxed);
}

void SynthPlugin::updateHostLatency() {
  engine_->updateEffectLatency();
  updateFxSampleRate();
  // JUCE notifies AU/VST3 hosts when this changes. Use the effective FX rate,
  // since Atlas automatically reduces oversampling at higher host rates.
  setLatencySamples(engine_->getLatencySamples());
}

void SynthPlugin::updateLatencyAfterControlChange(const std::string& name) {
  // Stopped hosts may not render. Publish editor/message-thread edits now so
  // PDC is ready for the first note. Audio-thread edits are handled by processBlock.
  // Bulk state loads publish once after restoration, while processing is suspended.
  if (isSuspended() || !MessageManager::existsAndIsCurrentThread())
    return;
  bool affects_latency = name == "oversampling" || name == "bus_1_destination" ||
      name == "bus_2_destination" || name == "bus_3_destination";
  const auto slot = name.find("fx_slot_");
  if (slot != std::string::npos) {
    const auto separator = name.find('_', slot + 8);
    if (separator != std::string::npos) {
      const auto start = separator + 1;
      affects_latency |= name.compare(start, std::string::npos, "type") == 0 ||
          name.compare(start, std::string::npos, "enabled") == 0 ||
          name.compare(start, std::string::npos, "spectral_gate_fft_size") == 0 ||
          name.compare(start, std::string::npos, "spectral_lock_mode") == 0 ||
          name.compare(start, std::string::npos, "filter_fx_model") == 0 ||
          name.compare(start, std::string::npos, "filter_fx_style") == 0 ||
          name.compare(start, std::string::npos, "maim_encoder") == 0 ||
          name.compare(start, std::string::npos, "maim_lpc_rate") == 0;
    }
  }
  if (affects_latency) {
    int latency = 0;
    {
      const ScopedLock lock(getCallbackLock());
      engine_->checkOversampling();
      engine_->updateEffectLatency();
      latency = engine_->getLatencySamples();
      updateFxSampleRate();
    }
    // A host may restart processing synchronously in response to this call.
    // Release the callback lock before notifying it.
    setLatencySamples(latency);
  }
}

void SynthPlugin::releaseResources() {
  cpu_load_.reset();
  cpu_last_render_ms_.store(0, std::memory_order_relaxed);
}

bool SynthPlugin::isBusesLayoutSupported(const BusesLayout& layouts) const {
  // Atlas renders a stereo pair; a mono output takes its left channel. Wider
  // layouts (a surround instrument track) would carry nothing in the rest.
  const AudioChannelSet output = layouts.getMainOutputChannelSet();
  return output == AudioChannelSet::stereo() || output == AudioChannelSet::mono();
}

double SynthPlugin::getCpuUsagePercent() const {
  const auto last_render = cpu_last_render_ms_.load(std::memory_order_relaxed);
  if (last_render == 0 || static_cast<uint32>(Time::getMillisecondCounter() - last_render) > 1000)
    return 0.0;
  return cpu_load_.getLoadAsPercentage();
}

void SynthPlugin::processBlock(AudioSampleBuffer& buffer, MidiBuffer& midi_messages) {
  vital::RealtimeEffectScope effect_scope(!isNonRealtime());
  // Measure this instance against the duration of the actual host block. The
  // timer uses stack storage, covers early returns, and skips zero-length MIDI
  // flush blocks so they cannot produce an invalid load percentage.
  std::optional<AudioProcessLoadMeasurer::ScopedTimer> cpu_timer;
  if (buffer.getNumSamples() > 0) {
    cpu_timer.emplace(cpu_load_, buffer.getNumSamples());
    cpu_last_render_ms_.store(Time::getMillisecondCounter(), std::memory_order_relaxed);
  }
  static constexpr double kSecondsPerMinute = 60.0f;

  // Apply quality changes at the host block boundary, including changes made
  // with the editor closed and during offline rendering. Do not reconfigure
  // the graph from a parameter callback while audio may be using its buffers.
  engine_->checkOversampling();

  // Offline renders right after a state restore must not run ahead of the
  // instruments still loading; apply them here instead of on the message thread.
  if (isNonRealtime() && multisample_manager_->hasPendingLoads()) {
    multisample_manager_->waitForPendingLoads(300000);
    multisample_manager_->processPendingResults(false);
  }
  // Likewise for Speech text still being rendered.
  if (isNonRealtime() && speech_manager_->hasPendingWork()) {
    speech_manager_->waitForPendingWork(300000);
    speech_manager_->processPendingResults(false);
  }

  updateHostLatency();

  if (bypass_parameter_ != nullptr && bypass_parameter_->getValue()) {
    // Note-offs are not seen while bypassed; release everything on entry so
    // notes held at that moment don't sustain after un-bypassing.
    if (!was_bypassed_) {
      engine_->allNotesOff(0);
      clearArpeggiators();
      was_bypassed_ = true;
    }
    processBlockBypassed(buffer, midi_messages);
    return;
  }
  was_bypassed_ = false;

  int total_samples = buffer.getNumSamples();
  int num_channels = getTotalNumOutputChannels();
  AudioPlayHead* play_head = getPlayHead();
  bool is_playing = false;
  if (play_head) {
    play_head->getCurrentPosition(position_info_);
    is_playing = position_info_.isPlaying;
    if (position_info_.bpm)
      engine_->setBpm(position_info_.bpm);

    const bool looping = position_info_.isLooping && position_info_.ppqLoopEnd > position_info_.ppqLoopStart;
    song_loop_start_ = looping ? position_info_.ppqLoopStart : 0.0;
    song_loop_end_ = looping ? position_info_.ppqLoopEnd : 0.0;
    if (is_playing && position_info_.bpm > 0.0) {
      double bps = position_info_.bpm / kSecondsPerMinute;
      last_seconds_time_ = position_info_.ppqPosition / bps;
    }
  }

  if (was_playing_ && !is_playing) {
    engine_->allNotesOff(0);
    clearArpeggiators();
  }
  // Playback without a song position sings the Speech text from its first
  // syllable; with one, the slots count the song's notes (SpeechSlot).
  if (!was_playing_ && is_playing)
    engine_->rewindSpeech();
  was_playing_ = is_playing;
  engine_->setTransportPlaying(is_playing);

  processModulationChanges();
  if (total_samples)
    processKeyboardEvents(midi_messages, total_samples);

  double sample_time = 1.0 / AudioProcessor::getSampleRate();
  const bool has_ppq_position = play_head && is_playing && position_info_.bpm > 0.0;
  const double block_start_ppq = position_info_.ppqPosition;
  const double quarters_per_sample = has_ppq_position ?
      position_info_.bpm / (kSecondsPerMinute * AudioProcessor::getSampleRate()) : 0.0;
  for (int sample_offset = 0; sample_offset < total_samples;) {
    int num_samples = std::min<int>(total_samples - sample_offset, vital::kMaxBufferSize);

    const double sub_block_ppq = block_start_ppq + quarters_per_sample * sample_offset;
    processAudioAndMidi(&buffer, num_channels, midi_messages, sample_offset, num_samples,
                        position_info_.bpm, sub_block_ppq, has_ppq_position, last_seconds_time_);

    last_seconds_time_ += num_samples * sample_time;
    sample_offset += num_samples;
  }
  // A community preset's preview the editor is playing, beside the synth.
  mixAudition(buffer);
  // MIDI-learned changes may have updated a slot inside the host block.
  setLatencySamples(engine_->getLatencySamples());
  // A slot selected without SynthBase::valueChanged (or before its
  // reservation ran) passes audio through until its controls are reserved.
  if (engine_->takeEffectControlReservationRequest())
    scheduleEffectControlReservation();
}

bool SynthPlugin::hasEditor() const {
  return true;
}

AudioProcessorEditor* SynthPlugin::createEditor() {
  // The move to the current library layout runs here, on the host's way to
  // the editor; never when a test builds the plugin or an editor of its own,
  // against whatever Atlas folder the config names.
  const atlas::library::MigrationReport migration =
      loadedByHost() ? atlas::library::migrateIfNeeded() : atlas::library::MigrationReport();
  auto* editor = new SynthEditor(*this);
  editor->announceLibraryMigration(migration);
  return editor;
}

void SynthPlugin::parameterChanged(const std::string& name, vital::mono_float value) {
  // Inside one of the editor's gestures the change is the user's, a step for
  // undo; otherwise the host or its automation made it, and undo starts from it.
  const bool user_edit = undo_history_.isRecordingUserEdit();
  if (user_edit)
    undo_history_.noteValueEdit(name);
  valueChangedExternal(name, value);
  if (!user_edit)
    undo_history_.absorbExternalChange(name);
  const auto direct = bridge_lookup_.find(name);
  if (direct == bridge_lookup_.end()) {
    if (auto* proxy = fxAutomationParameterForTarget(name)) {
      if (!proxy->isApplyingHostValue())
        proxy->setEngineValueNotifyHost(value);
    }
    else {
      // An editor control the host does not see (an effect's parameters, an
      // oscillator type's own controls): the host learns of it no other way.
      notifyHostStateChanged();
    }
  }
  updateLatencyAfterControlChange(name);
}

bool SynthPlugin::isProxiedSourceParameter(const std::string& parameterName) const {
  std::string suffix;
  const int slot = sourceSlotForParameter(parameterName, &suffix);
  if (slot < 0)
    return false;
  // Slots 1-4 keep their original wavetable/analog host parameters; everything a
  // slot gained (other types' groups, and all of slots 5-8 except what the legacy
  // aliases cover) is reached through the oscillator automation proxies.
  if (slot >= vital::source_types::kFirstLegacySourceSlot)
    return !legacySourceAliasTargetExists(parameterName);
  return vital::source_types::typeForParameterSuffix(suffix) >= 0;
}

std::string SynthPlugin::legacyAliasTarget(const std::string& parameterName) {
  return legacySourceAliasTarget(parameterName);
}

bool SynthPlugin::legacySourceAliasTargetExists(const std::string& engineName) {
  std::string suffix;
  const int slot = sourceSlotForParameter(engineName, &suffix);
  if (slot < vital::source_types::kFirstLegacySourceSlot)
    return false;
  static const char* kPrefixes[] = { "sample", "granular", "spectral", "noise" };
  const std::string legacy_prefix =
      std::string(kPrefixes[slot - vital::source_types::kFirstLegacySourceSlot]) + "_";
  // osc_5_sample_start <- sample_start; osc_5_on <- sample_on.
  const std::string alias = suffix.rfind(legacy_prefix, 0) == 0 ? suffix : legacy_prefix + suffix;
  return legacySourceAliasTarget(alias) == engineName;
}

const FxAutomationBinding* SynthPlugin::automationBinding(const std::string& parameterName) {
  const std::lock_guard<std::mutex> lock(fx_automation_bindings_mutex_);
  const auto found = fx_automation_bindings_.find(parameterName);
  if (found != fx_automation_bindings_.end())
    return &found->second;
  if (automationChainForParameter(parameterName) < 0 || !vital::Parameters::isParameter(parameterName))
    return nullptr;
  const auto control = controls_.find(parameterName);
  if (control == controls_.end() || control->second == nullptr)
    return nullptr;
  FxAutomationBinding binding;
  binding.details = &vital::Parameters::getDetails(parameterName);
  binding.value = control->second;
  binding.suppressor_sample_rate = &fx_sample_rate_;
  return &fx_automation_bindings_.emplace(parameterName, binding).first->second;
}

int SynthPlugin::automationChainForParameter(const std::string& parameterName) const {
  const int fx_chain = dynamicFxChainForParameter(parameterName);
  if (fx_chain >= 0)
    return fx_chain;
  const int slot = sourceSlotForParameter(parameterName);
  if (slot >= 0 && bridge_lookup_.count(parameterName) == 0 && isProxiedSourceParameter(parameterName))
    return kNumFxAutomationChains + slot;
  return -1;
}

FxAutomationParameter* SynthPlugin::fxAutomationParameterForTarget(
    const std::string& parameterName) const {
  for (const auto& chain : fx_automation_parameters_) {
    for (auto* parameter : chain) {
      if (parameter != nullptr && parameter->isAssignedTo(parameterName))
        return parameter;
    }
  }
  return nullptr;
}

bool SynthPlugin::setFxAutomationAssignment(int chain, int slot, const std::string& parameterName,
                                            bool notifyHost) {
  if (!isPositiveAndBelow(chain, kNumAutomationChains) ||
      !isPositiveAndBelow(slot, automationSlotsForChain(chain)) ||
      automationChainForParameter(parameterName) != chain)
    return false;

  const FxAutomationBinding* binding = automationBinding(parameterName);
  if (binding == nullptr)
    return false;

  auto* parameter = fx_automation_parameters_[chain][slot];
  if (parameter == nullptr)
    return false;
  parameter->setBinding(binding, notifyHost);
  return true;
}

int SynthPlugin::assignFxParameterToAutomationSlot(const std::string& parameterName) {
  const int chain = automationChainForParameter(parameterName);
  if (!isPositiveAndBelow(chain, kNumAutomationChains))
    return -1;
  for (int slot = 0; slot < automationSlotsForChain(chain); ++slot) {
    auto* parameter = fx_automation_parameters_[chain][slot];
    if (parameter != nullptr && parameter->isAssignedTo(parameterName))
      return slot;
  }
  for (int slot = 0; slot < automationSlotsForChain(chain); ++slot) {
    auto* parameter = fx_automation_parameters_[chain][slot];
    if (parameter != nullptr && parameter->getBinding() == nullptr &&
        setFxAutomationAssignment(chain, slot, parameterName, true))
      return slot;
  }
  return -1;
}

bool SynthPlugin::removeFxParameterAutomation(const std::string& parameterName) {
  if (auto* parameter = fxAutomationParameterForTarget(parameterName)) {
    parameter->setBinding(nullptr, true);
    return true;
  }
  return false;
}

String SynthPlugin::fxAutomationSlotNameForParameter(const std::string& parameterName) const {
  for (int chain = 0; chain < kNumAutomationChains; ++chain) {
    for (int slot = 0; slot < automationSlotsForChain(chain); ++slot) {
      auto* parameter = fx_automation_parameters_[chain][slot];
      if (parameter != nullptr && parameter->isAssignedTo(parameterName))
        return fxAutomationParameterName(chain, slot);
    }
  }
  return {};
}

void SynthPlugin::beginFxAutomationGesture(const std::string& parameterName) {
  if (auto* parameter = fxAutomationParameterForTarget(parameterName))
    parameter->beginChangeGesture();
}

void SynthPlugin::endFxAutomationGesture(const std::string& parameterName) {
  if (auto* parameter = fxAutomationParameterForTarget(parameterName))
    parameter->endChangeGesture();
}

void SynthPlugin::clearFxAutomationForPrefix(const std::string& parameterPrefix) {
  for (auto& chain : fx_automation_parameters_) {
    for (auto* parameter : chain) {
      const auto* binding = parameter == nullptr ? nullptr : parameter->getBinding();
      if (binding != nullptr && binding->details != nullptr &&
          binding->details->name.rfind(parameterPrefix, 0) == 0)
        parameter->setBinding(nullptr, true);
    }
  }
}

void SynthPlugin::swapFxAutomationPrefixes(const std::string& firstPrefix,
                                           const std::string& secondPrefix) {
  if (firstPrefix == secondPrefix)
    return;

  for (auto& chain : fx_automation_parameters_) {
    for (auto* parameter : chain) {
      const auto* binding = parameter == nullptr ? nullptr : parameter->getBinding();
      if (binding == nullptr || binding->details == nullptr)
        continue;

      const std::string& current = binding->details->name;
      std::string replacement;
      if (current.rfind(firstPrefix, 0) == 0)
        replacement = secondPrefix + current.substr(firstPrefix.size());
      else if (current.rfind(secondPrefix, 0) == 0)
        replacement = firstPrefix + current.substr(secondPrefix.size());
      else
        continue;

      parameter->setBinding(automationBinding(replacement), true);
    }
  }
}

void SynthPlugin::clearFxAutomationAssignments(bool notifyHost) {
  for (auto& chain : fx_automation_parameters_) {
    for (auto* parameter : chain) {
      if (parameter != nullptr)
        parameter->setBinding(nullptr, notifyHost);
    }
  }
}

json SynthPlugin::fxAutomationAssignmentsToJson() const {
  json assignments = json::object();
  assignments["version"] = 1;
  for (int chain = 0; chain < kNumAutomationChains; ++chain) {
    json chain_assignments = json::array();
    for (int slot = 0; slot < automationSlotsForChain(chain); ++slot) {
      const auto* parameter = fx_automation_parameters_[chain][slot];
      const auto* binding = parameter == nullptr ? nullptr : parameter->getBinding();
      chain_assignments.push_back(binding != nullptr && binding->details != nullptr
                                      ? binding->details->name : std::string());
    }
    assignments[fxAutomationChainStateName(chain)] = std::move(chain_assignments);
  }
  return assignments;
}

void SynthPlugin::loadFxAutomationAssignments(const json& data) {
  clearFxAutomationAssignments(false);
  if (!data.is_object())
    return;

  for (int chain = 0; chain < kNumAutomationChains; ++chain) {
    const std::string chain_name = fxAutomationChainStateName(chain);
    if (!data.count(chain_name) || !data[chain_name].is_array())
      continue;
    const json& chain_assignments = data[chain_name];
    const int count = std::min(automationSlotsForChain(chain), static_cast<int>(chain_assignments.size()));
    for (int slot = 0; slot < count; ++slot) {
      if (!chain_assignments[slot].is_string())
        continue;
      const std::string target = chain_assignments[slot].get<std::string>();
      if (target.empty() || fxAutomationParameterForTarget(target) != nullptr)
        continue;
      setFxAutomationAssignment(chain, slot, target, false);
    }
  }
}

void SynthPlugin::setFxBusEnabled(int bus, bool enabled) {
  if (!isPositiveAndBelow(bus, vital::kNumBuses + 1))
    return;

  const std::string prefix = fxBusPrefix(bus);
  const bool was_suspended = isSuspended();
  pauseProcessing(true);
  {
    // Paused: no callback lock, which the host's audio thread would wait on
    // (and which must not be held while notifying the host).
    for (int slot = 1; slot <= vital::constants::kMaxEffectSlots; ++slot) {
      const std::string slot_prefix = prefix + "fx_slot_" + std::to_string(slot) + "_";
      if (getControls().at(slot_prefix + "type")->value() <= 0.0f)
        continue;
      const std::string id = slot_prefix + "enabled";
      beginChangeGesture(id);
      valueChangedInternal(id, enabled ? 1.0f : 0.0f);
      endChangeGesture(id);
    }
  }
  if (!was_suspended) {
    pauseProcessing(false);
    updateLatencyAfterControlChange(prefix + "fx_slot_1_enabled");
  }
  markStateChanged();
}

json SynthPlugin::fxBusPreset(int bus) {
  if (!isPositiveAndBelow(bus, vital::kNumBuses + 1))
    return {};

  // This only reads values (an IR has its own lock), so it takes no callback
  // lock: that stopped the host's audio thread while the bus was written.
  const std::string prefix = fxBusPrefix(bus);
  const auto convolution_modules = getConvolutionReverbModules();
  const vital::control_map& controls = getControls();
  json slots = json::array();
  for (int slot = 1; slot <= vital::constants::kMaxEffectSlots; ++slot) {
    const std::string slot_prefix = prefix + "fx_slot_" + std::to_string(slot) + "_";
    const int type = static_cast<int>(controls.at(slot_prefix + "type")->value());
    json entry = { { "effect", nullptr }, { "enabled", true }, { "parameters", json::object() } };
    if (isPositiveAndBelow(type - 1, vital::constants::kNumEffects)) {
      const std::string effect = strings::kEffectOrder[type - 1];
      entry["effect"] = effect;
      entry["enabled"] = controls.at(slot_prefix + "enabled")->value() >= 0.5f;
      // The effect's parameters sit together in the sorted control map.
      const std::string effect_prefix = slot_prefix + effect + "_";
      for (auto control = controls.lower_bound(effect_prefix);
           control != controls.end() && control->first.compare(0, effect_prefix.size(), effect_prefix) == 0;
           ++control) {
        if (control->second != nullptr && vital::Parameters::isParameter(control->first))
          entry["parameters"][control->first.substr(slot_prefix.size())] = control->second->value();
      }
      if (effect == "convolution_reverb") {
        for (const auto& module : convolution_modules) {
          if (module.first == slot_prefix && module.second != nullptr && module.second->hasCustomIr())
            entry["impulse_response"] = module.second->irStateToJson();
        }
      }
      if (effect == "filter_fx") {
        if (WavetableCreator* creator = getFilterTableCreator(slot_prefix + "filter_fx", false))
          entry["filter_table"] = creator->stateToJson();
      }
    }
    slots.push_back(std::move(entry));
  }
  return { { "type", "Atlas FX Bus Preset" }, { "version", 1 }, { "slots", std::move(slots) } };
}

void SynthPlugin::clearFxBus(int bus) {
  String error;
  loadFxBusPreset(bus, { { "type", "Atlas FX Bus Preset" }, { "version", 1 },
                         { "slots", json::array() } }, error);
}

bool SynthPlugin::loadFxBusPreset(int bus, const json& preset, String& error) {
  error = "The selected file is not a valid Atlas FX bus preset.";
  if (!isPositiveAndBelow(bus, vital::kNumBuses + 1) || !preset.is_object() ||
      !preset.count("type") || preset["type"] != "Atlas FX Bus Preset" ||
      !preset.count("version") || preset["version"] != 1 ||
      !preset.count("slots") || !preset["slots"].is_array() ||
      preset["slots"].size() > vital::constants::kMaxEffectSlots)
    return false;

  struct SlotState {
    int type = 0;
    bool enabled = true;
    std::map<std::string, float> parameters;
    json impulse;
    json filter_table;
  };
  std::array<SlotState, vital::constants::kMaxEffectSlots> slots;
  const std::string prefix = fxBusPrefix(bus);

  // Validate everything before touching the current bus, including embedded audio.
  try {
    for (size_t index = 0; index < preset["slots"].size(); ++index) {
      const auto& entry = preset["slots"][index];
      if (!entry.is_object() || !entry.count("effect") || !entry.count("enabled") ||
          !entry["enabled"].is_boolean() || !entry.count("parameters") ||
          !entry["parameters"].is_object())
        return false;
      auto& slot = slots[index];
      slot.enabled = entry["enabled"].get<bool>();
      if (entry["effect"].is_null()) {
        if (!entry["parameters"].empty() || entry.count("impulse_response"))
          return false;
        continue;
      }
      if (!entry["effect"].is_string())
        return false;
      const std::string effect = entry["effect"].get<std::string>();
      for (int type = 0; type < vital::constants::kNumEffects; ++type) {
        if (effect == strings::kEffectOrder[type])
          slot.type = type + 1;
      }
      if (slot.type == 0) {
        error = "This preset contains an effect that this version of Atlas does not support.";
        return false;
      }
      const std::string slot_prefix = prefix + "fx_slot_" + std::to_string(index + 1) + "_";
      for (auto it = entry["parameters"].begin(); it != entry["parameters"].end(); ++it) {
        const std::string id = slot_prefix + it.key();
        if (it.key().rfind(effect + "_", 0) != 0 || !it.value().is_number() ||
            !vital::Parameters::isParameter(id) || getControls().count(id) == 0)
          return false;
        const auto& details = vital::Parameters::getDetails(id);
        const double value = it.value().get<double>();
        if (!std::isfinite(value) || value < details.min || value > details.max ||
            (details.value_scale == vital::ValueDetails::kIndexed && value != std::round(value)))
          return false;
        slot.parameters[id] = static_cast<float>(value);
      }
      if (entry.count("impulse_response")) {
        if (effect != "convolution_reverb")
          return false;
        const auto& ir = entry["impulse_response"];
        if (!ir.is_object() || !ir.count("length") || !ir["length"].is_number_integer() ||
            !ir.count("sample_rate") || !ir["sample_rate"].is_number_integer())
          return false;
        const double rate = ir["sample_rate"].get<double>();
        const double length = ir["length"].get<double>();
        if (rate <= 0 || rate > 768000 || length <= 0 ||
            length > rate * vital::ConvolutionReverbModule::kMaxIrSeconds ||
            vital::ConvolutionReverbModule::irFromJson(ir) == nullptr)
          return false;
        slot.impulse = ir;
      }
      if (entry.count("filter_table")) {
        if (effect != "filter_fx" || !entry["filter_table"].is_object())
          return false;
        slot.filter_table = entry["filter_table"];
      }
    }
  }
  catch (const json::exception&) {
    return false;
  }

  const std::string slots_prefix = prefix + "fx_slot_";
  const bool was_suspended = isSuspended();
  pauseProcessing(true);
  // Assignments belong to the replaced modules, never to a portable bus preset.
  clearFxAutomationForPrefix(slots_prefix);
  {
    // Processing is paused (the host hears silence), so no callback lock: the
    // host's audio thread would wait for all of this.
    std::vector<vital::ModulationConnection*> routes;
    for (auto* route : getModulationConnections()) {
      if (route != nullptr && route->destination_name.rfind(slots_prefix, 0) == 0)
        routes.push_back(route);
    }
    for (auto* route : routes)
      disconnectModulation(route);

    for (const auto& control : getControls()) {
      if (control.second != nullptr && control.first.rfind(slots_prefix, 0) == 0 &&
          vital::Parameters::isParameter(control.first))
        valueChanged(control.first, vital::Parameters::getDetails(control.first).default_value);
    }
    for (const auto& module : getConvolutionReverbModules()) {
      if (module.second == nullptr || module.first.rfind(slots_prefix, 0) != 0)
        continue;
      // Prefixes here are generated by ReorderableEffectChain, not by the file.
      const int slot = String(module.first.substr(slots_prefix.size())).getIntValue() - 1;
      if (!isPositiveAndBelow(slot, vital::constants::kMaxEffectSlots))
        continue;
      if (!slots[slot].impulse.is_null())
        module.second->irStateFromJson(slots[slot].impulse);
      else if (module.second->hasCustomIr())
        module.second->setDefaultIr();
      module.second->setLastBrowsedFile({});
    }
    for (int index = 0; index < vital::constants::kMaxEffectSlots; ++index) {
      const std::string table_prefix = prefix + "fx_slot_" + std::to_string(index + 1) + "_filter_fx";
      const json* table = index < int(slots.size()) && !slots[index].filter_table.is_null() ? &slots[index].filter_table : nullptr;
      if (table == nullptr) {
        resetFilterTable(table_prefix);
        continue;
      }
      if (WavetableCreator* creator = getFilterTableCreator(table_prefix, true)) {
        try {
          creator->jsonToState(*table);
          creator->render();
        }
        catch (const json::exception&) {
          resetFilterTable(table_prefix);
        }
      }
    }
    for (size_t index = 0; index < slots.size(); ++index) {
      const auto& slot = slots[index];
      const std::string slot_prefix = prefix + "fx_slot_" + std::to_string(index + 1) + "_";
      for (const auto& parameter : slot.parameters)
        valueChanged(parameter.first, parameter.second);
      valueChanged(slot_prefix + "enabled", slot.enabled ? 1.0f : 0.0f);
      valueChanged(slot_prefix + "type", static_cast<float>(slot.type));
    }
  }
  if (!was_suspended) {
    pauseProcessing(false);
    updateLatencyAfterControlChange(prefix + "fx_slot_1_type");
  }
  markStateChanged();
  error.clear();
  return true;
}

void SynthPlugin::getStateInformation(MemoryBlock& dest_data) {
  json data = LoadSave::stateToJson(this, getCallbackLock());
  data["tuning"] = getTuning()->stateToJson();
  data[kFxAutomationStateKey] = fxAutomationAssignmentsToJson();
  std::vector<double> song_notes[vital::kNumOscillators];
  std::vector<vital::SpeechSlot::Range> song_played[vital::kNumOscillators];
  bool any_song = false;
  {
    ScopedLock lock(getCallbackLock());
    for (int i = 0; i < vital::kNumOscillators; ++i) {
      song_notes[i] = engine_->getSpeechSlot(i)->songNotes();
      song_played[i] = engine_->getSpeechSlot(i)->playedRanges();
      any_song = any_song || !song_notes[i].empty() || !song_played[i].empty();
    }
  }
  if (any_song) {
    json slots = json::array();
    for (int i = 0; i < vital::kNumOscillators; ++i) {
      json played = json::array();
      for (const vital::SpeechSlot::Range& range : song_played[i])
        played.push_back({ range.start, range.end });
      slots.push_back({ { "notes", song_notes[i] }, { "played", std::move(played) } });
    }
    data[kSpeechSongKey] = std::move(slots);
  }

  // The JSON text and a terminating NUL, as MemoryOutputStream::writeString
  // wrote them, without its copies of the text.
  const std::string text = LoadSave::dumpState(data);
  dest_data.append(text.data(), text.size());
  const char terminator = 0;
  dest_data.append(&terminator, 1);
}

void SynthPlugin::setStateInformation(const void* data, int size_in_bytes) {
  // The host set this state: it is not a change for the host to save.
  struct Restoring {
    bool& flag;
    explicit Restoring(bool& restoring) : flag(restoring) { flag = true; }
    ~Restoring() { flag = false; }
  } restoring(restoring_state_);
  // The host's own state (a project, the host's undo, a host preset): the
  // patch's undo history no longer fits it.
  undo_history_.clear();

  // Parse while this instance still plays: only applying the state needs
  // processing paused. The state is JSON text, normally ending in a NUL.
  json json_data(json::value_t::discarded);
  if (data != nullptr && size_in_bytes > 0) {
    const char* text = static_cast<const char*>(data);
    size_t size = static_cast<size_t>(size_in_bytes);
    while (size > 0 && text[size - 1] == '\0')
      --size;
    if (size > 0)
      json_data = json::parse(text, text + size, nullptr, false);
    if (json_data.is_discarded()) {
      // Not plain UTF-8: read it the way JUCE reads text.
      MemoryInputStream stream(data, static_cast<size_t>(size_in_bytes), false);
      json_data = json::parse(stream.readEntireStreamAsString().toStdString(), nullptr, false);
    }
  }

  pauseProcessing(true);
  // Notes held across a restore (a host preset, undo, pasted FX state) are
  // released: note-offs sent while processing is paused never arrive, so a
  // note or an arpeggio would hang on into the new patch.
  engine_->allNotesOff(0);
  clearArpeggiators();
  if (json_data.is_discarded()) {
    NativeMessageBox::showMessageBoxAsync(MessageBoxIconType::WarningIcon, "Error opening preset",
                                          "There was an error open the preset. Preset file is corrupted.");
  }
  else {
    try {
      // jsonToState takes the state over; read the rest first.
      const bool has_fx_automation = json_data.is_object() && json_data.count(kFxAutomationStateKey);
      const bool has_tuning = json_data.is_object() && json_data.count("tuning");
      const bool has_song = json_data.is_object() && json_data.count(kSpeechSongKey) &&
                            json_data[kSpeechSongKey].is_array();
      const json song = has_song ? std::move(json_data[kSpeechSongKey]) : json();
      const json fx_automation = has_fx_automation ? std::move(json_data[kFxAutomationStateKey]) : json();
      const json tuning = has_tuning ? std::move(json_data["tuning"]) : json();
      LoadSave::jsonToState(this, save_info_, std::move(json_data));
      prepareSelectedSources();

      if (has_fx_automation)
        loadFxAutomationAssignments(fx_automation);
      else
        clearFxAutomationAssignments(false);

      if (has_tuning)
        getTuning()->jsonToState(tuning);

      for (int i = 0; i < vital::kNumOscillators; ++i) {
        std::vector<double> notes;
        std::vector<vital::SpeechSlot::Range> played;
        const json* slot = has_song && i < static_cast<int>(song.size()) && song[i].is_object() ? &song[i] : nullptr;
        if (slot != nullptr && slot->count("notes") && (*slot)["notes"].is_array()) {
          for (const json& note : (*slot)["notes"]) {
            if (note.is_number())
              notes.push_back(note.get<double>());
          }
        }
        if (slot != nullptr && slot->count("played") && (*slot)["played"].is_array()) {
          for (const json& range : (*slot)["played"]) {
            if (range.is_array() && range.size() == 2 && range[0].is_number() && range[1].is_number())
              played.push_back({ range[0].get<double>(), range[1].get<double>() });
          }
        }
        engine_->getSpeechSlot(i)->setSong(std::move(notes), std::move(played));
      }
    }
    catch (const json::exception& e) {
      std::string error = "There was an error open the preset. Preset file is corrupted.";
      NativeMessageBox::showMessageBoxAsync(MessageBoxIconType::WarningIcon,
                                            "Error opening preset", error);
    }
  }
  // Processing is paused, so none of this takes the callback lock, which
  // would make the host's audio thread wait for the effects to build.
  engine_->checkOversampling();
  prepareSelectedEffects();
  engine_->updateEffectLatency();
  updateFxSampleRate();
  setLatencySamples(engine_->getLatencySamples());
  suspendProcessing(false);

  SynthGuiInterface* editor = getGuiInterface();
  if (editor) {
    editor->updateFullGui();
    // The editor is open: what the user does next starts from this state.
    undo_history_.ensureBaseline();
  }
}

AudioProcessor* JUCE_CALLTYPE createPluginFilter() {
  return new SynthPlugin();
}
