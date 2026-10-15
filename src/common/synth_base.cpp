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

#include "synth_base.h"
#include "multisample_container.h"
#include "multisample_manager.h"
#include "multisample_slot.h"
#include "source_types.h"
#include "source_slot_module.h"

#include "sample_source.h"
#include "sound_engine.h"
#include "load_save.h"
#include "memory.h"
#include "modulation_connection_processor.h"
#include "startup.h"
#include "synth_gui_interface.h"
#include "synth_parameters.h"
#include "synth_strings.h"
#include "utils.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace {
  constexpr int kNumArpDestinations = 8;
  constexpr int kFirstAtlasArpDestinationVoiceChannel = 16;

  bool sourceIsEnvelope(const std::string& source) {
    return source.rfind("env_", 0) == 0;
  }

  bool sourceCanUpdateIdleDestination(const std::string& source) {
    return sourceIsEnvelope(source) ||
           source == "aftertouch" || source == "velocity" || source == "slide" || source == "lift" ||
           source == "mod_wheel" || source == "pitch_wheel";
  }

  bool sourceUsesControllerSmoothing(const std::string& source) {
    // Surge smooths performance controllers and custom controllers before
    // applying their routings. Mod Wheel already has one shared audio-rate
    // smoother in Atlas, so do not smooth it a second time per connection.
    return source == "aftertouch" || source == "slide" || source == "pitch_wheel" ||
           source.rfind("macro_control_", 0) == 0;
  }

  float controlValue(vital::control_map& controls, const std::string& name, float fallback) {
    auto found = controls.find(name);
    return found != controls.end() && found->second != nullptr ? found->second->value() : fallback;
  }

  constexpr std::array<const char*, 21> kArpeggiatorControlNames {
    "on", "time_mode", "sync_rate", "free_rate", "style", "gate", "random_gate", "repeats",
    "offset", "distance", "steps", "velocity", "velocity_decay", "random_velocity", "hold", "retrigger", "channel",
    "retrigger_division", "play_order", "loop", "octave"
  };

  AtlasArpeggiator::Settings arpeggiatorSettings(const std::array<vital::Value*, 21>& controls) {
    const auto value = [&](int index, float fallback) {
      return controls[index] ? controls[index]->value() : fallback;
    };
    AtlasArpeggiator::Settings settings;
    settings.enabled = value(0, 0) >= 0.5f;
    settings.time_mode = static_cast<int>(std::round(value(1, 0)));
    settings.sync_rate = static_cast<int>(std::round(value(2, 10)));
    settings.free_rate = value(3, 5);
    settings.style = static_cast<int>(std::round(value(4, 0)));
    settings.gate = value(5, 80);
    settings.random_gate = value(6, 0);
    settings.repeats = static_cast<int>(std::round(value(7, 1)));
    settings.offset = static_cast<int>(std::round(value(8, 0)));
    settings.distance = static_cast<int>(std::round(value(9, 12)));
    settings.steps = static_cast<int>(std::round(value(10, 1)));
    settings.velocity = static_cast<int>(std::round(value(11, 100)));
    settings.velocity_decay = value(12, 0);
    settings.random_velocity = value(13, 0);
    settings.hold = value(14, 0) >= 0.5f;
    settings.retrigger = static_cast<int>(std::round(value(15, 1)));
    settings.channel = static_cast<int>(std::round(value(16, 0)));
    settings.retrigger_division = static_cast<int>(std::round(
        value(17, AtlasArpeggiator::kDefaultRetriggerDivision)));
    settings.play_order = static_cast<int>(std::round(value(18, AtlasArpeggiator::kPlayOrderPitch)));
    settings.loop = static_cast<int>(std::round(value(19, AtlasArpeggiator::kLoopInfinite)));
    settings.octave = static_cast<int>(std::round(value(20, 0)));
    return settings;
  }

  bool arpeggiatorDestinationEnabled(vital::control_map& controls, int index, const std::string& destination) {
    const std::string prefix = "arp_" + std::to_string(index + 1) + "_";
    return controlValue(controls, prefix + "on", 0.0f) >= 0.5f &&
           controlValue(controls, prefix + destination, 1.0f) >= 0.5f;
  }

  bool sourceNeedsOriginalKeyboardPath(vital::control_map& controls, const std::string& destination) {
    for (int arp = 0; arp < vital::kNumArpeggiators; ++arp) {
      if (arpeggiatorDestinationEnabled(controls, arp, destination))
        return false;
    }
    return true;
  }

  bool anySourceNeedsOriginalKeyboardPath(vital::control_map& controls) {
    static const char* kDestinations[] = {
      "destination_osc_1", "destination_osc_2", "destination_osc_3", "destination_osc_4",
      "destination_osc_5", "destination_osc_6", "destination_osc_7", "destination_osc_8",
    };

    for (const char* destination : kDestinations) {
      if (sourceNeedsOriginalKeyboardPath(controls, destination))
        return true;
    }
    return false;
  }

  struct ArpDestinationZone {
    const char* destination;
    const char* key_start;
    const char* key_end;
    const char* velocity_start;
    const char* velocity_end;
  };

  const ArpDestinationZone kArpDestinationZones[] = {
    { "destination_osc_1", "osc_1_key_zone_start", "osc_1_key_zone_end",
      "osc_1_velocity_zone_start", "osc_1_velocity_zone_end" },
    { "destination_osc_2", "osc_2_key_zone_start", "osc_2_key_zone_end",
      "osc_2_velocity_zone_start", "osc_2_velocity_zone_end" },
    { "destination_osc_3", "osc_3_key_zone_start", "osc_3_key_zone_end",
      "osc_3_velocity_zone_start", "osc_3_velocity_zone_end" },
    { "destination_osc_4", "osc_4_key_zone_start", "osc_4_key_zone_end",
      "osc_4_velocity_zone_start", "osc_4_velocity_zone_end" },
    { "destination_osc_5", "osc_5_key_zone_start", "osc_5_key_zone_end",
      "osc_5_velocity_zone_start", "osc_5_velocity_zone_end" },
    { "destination_osc_6", "osc_6_key_zone_start", "osc_6_key_zone_end",
      "osc_6_velocity_zone_start", "osc_6_velocity_zone_end" },
    { "destination_osc_7", "osc_7_key_zone_start", "osc_7_key_zone_end",
      "osc_7_velocity_zone_start", "osc_7_velocity_zone_end" },
    { "destination_osc_8", "osc_8_key_zone_start", "osc_8_key_zone_end",
      "osc_8_velocity_zone_start", "osc_8_velocity_zone_end" },
  };


  bool noteIsInArpDestinationZone(vital::control_map& controls, int arp, const ArpDestinationZone& zone,
                                  int note, float velocity, bool check_velocity) {
    if (!arpeggiatorDestinationEnabled(controls, arp, zone.destination))
      return false;

    float key_start = controlValue(controls, zone.key_start, 0.0f);
    float key_end = controlValue(controls, zone.key_end, 127.0f);
    const float key_min = std::min(key_start, key_end);
    const float key_max = std::max(key_start, key_end);
    if (note < key_min || note > key_max)
      return false;

    if (!check_velocity)
      return true;

    float velocity_start = controlValue(controls, zone.velocity_start, 0.0f);
    float velocity_end = controlValue(controls, zone.velocity_end, 1.0f);
    const float velocity_min = std::min(velocity_start, velocity_end);
    const float velocity_max = std::max(velocity_start, velocity_end);
    return velocity >= velocity_min && velocity <= velocity_max;
  }

  bool shouldSendMessageToArpeggiator(vital::control_map& controls, int arp, int destination,
                                      const MidiMessage& message) {
    if (!isPositiveAndBelow(destination, kNumArpDestinations))
      return false;

    const auto& zone = kArpDestinationZones[destination];
    if (message.isNoteOn()) {
      return noteIsInArpDestinationZone(controls, arp, zone, message.getNoteNumber(),
                                        message.getFloatVelocity(), true);
    }
    if (message.isNoteOff()) {
      return noteIsInArpDestinationZone(controls, arp, zone, message.getNoteNumber(), 0.0f, false);
    }
    return false;
  }
} // namespace

SynthBase::SynthBase() : expired_(false) {
  expired_ = LoadSave::isExpired();
  engine_ = std::make_unique<vital::SoundEngine>();
  engine_->setTuning(&tuning_);
  multisample_manager_ = std::make_unique<vital::multisample::Manager>(
      [this](int slot, std::shared_ptr<const vital::multisample::Instrument> instrument, bool keep_sounding) {
        vital::MultiSampleSlot* target = engine_->getMultiSample(slot);
        // Freeing an instrument's audio can take a while: keep it out of the lock.
        std::shared_ptr<const vital::multisample::Instrument> previous = target->instrument();
        std::shared_ptr<const vital::multisample::Instrument> retired = target->retired();
        // A lock, not pauseProcessing: this also runs inside a paused preset load.
        ScopedLock lock(getCriticalSection());
        target->setInstrument(std::move(instrument), keep_sounding);
      });

  mod_connections_.reserve(vital::kMaxModulationConnections);

  for (int i = 0; i < vital::kNumOscillators; ++i) {
    vital::Wavetable* wavetable = engine_->getWavetable(i);
    if (wavetable) {
      wavetable_creators_[i] = std::make_unique<WavetableCreator>(wavetable);
      wavetable_creators_[i]->init();
    }
  }

  keyboard_state_ = std::make_unique<MidiKeyboardState>();
  midi_manager_ = std::make_unique<MidiManager>(this, keyboard_state_.get(), &save_info_, this);

  last_played_note_ = 0.0f;
  last_num_pressed_ = 0;
  audio_memory_ = std::make_unique<vital::StereoMemory>(vital::kAudioMemorySamples);
  memset(oscilloscope_memory_, 0, 2 * vital::kOscilloscopeMemoryResolution * sizeof(vital::poly_float));
  memset(oscilloscope_memory_write_, 0, 2 * vital::kOscilloscopeMemoryResolution * sizeof(vital::poly_float));
  memory_reset_period_ = vital::kOscilloscopeMemoryResolution;
  memory_input_offset_ = 0;
  memory_index_ = 0;

  controls_ = engine_->getControls();
  // The engine built its default slot types in its own init; the default
  // (one-frame) tables are already rendered, so only the flags need setting.
  for (int i = 0; i < vital::kNumOscillators; ++i)
    wavetable_render_pending_[i] = !slotUsesWavetable(i);
  for (int arp = 0; arp < vital::kNumArpeggiators; ++arp) {
    const std::string prefix = "arp_" + std::to_string(arp + 1) + "_";
    for (size_t i = 0; i < kArpeggiatorControlNames.size(); ++i) {
      const auto found = controls_.find(prefix + kArpeggiatorControlNames[i]);
      if (found != controls_.end()) arpeggiator_controls_[arp][i] = found->second;
    }
  }
  for (int destination = 0; destination < kNumArpDestinations; ++destination) {
    const auto found = controls_.find("osc_" + std::to_string(destination + 1) + "_on");
    if (found != controls_.end()) arp_destination_osc_on_[destination] = found->second;
  }
  const std::string compression_suffix = "spectral_compressor_compression";
  for (const auto& control : controls_) {
    const auto& name = control.first;
    if (name.size() >= compression_suffix.size() &&
        name.compare(name.size() - compression_suffix.size(), compression_suffix.size(), compression_suffix) == 0) {
      const auto prefix = name.substr(0, name.size() - std::string("compression").size());
      spectral_compression_targets_[name] = {{ prefix + "downward", prefix + "upward" }};
    }
  }
  for (const auto& control : controls_)
    if (!isEffectParameter(control.first))
      pending_gui_updates_.try_emplace(control.first);
  for (int arp = 0; arp < vital::kNumArpeggiators; ++arp) {
    for (int destination = 0; destination < kNumArpDestinations; ++destination) {
      const int64 seed = 0x41544c4153415250ll + arp * kNumArpDestinations + destination;
      arpeggiators_[arp][destination].prepare(engine_->getSampleRate(), seed);
    }
  }
  prepareMidiSchedulingBuffers();

  Startup::doStartupChecks(midi_manager_.get());
  if (LoadSave::shouldScanDownloads())
    downloaded_presets_imported_ = LoadSave::scanDownloadsForPresets();
}

SynthBase::~SynthBase() {
  alive_->store(false);
}

bool SynthBase::isSourceTypeControl(const std::string& name) {
  if (name.size() < 10 || name.rfind("osc_", 0) != 0 || name.compare(name.size() - 5, 5, "_type") != 0)
    return false;
  for (int slot = 0; slot < vital::kNumOscillators; ++slot) {
    if (name == vital::source_types::slotPrefix(slot) + "type")
      return true;
  }
  return false;
}

bool SynthBase::isEffectSlotTypeControl(const std::string& name) {
  size_t position = 0;
  if (name.rfind("bus_", 0) == 0) {
    position = name.find('_', 4);
    if (position == std::string::npos) return false;
    ++position;
  }
  static const std::string slot = "fx_slot_", type = "_type";
  if (name.compare(position, slot.size(), slot) != 0) return false;
  position += slot.size();
  const size_t digits = position;
  while (position < name.size() && std::isdigit(static_cast<unsigned char>(name[position]))) ++position;
  return position > digits && name.compare(position, std::string::npos, type) == 0;
}

void SynthBase::scheduleEffectControlReservation() {
  // An effect's parameter controls grow their buffers when a slot first
  // selects it. That allocates and must not race the controls' writers, so it
  // runs on the message thread holding the callback lock. Until then the slot
  // passes audio through, as while its effect is being built.
  auto reserve = [this] {
    const ScopedLock lock(getCriticalSection());
    updateEffectShells();
  };
  if (MessageManager::getInstance()->isThisTheMessageThread()) {
    reserve();
    return;
  }
  if (effect_reservation_queued_.exchange(true))
    return;
  std::shared_ptr<std::atomic<bool>> alive = alive_;
  MessageManager::callAsync([this, alive, reserve] {
    if (!alive->load())
      return;
    effect_reservation_queued_ = false;
    reserve();
  });
}

void SynthBase::updateEffectShells() {
  processModulationChanges();
  engine_->parkUnselectedEffects();
  engine_->reserveSelectedEffectControls();
}

void SynthBase::prepareSelectedEffects() {
  updateEffectShells();
  engine_->prepareSelectedEffects();
}

void SynthBase::scheduleSourcePreparation() {
  // Building a type's engine allocates, so it never happens on the audio thread.
  if (MessageManager::getInstance()->isThisTheMessageThread()) {
    pauseProcessing(true);
    prepareSelectedSources();
    pauseProcessing(false);
    return;
  }
  if (source_preparation_queued_)
    return;
  source_preparation_queued_ = true;
  std::shared_ptr<std::atomic<bool>> alive = alive_;
  MessageManager::callAsync([this, alive] {
    if (!alive->load())
      return;
    source_preparation_queued_ = false;
    pauseProcessing(true);
    prepareSelectedSources();
    pauseProcessing(false);
  });
}

void SynthBase::valueChanged(const std::string& name, vital::mono_float value) {
  const bool changed = controls_[name]->value() != value;
  controls_[name]->set(value);
  if (changed && isSourceTypeControl(name))
    scheduleSourcePreparation();
  if (changed && isEffectSlotTypeControl(name))
    scheduleEffectControlReservation();
  // Filter Table allocates its convolution buffers when a filter first selects it.
  if (static_cast<int>(value + 0.5f) == vital::constants::kFilterTable) {
    const std::string filter_prefix = filterPrefixForModelControl(name);
    if (!filter_prefix.empty())
      prepareFilterTable(filter_prefix);
  }
  const auto macro = spectral_compression_targets_.find(name);
  if (changed && macro != spectral_compression_targets_.end()) {
    const float amount = std::max(0.0f, std::min(100.0f, value));
    for (const auto& target : macro->second) {
      const auto found = controls_.find(target);
      if (found == controls_.end()) continue;
      found->second->set(amount);
      beginChangeGesture(target);
      setValueNotifyHost(target, amount);
      endChangeGesture(target);
      queueGuiControlUpdate(target, amount);
    }
  }
}

void SynthBase::valueChangedInternal(const std::string& name, vital::mono_float value) {
  valueChanged(name, value);
  setValueNotifyHost(name, value);
}

void SynthBase::valueChangedThroughMidi(const std::string& name, vital::mono_float value) {
  valueChanged(name, value);
  setValueNotifyHost(name, value);
  queueGuiControlUpdate(name, value);
}

void SynthBase::valueMidiLearned(const std::string& name, int midi_id) {
  if (!vital::Parameters::isParameter(name))
    return;
  midi_learned_id_.store(midi_id, std::memory_order_relaxed);
  midi_learned_name_.store(&vital::Parameters::getDetails(name).name, std::memory_order_release);
  gui_updates_pending_.store(true, std::memory_order_release);
}

bool SynthBase::isEffectParameter(const std::string& name) {
  size_t position = 0;
  if (name.rfind("bus_", 0) == 0) {
    position = name.find('_', 4);
    if (position == std::string::npos) return false;
    ++position;
  }
  if (name.compare(position, 8, "fx_slot_") == 0)
    return true;
  for (const auto& effect : strings::kEffectOrder) {
    if (name.compare(position, effect.size(), effect) == 0 && name.size() > position + effect.size() &&
        name[position + effect.size()] == '_')
      return true;
  }
  return false;
}

void SynthBase::pitchWheelMidiChanged(vital::mono_float value) {
  queueGuiControlUpdate("pitch_wheel", value);
}

void SynthBase::modWheelMidiChanged(vital::mono_float value) {
  queueGuiControlUpdate("mod_wheel", value);
}

void SynthBase::pitchWheelGuiChanged(vital::mono_float value) {
  engine_->setZonedPitchWheel(value, 0, vital::kNumMidiChannels - 1);
}

void SynthBase::modWheelGuiChanged(vital::mono_float value) {
  engine_->setModWheelAllChannels(value);
}

void SynthBase::presetChangedThroughMidi(File preset) {
  SynthGuiInterface* gui_interface = getGuiInterface();
  if (gui_interface) {
    gui_interface->updateFullGui();
    gui_interface->notifyFresh();
  }
}

void SynthBase::valueChangedExternal(const std::string& name, vital::mono_float value) {
  valueChanged(name, value);
  if (name == "mod_wheel")
    engine_->setModWheelAllChannels(value);
  else if (name == "pitch_wheel")
    engine_->setZonedPitchWheel(value, 0, vital::kNumMidiChannels - 1);

  queueGuiControlUpdate(name, value);
}

vital::ModulationConnection* SynthBase::getConnection(const std::string& source, const std::string& destination) {
  for (vital::ModulationConnection* connection : mod_connections_) {
    if (connection->source_name == source && connection->destination_name == destination)
      return connection;
  }
  return nullptr;
}

int SynthBase::getConnectionIndex(const std::string& source, const std::string& destination) {
  vital::ModulationConnectionBank& modulation_bank = getModulationBank();
  for (int i = 0; i < vital::kMaxModulationConnections; ++i) {
    vital::ModulationConnection* connection = modulation_bank.atIndex(i);
    if (connection->source_name == source && connection->destination_name == destination)
      return i;
  }
  return -1;
}

vital::modulation_change SynthBase::createModulationChange(vital::ModulationConnection* connection) {
  vital::modulation_change change;
  change.source = engine_->getModulationSource(connection->source_name);
  change.independent_random_source = engine_->getIndependentRandomModulationSource(
      connection->source_name, connection->modulation_processor->index());
  change.mono_destination = engine_->getMonoModulationDestination(connection->destination_name);
  change.mono_modulation_switch = engine_->getMonoModulationSwitch(connection->destination_name);
  VITAL_ASSERT(change.source != nullptr);
  VITAL_ASSERT(change.mono_destination != nullptr);
  VITAL_ASSERT(change.mono_modulation_switch != nullptr);

  change.destination_scale = vital::Parameters::getParameterRange(connection->destination_name);
  change.poly_modulation_switch = engine_->getPolyModulationSwitch(connection->destination_name);
  change.poly_destination = engine_->getPolyModulationDestination(connection->destination_name);
  change.modulation_processor = connection->modulation_processor.get();
  change.source_slot = vital::source_types::slotForOutputSource(connection->source_name);
  if (change.source_slot >= 0)
    change.destination_slot = vital::source_types::slotForControl(connection->destination_name);
  change.modulation_processor->setProcessWhenIdle(sourceCanUpdateIdleDestination(connection->source_name));
  change.modulation_processor->setRefreshSourceWhenIdle(sourceIsEnvelope(connection->source_name));
  change.modulation_processor->setSourceSmoothing(
      sourceUsesControllerSmoothing(connection->source_name));

  int num_audio_rate = 0;
  vital::ModulationConnectionBank& modulation_bank = getModulationBank();
  for (int i = 0; i < vital::kMaxModulationConnections; ++i) {
    if (modulation_bank.atIndex(i)->source_name == connection->source_name &&
        modulation_bank.atIndex(i)->destination_name != connection->destination_name &&
        !modulation_bank.atIndex(i)->modulation_processor->isControlRate()) {
      num_audio_rate++;
    }
  }
  change.num_audio_rate = num_audio_rate;
  return change;
}

bool SynthBase::isInvalidConnection(const vital::modulation_change& change) {
  // Some parameters (e.g. the envelope power sliders) are plain base controls and are not
  // registered as modulation destinations, so the engine has no destination to plug into.
  // Connecting to one would dereference a null destination on the audio thread, so treat any
  // change with a missing destination as invalid here rather than letting it reach the engine.
  if (change.mono_destination == nullptr)
    return true;
  return change.poly_destination && change.poly_destination->router() == change.modulation_processor;
}

bool SynthBase::acceptsOscillatorModulation(int slot, const std::string& destination) {
  // Another oscillator or a voice filter: both render after the source
  // oscillator within the block. LFOs and the like render before it, and
  // feeding audio back into a modulator invites loops, so they are left out.
  const int destination_slot = vital::source_types::slotForControl(destination);
  const bool voice_filter = destination.rfind("filter_1_", 0) == 0 || destination.rfind("filter_2_", 0) == 0;
  if (slot < 0 || destination_slot == slot || (destination_slot < 0 && !voice_filter))
    return false;
  vital::Processor* poly_destination = engine_->getPolyModulationDestination(destination);
  return poly_destination != nullptr && !poly_destination->isControlRate();
}

void SynthBase::prepareModulationDestination(const std::string& destination) {
  // A parked effect has no modulation wiring; build the destination's (it
  // stays while a route reaches it). This changes the graph, so the audio
  // thread must be out of it.
  const ScopedLock lock(getCriticalSection());
  engine_->wireModulationDestination(destination);
}

void SynthBase::connectModulation(vital::ModulationConnection* connection) {
  prepareModulationDestination(connection->destination_name);
  vital::modulation_change change = createModulationChange(connection);
  // An oscillator's audio reaches only the audio-rate controls of the other
  // oscillators and the voice filters.
  const bool refused_oscillator_route =
      change.source_slot >= 0 && !acceptsOscillatorModulation(change.source_slot, connection->destination_name);
  if (refused_oscillator_route || isInvalidConnection(change)) {
    connection->destination_name = "";
    connection->source_name = "";
  }
  else if (mod_connections_.count(connection) == 0) {
    change.disconnecting = false;
    mod_connections_.push_back(connection);
    modulation_change_queue_.enqueue(change);
  }
}

bool SynthBase::connectModulation(const std::string& source, const std::string& destination) {
  vital::ModulationConnection* connection = getConnection(source, destination);
  bool create = connection == nullptr;
  if (create) {
    connection = getModulationBank().createConnection(source, destination);

    // Empty modulation slots retain their parameter values so they can be
    // automated by a host. Never let those stale values become active on a
    // newly assigned route: initialize the complete slot before queueing the
    // connection to the audio thread.
    if (connection != nullptr) {
      const int index = getConnectionIndex(source, destination);
      if (index >= 0) {
        const std::string prefix = "modulation_" + std::to_string(index + 1) + "_";
        auto reset_control = [this, &prefix](const char* suffix, vital::mono_float value) {
          auto found = controls_.find(prefix + suffix);
          if (found != controls_.end() && found->second != nullptr)
            found->second->set(value);
        };
        reset_control("amount", 0.0f);
        reset_control("power", 0.0f);
        reset_control("bipolar",
                      vital::ModulationConnection::isModulationSourceDefaultBipolar(source) ? 1.0f : 0.0f);
        reset_control("stereo", 0.0f);
        reset_control("bypass", 0.0f);
        reset_control("random_mode", 0.0f);
        connection->modulation_processor->initializeMapping();
      }
    }
  }

  if (connection)
    connectModulation(connection);

  return create;
}

void SynthBase::disconnectModulation(vital::ModulationConnection* connection) {
  if (mod_connections_.count(connection) == 0)
    return;

  vital::modulation_change change = createModulationChange(connection);
  connection->source_name = "";
  connection->destination_name = "";

  mod_connections_.remove(connection);
  change.disconnecting = true;
  modulation_change_queue_.enqueue(change);
}

void SynthBase::disconnectModulation(const std::string& source, const std::string& destination) {
  vital::ModulationConnection* connection = getConnection(source, destination);
  if (connection)
    disconnectModulation(connection);
}

void SynthBase::clearModulations() {
  clearModulationQueue();
  
  while (mod_connections_.size()) {
    vital::ModulationConnection* connection = *mod_connections_.begin();
    mod_connections_.remove(connection);
    vital::modulation_change change = createModulationChange(connection);
    change.disconnecting = true;
    engine_->disconnectModulation(change);
    connection->source_name = "";
    connection->destination_name = "";
  }

  int num_connections = static_cast<int>(getModulationBank().numConnections());
  for (int i = 0; i < num_connections; ++i)
    getModulationBank().atIndex(i)->modulation_processor->lineMapGenerator()->initLinear();

  engine_->disableUnnecessaryModSources();
}

void SynthBase::forceShowModulation(const std::string& source, bool force) {
  if (force)
    engine_->enableModSource(source);
  else if (!isSourceConnected(source))
    engine_->disableModSource(source);
}

bool SynthBase::isModSourceEnabled(const std::string& source) {
  return engine_->isModSourceEnabled(source);
}

int SynthBase::getNumModulations(const std::string& destination) {
  int connections = 0;
  for (vital::ModulationConnection* connection : mod_connections_) {
    if (connection->destination_name == destination)
      connections++;
  }
  return connections;
}

std::vector<vital::ModulationConnection*> SynthBase::getSourceConnections(const std::string& source) {
  std::vector<vital::ModulationConnection*> connections;
  for (vital::ModulationConnection* connection : mod_connections_) {
    if (connection->source_name == source)
      connections.push_back(connection);
  }
  return connections;
}

bool SynthBase::isSourceConnected(const std::string& source) {
  for (vital::ModulationConnection* connection : mod_connections_) {
    if (connection->source_name == source)
      return true;
  }
  return false;
}

std::vector<vital::ModulationConnection*> SynthBase::getDestinationConnections(const std::string& destination) {
  std::vector<vital::ModulationConnection*> connections;
  for (vital::ModulationConnection* connection : mod_connections_) {
    if (connection->destination_name == destination)
      connections.push_back(connection);
  }
  return connections;
}

const vital::StatusOutput* SynthBase::getStatusOutput(const std::string& name) {
  return engine_->getStatusOutput(name);
}

vital::Wavetable* SynthBase::getWavetable(int index) {
  return engine_->getWavetable(index);
}

WavetableCreator* SynthBase::getWavetableCreator(int index) {
  return wavetable_creators_[index].get();
}

WavetableCreator* SynthBase::getFilterTableCreator(const std::string& prefix, bool create) {
  auto found = filter_table_creators_.find(prefix);
  if (found != filter_table_creators_.end())
    return found->second.get();
  if (!create)
    return nullptr;
  // Tables live for the plugin's lifetime: the audio thread may still hold a
  // pointer after a reset, so a detached table is kept rather than freed.
  std::shared_ptr<vital::Wavetable>& table = filter_tables_[prefix];
  if (table == nullptr)
    table = std::make_shared<vital::Wavetable>(vital::kNumOscillatorWaveFrames);
  auto creator = std::make_unique<WavetableCreator>(table.get());
  creator->init();
  WavetableCreator* result = creator.get();
  filter_table_creators_[prefix] = std::move(creator);
  prepareFilterTable(prefix);
  engine_->setFilterTable(prefix, table.get());
  return result;
}

std::vector<std::string> SynthBase::filterTablePrefixes() const {
  std::vector<std::string> result;
  for (const auto& entry : filter_table_creators_)
    result.push_back(entry.first);
  return result;
}

void SynthBase::resetFilterTable(const std::string& prefix) {
  if (filter_table_creators_.erase(prefix))
    engine_->setFilterTable(prefix, nullptr);
}

std::string SynthBase::filterPrefixForModelControl(const std::string& name) {
  const std::string suffix = "_model";
  if (name.size() <= suffix.size() || name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0)
    return {};
  const std::string prefix = name.substr(0, name.size() - suffix.size());
  const std::string fx = "filter_fx";
  const bool filter_fx = prefix.size() >= fx.size() && prefix.compare(prefix.size() - fx.size(), fx.size(), fx) == 0;
  if (prefix == "filter_1" || prefix == "filter_2" || filter_fx)
    return prefix;
  return {};
}

void SynthBase::prepareFilterTable(const std::string& prefix) {
  // Allocation belongs on the message thread; host automation may reach here
  // from the audio thread, in which case the filter passes audio through until
  // its buffers exist.
  if (MessageManager::existsAndIsCurrentThread()) {
    const ScopedLock lock(getCriticalSection());
    engine_->prepareFilterTable(prefix);
    return;
  }
  std::shared_ptr<std::atomic<bool>> alive = alive_;
  MessageManager::callAsync([this, alive, prefix] {
    if (!alive->load())
      return;
    const ScopedLock lock(getCriticalSection());
    engine_->prepareFilterTable(prefix);
  });
}

void SynthBase::clearFilterTables() {
  for (const auto& entry : filter_table_creators_)
    engine_->setFilterTable(entry.first, nullptr);
  filter_table_creators_.clear();
}

vital::Sample* SynthBase::getSample(int index) {
  return engine_->getSample(index);
}

void SynthBase::prepareSelectedSources() {
  // Wavetables exist only for the slots that play them. A slot leaving the
  // wavetable group drops its table to one frame (the creator keeps the state);
  // a slot joining it renders the table it was holding back.
  for (int slot = 0; slot < vital::kNumOscillators; ++slot) {
    WavetableCreator* creator = getWavetableCreator(slot);
    vital::Wavetable* wavetable = engine_->getWavetable(slot);
    if (creator == nullptr || wavetable == nullptr)
      continue;
    if (slotUsesWavetable(slot)) {
      if (wavetable_render_pending_[slot])
        creator->render();
      wavetable_render_pending_[slot] = false;
    }
    else {
      if (wavetable->numFrames() > 1)
        wavetable->loadDefaultWavetable();
      wavetable_render_pending_[slot] = true;
    }
  }
  prepareSourceEngines();
}

void SynthBase::prepareSourceEngines() {
  // A slot changing type gets a new engine, with its own modulation
  // destinations. Routes into a parameter of the type it leaves are dropped.
  // The others (level, pan, the FM/RM amounts) are lifted off the old engine
  // and plugged into the new one; left alone they would feed a destroyed
  // destination and modulate nothing.
  std::vector<vital::ModulationConnection*> stale;
  std::vector<std::pair<vital::ModulationConnection*, std::pair<std::string, std::string>>> moving;
  for (int slot = 0; slot < vital::kNumOscillators; ++slot) {
    vital::SourceSlotModule* module = engine_->getSourceSlot(slot);
    if (module == nullptr)
      continue;
    const int requested = module->requestedType();
    const int active = module->activeType();
    if (module->hasActiveModule() && vital::source_types::shareModule(active, requested))
      continue;
    const std::string prefix = vital::source_types::slotPrefix(slot);
    for (vital::ModulationConnection* connection : mod_connections_) {
      const std::string& destination = connection->destination_name;
      if (destination.rfind(prefix, 0) != 0)
        continue;
      const std::string suffix = destination.substr(prefix.size());
      const int owner = vital::source_types::typeForParameterSuffix(suffix);
      const bool wavetable_group = owner < 0;
      const bool stays = wavetable_group ? (vital::source_types::describe(requested).uses_wavetable ||
                                            vital::source_types::isSharedSlotParameter(suffix))
                                         : vital::source_types::shareModule(owner, requested);
      if (stays)
        moving.push_back({ connection, { connection->source_name, destination } });
      else
        stale.push_back(connection);
    }
  }
  for (vital::ModulationConnection* connection : stale)
    disconnectModulation(connection);
  for (auto& route : moving)
    disconnectModulation(route.first);
  // Route changes are queued for the audio thread; apply them now, while
  // processing is paused, so none of them touches an engine we destroy below.
  processModulationChanges();
  engine_->prepareSelectedSources();
  for (auto& route : moving) {
    route.first->source_name = route.second.first;
    route.first->destination_name = route.second.second;
    connectModulation(route.first);
  }
  processModulationChanges();
}

bool SynthBase::slotUsesWavetable(int slot) const {
  const auto found = controls_.find(vital::source_types::slotPrefix(slot) + "type");
  const int type = found == controls_.end() ? vital::source_types::kWavetable
                                            : static_cast<int>(found->second->value() + 0.5f);
  return vital::source_types::describe(type).uses_wavetable;
}

bool SynthBase::isWavetableRenderPending(int slot) const {
  return isPositiveAndBelow(slot, vital::kNumOscillators) && wavetable_render_pending_[slot];
}

void SynthBase::renderWavetable(int slot) {
  WavetableCreator* creator = getWavetableCreator(slot);
  if (creator == nullptr)
    return;
  if (slotUsesWavetable(slot)) {
    creator->render();
    wavetable_render_pending_[slot] = false;
  }
  else {
    wavetable_render_pending_[slot] = true;
  }
}

LineGenerator* SynthBase::getLfoSource(int index) {
  return engine_->getLfoSource(index);
}

std::vector<std::pair<std::string, vital::ConvolutionReverbModule*>> SynthBase::getConvolutionReverbModules() {
  return engine_->getConvolutionReverbModules();
}

json SynthBase::saveToJson() {
  return LoadSave::stateToJson(this, getCriticalSection());
}

int SynthBase::getSampleRate() {
  return engine_->getSampleRate();
}

void SynthBase::initEngine() {
  clearPendingGuiUpdates();
  clearModulations();
  multisample_manager_->clearAll();
  if (getWavetableCreator(0)) {
    for (int i = 0; i < vital::kNumOscillators; ++i)
      getWavetableCreator(i)->init();

    for (int i = 0; i < vital::kNumOscillators; ++i)
      engine_->getSample(i)->init();
  }
  clearFilterTables();

  for (int i = 0; i < vital::kNumLfos; ++i)
    getLfoSource(i)->initTriangle();

  vital::control_map controls = engine_->getControls();
  for (auto& control : controls) {
    vital::ValueDetails details = vital::Parameters::getDetails(control.first);
    control.second->set(details.default_value);
  }
  prepareSelectedSources();
  for (auto& arp_group : arpeggiators_) {
    for (auto& arp : arp_group)
      arp.clear();
  }
  checkOversampling();

  clearActiveFile();
}

void SynthBase::loadTuningFile(const File& file) {
  tuning_.loadFile(file);
}

void SynthBase::loadInitPreset() {
  pauseProcessing(true);
  engine_->allSoundsOff();
  initEngine();
  LoadSave::initSaveInfo(save_info_);
  pauseProcessing(false);
}

bool SynthBase::loadFromJson(const json& data) {
  pauseProcessing(true);
  engine_->allSoundsOff();
  try {
    bool result = LoadSave::jsonToState(this, save_info_, data);
    prepareSelectedSources();
    pauseProcessing(false);
    return result;
  }
  catch (const json::exception& e) {
    pauseProcessing(false);
    throw e;
  }
}

bool SynthBase::loadFromFile(File preset, std::string& error) {
  if (!preset.exists())
    return false;
  
  std::string text;
  if (!vital::multisample::container::readPresetText(preset, text)) {
    error = "Preset file is corrupted.";
    return false;
  }
  // A container preset carries its instruments' files; the state names them.
  multisample_container_ = vital::multisample::container::isContainer(preset) ? preset : File();
  try {
    json parsed_json_state = json::parse(text, nullptr);
    if (!loadFromJson(parsed_json_state)) {
      multisample_container_ = File();
      error = "Preset was created with a newer version.";
      return false;
    }

    active_file_ = preset;
  }
  catch (const json::exception& e) {
    multisample_container_ = File();
    error = "Preset file is corrupted.";
    return false;
  }
  multisample_container_ = File();
  
  setPresetName(preset.getFileNameWithoutExtension());

  SynthGuiInterface* gui_interface = getGuiInterface();
  if (gui_interface) {
    gui_interface->updateFullGui();
    gui_interface->notifyFresh();
  }

  return true;
}

void SynthBase::renderAudioToFile(File file, float seconds, float bpm, std::vector<int> notes, bool render_images) {
  static constexpr int kSampleRate = 44100;
  static constexpr int kPreProcessSamples = 44100;
  static constexpr int kFadeSamples = 200;
  static constexpr int kBufferSize = 64;
  static constexpr int kVideoRate = 30;
  static constexpr int kImageNumberPlaces = 3;
  static constexpr int kImageWidth = 500;
  static constexpr int kImageHeight = 250;
  static constexpr int kOscilloscopeResolution = 512;
  static constexpr float kFadeRatio = 0.3f;

  ScopedLock lock(getCriticalSection());

  processModulationChanges();
  engine_->setSampleRate(kSampleRate);
  engine_->setBpm(bpm);
  engine_->updateAllModulationSwitches();

  double sample_time = 1.0 / getSampleRate();
  double current_time = -kPreProcessSamples * sample_time;

  for (int samples = 0; samples < kPreProcessSamples; samples += kBufferSize) {
    engine_->correctToTime(current_time);
    current_time += kBufferSize * sample_time;
    engine_->process(kBufferSize);
  }

  for (int note : notes)
    engine_->noteOn(note, 0.7f, 0, 0);

  file.deleteFile();
  std::unique_ptr<FileOutputStream> file_stream = file.createOutputStream();
  WavAudioFormat wav_format;
  std::unique_ptr<AudioFormatWriter> writer(wav_format.createWriterFor(file_stream.get(), kSampleRate, 2, 16, {}, 0));

  int on_samples = seconds * kSampleRate;
  int total_samples = on_samples + seconds * kSampleRate * kFadeRatio;
  std::unique_ptr<float[]> left_buffer = std::make_unique<float[]>(kBufferSize);
  std::unique_ptr<float[]> right_buffer = std::make_unique<float[]>(kBufferSize);
  float* buffers[2] = { left_buffer.get(), right_buffer.get() };
  const vital::mono_float* engine_output = (const vital::mono_float*)engine_->output(0)->buffer;

#if JUCE_MODULE_AVAILABLE_juce_graphics
  int current_image_index = -1;
  PNGImageFormat png;
  File images_folder = File::getCurrentWorkingDirectory().getChildFile("images");
  if (!images_folder.exists() && render_images)
    images_folder.createDirectory();
  const vital::poly_float* memory = getOscilloscopeMemory();
#endif

  for (int samples = 0; samples < total_samples; samples += kBufferSize) {
    engine_->correctToTime(current_time);
    current_time += kBufferSize * sample_time;
    engine_->process(kBufferSize);
    updateMemoryOutput(kBufferSize, engine_->output(0)->buffer);

    if (on_samples > samples && on_samples <= samples + kBufferSize) {
      for (int note : notes)
        engine_->noteOff(note, 0.5f, 0, 0);
    }

    for (int i = 0; i < kBufferSize; ++i) {
      vital::mono_float t = (total_samples - samples) / (1.0f * kFadeSamples);
      t = vital::utils::min(t, 1.0f);
      left_buffer[i] = t * engine_output[vital::poly_float::kSize * i];
      right_buffer[i] = t * engine_output[vital::poly_float::kSize * i + 1];
    }

    writer->writeFromFloatArrays(buffers, 2, kBufferSize);

  #if JUCE_MODULE_AVAILABLE_juce_graphics
    int image_index = (samples * kVideoRate) / kSampleRate;
    if (image_index > current_image_index && render_images) {
      current_image_index = image_index;
      String number(image_index);
      while (number.length() < kImageNumberPlaces)
        number = "0" + number;

      File image_file = images_folder.getChildFile("rendered_image" + number + ".png");
      FileOutputStream image_file_stream(image_file);
      Image image(Image::RGB, kImageWidth, kImageHeight, true);
      Graphics g(image);
      g.fillAll(Colour(0xff1d2125));

      Path left_path;
      Path right_path;
      left_path.startNewSubPath(-2.0f, kImageHeight / 2);
      right_path.startNewSubPath(-2.0f, kImageHeight / 2);

      for (int i = 0; i < kOscilloscopeResolution; ++i) {
        float t = i / (kOscilloscopeResolution - 1.0f);
        float memory_spot = (1.0f * i * vital::kOscilloscopeMemoryResolution) / kOscilloscopeResolution;
        int memory_index = memory_spot;
        float remainder = memory_spot - memory_index;
        vital::poly_float from = memory[memory_index];
        vital::poly_float to = memory[memory_index + 1];
        vital::poly_float y = -vital::utils::interpolate(from, to, remainder) * kImageHeight / 2.0f + kImageHeight / 2;
        left_path.lineTo(t * kImageWidth, y[0]);
        right_path.lineTo(t * kImageWidth, y[1]);
      }
      left_path.lineTo(kImageWidth + 2.0f, kImageHeight / 2.0f);
      right_path.lineTo(kImageWidth + 2.0f, kImageHeight / 2.0f);

      g.setColour(Colour(0x64aa88ff));
      g.fillPath(left_path);
      g.fillPath(right_path);

      g.setColour(Colour(0xffaa88ff));
      g.strokePath(left_path, PathStrokeType(2.0f, PathStrokeType::curved, PathStrokeType::rounded));
      g.strokePath(right_path, PathStrokeType(2.0f, PathStrokeType::curved, PathStrokeType::rounded));

      png.writeImageToStream(image, image_file_stream);
    }
  #endif
  }

  writer->flush();
  file_stream->flush();

  writer = nullptr;
  file_stream.release();
}

void SynthBase::renderAudioForResynthesis(float* data, int samples, int note) {
  static constexpr int kPreProcessSamples = 44100;
  static constexpr int kBufferSize = 64;

  ScopedLock lock(getCriticalSection());

  double sample_time = 1.0 / getSampleRate();
  double current_time = -kPreProcessSamples * sample_time;

  engine_->allSoundsOff();
  for (int s = 0; s < kPreProcessSamples; s += kBufferSize) {
    engine_->correctToTime(current_time);
    current_time += kBufferSize * sample_time;
    engine_->process(kBufferSize);
  }

  engine_->noteOn(note, 0.7f, 0, 0);
  const vital::poly_float* engine_output = engine_->output(0)->buffer;
  float max_value = 0.01f;
  for (int s = 0; s < samples; s += kBufferSize) {
    int num_samples = std::min(samples - s, kBufferSize);
    engine_->correctToTime(current_time);
    current_time += num_samples * sample_time;
    engine_->process(num_samples);

    for (int i = 0; i < num_samples; ++i) {
      float sample = engine_output[i][0];
      data[s + i] = sample;
      max_value = std::max(max_value, fabsf(sample));
    }
  }

  float scale = 1.0f / max_value;
  for (int s = 0; s < samples; ++s)
    data[s] *= scale;

  engine_->allSoundsOff();
}

bool SynthBase::saveCopyToFile(File preset) {
  preset = preset.withFileExtension(String(vital::kPresetExtension));

  File parent = preset.getParentDirectory();
  if (!parent.exists()) {
    if (!parent.createDirectory().wasOk() || !parent.hasWriteAccess())
      return false;
  }

  return writePresetFile(preset);
}

bool SynthBase::writePresetFile(const File& preset, std::string* error) {
  namespace multisample = vital::multisample;
  // Loads still running would change what the state names.
  multisample_manager_->waitForPendingLoads(120000);
  multisample_manager_->processPendingResults();

  json data = saveToJson();
  std::vector<multisample::container::Instrument> instruments;
  json& settings = data["settings"];
  for (int slot = 0; slot < vital::kNumOscillators; ++slot) {
    const std::string key = vital::source_types::slotPrefix(slot) + "multisample";
    if (!settings.count(key))
      continue;
    std::shared_ptr<const multisample::LoadResult> result = multisample_manager_->result(slot);
    multisample::InstrumentSpec spec = multisample_manager_->spec(slot);
    if (result == nullptr || result->hash != spec.hash)
      continue;
    spec.files = result->files;
    for (const multisample::FileInfo& file : result->files) {
      if (file.relative.rfind("_external/", 0) == 0 && !file.written.empty())
        spec.path_overrides[file.written] = file.relative;
    }
    settings[key] = spec.toJson(true);
    instruments.push_back({ result->hash, result->files });
  }

  if (instruments.empty())
    return preset.replaceWithText(data.dump());

  std::string message;
  const bool written = multisample::container::write(preset, data.dump(), instruments, message);
  if (!written && error != nullptr)
    *error = message;
  return written;
}

bool SynthBase::saveToFile(File preset) {
  preset = preset.withFileExtension(String(vital::kPresetExtension));

  File parent = preset.getParentDirectory();
  if (!parent.exists()) {
    if (!parent.createDirectory().wasOk() || !parent.hasWriteAccess())
      return false;
  }

  setPresetName(preset.getFileNameWithoutExtension());

  SynthGuiInterface* gui_interface = getGuiInterface();
  if (gui_interface)
    gui_interface->notifyFresh();

  if (writePresetFile(preset)) {
    active_file_ = preset;
    return true;
  }
  return false;
}

bool SynthBase::saveToActiveFile() {
  if (!active_file_.exists() || !active_file_.hasWriteAccess())
    return false;

  return saveToFile(active_file_);
}

void SynthBase::setMpeEnabled(bool enabled) {
  midi_manager_->setMpeEnabled(enabled);
}

void SynthBase::processAudio(AudioSampleBuffer* buffer, int channels, int samples, int offset) {
  if (expired_)
    return;

  engine_->process(samples);
  writeAudio(buffer, channels, samples, offset);
}

void SynthBase::processAudioWithInput(AudioSampleBuffer* buffer, const vital::poly_float* input_buffer,
                                      int channels, int samples, int offset) {
  if (expired_)
    return;

  engine_->processWithInput(input_buffer, samples);
  writeAudio(buffer, channels, samples, offset);
}

void SynthBase::prepareMidiSchedulingBuffers() {
  midi_scheduling_buffers_.input.ensureSize(kMidiBufferReserveBytes);
  midi_scheduling_buffers_.merged_output.ensureSize(kMidiBufferReserveBytes);
  midi_scheduling_buffers_.arp_segment_input.ensureSize(kMidiBufferReserveBytes);
  midi_scheduling_buffers_.arp_output.ensureSize(kMidiBufferReserveBytes);
  for (auto& arp_group : midi_scheduling_buffers_.arp_input) {
    for (MidiBuffer& buffer : arp_group)
      buffer.ensureSize(kMidiBufferReserveBytes);
  }
}

void SynthBase::clearMidiSchedulingBuffers() {
  midi_scheduling_buffers_.input.clear();
  midi_scheduling_buffers_.merged_output.clear();
  midi_scheduling_buffers_.arp_segment_input.clear();
  midi_scheduling_buffers_.arp_output.clear();
  for (auto& arp_group : midi_scheduling_buffers_.arp_input) {
    for (MidiBuffer& buffer : arp_group)
      buffer.clear();
  }
}

int SynthBase::scheduledMidiEventPriority(const ScheduledMidiEvent& event) {
  if (event.size == 0)
    return 6;

  const uint8 status = event.data[0] & 0xf0;
  const bool note_off = status == 0x80 || (status == 0x90 && event.size >= 3 && event.data[2] == 0);
  if (status == 0xb0 && event.size >= 2 && (event.data[1] == 120 || event.data[1] == 123))
    return 0;
  // Host/keyboard events keep their incoming order among themselves (as in
  // Vital). Reordering them would turn a same-sample note-on/note-off pair
  // into off-then-on and leave the note stuck, or move a sustain-pedal change
  // across a note-off. Arpeggiator events still sort around them.
  if (event.engine_channel < 0)
    return 3;
  if (note_off)
    return 1;
  if (status == 0xb0 && event.size >= 2 && (event.data[1] == 64 || event.data[1] == 66))
    return 2;
  if (status == 0x90)
    return 3;
  if (status == 0xa0 || status == 0xd0)
    return 4;
  return 5;
}

void SynthBase::addScheduledMidiEvent(const MidiMessage& message, int sample_offset, int engine_channel) {
  const int message_size = message.getRawDataSize();
  if (message_size <= 0 || message_size > 3 || scheduled_midi_event_count_ >= kMaxScheduledMidiEvents) {
    ++scheduled_midi_overflow_count_;
    return;
  }

  ScheduledMidiEvent& event = scheduled_midi_events_[scheduled_midi_event_count_++];
  event.size = static_cast<uint8>(message_size);
  std::memset(event.data, 0, sizeof(event.data));
  std::memcpy(event.data, message.getRawData(), static_cast<size_t>(message_size));
  event.sample_offset = std::max(0, sample_offset);
  event.engine_channel = engine_channel;
  event.sequence = scheduled_midi_sequence_++;
}

void SynthBase::buildScheduledMidiEvents(MidiBuffer& midi_messages, int start_sample, int end_sample,
                                         double host_bpm, double ppq_position, bool has_ppq_position) {
  scheduled_midi_event_count_ = 0;
  scheduled_midi_sequence_ = 0;
  clearMidiSchedulingBuffers();

  bool arpeggiator_enabled = false;
  for (const auto& controls : arpeggiator_controls_)
    arpeggiator_enabled = arpeggiator_enabled || (controls[0] && controls[0]->value() >= 0.5f);
  const bool arpeggiators_were_enabled = arpeggiators_were_enabled_;
  arpeggiators_were_enabled_ = arpeggiator_enabled;
  if (!arpeggiator_enabled) {
    if (arpeggiators_were_enabled) {
      // Every arp was just switched off: let each run once disabled so it
      // releases any generated note still waiting for its note-off.
      AtlasArpeggiator::Settings disabled_settings[vital::kNumArpeggiators];
      for (int arp = 0; arp < vital::kNumArpeggiators; ++arp)
        disabled_settings[arp] = arpeggiatorSettings(arpeggiator_controls_[arp]);
      MidiBuffer& empty_input = midi_scheduling_buffers_.arp_segment_input;
      MidiBuffer& arp_output = midi_scheduling_buffers_.arp_output;
      empty_input.clear();
      for (int arp = 0; arp < vital::kNumArpeggiators; ++arp) {
        for (int destination = 0; destination < kNumArpDestinations; ++destination) {
          arp_output.clear();
          arpeggiators_[arp][destination].process(empty_input, arp_output, end_sample - start_sample,
                                                  host_bpm, ppq_position, has_ppq_position,
                                                  disabled_settings[arp]);
          for (const MidiMessageMetadata message : arp_output) {
            if (message.getMessage().isNoteOff())
              addScheduledMidiEvent(message.getMessage(), message.samplePosition,
                                    kFirstAtlasArpDestinationVoiceChannel + destination);
          }
        }
      }
    }
    for (const MidiMessageMetadata message : midi_messages) {
      const int midi_sample = message.samplePosition;
      if (midi_sample >= start_sample && midi_sample < end_sample)
        addScheduledMidiEvent(message.getMessage(), midi_sample - start_sample);
    }
  }
  else {
    AtlasArpeggiator::Settings arp_settings[vital::kNumArpeggiators];
    for (int arp = 0; arp < vital::kNumArpeggiators; ++arp)
      arp_settings[arp] = arpeggiatorSettings(arpeggiator_controls_[arp]);
    const double bpm = host_bpm > 0.0 ? host_bpm :
        60.0 * controlValue(controls_, "beats_per_minute", 2.0f);
    for (auto& arp_group : arpeggiators_) {
      for (auto& arp : arp_group)
        arp.setSampleRate(engine_->getSampleRate());
    }

    MidiBuffer& input = midi_scheduling_buffers_.input;
    for (const MidiMessageMetadata message : midi_messages) {
      const int midi_sample = message.samplePosition;
      if (midi_sample < start_sample || midi_sample >= end_sample)
        continue;

      const int relative_sample = midi_sample - start_sample;
      const MidiMessage midi_message = message.getMessage();
      input.addEvent(midi_message, relative_sample);
      if (midi_message.isAllNotesOff() || midi_message.isAllSoundOff()) {
        MidiBuffer& arp_output = midi_scheduling_buffers_.arp_output;
        for (int arp = 0; arp < vital::kNumArpeggiators; ++arp) {
          for (int destination = 0; destination < kNumArpDestinations; ++destination) {
            arp_output.clear();
            arpeggiators_[arp][destination].panic(arp_output, relative_sample);
            for (const MidiMessageMetadata off : arp_output)
              addScheduledMidiEvent(off.getMessage(), off.samplePosition,
                                    kFirstAtlasArpDestinationVoiceChannel + destination);
          }
        }
      }
      for (int arp = 0; arp < vital::kNumArpeggiators; ++arp) {
        for (int destination = 0; destination < kNumArpDestinations; ++destination) {
          if (shouldSendMessageToArpeggiator(controls_, arp, destination, midi_message))
            midi_scheduling_buffers_.arp_input[arp][destination].addEvent(midi_message, relative_sample);
        }
      }
    }

    MidiBuffer& merged_output = midi_scheduling_buffers_.merged_output;
    const bool keep_original_notes = anySourceNeedsOriginalKeyboardPath(controls_);
    for (const MidiMessageMetadata message : input) {
      const MidiMessage midi_message = message.getMessage();
      // Note-offs always pass through so notes that started before an arp
      // was enabled are still released on their original channel.
      if (!midi_message.isNoteOn() || keep_original_notes)
        merged_output.addEvent(midi_message, message.samplePosition);
    }

    const int num_samples = end_sample - start_sample;
    for (int arp = 0; arp < vital::kNumArpeggiators; ++arp) {
      for (int destination = 0; destination < kNumArpDestinations; ++destination) {
        const MidiBuffer& destination_input = midi_scheduling_buffers_.arp_input[arp][destination];
        MidiBuffer& segment_input = midi_scheduling_buffers_.arp_segment_input;
        MidiBuffer& arp_output = midi_scheduling_buffers_.arp_output;
        int cursor = 0;
        while (cursor < num_samples) {
          int next_event = num_samples;
          bool has_event_at_cursor = false;
          for (const MidiMessageMetadata message : destination_input) {
            if (message.samplePosition == cursor)
              has_event_at_cursor = true;
            else if (message.samplePosition > cursor) {
              next_event = message.samplePosition;
              break;
            }
          }

          if (!has_event_at_cursor && next_event > cursor) {
            segment_input.clear();
          }
          else {
            segment_input.clear();
            for (const MidiMessageMetadata message : destination_input) {
              if (message.samplePosition == cursor)
                segment_input.addEvent(message.getMessage(), 0);
              else if (message.samplePosition > cursor)
                break;
            }
          }

          const int segment_samples = next_event - cursor;
          if (segment_samples <= 0)
            break;
          arp_output.clear();
          const double quarters_per_sample = has_ppq_position ? bpm / (60.0 * engine_->getSampleRate()) : 0.0;
          arpeggiators_[arp][destination].process(segment_input, arp_output, segment_samples, bpm,
                                                  ppq_position + cursor * quarters_per_sample,
                                                  has_ppq_position, arp_settings[arp]);
          for (const MidiMessageMetadata message : arp_output) {
            const MidiMessage midi_message = message.getMessage();
            const int event_sample = cursor + message.samplePosition;
            if (midi_message.isNoteOn() && !arpDestinationSounds(destination))
              continue;
            if (midi_message.isNoteOnOrOff()) {
              addScheduledMidiEvent(midi_message, event_sample,
                                    kFirstAtlasArpDestinationVoiceChannel + destination);
            }
            else
              addScheduledMidiEvent(midi_message, event_sample);
          }
          cursor = next_event;
        }
      }
    }

    for (const MidiMessageMetadata message : merged_output)
      addScheduledMidiEvent(message.getMessage(), message.samplePosition);
  }

  std::sort(scheduled_midi_events_.begin(),
            scheduled_midi_events_.begin() + scheduled_midi_event_count_,
            [](const ScheduledMidiEvent& left, const ScheduledMidiEvent& right) {
    if (left.sample_offset != right.sample_offset)
      return left.sample_offset < right.sample_offset;
    const int left_priority = scheduledMidiEventPriority(left);
    const int right_priority = scheduledMidiEventPriority(right);
    if (left_priority != right_priority)
      return left_priority < right_priority;
    return left.sequence < right.sequence;
  });
}

// Each arp plays every destination oscillator on its own voices, so a chord
// takes notes x destinations voices from the shared pool. Notes for an
// oscillator that is off would only take voices from the ones that sound.
bool SynthBase::arpDestinationSounds(int destination) const {
  const vital::Value* on = arp_destination_osc_on_[static_cast<size_t>(destination)];
  return on == nullptr || on->value() >= 0.5f;
}

void SynthBase::dispatchScheduledMidiEvent(const ScheduledMidiEvent& event) {
  const MidiMessage message(event.data, event.size);
  if (event.engine_channel >= 0 && message.isNoteOn()) {
    engine_->noteOn(message.getNoteNumber(), message.getFloatVelocity(), 0, event.engine_channel);
  }
  else if (event.engine_channel >= 0 && message.isNoteOff()) {
    engine_->noteOff(message.getNoteNumber(), message.getFloatVelocity(), 0, event.engine_channel);
  }
  else
    midi_manager_->processMidiMessage(message, 0);
}

void SynthBase::processAudioAndMidi(AudioSampleBuffer* buffer, int channels, MidiBuffer& midi_messages,
                                    int start_sample, int num_samples, double host_bpm,
                                    double ppq_position, bool has_ppq_position, double start_seconds) {
  if (num_samples <= 0)
    return;

  buildScheduledMidiEvents(midi_messages, start_sample, start_sample + num_samples,
                           host_bpm, ppq_position, has_ppq_position);

  const double seconds_per_sample = 1.0 / std::max(1, engine_->getSampleRate());
  // With the transport running, synced LFOs follow the song position. With it
  // stopped there is no grid: the clock restarts on the first note played from
  // silence, as the arpeggiator's does, and later notes join it while any voice
  // still sounds, so what is played together stays in time.
  if (has_ppq_position)
    engine_->setVoiceClockOrigin(0.0);
  int cursor = 0;
  int event_index = 0;
  while (event_index < scheduled_midi_event_count_) {
    const int event_offset = std::min(num_samples, scheduled_midi_events_[event_index].sample_offset);
    if (event_offset > cursor) {
      engine_->correctToTime(start_seconds + cursor * seconds_per_sample);
      processAudio(buffer, channels, event_offset - cursor, start_sample + cursor);
      cursor = event_offset;
    }

    while (event_index < scheduled_midi_event_count_ &&
           scheduled_midi_events_[event_index].sample_offset == event_offset) {
      const auto& event = scheduled_midi_events_[event_index];
      if (!has_ppq_position && event.size >= 3 && (event.data[0] & 0xf0) == 0x90 && event.data[2] > 0 &&
          engine_->getNumActiveVoices() == 0) {
        engine_->setVoiceClockOrigin(start_seconds + event_offset * seconds_per_sample);
        engine_->correctToTime(start_seconds + event_offset * seconds_per_sample);
      }
      dispatchScheduledMidiEvent(event);
      ++event_index;
    }
  }

  if (cursor < num_samples) {
    engine_->correctToTime(start_seconds + cursor * seconds_per_sample);
    processAudio(buffer, channels, num_samples - cursor, start_sample + cursor);
  }
}

void SynthBase::writeAudio(AudioSampleBuffer* buffer, int channels, int samples, int offset) {
  const vital::mono_float* engine_output = (const vital::mono_float*)engine_->output(0)->buffer;
  for (int channel = 0; channel < channels; ++channel) {
    float* channel_data = buffer->getWritePointer(channel, offset);

    for (int i = 0; i < samples; ++i) {
      channel_data[i] = engine_output[vital::poly_float::kSize * i + channel];
      VITAL_ASSERT(std::isfinite(channel_data[i]));
    }
  }

  if (audio_history_enabled_.load(std::memory_order_relaxed))
    updateMemoryOutput(samples, engine_->output(0)->buffer);
}

void SynthBase::processMidi(MidiBuffer& midi_messages, int start_sample, int end_sample,
                            double host_bpm, double ppq_position, bool has_ppq_position) {
  bool process_all = end_sample == 0;
  if (!process_all && start_sample < end_sample) {
    AtlasArpeggiator::Settings arp_settings[vital::kNumArpeggiators];
    bool any_arp_enabled = false;
    for (int arp = 0; arp < vital::kNumArpeggiators; ++arp) {
      arp_settings[arp] = arpeggiatorSettings(arpeggiator_controls_[arp]);
      any_arp_enabled = any_arp_enabled || arp_settings[arp].enabled;
    }
    if (any_arp_enabled) {
      const double bpm = host_bpm > 0.0 ? host_bpm : 60.0 * controlValue(controls_, "beats_per_minute", 2.0f);
      for (auto& arp_group : arpeggiators_) {
        for (auto& arp : arp_group)
          arp.setSampleRate(engine_->getSampleRate());
      }

      MidiBuffer input;
      MidiBuffer arp_input[vital::kNumArpeggiators][kNumArpDestinations];
      for (const MidiMessageMetadata message : midi_messages) {
        int midi_sample = message.samplePosition;
        if (midi_sample >= start_sample && midi_sample < end_sample) {
          const int relative_sample = midi_sample - start_sample;
          const MidiMessage midi_message = message.getMessage();
          input.addEvent(midi_message, relative_sample);
          for (int arp = 0; arp < vital::kNumArpeggiators; ++arp) {
            for (int destination = 0; destination < kNumArpDestinations; ++destination) {
              if (shouldSendMessageToArpeggiator(controls_, arp, destination, midi_message))
                arp_input[arp][destination].addEvent(midi_message, relative_sample);
            }
          }
        }
      }

      MidiBuffer merged_output;
      const int num_samples = end_sample - start_sample;
      const bool keep_original_notes = anySourceNeedsOriginalKeyboardPath(controls_);
      for (const MidiMessageMetadata message : input) {
        const MidiMessage midi_message = message.getMessage();
        // Note-offs always pass through so notes that started before an arp
        // was enabled are still released on their original channel.
        if (!midi_message.isNoteOn() || keep_original_notes)
          merged_output.addEvent(midi_message, message.samplePosition);
      }

      for (int arp = 0; arp < vital::kNumArpeggiators; ++arp) {
        for (int destination = 0; destination < kNumArpDestinations; ++destination) {
          MidiBuffer arp_output;
          arpeggiators_[arp][destination].process(arp_input[arp][destination], arp_output, num_samples,
                                                  bpm, ppq_position, has_ppq_position, arp_settings[arp]);
          for (const MidiMessageMetadata message : arp_output) {
            const MidiMessage midi_message = message.getMessage();
            const int voice_channel = kFirstAtlasArpDestinationVoiceChannel + destination;
            if (midi_message.isNoteOn() && arpDestinationSounds(destination))
              engine_->noteOn(midi_message.getNoteNumber(), midi_message.getFloatVelocity(),
                              message.samplePosition, voice_channel);
            else if (midi_message.isNoteOff())
              engine_->noteOff(midi_message.getNoteNumber(), midi_message.getFloatVelocity(),
                               message.samplePosition, voice_channel);
          }
        }
      }
      for (const MidiMessageMetadata message : merged_output)
        midi_manager_->processMidiMessage(message.getMessage(), message.samplePosition);
      return;
    }
  }

  for (const MidiMessageMetadata message : midi_messages) {
    int midi_sample = message.samplePosition;
    if (process_all || (midi_sample >= start_sample && midi_sample < end_sample))
      midi_manager_->processMidiMessage(message.getMessage(), midi_sample - start_sample);
  }
}

void SynthBase::processKeyboardEvents(MidiBuffer& buffer, int num_samples) {
  midi_manager_->replaceKeyboardMessages(buffer, num_samples);
}

void SynthBase::processModulationChanges() {
  vital::modulation_change change;
  while (getNextModulationChange(change)) {
    if (change.disconnecting)
      engine_->disconnectModulation(change);
    else
      engine_->connectModulation(change);
  }
}

void SynthBase::updateMemoryOutput(int samples, const vital::poly_float* audio) {
  for (int i = 0; i < samples; ++i)
    audio_memory_->push(audio[i]);

  vital::mono_float last_played = engine_->getLastActiveNote();
  last_played = vital::utils::clamp(last_played, kOutputWindowMinNote, kOutputWindowMaxNote);

  int num_pressed = engine_->getNumPressedNotes();
  int output_inc = std::max<int>(1, engine_->getSampleRate() / vital::kOscilloscopeMemorySampleRate);
  int oscilloscope_samples = 2 * vital::kOscilloscopeMemoryResolution;

  if (last_played && (last_played_note_ != last_played || num_pressed > last_num_pressed_)) {
    last_played_note_ = last_played;
    
    vital::mono_float frequency = vital::utils::midiNoteToFrequency(last_played_note_);
    vital::mono_float period = engine_->getSampleRate() / frequency;
    int window_length = output_inc * vital::kOscilloscopeMemoryResolution;

    memory_reset_period_ = period;
    while (memory_reset_period_ < window_length)
      memory_reset_period_ += memory_reset_period_;

    memory_reset_period_ = std::min(memory_reset_period_, 2.0f * window_length);
    memory_index_ = 0;
    vital::utils::copyBuffer(oscilloscope_memory_, oscilloscope_memory_write_, oscilloscope_samples);
  }
  last_num_pressed_ = num_pressed;

  for (; memory_input_offset_ < samples; memory_input_offset_ += output_inc) {
    int input_index = vital::utils::iclamp(memory_input_offset_, 0, samples);
    memory_index_ = vital::utils::iclamp(memory_index_, 0, oscilloscope_samples - 1);
    VITAL_ASSERT(input_index >= 0);
    VITAL_ASSERT(input_index < samples);
    VITAL_ASSERT(memory_index_ >= 0);
    VITAL_ASSERT(memory_index_ < oscilloscope_samples);
    oscilloscope_memory_write_[memory_index_++] = audio[input_index];

    if (memory_index_ * output_inc >= memory_reset_period_) {
      memory_input_offset_ += memory_reset_period_ - memory_index_ * output_inc;
      memory_index_ = 0;
      vital::utils::copyBuffer(oscilloscope_memory_, oscilloscope_memory_write_, oscilloscope_samples);
    }
  }

  memory_input_offset_ -= samples;
}

void SynthBase::armMidiLearn(const std::string& name) {
  midi_manager_->armMidiLearn(name);
}

void SynthBase::cancelMidiLearn() {
  midi_manager_->cancelMidiLearn();
}

void SynthBase::clearMidiLearn(const std::string& name) {
  midi_manager_->clearMidiLearn(name);
}

std::vector<std::pair<int, std::string>> SynthBase::midiLearnMappings() {
  std::vector<std::pair<int, std::string>> mappings;
  for (const auto& controller : midi_manager_->getMidiLearnMap())
    for (const auto& destination : controller.second)
      mappings.push_back({ controller.first, destination.first });
  return mappings;
}

std::string SynthBase::midiLearnArmedName() const {
  return midi_manager_->armedName();
}

void SynthBase::clearAllMidiLearn() {
  midi_manager_->clearAllMidiLearn();
}

bool SynthBase::isMidiMapped(const std::string& name) {
  return midi_manager_->isMidiMapped(name);
}

void SynthBase::setAuthor(const String& author) {
  save_info_["author"] = author;
}

void SynthBase::setComments(const String& comments) {
  save_info_["comments"] = comments;
}

void SynthBase::setStyle(const String& style) {
  save_info_["style"] = style;
}

void SynthBase::setTags(const String& tags) {
  save_info_["tags"] = tags;
}

void SynthBase::setPresetName(const String& preset_name) {
  save_info_["preset_name"] = preset_name;
}

void SynthBase::setMacroName(int index, const String& macro_name) {
  save_info_["macro" + std::to_string(index + 1)] = macro_name;
}

String SynthBase::getAuthor() {
  return save_info_["author"];
}

String SynthBase::getComments() {
  return save_info_["comments"];
}

String SynthBase::getStyle() {
  return save_info_["style"];
}

String SynthBase::getTags() {
  return save_info_["tags"];
}

String SynthBase::getPresetName() {
  return save_info_["preset_name"];
}

String SynthBase::getMacroName(int index) {
  String name = save_info_["macro" + std::to_string(index + 1)];
  if (name.trim().isEmpty())
    return "MACRO " + String(index + 1);
  return name;
}

const vital::StereoMemory* SynthBase::getEqualizerMemory() {
  if (engine_)
    return engine_->getEqualizerMemory();
  return nullptr;
}

vital::ModulationConnectionBank& SynthBase::getModulationBank() {
  return engine_->getModulationBank();
}

void SynthBase::notifyOversamplingChanged() {
  pauseProcessing(true);
  engine_->allSoundsOff();
  checkOversampling();
  pauseProcessing(false);
}

void SynthBase::checkOversampling() {
  return engine_->checkOversampling();
}

void SynthBase::clearArpeggiators() {
  for (auto& arp_group : arpeggiators_) {
    for (auto& arp : arp_group)
      arp.clear();
  }
}

void SynthBase::queueGuiControlUpdate(const std::string& name, vital::mono_float value) {
  const auto update = pending_gui_updates_.find(name);
  if (update == pending_gui_updates_.end())
    return;

  update->second.value.store(value, std::memory_order_relaxed);
  update->second.dirty.store(true, std::memory_order_release);
  gui_updates_pending_.store(true, std::memory_order_release);
}

void SynthBase::dispatchPendingGuiUpdates(SynthGuiInterface& gui_interface) {
  if (!gui_updates_pending_.exchange(false, std::memory_order_acquire))
    return;

  bool changed = false;
  for (auto& update : pending_gui_updates_) {
    if (update.second.dirty.exchange(false, std::memory_order_acquire)) {
      gui_interface.updateGuiControl(update.first, update.second.value.load(std::memory_order_relaxed));
      changed = changed || update.first != "pitch_wheel";
    }
  }
  if (const std::string* learned = midi_learned_name_.exchange(nullptr, std::memory_order_acquire))
    gui_interface.notifyMidiLearned(*learned, midi_learned_id_.load(std::memory_order_relaxed));
  if (changed)
    gui_interface.notifyChange();
}

void SynthBase::clearPendingGuiUpdates() {
  gui_updates_pending_.store(false, std::memory_order_relaxed);
  for (auto& update : pending_gui_updates_)
    update.second.dirty.store(false, std::memory_order_relaxed);
  midi_learned_name_.store(nullptr, std::memory_order_relaxed);
}
