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

#include "load_save.h"
#include "multisample_container.h"
#include "multisample_manager.h"
#include <mutex>
#include "source_types.h"
#include "convolution_reverb_module.h"
#include "modulation_connection_processor.h"
#include "sound_engine.h"
#include "midi_manager.h"
#include "sample_source.h"
#include "synth_base.h"
#include "synth_constants.h"
#include "synth_parameters.h"
#include "synth_oscillator.h"
#include "synth_strings.h"

#include <algorithm>
#include <cmath>

#define QUOTE(x) #x
#define STRINGIFY(x) QUOTE(x)

namespace {
  const std::string kLinuxUserDataDirectory = "~/.local/share/Atlas/";
  const std::string kAvailablePacksFile = "available_packs.json";
  const std::string kInstalledPacksFile = "packs.json";

  Time getBuildTime() {
    StringArray date_tokens;
    date_tokens.addTokens(STRINGIFY(BUILD_DATE), true);
    if (date_tokens.size() != 5)
      return Time::getCompilationDate();

    int year = date_tokens[0].getIntValue();
    int month = date_tokens[1].getIntValue() - 1;
    int day = date_tokens[2].getIntValue();
    int hour = date_tokens[3].getIntValue();
    int minute = date_tokens[4].getIntValue();

    return Time(year, month, day, hour, minute);
  }

  bool jsonHasObject(const json& data, const char* field) {
    return data.count(field) && data[field].is_object();
  }

  bool jsonHasArray(const json& data, const char* field) {
    return data.count(field) && data[field].is_array();
  }

  bool getJsonString(const json& data, const char* field, std::string& value) {
    if (!data.count(field) || !data[field].is_string())
      return false;

    value = data[field].get<std::string>();
    return true;
  }

  bool getJsonFloat(const json& data, const std::string& field, float& value) {
    if (!data.count(field))
      return false;

    const json& json_value = data[field];
    if (json_value.is_number())
      value = json_value.get<float>();
    else if (json_value.is_boolean())
      value = json_value.get<bool>() ? 1.0f : 0.0f;
    else
      return false;

    return std::isfinite(value);
  }

  float clampControlValue(const vital::ValueDetails& details, float value) {
    float clamped = std::min(std::max(value, details.min), details.max);
    if (details.value_scale == vital::ValueDetails::kIndexed)
      clamped = std::round(clamped);

    return clamped;
  }

  json arrayFieldOrEmpty(const json& data, const char* field) {
    if (jsonHasArray(data, field))
      return data[field];

    return json::array();
  }

  json objectFieldOrEmpty(const json& data, const char* field) {
    if (jsonHasObject(data, field))
      return data[field];

    return json::object();
  }

  String safeVersionString(const json& data) {
    if (data.count("synth_version") && data["synth_version"].is_string())
      return String(data["synth_version"].get<std::string>());

    return "0.0.0";
  }

  void shiftOldDirectEffectDestination(json& settings, const std::string& field) {
    static constexpr int kOldDirectEffectDestinationStart = 7;
    float value = 0.0f;
    if (getJsonFloat(settings, field, value) && value >= kOldDirectEffectDestinationStart)
      settings[field] = value + 1.0f;
  }

  void migrateVitalSourceDestinationLayout(json& settings) {
    // Native Atlas presets contain the expanded producers/bus controls. Vital
    // presets do not, and use 4 for Direct Out with direct-effect routes
    // starting at 5. Atlas inserted Bus 1, Bus 2 and Bus 3 into that enum.
    if (settings.count("bus_1_destination") || settings.count("osc_4_destination"))
      return;

    auto migrate = [&settings](const std::string& field) {
      float value = 0.0f;
      if (!getJsonFloat(settings, field, value))
        return;

      static constexpr int kVitalDirectOut = 4;
      static constexpr int kVitalFirstEffect = 5;
      static constexpr int kAtlasInsertedDestinations = 3;
      if (value == kVitalDirectOut)
        settings[field] = static_cast<float>(vital::constants::kDirectOut);
      else if (value >= kVitalFirstEffect)
        settings[field] = value + kAtlasInsertedDestinations;
    };

    static constexpr int kVitalNumOscillators = 3;
    for (int oscillator = 1; oscillator <= kVitalNumOscillators; ++oscillator)
      migrate("osc_" + std::to_string(oscillator) + "_destination");
    migrate("sample_destination");
    migrate("filter_1_destination");
    migrate("filter_2_destination");
  }

  bool hasDynamicEffectSlots(const json& settings, const std::string& chain_prefix) {
    for (int slot = 0; slot < vital::constants::kMaxEffectSlots; ++slot) {
      float type = 0.0f;
      if (getJsonFloat(settings, chain_prefix + "fx_slot_" + std::to_string(slot + 1) + "_type", type) &&
          type > 0.5f)
        return true;
    }

    return false;
  }

  bool isLegacyEffectEnabled(const json& settings, const std::string& chain_prefix, int effect) {
    float enabled = 0.0f;
    return getJsonFloat(settings, chain_prefix + strings::kEffectOrder[effect] + "_on", enabled) &&
           enabled >= 0.5f;
  }

  void readLegacyEffectOrder(const json& settings, const std::string& chain_prefix, int* order) {
    bool seen[vital::constants::kNumEffects] = {};
    int position = 0;

    for (int slot = 0; slot < vital::constants::kNumEffects; ++slot) {
      float value = 0.0f;
      const std::string slot_id = chain_prefix + "effect_chain_slot_" + std::to_string(slot + 1);
      if (!getJsonFloat(settings, slot_id, value))
        continue;

      const int effect = static_cast<int>(std::round(value));
      if (!isPositiveAndBelow(effect, vital::constants::kNumEffects) || seen[effect])
        continue;

      order[position++] = effect;
      seen[effect] = true;
    }

    if (position == 0) {
      for (int i = 0; i < vital::constants::kNumEffects; ++i)
        order[i] = i;

      float encoded_order = 0.0f;
      if (getJsonFloat(settings, chain_prefix + "effect_chain_order", encoded_order)) {
        int reorderable[vital::constants::kNumReorderableEffects];
        const float max_order = vital::utils::factorial(vital::constants::kNumReorderableEffects) - 1;
        vital::utils::decodeFloatToOrder(reorderable, std::min(std::max(encoded_order, 0.0f), max_order),
                                         vital::constants::kNumReorderableEffects);
        for (int i = 0; i < vital::constants::kNumReorderableEffects; ++i)
          order[i] = reorderable[i];
      }

      position = vital::constants::kNumEffects;
      for (int i = 0; i < vital::constants::kNumEffects; ++i)
        seen[i] = true;
    }

    for (int effect = 0; position < vital::constants::kNumEffects && effect < vital::constants::kNumEffects; ++effect) {
      if (seen[effect])
        continue;

      order[position++] = effect;
      seen[effect] = true;
    }

    float post_order = 0.0f;
    if (getJsonFloat(settings, chain_prefix + "post_effect_order", post_order) && post_order >= 0.5f) {
      int frequency_slot = -1;
      int limiter_slot = -1;
      for (int i = 0; i < vital::constants::kNumEffects; ++i) {
        if (order[i] == vital::constants::kFrequencyShifter)
          frequency_slot = i;
        else if (order[i] == vital::constants::kLimiter)
          limiter_slot = i;
      }
      if (frequency_slot >= 0 && limiter_slot >= 0)
        std::swap(order[frequency_slot], order[limiter_slot]);
    }
  }

  void migrateLegacyEffectChain(json& settings, json& modulations, const std::string& chain_prefix) {
    if (hasDynamicEffectSlots(settings, chain_prefix))
      return;

    int legacy_order[vital::constants::kNumEffects];
    readLegacyEffectOrder(settings, chain_prefix, legacy_order);

    std::map<std::string, std::string> modulation_destination_map;
    int dynamic_slot = 0;
    for (int position = 0; position < vital::constants::kNumEffects; ++position) {
      const int effect = legacy_order[position];
      if (!isPositiveAndBelow(effect, vital::constants::kNumEffects) ||
          !isLegacyEffectEnabled(settings, chain_prefix, effect))
        continue;
      if (dynamic_slot >= vital::constants::kMaxEffectSlots)
        break;

      const std::string effect_id = strings::kEffectOrder[effect];
      const std::string old_prefix = chain_prefix + effect_id + "_";
      const std::string slot_prefix = chain_prefix + "fx_slot_" + std::to_string(dynamic_slot + 1) + "_";
      const std::string new_prefix = slot_prefix + effect_id + "_";
      settings[slot_prefix + "type"] = static_cast<float>(effect + 1);
      settings[slot_prefix + "enabled"] = 1.0f;

      for (auto it = settings.begin(); it != settings.end(); ++it) {
        if (!it.key().rfind(old_prefix, 0)) {
          const std::string suffix = it.key().substr(old_prefix.length());
          const std::string destination = new_prefix + suffix;
          settings[destination] = it.value();
          modulation_destination_map[it.key()] = destination;
        }
      }

      ++dynamic_slot;
    }

    if (!modulations.is_array() || modulation_destination_map.empty())
      return;

    for (json& modulation : modulations) {
      if (!modulation.is_object() || !modulation.count("destination") || !modulation["destination"].is_string())
        continue;

      const std::string destination = modulation["destination"].get<std::string>();
      auto mapped = modulation_destination_map.find(destination);
      if (mapped != modulation_destination_map.end())
        modulation["destination"] = mapped->second;
    }
  }

  void migrateLegacyEffectChains(json& settings, json& modulations) {
    migrateLegacyEffectChain(settings, modulations, "");
    for (int bus = 0; bus < vital::kNumBuses; ++bus)
      migrateLegacyEffectChain(settings, modulations, "bus_" + std::to_string(bus + 1) + "_");
  }

} // namespace

const std::string LoadSave::kUserDirectoryName = "User";
const std::string LoadSave::kPresetFolderName = "Presets";
const std::string LoadSave::kWavetableFolderName = "Wavetables";
const std::string LoadSave::kSkinFolderName = "Skins";
const std::string LoadSave::kSampleFolderName = "Samples";
const std::string LoadSave::kLfoFolderName = "LFOs";
const std::string LoadSave::kFxFolderName = "FX";
const std::string LoadSave::kIrFolderName = "Impulse Responses";

const std::string LoadSave::kAdditionalWavetableFoldersName = "wavetable_folders";
const std::string LoadSave::kAdditionalSampleFoldersName = "sample_folders";

void LoadSave::convertBufferToPcm(json& data, const std::string& field) {
  if (data.count(field) == 0)
    return;

  MemoryOutputStream decoded;
  std::string wave_data = data[field];
  Base64::convertFromBase64(decoded, wave_data);
  int size = static_cast<int>(decoded.getDataSize()) / sizeof(float);
  std::unique_ptr<float[]> float_data = std::make_unique<float[]>(size);
  memcpy(float_data.get(), decoded.getData(), size * sizeof(float));
  std::unique_ptr<int16_t[]> pcm_data = std::make_unique<int16_t[]>(size);
  vital::utils::floatToPcmData(pcm_data.get(), float_data.get(), size);

  String encoded = Base64::toBase64(pcm_data.get(), sizeof(int16_t) * size);
  data[field] = encoded.toStdString();
}

void LoadSave::convertPcmToFloatBuffer(json& data, const std::string& field) {
  if (data.count(field) == 0)
    return;

  MemoryOutputStream decoded;
  std::string wave_data = data[field];
  Base64::convertFromBase64(decoded, wave_data);
  int size = static_cast<int>(decoded.getDataSize()) / sizeof(int16_t);
  std::unique_ptr<int16_t[]> pcm_data = std::make_unique<int16_t[]>(size);
  memcpy(pcm_data.get(), decoded.getData(), size * sizeof(int16_t));
  std::unique_ptr<float[]> float_data = std::make_unique<float[]>(size);
  vital::utils::pcmToFloatData(float_data.get(), pcm_data.get(), size);

  String encoded = Base64::toBase64(float_data.get(), sizeof(float) * size);
  data[field] = encoded.toStdString();
}

json LoadSave::stateToJson(SynthBase* synth, const CriticalSection& critical_section) {
  json settings_data;
  vital::control_map& controls = synth->getControls();
  for (auto& control : controls)
    settings_data[control.first] = control.second->value();

  // A slot's sample is saved while a sample-based type uses it; the built-in
  // default is not written (loading restores it).
  for (int slot = 0; slot < vital::kNumOscillators; ++slot) {
    const std::string prefix = vital::source_types::slotPrefix(slot);
    const auto type = controls.find(prefix + "type");
    const int type_value = type == controls.end() ? 0 : static_cast<int>(type->second->value() + 0.5f);
    vital::Sample* sample = synth->getSample(slot);
    if (sample && vital::source_types::describe(type_value).uses_sample && !sample->isDefault())
      settings_data[prefix + "sample"] = sample->stateToJson();

    // A Multi-sample slot names its instrument (path and content hash);
    // container presets add the file list when they are written.
    vital::multisample::Manager* multisample = synth->getMultiSampleManager();
    if (multisample != nullptr && type_value == vital::source_types::kMultiSample) {
      const vital::multisample::InstrumentSpec spec = multisample->spec(slot);
      if (!spec.empty())
        settings_data[prefix + "multisample"] = spec.toJson(false);
    }
  }

  json convolution_irs = json::object();
  for (auto& prefix_module : synth->getConvolutionReverbModules()) {
    if (prefix_module.second && prefix_module.second->hasCustomIr()) {
      json ir_state = prefix_module.second->irStateToJson();
      if (!ir_state.is_null())
        convolution_irs[prefix_module.first] = std::move(ir_state);
    }
  }
  if (!convolution_irs.empty())
    settings_data["convolution_irs"] = convolution_irs;

  json modulations;
  vital::ModulationConnectionBank& modulation_bank = synth->getModulationBank();
  for (int i = 0; i < vital::kMaxModulationConnections; ++i) {
    vital::ModulationConnection* connection = modulation_bank.atIndex(i);
    json modulation_data;
    modulation_data["source"] = connection->source_name;
    modulation_data["destination"] = connection->destination_name;

    LineGenerator* line_mapping = connection->modulation_processor->lineMapGenerator();
    if (!line_mapping->linear())
      modulation_data["line_mapping"] = line_mapping->stateToJson();

    modulations.push_back(modulation_data);
  }

  settings_data["modulations"] = modulations;

  if (synth->getWavetableCreator(0)) {
    json wavetables;
    for (int i = 0; i < vital::kNumOscillators; ++i) {
      WavetableCreator* wavetable_creator = synth->getWavetableCreator(i);
      wavetables.push_back(wavetable_creator->stateToJson());
    }

    settings_data["wavetables"] = wavetables;
  }

  // Filter Table wavetables, keyed by filter prefix.
  {
    json filter_tables = json::object();
    for (const std::string& prefix : synth->filterTablePrefixes()) {
      if (WavetableCreator* creator = synth->getFilterTableCreator(prefix, false))
        filter_tables[prefix] = creator->stateToJson();
    }
    if (!filter_tables.empty())
      settings_data["filter_tables"] = filter_tables;
  }

  json lfos;
  for (int i = 0; i < vital::kNumLfos; ++i) {
    LineGenerator* lfo_source = synth->getLfoSource(i);
    lfos.push_back(lfo_source->stateToJson());
  }

  settings_data["lfos"] = lfos;

  json data;
  data["synth_version"] = ProjectInfo::versionString;
  data["engine_behavior_version"] = kEngineBehaviorVersion;
  data["preset_name"] = synth->getPresetName().toStdString();
  data["author"] = synth->getAuthor().toStdString();
  data["comments"] = synth->getComments().toStdString();
  data["preset_style"] = synth->getStyle().toStdString();
  data["preset_tags"] = synth->getTags().toStdString();
  for (int i = 0; i < vital::kNumMacros; ++i) {
    std::string name = synth->getMacroName(i).toStdString();
    data["macro" + std::to_string(i + 1)] = name;
  }
  data["settings"] = settings_data;
  return data;
}

void LoadSave::loadControls(SynthBase* synth, const json& data) {
  vital::control_map controls = synth->getControls();
  for (auto& control : controls) {
    std::string name = control.first;
    float value = 0.0f;
    vital::ValueDetails details = vital::Parameters::getDetails(name);
    if (getJsonFloat(data, name, value)) {
      control.second->set(clampControlValue(details, value));
    }
    else {
      control.second->set(details.default_value);
    }
  }

  synth->modWheelGuiChanged(controls["mod_wheel"]->value());
}

void LoadSave::loadModulations(SynthBase* synth, const json& modulations) {
  if (!modulations.is_array())
    return;

  synth->clearModulations();
  vital::ModulationConnectionBank& modulation_bank = synth->getModulationBank();
  int index = 0;
  for (const json& modulation : modulations) {
    if (index >= vital::kMaxModulationConnections)
      break;

    if (!modulation.is_object())
      continue;

    std::string source;
    std::string destination;
    if (!getJsonString(modulation, "source", source) || !getJsonString(modulation, "destination", destination))
      continue;

    vital::ModulationConnection* connection = modulation_bank.atIndex(index);
    index++;

    if (!destination.empty())
      synth->prepareModulationDestination(destination);
    if (synth->getEngine()->getModulationSource(source) == nullptr ||
        synth->getEngine()->getMonoModulationDestination(destination) == nullptr) {
      continue;
    }
    
    if (source.length() && destination.length()) {
      connection->source_name = source;
      connection->destination_name = destination;
      synth->connectModulation(connection);
    }

    if (modulation.count("line_mapping") && modulation["line_mapping"].is_object() &&
        LineGenerator::isValidJson(modulation["line_mapping"])) {
      try {
        connection->modulation_processor->lineMapGenerator()->jsonToState(modulation["line_mapping"]);
      }
      catch (const json::exception&) {
        connection->modulation_processor->lineMapGenerator()->initLinear();
      }
    }
    else {
      connection->modulation_processor->lineMapGenerator()->initLinear();
    }
  }
}

void LoadSave::loadSlotSamples(SynthBase* synth, const json& settings) {
  for (int slot = 0; slot < vital::kNumOscillators; ++slot) {
    vital::Sample* sample = synth->getSample(slot);
    if (sample == nullptr)
      continue;
    const std::string key = vital::source_types::slotPrefix(slot) + "sample";
    const json json_sample = objectFieldOrEmpty(settings, key.c_str());
    if (!json_sample.is_object() || json_sample.empty()) {
      // Rebuilding the default noise sample costs a few milliseconds per slot
      // and changes nothing when the slot already holds it.
      if (!sample->isDefault())
        sample->init();
      continue;
    }
    try {
      sample->jsonToState(json_sample);
    }
    catch (const json::exception&) { }
  }
}

void LoadSave::loadSlotInstruments(SynthBase* synth, const json& settings) {
  vital::multisample::Manager* multisample = synth->getMultiSampleManager();
  if (multisample == nullptr)
    return;
  for (int slot = 0; slot < vital::kNumOscillators; ++slot) {
    const std::string prefix = vital::source_types::slotPrefix(slot);
    const json data = objectFieldOrEmpty(settings, (prefix + "multisample").c_str());
    vital::multisample::InstrumentSpec spec;
    const auto type = settings.find(prefix + "type");
    const bool multisample_type = type != settings.end() && type->is_number() &&
                                  static_cast<int>(type->get<float>() + 0.5f) == vital::source_types::kMultiSample;
    if (multisample_type && data.is_object() && !data.empty())
      spec = vital::multisample::InstrumentSpec::fromJson(data);
    multisample->applySpec(slot, spec, synth->multiSampleContainer());
  }
}

void LoadSave::migrateFixedSourcesToSlots(json& settings, json& modulations) {
  struct LegacySource {
    const char* prefix;
    const char* sample_key;
    const char* phase_source;
    int slot;
  };
  static const LegacySource kLegacySources[] = {
    { "sample", "sample", "sample_phase", 4 },
    { "granular", "granular_sample", "granular_phase", 5 },
    { "spectral", "spectral_sample", nullptr, 6 },
    { "noise", nullptr, nullptr, 7 },
  };
  static const char* kSlotLevel[] = {
    "on", "solo", "destination", "key_zone_start", "key_zone_end", "velocity_zone_start", "velocity_zone_end",
    "level", "pan"
  };

  // Maps a legacy fixed-source parameter name to its slot parameter, or returns empty.
  auto slotName = [&](const std::string& name) -> std::string {
    for (const LegacySource& source : kLegacySources) {
      const std::string legacy_prefix = std::string(source.prefix) + "_";
      if (name.rfind(legacy_prefix, 0) != 0)
        continue;
      const std::string suffix = name.substr(legacy_prefix.size());
      const std::string slot_prefix = vital::source_types::slotPrefix(source.slot);
      for (const char* slot_level : kSlotLevel) {
        if (suffix == slot_level)
          return slot_prefix + suffix;
      }
      const std::string target = slot_prefix + legacy_prefix + suffix;
      // Effects such as spectral_gate_* also start with "spectral_"; only real slot parameters move.
      if (vital::Parameters::isParameter(target))
        return target;
      return {};
    }
    return {};
  };

  if (!settings.is_object())
    return;

  std::vector<std::string> keys;
  for (auto it = settings.begin(); it != settings.end(); ++it)
    keys.push_back(it.key());
  for (const std::string& key : keys) {
    const std::string target = slotName(key);
    if (target.empty())
      continue;
    json value = settings[key];
    settings.erase(key);
    settings[target] = std::move(value);
    for (const LegacySource& source : kLegacySources) {
      if (key.rfind(std::string(source.prefix) + "_", 0) == 0) {
        const std::string type_key = vital::source_types::slotPrefix(source.slot) + "type";
        settings[type_key] = static_cast<float>(
            vital::source_types::kLegacySourceTypes[source.slot - vital::source_types::kFirstLegacySourceSlot]);
      }
    }
  }

  for (const LegacySource& source : kLegacySources) {
    if (source.sample_key != nullptr && settings.count(source.sample_key)) {
      json sample = settings[source.sample_key];
      settings.erase(source.sample_key);
      settings[vital::source_types::slotPrefix(source.slot) + "sample"] = std::move(sample);
    }
    for (int arp = 1; arp <= 2; ++arp) {
      const std::string legacy = "arp_" + std::to_string(arp) + "_destination_" + source.prefix;
      if (settings.count(legacy)) {
        json value = settings[legacy];
        settings.erase(legacy);
        settings["arp_" + std::to_string(arp) + "_destination_osc_" + std::to_string(source.slot + 1)] =
            std::move(value);
      }
    }
  }

  if (modulations.is_array()) {
    for (json& modulation : modulations) {
      if (!modulation.is_object())
        continue;
      if (modulation.count("destination") && modulation["destination"].is_string()) {
        const std::string target = slotName(modulation["destination"].get<std::string>());
        if (!target.empty())
          modulation["destination"] = target;
      }
      if (modulation.count("source") && modulation["source"].is_string()) {
        const std::string source_name = modulation["source"].get<std::string>();
        for (const LegacySource& source : kLegacySources) {
          if (source.phase_source != nullptr && source_name == source.phase_source)
            modulation["source"] = vital::source_types::slotPrefix(source.slot) + "playhead";
        }
      }
    }
  }
}

// Early slot builds saved level and pan per type (osc_N_sample_level ...). They
// are slot controls now: the active type's values move to osc_N_level/pan and
// the rest are dropped.
void LoadSave::migrateSlotTypeLevels(json& settings, json& modulations) {
  if (!settings.is_object())
    return;
  static const char* kGroups[] = { "sample", "granular", "spectral", "noise" };
  static const int kGroupTypes[] = { vital::source_types::kSample, vital::source_types::kGranular,
                                     vital::source_types::kSpectral, vital::source_types::kNoise };
  static const char* kSuffixes[] = { "level", "pan" };
  for (int slot = 0; slot < vital::kNumOscillators; ++slot) {
    const std::string prefix = vital::source_types::slotPrefix(slot);
    const std::string type_key = prefix + "type";
    int type = static_cast<int>(vital::Parameters::getDetails(type_key).default_value);
    if (settings.count(type_key) && settings[type_key].is_number())
      type = static_cast<int>(settings[type_key].get<float>() + 0.5f);
    for (int group = 0; group < 4; ++group) {
      for (const char* suffix : kSuffixes) {
        const std::string key = prefix + kGroups[group] + "_" + suffix;
        const std::string target = prefix + suffix;
        if (settings.count(key)) {
          if (type == kGroupTypes[group])
            settings[target] = settings[key];
          settings.erase(key);
        }
        if (!modulations.is_array())
          continue;
        for (auto it = modulations.begin(); it != modulations.end();) {
          const bool matches = it->is_object() && it->count("destination") && (*it)["destination"].is_string() &&
                               (*it)["destination"].get<std::string>() == key;
          if (!matches) {
            ++it;
            continue;
          }
          if (type == kGroupTypes[group]) {
            (*it)["destination"] = target;
            ++it;
          }
          else {
            it = modulations.erase(it);
          }
        }
      }
    }
  }
}

void LoadSave::loadConvolutionIrs(SynthBase* synth, const json& convolution_irs) {
  for (auto& prefix_module : synth->getConvolutionReverbModules()) {
    vital::ConvolutionReverbModule* module = prefix_module.second;
    if (module == nullptr)
      continue;

    bool loaded = false;
    if (convolution_irs.is_object() && convolution_irs.count(prefix_module.first)) {
      try {
        loaded = module->irStateFromJson(convolution_irs[prefix_module.first]);
      }
      catch (const json::exception&) { }
    }
    if (!loaded && module->hasCustomIr())
      module->setDefaultIr();
  }
}

void LoadSave::loadWavetables(SynthBase* synth, const json& wavetables) {
  if (synth->getWavetableCreator(0) == nullptr || !wavetables.is_array())
    return;

  int i = 0;
  for (const json& wavetable : wavetables) {
    if (i >= vital::kNumOscillators)
      break;

    if (!wavetable.is_object()) {
      i++;
      continue;
    }

    WavetableCreator* wavetable_creator = synth->getWavetableCreator(i);
    try {
      wavetable_creator->jsonToState(wavetable);
      // Controls are loaded already, so the slot's type decides whether the
      // table renders now or waits for the slot to play a wavetable.
      synth->renderWavetable(i);
    }
    catch (const json::exception&) { }
    i++;
  }
}

void LoadSave::loadFilterTables(SynthBase* synth, const json& filter_tables) {
  synth->clearFilterTables();
  if (!filter_tables.is_object())
    return;
  for (auto it = filter_tables.begin(); it != filter_tables.end(); ++it) {
    if (!it.value().is_object())
      continue;
    WavetableCreator* creator = synth->getFilterTableCreator(it.key(), true);
    if (creator == nullptr)
      continue;
    try {
      creator->jsonToState(it.value());
      creator->render();
    }
    catch (const json::exception&) { }
  }
}

void LoadSave::loadLfos(SynthBase* synth, const json& lfos) {
  if (!lfos.is_array())
    return;

  int i = 0;
  for (const json& lfo : lfos) {
    if (i >= vital::kNumLfos)
      break;

    if (!lfo.is_object() || !LineGenerator::isValidJson(lfo)) {
      i++;
      continue;
    }

    LineGenerator* lfo_source = synth->getLfoSource(i);
    try {
      lfo_source->jsonToState(lfo);
      lfo_source->render();
    }
    catch (const json::exception&) { }
    i++;
  }
}

void LoadSave::loadSaveState(std::map<std::string, String>& state, json data) {
  if (data.count("preset_name") && data["preset_name"].is_string()) {
    std::string preset_name = data["preset_name"];
    state["preset_name"] = preset_name;
  }

  if (data.count("author") && data["author"].is_string()) {
    std::string author = data["author"];
    state["author"] = author;
  }

  if (data.count("comments") && data["comments"].is_string()) {
    std::string comments = data["comments"];
    state["comments"] = comments;
  }

  if (data.count("preset_style") && data["preset_style"].is_string()) {
    std::string style = data["preset_style"];
    state["style"] = style;
  }

  if (data.count("preset_tags")) {
    if (data["preset_tags"].is_string()) {
      std::string tags = data["preset_tags"];
      state["tags"] = tags;
    }
    else if (data["preset_tags"].is_array()) {
      StringArray tags;
      for (const auto& tag : data["preset_tags"]) {
        if (tag.is_string()) {
          std::string tag_text = tag;
          tags.add(String(tag_text).trim());
        }
      }
      state["tags"] = tags.joinIntoString(", ");
    }
  }

  for (int i = 0; i < vital::kNumMacros; ++i) {
    std::string key = "macro" + std::to_string(i + 1);
    if (data.count(key) && data[key].is_string()) {
      std::string name = data[key];
      state[key] = name;
    }
    else
      state[key] = "MACRO " + std::to_string(i + 1);
  }
}

void LoadSave::initSaveInfo(std::map<std::string, String>& save_info) {
  save_info["preset_name"] = "";
  save_info["author"] = "";
  save_info["comments"] = "";
  save_info["style"] = "";
  save_info["tags"] = "";

  for (int i = 0; i < vital::kNumMacros; ++i)
    save_info["macro" + std::to_string(i + 1)] = "MACRO " + std::to_string(i + 1);
}

json LoadSave::updateFromOldVersion(json state) {
  json settings = state["settings"];
  json modulations = settings["modulations"];
  json sample = settings["sample"];
  json wavetables = settings["wavetables"];

  std::string version = state["synth_version"];

  if (compareVersionStrings(version, "0.2.0") < 0 || settings.count("sub_octave")) {
    int sub_waveform = settings["sub_waveform"];
    if (sub_waveform == 4)
      sub_waveform = 5;
    else if (sub_waveform == 5)
      sub_waveform = 4;
    settings["sub_waveform"] = sub_waveform;

    int sub_octave = settings["sub_octave"];
    settings["sub_transpose"] = vital::kNotesPerOctave * sub_octave;

    int osc_1_filter_routing = settings["osc_1_filter_routing"];
    int osc_2_filter_routing = settings["osc_2_filter_routing"];
    int sample_filter_routing = settings["sample_filter_routing"];
    int sub_filter_routing = settings["sub_filter_routing"];
    settings["filter_1_osc1_input"] = 1 - osc_1_filter_routing;
    settings["filter_1_osc2_input"] = 1 - osc_2_filter_routing;
    settings["filter_1_sample_input"] = 1 - sample_filter_routing;
    settings["filter_1_sub_input"] = 1 - sub_filter_routing;
    settings["filter_2_osc1_input"] = osc_1_filter_routing;
    settings["filter_2_osc2_input"] = osc_2_filter_routing;
    settings["filter_2_sample_input"] = sample_filter_routing;
    settings["filter_2_sub_input"] = sub_filter_routing;

    int filter_1_style = settings["filter_1_style"];
    if (filter_1_style == 2)
      filter_1_style = 3;
    else if (filter_1_style == 3)
      filter_1_style = 2;
    settings["filter_1_style"] = filter_1_style;

    int filter_2_style = settings["filter_2_style"];
    if (filter_2_style == 2)
      filter_2_style = 3;
    else if (filter_2_style == 3)
      filter_2_style = 2;
    settings["filter_2_style"] = filter_2_style;
  }

  if (compareVersionStrings(version, "0.2.1") < 0) {
    std::string env_start = "env_";

    for (int i = 0; i < vital::kNumEnvelopes; ++i) {
      std::string number = std::to_string(i + 1);
      std::string attack_string = env_start + number + "_attack";
      std::string decay_string = env_start + number + "_decay";
      std::string release_string = env_start + number + "_release";
      if (settings.count(attack_string) == 0)
        break;

      settings[attack_string] = std::pow(settings[attack_string], 1.0f / 1.5f);
      settings[decay_string] = std::pow(settings[decay_string], 1.0f / 1.5f);
      settings[release_string] = std::pow(settings[release_string], 1.0f / 1.5f);
    }

    if (settings.count("wave_tables"))
      settings["wavetables"] = settings["wave_tables"];
  }

  wavetables = settings["wavetables"];

  if (compareVersionStrings(version, "0.2.4") < 0) {
    int portamento_type = settings["portamento_type"];
    settings["portamento_force"] = std::max(0, portamento_type - 1);

    if (portamento_type == 0)
      settings["portamento_time"] = -10.0f;
  }

  if (compareVersionStrings(version, "0.2.5") < 0) {
    std::string env_start = "env_";

    for (int i = 0; i < vital::kNumEnvelopes; ++i) {
      std::string number = std::to_string(i + 1);
      std::string attack_string = env_start + number + "_attack";
      std::string decay_string = env_start + number + "_decay";
      std::string release_string = env_start + number + "_release";
      if (settings.count(attack_string) == 0)
        break;

      float adjust_power = 3.0f / 4.0f;
      settings[attack_string] = std::pow(settings[attack_string], adjust_power);
      settings[decay_string] = std::pow(settings[decay_string], adjust_power);
      settings[release_string] = std::pow(settings[release_string], adjust_power);
    }
  }

  if (compareVersionStrings(version, "0.2.6") < 0) {
    std::string lfo_start = "lfo_";

    for (int i = 0; i < vital::kNumLfos; ++i) {
      std::string number = std::to_string(i + 1);
      std::string fade_string = lfo_start + number + "_fade_time";
      std::string delay_string = lfo_start + number + "_delay_time";
      settings[fade_string] = 0.0f;
      settings[delay_string] = 0.0f;
    }
  }

  if (compareVersionStrings(version, "0.2.7") < 0) {
    static constexpr float adjustment = 0.70710678119f;
    float osc_1_level = settings["osc_1_level"];
    float osc_2_level = settings["osc_2_level"];
    float sub_level = settings["sub_level"];
    float sample_level = settings["sample_level"];
    osc_1_level = adjustment * osc_1_level * osc_1_level;
    osc_2_level = adjustment * osc_2_level * osc_2_level;
    sub_level = adjustment * sub_level * sub_level;
    sample_level = adjustment * sample_level * sample_level;

    settings["osc_1_level"] = sqrtf(osc_1_level);
    settings["osc_2_level"] = sqrtf(osc_2_level);
    settings["sub_level"] = sqrtf(sub_level);
    settings["sample_level"] = sqrtf(sample_level);
  }

  if (compareVersionStrings(version, "0.3.0") < 0) {
    float reverb_damping = settings["reverb_damping"];
    float reverb_feedback = settings["reverb_feedback"];
    settings["decay_time"] = (reverb_feedback - 0.8f) * 10.0f;
    settings["reverb_high_shelf_gain"] = -reverb_damping * 4.0f;
    settings["reverb_pre_high_cutoff"] = 128.0f;

    json new_modulations;
    for (json& modulation : modulations) {
      if (modulation["destination"] == "reverb_damping")
        modulation["destination"] = "reverb_high_shelf_gain";
      if (modulation["destination"] == "reverb_feedback")
        modulation["destination"] = "reverb_decay_time";

      new_modulations.push_back(modulation);
    }
    modulations = new_modulations;
  }

  if (compareVersionStrings(version, "0.3.1") < 0) {
    float sample_transpose = settings["sample_transpose"];
    float sample_keytrack = settings["sample_keytrack"];
    if (sample_keytrack)
      settings["sample_transpose"] = sample_transpose + 28.0f;
  }

  if (compareVersionStrings(version, "0.3.2") < 0) {
    float osc_1_transpose = settings["osc_1_transpose"];
    float osc_1_midi_track = settings["osc_1_midi_track"];
    if (osc_1_midi_track == 0.0f)
      settings["osc_1_transpose"] = osc_1_transpose - 48.0f;

    float osc_2_transpose = settings["osc_2_transpose"];
    float osc_2_midi_track = settings["osc_2_midi_track"];
    if (osc_2_midi_track == 0.0f)
      settings["osc_2_transpose"] = osc_2_transpose - 48.0f;
  }

  if (compareVersionStrings(version, "0.3.4") < 0) {
    static constexpr int kEffectsBeforeFilterFx = 8;

    float float_order = settings["effect_chain_order"];
    int effect_order[vital::constants::kNumReorderableEffects];
    int old_effect_order[kEffectsBeforeFilterFx];
    vital::utils::decodeFloatToOrder(old_effect_order, float_order, kEffectsBeforeFilterFx);
    for (int i = 0; i < kEffectsBeforeFilterFx; ++i) {
      effect_order[i] = old_effect_order[i];
      if (effect_order[i] >= vital::constants::kFilterFx)
        effect_order[i] += 1;
    }
    effect_order[kEffectsBeforeFilterFx] = vital::constants::kFilterFx;
    settings["effect_chain_order"] = vital::utils::encodeOrderToFloat(effect_order,
                                                                      vital::constants::kNumReorderableEffects);
  }

  if (compareVersionStrings(version, "0.3.5") < 0) {
    float osc_1_distortion_type = settings["osc_1_distortion_type"];
    float osc_2_distortion_type = settings["osc_2_distortion_type"];
    if (osc_1_distortion_type >= 10.0f)
      settings["osc_1_distortion_type"] = osc_1_distortion_type + 1.0f;
    if (osc_2_distortion_type >= 10.0f)
      settings["osc_2_distortion_type"] = osc_2_distortion_type + 1.0f;
  }

  if (compareVersionStrings(version, "0.3.6") < 0) {
    std::string lfo_start = "lfo_";

    for (int i = 0; i < vital::kNumLfos; ++i) {
      std::string sync_type_string = lfo_start + std::to_string(i + 1) + "_sync_type";
      if (settings.count(sync_type_string)) {
        float value = settings[sync_type_string];
        if (value >= 2.0f)
          settings[sync_type_string] = value - 1.0f;
      }
    }
  }

  if (compareVersionStrings(version, "0.3.7") < 0) {
    convertBufferToPcm(sample, "samples");
    convertBufferToPcm(sample, "samples_stereo");
  }

  if (compareVersionStrings(version, "0.4.1") < 0) {
    bool update = false;
    json new_modulations;
    for (json& modulation : modulations) {
      if (modulation["source"] == "perlin") {
        update = true;
        modulation["source"] = "random_1";
      }

      new_modulations.push_back(modulation);
    }

    if (update) {
      modulations = new_modulations;
      settings["random_1_sync"] = 0.0f;
      settings["random_1_frequency"] = 1.65149612947f;
      settings["random_1_stereo"] = 1.0f;
    }
  }

  if (compareVersionStrings(version, "0.4.3") < 0) {
    float osc_1_distortion_type = settings["osc_1_distortion_type"];
    float osc_1_distortion_amount = settings["osc_1_distortion_amount"];
    if (osc_1_distortion_type == vital::SynthOscillator::kFormant) {
      settings["osc_1_distortion_amount"] = 0.5f + 0.5f * osc_1_distortion_amount;

      int index = 1;
      for (json& modulation : modulations) {
        if (modulation["destination"] == "osc_1_distortion_amount") {
          std::string amount_string = "modulation_" + std::to_string(index) + "_amount";
          float last_amount = settings[amount_string];
          settings[amount_string] = 0.5f * last_amount;
        }

        index++;
      }
    }

    float osc_2_distortion_type = settings["osc_2_distortion_type"];
    float osc_2_distortion_amount = settings["osc_2_distortion_amount"];
    if (osc_2_distortion_type == vital::SynthOscillator::kFormant) {
      settings["osc_2_distortion_amount"] = 0.5f + 0.5f * osc_2_distortion_amount;

      int index = 1;
      for (json& modulation : modulations) {
        if (modulation["destination"] == "osc_2_distortion_amount") {
          std::string amount_string = "modulation_" + std::to_string(index) + "_amount";
          float last_amount = settings[amount_string];
          settings[amount_string] = 0.5f * last_amount;
        }

        index++;
      }
    }
  }

  if (compareVersionStrings(version, "0.4.4") < 0) {
    float osc_1_distortion_type = settings["osc_1_distortion_type"];
    float osc_1_distortion_amount = settings["osc_1_distortion_amount"];
    if (osc_1_distortion_type == vital::SynthOscillator::kSync) {
      settings["osc_1_distortion_amount"] = 0.5f + 0.5f * osc_1_distortion_amount;

      int index = 1;
      for (json& modulation : modulations) {
        if (modulation["destination"] == "osc_1_distortion_amount") {
          std::string amount_string = "modulation_" + std::to_string(index) + "_amount";
          float last_amount = settings[amount_string];
          settings[amount_string] = 0.5f * last_amount;
        }

        index++;
      }
    }

    float osc_2_distortion_type = settings["osc_2_distortion_type"];
    float osc_2_distortion_amount = settings["osc_2_distortion_amount"];
    if (osc_2_distortion_type == vital::SynthOscillator::kSync) {
      settings["osc_2_distortion_amount"] = 0.5f + 0.5f * osc_2_distortion_amount;

      int index = 1;
      for (json& modulation : modulations) {
        if (modulation["destination"] == "osc_2_distortion_amount") {
          std::string amount_string = "modulation_" + std::to_string(index) + "_amount";
          float last_amount = settings[amount_string];
          settings[amount_string] = 0.5f * last_amount;
        }

        index++;
      }
    }
  }

  if (compareVersionStrings(version, "0.4.5") < 0) {
    float compressor_low_band = settings["compressor_low_band"];
    float compressor_high_band = settings["compressor_high_band"];
    if (compressor_low_band && compressor_high_band)
      settings["compressor_enabled_bands"] = 0;
    else if (compressor_low_band)
      settings["compressor_enabled_bands"] = 1;
    else if (compressor_high_band)
      settings["compressor_enabled_bands"] = 2;
    else
      settings["compressor_enabled_bands"] = 3;
  }

  if (compareVersionStrings(version, "0.4.7") < 0 && settings.count("osc_1_distortion_type")) {
    static const int kRemapResolution = 32;

    float osc_1_distortion_type = settings["osc_1_distortion_type"];
    if (osc_1_distortion_type)
      settings["osc_1_distortion_type"] = osc_1_distortion_type - 1.0f;

    float osc_2_distortion_type = settings["osc_2_distortion_type"];
    if (osc_2_distortion_type)
      settings["osc_2_distortion_type"] = osc_2_distortion_type - 1.0f;

    if (osc_1_distortion_type == 1.0f)
      settings["osc_1_spectral_morph_type"] = vital::SynthOscillator::kLowPass;

    if (osc_2_distortion_type == 1.0f)
      settings["osc_2_spectral_morph_type"] = vital::SynthOscillator::kLowPass;

    json new_modulations;
    for (json& modulation : modulations) {
      if (osc_1_distortion_type == 1.0f && modulation["destination"] == "osc_1_distortion_amount")
        modulation["destination"] = "osc_1_spectral_morph_amount";
      else if (osc_2_distortion_type == 1.0f && modulation["destination"] == "osc_2_distortion_amount")
        modulation["destination"] = "osc_2_spectral_morph_amount";

      new_modulations.push_back(modulation);
    }

    osc_1_distortion_type = settings["osc_1_distortion_type"];
    osc_2_distortion_type = settings["osc_2_distortion_type"];
    if (osc_1_distortion_type == 7 || osc_1_distortion_type == 8 || osc_1_distortion_type == 9) {
      float original_fm_amount = settings["osc_1_distortion_amount"];
      float new_fm_amount = std::pow(original_fm_amount, 1.0f / 2.0f);
      settings["osc_1_distortion_amount"] = new_fm_amount;

      int index = 1;
      for (json& modulation : new_modulations) {
        if (modulation["destination"] == "osc_1_distortion_amount") {
          std::string number = std::to_string(index);
          std::string amount_string = "modulation_" + number + "_amount";
          float last_amount = settings[amount_string];
          if (last_amount == 0.0f)
            continue;

          bool bipolar = settings["modulation_" + number + "_bipolar"] != 0.0f;
          float min = std::min(original_fm_amount, original_fm_amount + last_amount);
          float max = std::max(original_fm_amount, original_fm_amount + last_amount);

          if (bipolar) {
            min = std::min(original_fm_amount + last_amount * 0.5f, original_fm_amount - last_amount * 0.5f);
            max = std::max(original_fm_amount + last_amount * 0.5f, original_fm_amount - last_amount * 0.5f);
          }

          float min_target = std::pow(min, 1.0f / 2.0f);
          float max_target = std::pow(max, 1.0f / 2.0f);
          float new_amount = max_target - min_target;
          if (bipolar)
            new_amount = 2.0f * std::max(new_fm_amount - min_target, max_target - new_fm_amount);
          settings[amount_string] = new_amount;

          LineGenerator scale;
          scale.initLinear();
          scale.setNumPoints(kRemapResolution);
          for (int i = 0; i < kRemapResolution; ++i) {
            float t = i / (kRemapResolution - 1.0f);
            float old_value = vital::utils::interpolate(min, max, t);
            float adjusted_old_value = std::pow(old_value, 1.0f / 2.0f);
            float y = 1.0f - (adjusted_old_value - min_target) / new_amount;
            scale.setPoint(i, std::pair<float, float>(t, y));
          }
          modulation["line_mapping"] = scale.stateToJson();
        }
        index++;
      }
    }

    if (osc_2_distortion_type == 7 || osc_2_distortion_type == 8 || osc_2_distortion_type == 9) {
      float original_fm_amount = settings["osc_2_distortion_amount"];
      float new_fm_amount = std::pow(original_fm_amount, 1.0f / 2.0f);
      settings["osc_2_distortion_amount"] = new_fm_amount;

      int index = 1;
      for (json& modulation : new_modulations) {
        if (modulation["destination"] == "osc_2_distortion_amount") {
          std::string number = std::to_string(index);
          std::string amount_string = "modulation_" + number + "_amount";
          float last_amount = settings[amount_string];
          if (last_amount == 0.0f)
            continue;

          bool bipolar = settings["modulation_" + number + "_bipolar"] != 0.0f;
          float min = std::min(original_fm_amount, original_fm_amount + last_amount);
          float max = std::max(original_fm_amount, original_fm_amount + last_amount);

          if (bipolar) {
            min = std::min(original_fm_amount + last_amount * 0.5f, original_fm_amount - last_amount * 0.5f);
            max = std::max(original_fm_amount + last_amount * 0.5f, original_fm_amount - last_amount * 0.5f);
          }

          float min_target = std::pow(min, 1.0f / 2.0f);
          float max_target = std::pow(max, 1.0f / 2.0f);
          float new_amount = max_target - min_target;
          if (bipolar)
            new_amount = 2.0f * std::max(new_fm_amount - min_target, max_target - new_fm_amount);
          settings[amount_string] = new_amount;

          LineGenerator scale;
          scale.initLinear();
          scale.setNumPoints(kRemapResolution);
          for (int i = 0; i < kRemapResolution; ++i) {
            float t = i / (kRemapResolution - 1.0f);
            float old_value = vital::utils::interpolate(min, max, t);
            float adjusted_old_value = std::pow(old_value, 1.0f / 2.0f);
            float y = 1.0f - (adjusted_old_value - min_target) / new_amount;
            scale.setPoint(i, std::pair<float, float>(t, y));
          }
          modulation["line_mapping"] = scale.stateToJson();
        }
        index++;
      }
    }

    modulations = new_modulations;
  }

  if (compareVersionStrings(version, "0.5.0") < 0 && settings.count("sub_on")) {
    settings["osc_3_on"] = settings["sub_on"];
    settings["osc_3_level"] = settings["sub_level"];
    settings["osc_3_pan"] = settings["sub_pan"];
    settings["osc_3_transpose"] = settings["sub_transpose"];

    if (settings.count("sub_transpose_quantize"))
      settings["osc_3_transpose_quantize"] = settings["sub_transpose_quantize"];

    settings["osc_3_tune"] = settings["sub_tune"];
    settings["osc_3_phase"] = 0.25f;
    settings["osc_3_random_phase"] = 0.0f;
    
    float sub_waveform = settings["sub_waveform"];
    settings["osc_3_wave_frame"] = sub_waveform * 257.0f / 6.0f;

    float sub_filter1 = settings["filter_1_sub_input"];
    float sub_filter2 = settings["filter_2_sub_input"];
    float sub_direct_out = settings["sub_direct_out"];
    if (sub_direct_out)
      settings["osc_3_destination"] = 4.0f;
    else if (sub_filter1 && sub_filter2)
      settings["osc_3_destination"] = 2.0f;
    else if (sub_filter2)
      settings["osc_3_destination"] = 1.0f;
    else if (sub_filter1)
      settings["osc_3_destination"] = 0.0f;
    else
      settings["osc_3_destination"] = 3.0f;

    float osc1_filter1 = settings["filter_1_osc1_input"];
    float osc1_filter2 = settings["filter_2_osc1_input"];
    if (osc1_filter1 && osc1_filter2)
      settings["osc_1_destination"] = 2.0f;
    else if (osc1_filter2)
      settings["osc_1_destination"] = 1.0f;
    else if (osc1_filter1)
      settings["osc_1_destination"] = 0.0f;
    else
      settings["osc_1_destination"] = 3.0f;

    float osc2_filter1 = settings["filter_1_osc2_input"];
    float osc2_filter2 = settings["filter_2_osc2_input"];
    if (osc2_filter1 && osc2_filter2)
      settings["osc_2_destination"] = 2.0f;
    else if (osc2_filter2)
      settings["osc_2_destination"] = 1.0f;
    else if (osc2_filter1)
      settings["osc_2_destination"] = 0.0f;
    else
      settings["osc_2_destination"] = 3.0f;

    float sample_filter1 = settings["filter_1_sample_input"];
    float sample_filter2 = settings["filter_2_sample_input"];
    if (sample_filter1 && sample_filter2)
      settings["sample_destination"] = 2.0f;
    else if (sample_filter2)
      settings["sample_destination"] = 1.0f;
    else if (sample_filter1)
      settings["sample_destination"] = 0.0f;
    else
      settings["sample_destination"] = 3.0f;

    vital::Wavetable wavetable(vital::kNumOscillatorWaveFrames);
    WavetableCreator wavetable_creator(&wavetable);
    wavetable_creator.initPredefinedWaves();
    wavetable_creator.setName("Sub");

    json new_wavetables;
    for (int i = (int)wavetables.size() - 1; i >= 0; --i)
      new_wavetables.push_back(wavetables[i]);

    new_wavetables.push_back(wavetable_creator.stateToJson());
    settings["wavetables"] = new_wavetables;

    json new_modulations;
    for (json& modulation : modulations) {
      if (modulation["destination"] == "sub_transpose")
        modulation["destination"] = "osc_3_transpose";
      else if (modulation["destination"] == "sub_tune")
        modulation["destination"] = "osc_3_tune";
      else if (modulation["destination"] == "sub_level")
        modulation["destination"] = "osc_3_level";
      else if (modulation["destination"] == "sub_pan")
        modulation["destination"] = "osc_3_pan";

      new_modulations.push_back(modulation);
    }

    modulations = new_modulations;
  }

  if (compareVersionStrings(version, "0.5.5") < 0) {
    float flanger_tempo = settings["flanger_tempo"];
    settings["flanger_tempo"] = flanger_tempo + 1.0f;

    float phaser_tempo = settings["phaser_tempo"];
    settings["phaser_tempo"] = phaser_tempo + 1.0f;

    float chorus_tempo = settings["chorus_tempo"];
    settings["chorus_tempo"] = chorus_tempo + 1.0f;

    float delay_tempo = settings["delay_tempo"];
    settings["delay_tempo"] = delay_tempo + 1.0f;

    std::string lfo_start = "lfo_";
    for (int i = 0; i < vital::kNumLfos; ++i) {
      std::string tempo_string = lfo_start + std::to_string(i + 1) + "_tempo";
      if (settings.count(tempo_string)) {
        float tempo = settings[tempo_string];
        settings[tempo_string] = tempo + 1.0f;
      }
    }

    std::string random_start = "random_";
    for (int i = 0; i < vital::kNumRandomLfos; ++i) {
      std::string tempo_string = random_start + std::to_string(i + 1) + "_tempo";
      if (settings.count(tempo_string)) {
        float tempo = settings[tempo_string];
        settings[tempo_string] = tempo + 1.0f;
      }
    }
  }

  if (compareVersionStrings(version, "0.5.7") < 0) {
    settings["delay_aux_sync"] = settings["delay_sync"];
    settings["delay_aux_frequency"] = settings["delay_frequency"];
    settings["delay_aux_tempo"] = settings["delay_tempo"];

    float style = settings["delay_style"];
    if (style)
      settings["delay_style"] = style + 1.0f;
  }

  if (compareVersionStrings(version, "0.5.8") < 0)
    settings["chorus_damping"] = 1.0f;

  if (compareVersionStrings(version, "0.6.5") < 0) {
    settings["stereo_mode"] = 1.0f;
    float routing = settings["stereo_routing"];
    routing *= 0.125f;
    if (routing < 0.0f)
      settings["stereo_routing"] = 1.0f - routing;
    else
      settings["stereo_routing"] = routing;
  }

  if (compareVersionStrings(version, "0.6.6") < 0) {
    float stereo_mode = settings["stereo_mode"];
    float routing = settings["stereo_routing"];
    if (stereo_mode == 0.0f)
      settings["stereo_routing"] = 1.0f - routing;
  }

  if (compareVersionStrings(version, "0.6.7") < 0) {
    float chorus_damping = settings["chorus_damping"];
    settings["chorus_cutoff"] = 20.0f;
    settings["chorus_spread"] = chorus_damping;

    json new_modulations;
    for (json& modulation : modulations) {
      if (modulation["destination"] == "chorus_damping")
        modulation["destination"] = "chorus_spread";

      new_modulations.push_back(modulation);
    }
    modulations = new_modulations;
  }

  if (compareVersionStrings(version, "0.7.1") < 0) {
    float osc_1_spectral_morph_type = 0.0f;
    if (settings.count("osc_1_spectral_morph_type"))
      osc_1_spectral_morph_type = settings["osc_1_spectral_morph_type"];

    float osc_2_spectral_morph_type = 0.0f;
    if (settings.count("osc_2_spectral_morph_type"))
      osc_2_spectral_morph_type = settings["osc_2_spectral_morph_type"];

    if (osc_1_spectral_morph_type == 9.0f) {
      float osc_1_spectral_morph_amount = settings["osc_1_spectral_morph_amount"];
      settings["osc_1_spectral_morph_amount"] = -0.5f * osc_1_spectral_morph_amount + 0.5f;

      int index = 1;
      for (json& modulation : modulations) {
        if (modulation["destination"] == "osc_1_spectral_morph_amount") {
          std::string name = "modulation_" + std::to_string(index) + "_amount";
          float modulation_amount = settings[name];
          settings[name] = modulation_amount * -0.5f;
        }

        index++;
      }
    }
    if (osc_2_spectral_morph_type == 9.0f) {
      float osc_2_spectral_morph_amount = settings["osc_2_spectral_morph_amount"];
      settings["osc_2_spectral_morph_amount"] = -0.5f * osc_2_spectral_morph_amount + 0.5f;

      int index = 1;
      for (json& modulation : modulations) {
        if (modulation["destination"] == "osc_2_spectral_morph_amount") {
          std::string name = "modulation_" + std::to_string(index) + "_amount";
          float modulation_amount = settings[name];
          settings[name] = modulation_amount * -0.5f;
        }

        index++;
      }
    }
  }

  if (compareVersionStrings(version, "0.7.5") < 0) {
    static constexpr float kFlangerCenterMultiply = 48.0f / 128.0f;
    static constexpr float kFlangerCenterOffset = 53.69f;
    static constexpr float kPhaserCenterMultiply = 48.0f / 128.0f;

    if (settings.count("flanger_center")) {
      float flanger_center = settings["flanger_center"];
      settings["flanger_center"] = flanger_center + kFlangerCenterOffset;
    }

    int index = 1;
    for (json& modulation : modulations) {
      if (modulation["destination"] == "flanger_center") {
        std::string name = "modulation_" + std::to_string(index) + "_amount";
        float modulation_amount = settings[name];
        settings[name] = modulation_amount * kFlangerCenterMultiply;
      }
      if (modulation["destination"] == "phaser_center") {
        std::string name = "modulation_" + std::to_string(index) + "_amount";
        float modulation_amount = settings[name];
        settings[name] = modulation_amount * kPhaserCenterMultiply;
      }

      index++;
    }
  }

  if (compareVersionStrings(version, "0.7.6") < 0) {
    int filter_1_model = settings["filter_1_model"];
    int filter_2_model = settings["filter_2_model"];

    if (filter_1_model == 6) {
      int filter_1_style = settings["filter_1_style"];
      if (filter_1_style == 1)
        settings["filter_1_style"] = 3;
    }

    if (filter_2_model == 6) {
      int filter_2_style = settings["filter_2_style"];
      if (filter_2_style == 1)
        settings["filter_2_style"] = 3;
    }

    if (settings.count("filter_fx_model")) {
      int filter_fx_model = settings["filter_fx_model"];
      if (filter_fx_model == 6) {
        int filter_fx_style = settings["filter_fx_style"];
        if (filter_fx_style == 1)
          settings["filter_fx_style"] = 3;
      }
    }
  }

  if (compareVersionStrings(version, "0.7.6") < 0) {
    if (settings.count("osc_1_spectral_morph_type")) {
      float osc_1_spectral_morph_type = settings["osc_1_spectral_morph_type"];
      if (osc_1_spectral_morph_type == 10.0f)
        settings["osc_1_spectral_morph_type"] = 7.0f;
    }

    if (settings.count("osc_2_spectral_morph_type")) {
      float osc_2_spectral_morph_type = settings["osc_2_spectral_morph_type"];
      if (osc_2_spectral_morph_type == 10.0f)
        settings["osc_2_spectral_morph_type"] = 7.0f;
    }
    
    if (settings.count("osc_3_spectral_morph_type")) {
      float osc_3_spectral_morph_type = settings["osc_3_spectral_morph_type"];
      if (osc_3_spectral_morph_type == 10.0f)
        settings["osc_3_spectral_morph_type"] = 7.0f;
    }
  }

  if (compareVersionStrings(version, "0.8.1") < 0) {
    for (int i = 0; i < vital::kNumLfos; ++i) {
      std::string name = "lfo_" + std::to_string(i) + "_smooth_mode";
      settings[name] = 0.0f;
    }
  }

  if (compareVersionStrings(version, "0.9.0") < 0) {
    float filter_1_model = settings["filter_1_model"];
    if (filter_1_model == 4) {
      settings["filter_1_blend"] = 0.0f;
      settings["filter_1_style"] = 0.0f;
      int index = 1;
      for (json& modulation : modulations) {
        if (modulation["destination"] == "filter_1_blend") {
          std::string name = "modulation_" + std::to_string(index) + "_amount";
          settings[name] = 0.0f;
        }

        index++;
      }
    }

    float filter_2_model = settings["filter_2_model"];
    if (filter_2_model == 4) {
      settings["filter_2_blend"] = 0.0f;
      settings["filter_2_style"] = 0.0f;
      int index = 1;
      for (json& modulation : modulations) {
        if (modulation["destination"] == "filter_2_blend") {
          std::string name = "modulation_" + std::to_string(index) + "_amount";
          settings[name] = 0.0f;
        }

        index++;
      }
    }
  }

  if (compareVersionStrings(version, "1.0.8") < 0) {
    for (int i = 0; i < 3; ++i)
      shiftOldDirectEffectDestination(settings, "osc_" + std::to_string(i + 1) + "_destination");
    shiftOldDirectEffectDestination(settings, "sample_destination");
  }

  migrateVitalSourceDestinationLayout(settings);

  settings["modulations"] = modulations;
  settings["sample"] = sample;
  state["settings"] = settings;
  return state;
}

bool LoadSave::jsonToState(SynthBase* synth, std::map<std::string, String>& save_info, json data) {
  if (!data.is_object() || !data.count("settings") || !data["settings"].is_object())
    return false;

  const bool has_version = data.count("synth_version") && data["synth_version"].is_string();
  String version = has_version ? safeVersionString(data) : String(ProjectInfo::versionString);
  if (!has_version)
    data["synth_version"] = version.toStdString();

  // Behaviour version 2 is the sample-accurate modulation engine. Older
  // presets are intentionally migrated to the corrected clock/event rules;
  // subsequent saves persist this marker instead of silently changing again.
  int engine_behavior_version = 1;
  if (data.count("engine_behavior_version") && data["engine_behavior_version"].is_number_integer())
    engine_behavior_version = data["engine_behavior_version"].get<int>();
  if (engine_behavior_version < kEngineBehaviorVersion)
    data["engine_behavior_version"] = kEngineBehaviorVersion;
  
  int compare_versions = compareVersionStrings(version, ProjectInfo::versionString);
  if ((has_version && compare_versions < 0) || data["settings"].count("sub_octave"))
    data = updateFromOldVersion(data);
  
  json settings = objectFieldOrEmpty(data, "settings");
  json modulations = arrayFieldOrEmpty(settings, "modulations");
  migrateFixedSourcesToSlots(settings, modulations);
  migrateSlotTypeLevels(settings, modulations);
  json wavetables = arrayFieldOrEmpty(settings, "wavetables");
  json lfos = arrayFieldOrEmpty(settings, "lfos");

  migrateLegacyEffectChains(settings, modulations);

  loadControls(synth, settings);
  // Routes plug into the controls of each oscillator's type, so build the
  // types the state selects before reading them. Clear the routes first: they
  // belong to the state being replaced.
  synth->clearModulations();
  synth->prepareSourceEngines();
  loadModulations(synth, modulations);
  loadSlotSamples(synth, settings);
  loadSlotInstruments(synth, settings);
  loadConvolutionIrs(synth, objectFieldOrEmpty(settings, "convolution_irs"));
  loadWavetables(synth, wavetables);
  loadFilterTables(synth, objectFieldOrEmpty(settings, "filter_tables"));
  loadLfos(synth, lfos);
  loadSaveState(save_info, data);
  synth->checkOversampling();
  synth->clearPendingGuiUpdates();
  
  return true;
}

namespace {
  // Preset metadata reads work on plain and container presets alike.
  std::unique_ptr<InputStream> openPresetStream(const File& file) {
    if (vital::multisample::container::isContainer(file)) {
      std::string text;
      if (!vital::multisample::container::readPresetText(file, text, 65536))
        return nullptr;
      return std::make_unique<MemoryInputStream>(text.data(), text.size(), true);
    }
    auto stream = std::make_unique<FileInputStream>(file);
    if (!stream->openedOk())
      return nullptr;
    return stream;
  }

  std::string loadPresetText(const File& file) {
    std::string text;
    vital::multisample::container::readPresetText(file, text);
    return text;
  }
}

String LoadSave::getAuthorFromFile(const File& file) {
  static constexpr int kMaxCharacters = 40;
  static constexpr int kMinSize = 60;
  std::unique_ptr<InputStream> stream_owner = openPresetStream(file);
  if (stream_owner == nullptr)
    return "";
  InputStream& file_stream = *stream_owner;

  if (file_stream.getTotalLength() < kMinSize)
    return "";

  file_stream.readByte();
  file_stream.readByte();
  MemoryBlock author_memory_block;
  file_stream.readIntoMemoryBlock(author_memory_block, 6);

  char end_quote = file_stream.readByte();
  char colon = file_stream.readByte();
  char begin_quote = file_stream.readByte();
  if (author_memory_block.toString() != "author" || end_quote != '"' || colon != ':' || begin_quote != '"') {
    try {
      json parsed_json_state = json::parse(loadPresetText(file), nullptr, false);
      return getAuthor(parsed_json_state);
    }
    catch (const json::exception& e) {
      return "";
    }
  }

  MemoryBlock name_memory_block;
  file_stream.readIntoMemoryBlock(name_memory_block, kMaxCharacters);
  String name = name_memory_block.toString();

  if (!name.contains("\""))
    return name.toStdString();

  StringArray tokens;
  tokens.addTokens(name, "\"", "");

  return tokens[0];
}

String LoadSave::getStyleFromFile(const File& file) {
  static constexpr int kMinSize = 5000;
  std::unique_ptr<InputStream> stream_owner = openPresetStream(file);
  if (stream_owner == nullptr)
    return "";
  InputStream& file_stream = *stream_owner;

  if (file_stream.getTotalLength() < kMinSize)
    return "";

  MemoryBlock style_memory_block;
  file_stream.readIntoMemoryBlock(style_memory_block, kMinSize);

  StringArray tokens;
  tokens.addTokens(style_memory_block.toString(), "\"", "");
  bool found_style = false;
  for (String token : tokens) {
    if (found_style && token.trim() != ":")
      return token;
    if (token == "preset_style")
      found_style = true;
  }

  return "";
}

String LoadSave::getTagsFromFile(const File& file) {
  static constexpr int kMetadataScanSize = 16384;
  std::unique_ptr<InputStream> stream_owner = openPresetStream(file);
  if (stream_owner == nullptr)
    return "";
  InputStream& file_stream = *stream_owner;

  MemoryBlock memory_block;
  file_stream.readIntoMemoryBlock(memory_block, kMetadataScanSize);
  const String text = memory_block.toString();
  const String key = "\"preset_tags\"";
  const int key_index = text.indexOf(key);
  if (key_index < 0)
    return "";

  const int colon = text.indexOfChar(key_index + key.length(), ':');
  if (colon < 0)
    return "";

  int value_start = colon + 1;
  while (value_start < text.length() && CharacterFunctions::isWhitespace(text[value_start]))
    ++value_start;

  if (value_start >= text.length())
    return "";

  if (text[value_start] == '"') {
    const int value_end = text.indexOfChar(value_start + 1, '"');
    if (value_end > value_start)
      return text.substring(value_start + 1, value_end);
  }
  else if (text[value_start] == '[') {
    const int value_end = text.indexOfChar(value_start + 1, ']');
    if (value_end > value_start) {
      StringArray tags;
      StringArray tokens;
      tokens.addTokens(text.substring(value_start + 1, value_end), ",", "");
      for (auto token : tokens) {
        token = token.trim().unquoted().trim();
        if (token.isNotEmpty())
          tags.add(token);
      }
      return tags.joinIntoString(", ");
    }
  }

  return "";
}

std::string LoadSave::getAuthor(json data) {
  if (data.count("author"))
    return data["author"];
  return "";
}

std::string LoadSave::getLicense(json data) {
  if (data.count("license"))
    return data["license"];
  return "";
}

File LoadSave::getConfigFile() {
#if defined(JUCE_DATA_STRUCTURES_H_INCLUDED)
  PropertiesFile::Options config_options;
  config_options.applicationName = "Atlas";
  config_options.osxLibrarySubFolder = "Application Support";
  config_options.filenameSuffix = "config";

#ifdef LINUX
  config_options.folderName = "." + String(ProjectInfo::projectName).toLowerCase();
#else
  config_options.folderName = String(ProjectInfo::projectName).toLowerCase();
#endif

  return config_options.getDefaultFile();
#else
  return File();
#endif
}

void LoadSave::writeCrashLog(String crash_log) {
  File data_dir = getDataDirectory();
  if (!data_dir.exists() || !data_dir.isDirectory())
    return;
  File file = data_dir.getChildFile("crash.txt");
  file.replaceWithText(crash_log);
}

void LoadSave::writeErrorLog(String error_log) {
  File data_dir = getDataDirectory();
  if (!data_dir.exists() || !data_dir.isDirectory())
    return; 

  File file = getDataDirectory().getChildFile("errors.txt");
  file.appendText(error_log + "\n");
}

File LoadSave::getFavoritesFile() {
#if defined(JUCE_DATA_STRUCTURES_H_INCLUDED)
  PropertiesFile::Options config_options;
  config_options.applicationName = "Atlas";
  config_options.osxLibrarySubFolder = "Application Support";
  config_options.filenameSuffix = "favorites";

#ifdef LINUX
  config_options.folderName = "." + String(ProjectInfo::projectName).toLowerCase();
#else
  config_options.folderName = String(ProjectInfo::projectName).toLowerCase();
#endif

  return config_options.getDefaultFile();
#else
  return File();
#endif
}

File LoadSave::getDefaultSkin() {
#if defined(JUCE_DATA_STRUCTURES_H_INCLUDED)
  PropertiesFile::Options config_options;
  config_options.applicationName = "Atlas";
  config_options.osxLibrarySubFolder = "Application Support";
  config_options.filenameSuffix = "skin";

#ifdef LINUX
  config_options.folderName = "." + String(ProjectInfo::projectName).toLowerCase();
#else
  config_options.folderName = String(ProjectInfo::projectName).toLowerCase();
#endif

  return config_options.getDefaultFile();
#else
  return File();
#endif
}

json LoadSave::getConfigJson() {
  File config_file = getConfigFile();
  if (!config_file.exists())
    return json();

  try {
    json parsed = json::parse(config_file.loadFileAsString().toStdString(), nullptr, false);
    if (parsed.is_discarded())
      return json();
    return parsed;
  }
  catch (const json::exception& e) {
    return json();
  }
}

json LoadSave::getFavoritesJson() {
  File favorites_file = getFavoritesFile();
  if (!favorites_file.exists())
    return json();

  try {
    json parsed = json::parse(favorites_file.loadFileAsString().toStdString(), nullptr, false);
    if (parsed.is_discarded())
      return json();
    return parsed;
  }
  catch (const json::exception& e) {
    return json();
  }
}

void LoadSave::addFavorite(const File& new_favorite) {
  json favorites = getFavoritesJson();
  favorites[new_favorite.getFullPathName().toStdString()] = 1;
  saveJsonToFavorites(favorites);
}

void LoadSave::removeFavorite(const File& old_favorite) {
  json favorites = getFavoritesJson();
  std::string path = old_favorite.getFullPathName().toStdString();

  if (favorites.count(path)) {
    favorites.erase(path);
    saveJsonToFavorites(favorites);
  }
}

std::set<std::string> LoadSave::getFavorites() {
  json favorites_json = getFavoritesJson();
  
  std::set<std::string> favorites;
  for (auto& pair : favorites_json.items())
    favorites.insert(pair.key());

  return favorites;
}

void LoadSave::saveJsonToConfig(json config_state) {
  File config_file = getConfigFile();

  if (!config_file.exists())
    config_file.create();
  config_file.replaceWithText(config_state.dump());
}

void LoadSave::saveJsonToFavorites(json favorites_json) {
  File favorites_file = getFavoritesFile();

  if (!favorites_file.exists())
    favorites_file.create();
  favorites_file.replaceWithText(favorites_json.dump());
}

void LoadSave::saveAuthor(std::string author) {
  json data = getConfigJson();

  data["author"] = author;
  saveJsonToConfig(data);
}

void LoadSave::savePreferredTTWTLanguage(std::string language) {
  json data = getConfigJson();
  data["ttwt_language"] = language;
  saveJsonToConfig(data);
}

void LoadSave::saveVersionConfig() {
  json data = getConfigJson();

  data["synth_version"] = ProjectInfo::versionString;
  saveJsonToConfig(data);
}

void LoadSave::saveContentVersion(std::string version) {
  json data = getConfigJson();

  data["content_version"] = version;
  saveJsonToConfig(data);
}

void LoadSave::saveUpdateCheckConfig(bool check_for_updates) {
  json data = getConfigJson();
  data["check_for_updates"] = check_for_updates;
  saveJsonToConfig(data);
}

void LoadSave::saveWorkOffline(bool work_offline) {
  json data = getConfigJson();
  data["work_offline"] = work_offline;
  saveJsonToConfig(data);
}

void LoadSave::saveLoadedSkin(const std::string& name) {
  json data = getConfigJson();
  data["loaded_skin"] = name;
  saveJsonToConfig(data);
}

void LoadSave::saveAnimateWidgets(bool animate_widgets) {
  json data = getConfigJson();
  data["animate_widgets"] = animate_widgets;
  saveJsonToConfig(data);
}

void LoadSave::saveScanDownloads(bool scan_downloads) {
  json data = getConfigJson();
  data["scan_downloads"] = scan_downloads;
  saveJsonToConfig(data);
}

void LoadSave::saveDisplayHzFrequency(bool hz_frequency) {
  json data = getConfigJson();
  data["hz_frequency"] = hz_frequency;
  saveJsonToConfig(data);
}

void LoadSave::saveAuthenticated(bool authenticated) {
  json data = getConfigJson();
  data["authenticated"] = authenticated;
  saveJsonToConfig(data);
}

void LoadSave::saveWindowSize(float window_size) {
  json data = getConfigJson();
  data["window_size"] = window_size;
  saveJsonToConfig(data);
}

void LoadSave::saveLayoutConfig(vital::StringLayout* layout) {
  std::wstring chromatic_layout;
  wchar_t up_key;
  wchar_t down_key;

  if (layout) {
    chromatic_layout = layout->getLayout();
    up_key = layout->getUpKey();
    down_key = layout->getDownKey();
  }
  else {
    chromatic_layout = getComputerKeyboardLayout();
    std::pair<wchar_t, wchar_t> octave_controls = getComputerKeyboardOctaveControls();
    down_key = octave_controls.first;
    up_key = octave_controls.second;
  }

  json layout_data;

  layout_data["chromatic_layout"] = chromatic_layout;
  layout_data["octave_up"] = up_key;
  layout_data["octave_down"] = down_key;

  json data = getConfigJson();
  data["keyboard_layout"] = layout_data;
  saveJsonToConfig(data);
}

void LoadSave::saveMidiMapConfig(MidiManager* midi_manager) {
  MidiManager::midi_map midi_learn_map = midi_manager->getMidiLearnMap();

  json midi_mapping_data;

  for (auto& midi_mapping : midi_learn_map) {
    json midi_map_data;
    midi_map_data["source"] = midi_mapping.first;

    json destinations_data;
    for (auto& midi_destination : midi_mapping.second) {
      json destination_data;

      destination_data["destination"] = midi_destination.first;
      destination_data["min_range"] = midi_destination.second->min;
      destination_data["max_range"] = midi_destination.second->max;
      destinations_data.push_back(destination_data);
    }

    midi_map_data["destinations"] = destinations_data;
    midi_mapping_data.push_back(midi_map_data);
  }

  json data = getConfigJson();
  data["midi_learn"] = midi_mapping_data;
  saveJsonToConfig(data);
}

void LoadSave::loadConfig(MidiManager* midi_manager, vital::StringLayout* layout) {
  json data = getConfigJson();

  // Computer Keyboard Layout
  if (layout) {
    layout->setLayout(getComputerKeyboardLayout());
    std::pair<wchar_t, wchar_t> octave_controls = getComputerKeyboardOctaveControls();
    layout->setDownKey(octave_controls.first);
    layout->setUpKey(octave_controls.second);
  }

  // Midi Learn Map
  if (data.count("midi_learn")) {
    MidiManager::midi_map midi_learn_map = midi_manager->getMidiLearnMap();

    json midi_mapping_data = data["midi_learn"];
    for (json& midi_map_data : midi_mapping_data) {
      int source = midi_map_data["source"];

      if (midi_map_data.count("destinations")) {
        json destinations_data = midi_map_data["destinations"];
        for (json& midi_destination : destinations_data) {
          std::string dest = midi_destination["destination"];
          midi_learn_map[source][dest] = &vital::Parameters::getDetails(dest);
        }
      }
    }
    midi_manager->setMidiLearnMap(midi_learn_map);
  }
}

bool LoadSave::hasDataDirectory() {
  json data = getConfigJson();
  if (data.count("data_directory")) {
    std::string path = data["data_directory"];
    File directory(path);
    File packages = directory.getChildFile(kInstalledPacksFile);
    return directory.exists() && directory.isDirectory() && packages.exists();
  }
  return false;
}

File LoadSave::getAvailablePacksFile() {
  json data = getConfigJson();
  if (data.count("data_directory") == 0)
    return File();

  std::string path = data["data_directory"];
  File directory(path);
  if (!directory.exists() || !directory.isDirectory())
    return File();

  return directory.getChildFile(kAvailablePacksFile);
}

json LoadSave::getAvailablePacks() {
  File packs_file = getAvailablePacksFile();
  if (!packs_file.exists())
    return json();

  try {
    json parsed = json::parse(packs_file.loadFileAsString().toStdString(), nullptr, false);
    if (parsed.is_discarded())
      return json();
    return parsed;
  }
  catch (const json::exception& e) {
    return json();
  }
}

File LoadSave::getInstalledPacksFile() {
  json data = getConfigJson();
  if (data.count("data_directory") == 0)
    return File();

  std::string path = data["data_directory"];
  File directory(path);
  if (!directory.exists() || !directory.isDirectory())
    return File();

  return directory.getChildFile(kInstalledPacksFile);
}

json LoadSave::getInstalledPacks() {
  File packs_file = getInstalledPacksFile();
  if (!packs_file.exists())
    return json();

  try {
    json parsed = json::parse(packs_file.loadFileAsString().toStdString(), nullptr, false);
    if (parsed.is_discarded())
      return json();
    return parsed;
  }
  catch (const json::exception& e) {
    return json();
  }
}

void LoadSave::saveInstalledPacks(const json& packs) {
  File packs_file = getInstalledPacksFile();

  if (!packs_file.exists())
    packs_file.create();
  packs_file.replaceWithText(packs.dump());
}

void LoadSave::markPackInstalled(int id) {
  json packs = getInstalledPacks();
  packs[std::to_string(id)] = 1;
  saveInstalledPacks(packs);
}

void LoadSave::markPackInstalled(const std::string& name) {
  json packs = getInstalledPacks();
  std::string cleaned = String(name).removeCharacters(" ._").toLowerCase().toStdString();

  packs[cleaned] = 1;
  saveInstalledPacks(packs);
}

void LoadSave::saveDataDirectory(const File& data_directory) {
  json data = getConfigJson();
  std::string path = data_directory.getFullPathName().toStdString();
  data["data_directory"] = path;
  saveJsonToConfig(data);
}

bool LoadSave::isInstalled() {
  return getDataDirectory().exists();
}

bool LoadSave::wasUpgraded() {
  json data = getConfigJson();

  if (!data.count("synth_version"))
    return true;

  std::string version = data["synth_version"];
  return compareVersionStrings(version, ProjectInfo::versionString) < 0;
}

bool LoadSave::isExpired() {
  return getDaysToExpire() < 0;
}

bool LoadSave::doesExpire() {
#ifdef EXPIRE_DAYS
  return true;
#else
  return false;
#endif
}

int LoadSave::getDaysToExpire() {
#ifdef EXPIRE_DAYS
  Time current_time = Time::getCurrentTime();
  Time build_time = getBuildTime();

  RelativeTime time_since_compile = current_time - build_time;
  int days_since_compile = time_since_compile.inDays();
  return EXPIRE_DAYS - days_since_compile;
#else
  return 0;
#endif
}

bool LoadSave::shouldCheckForUpdates() {
  json data = getConfigJson();

  if (!data.count("check_for_updates"))
    return true;

  return data["check_for_updates"];
}

bool LoadSave::shouldWorkOffline() {
  json data = getConfigJson();

  if (!data.count("work_offline"))
    return false;

  return data["work_offline"];
}

std::string LoadSave::getLoadedSkin() {
  json data = getConfigJson();

  if (!data.count("loaded_skin"))
    return "";

  return data["loaded_skin"];
}

bool LoadSave::shouldAnimateWidgets() {
  json data = getConfigJson();

  if (!data.count("animate_widgets"))
    return true;

  return data["animate_widgets"];
}

bool LoadSave::shouldScanDownloads() {
  json data = getConfigJson();

  if (!data.count("scan_downloads"))
    return false;

  return data["scan_downloads"];
}

bool LoadSave::displayHzFrequency() {
  json data = getConfigJson();

  if (!data.count("hz_frequency"))
    return false;

  return data["hz_frequency"];
}

bool LoadSave::authenticated() {
  json data = getConfigJson();

  if (!data.count("authenticated"))
    return false;

  return data["authenticated"];
}

int LoadSave::getOversamplingAmount() {
  json data = getConfigJson();

  if (!data.count("oversampling_amount"))
    return 2;

  return data["oversampling_amount"];
}

float LoadSave::loadWindowSize() {
  static constexpr float kMinWindowSize = 0.25f;
  
  json data = getConfigJson();

  if (!data.count("window_size"))
    return 1.0f;

  return std::max<float>(kMinWindowSize, data["window_size"]);
}

String LoadSave::loadVersion() {
  json data = getConfigJson();

  if (!data.count("synth_version"))
    return "0.0.0";

  std::string version = data["synth_version"];
  return version;
}

String LoadSave::loadContentVersion() {
  json data = getConfigJson();

  if (!data.count("content_version"))
    return "0.0";

  std::string version = data["content_version"];
  return version;
}

std::wstring LoadSave::getComputerKeyboardLayout() {
  json data = getConfigJson();

  if (data.count("keyboard_layout")) {
    json layout = data["keyboard_layout"];

    if (layout.count("chromatic_layout"))
      return layout["chromatic_layout"];
  }

  return vital::kDefaultKeyboard;
}

std::string LoadSave::getPreferredTTWTLanguage() {
  json data = getConfigJson();

  if (!data.count("ttwt_language"))
    return "";

  std::string language = data["ttwt_language"];
  return language;
}

std::string LoadSave::getAuthor() {
  json data = getConfigJson();

  if (data.count("author"))
    return data["author"];

  return "";
}

std::pair<wchar_t, wchar_t> LoadSave::getComputerKeyboardOctaveControls() {
  std::pair<wchar_t, wchar_t> octave_controls(vital::kDefaultKeyboardOctaveDown, vital::kDefaultKeyboardOctaveUp);
  json data = getConfigJson();

  if (data.count("keyboard_layout")) {
    json layout = data["keyboard_layout"];
    std::wstring down = layout["octave_down"];
    std::wstring up = layout["octave_up"];
    octave_controls.first = down[0];
    octave_controls.second = up[0];
  }

  return octave_controls;
}

void LoadSave::saveAdditionalFolders(const std::string& name, std::vector<std::string> folders) {
  json data = getConfigJson();
  json wavetable_folders;
  for (std::string& folder : folders)
    wavetable_folders.push_back(folder);
  data[name] = wavetable_folders;

  saveJsonToConfig(data);
}

std::vector<std::string> LoadSave::getAdditionalFolders(const std::string& name) {
  json data = getConfigJson();
  std::vector<std::string> folders;

  if (data.count(name)) {
    json folder_list = data[name];
    for (json& folder : folder_list)
      folders.push_back(folder);
  }

  return folders;
}

namespace {
  // Rewrites every path in a JSON value that sits inside `from` to sit inside `to`.
  bool rebasePaths(json& value, const String& from, const String& to) {
    bool changed = false;
    if (value.is_string()) {
      const String path(value.get<std::string>());
      if (path == from || path.startsWith(from + File::getSeparatorString())) {
        value = (to + path.substring(from.length())).toStdString();
        changed = true;
      }
    }
    else if (value.is_array() || value.is_object()) {
      for (auto& item : value)
        changed = rebasePaths(item, from, to) || changed;
    }
    return changed;
  }
}

File LoadSave::getLegacyDataDirectory() {
  return File::getSpecialLocation(File::userDocumentsDirectory).getChildFile("Alessio Plugins").getChildFile("Atlas");
}

void LoadSave::migrateLegacyDataDirectory() {
#ifndef LINUX
  // Atlas's library used to default to Documents/Alessio Plugins/Atlas. Move
  // it once to Documents/Blind Producers Plugins/Atlas and repoint the
  // settings and favourites; never merge into a library already there.
  const File legacy = getLegacyDataDirectory();
  const File target = getDefaultDataDirectory();
  json config = getConfigJson();
  const bool configured = config.is_object() && config.count("data_directory") && config["data_directory"].is_string();
  const bool points_at_legacy = configured && File(String(config["data_directory"].get<std::string>())) == legacy;
  if (configured && !points_at_legacy)
    return;
  if (legacy.isDirectory()) {
    if (target.exists() || !target.getParentDirectory().createDirectory() || !legacy.moveFileTo(target)) {
      if (!configured)
        saveDataDirectory(legacy);   // keep using the old library where it is
      return;
    }
  }
  else if (!(points_at_legacy && target.isDirectory())) {
    return;   // nothing to move, or the installer already moved it and nothing points at the old place
  }
  const String from = legacy.getFullPathName(), to = target.getFullPathName();
  if (!config.is_object())
    config = json::object();
  rebasePaths(config, from, to);
  config["data_directory"] = to.toStdString();
  saveJsonToConfig(config);

  json favorites = getFavoritesJson();
  if (favorites.is_object()) {
    json moved = json::object();
    for (auto& entry : favorites.items()) {
      const String key(entry.key());
      moved[(key.startsWith(from + File::getSeparatorString()) ? to + key.substring(from.length()) : key).toStdString()] =
          entry.value();
    }
    saveJsonToFavorites(moved);
  }
#endif
}

File LoadSave::getDataDirectory() {
  static std::once_flag migrated;
  std::call_once(migrated, [] { migrateLegacyDataDirectory(); });
  json data = getConfigJson();
  if (data.count("data_directory")) {
    std::string path = data["data_directory"];
    File folder(path);
    if (folder.exists() && folder.isDirectory())
      return folder;
  }

#ifdef LINUX
  File directory = File(kLinuxUserDataDirectory);
  String xdg_data_home = SystemStats::getEnvironmentVariable ("XDG_DATA_HOME", {});

  if (!xdg_data_home.trim().isEmpty())
    directory = File(xdg_data_home).getChildFile("Atlas");

#else
  File directory = getDefaultDataDirectory();
#endif

  return directory;
}

File LoadSave::getDefaultDataDirectory() {
  return File::getSpecialLocation(File::userDocumentsDirectory)
      .getChildFile("Blind Producers Plugins").getChildFile("Atlas");
}

std::vector<File> LoadSave::getDirectories(const String& folder_name) {
  File data_dir = getDataDirectory();
  std::vector<File> directories;

  if (!data_dir.exists() || !data_dir.isDirectory())
    return directories;

  Array<File> sub_folders;
  sub_folders.add(data_dir);
  data_dir.findChildFiles(sub_folders, File::findDirectories, false);
  for (const File& sub_folder : sub_folders) {
    File directory = sub_folder.getChildFile(folder_name);
    if (directory.exists() && directory.isDirectory())
      directories.push_back(directory);
  }

  return directories;
}

std::vector<File> LoadSave::getPresetDirectories() {
  return getDirectories(kPresetFolderName);
}

std::vector<File> LoadSave::getWavetableDirectories() {
  return getDirectories(kWavetableFolderName);
}

std::vector<File> LoadSave::getSkinDirectories() {
  return getDirectories(kSkinFolderName);
}

std::vector<File> LoadSave::getSampleDirectories() {
  return getDirectories(kSampleFolderName);
}

std::vector<File> LoadSave::getLfoDirectories() {
  return getDirectories(kLfoFolderName);
}

std::vector<File> LoadSave::getIrDirectories() {
  return getDirectories(kIrFolderName);
}

std::vector<File> LoadSave::getFxDirectories() {
  return getDirectories(kFxFolderName);
}

File LoadSave::getUserDirectory() {
  File directory = getDataDirectory().getChildFile(kUserDirectoryName);
  if (!directory.exists())
    directory.createDirectory();
  return directory;
}

File LoadSave::getUserPresetDirectory() {
  File directory = getUserDirectory().getChildFile(kPresetFolderName);
  if (!directory.exists())
    directory.createDirectory();
  return directory;
}

File LoadSave::getUserWavetableDirectory() {
  File directory = getUserDirectory().getChildFile(kWavetableFolderName);
  if (!directory.exists())
    directory.createDirectory();
  return directory;
}

File LoadSave::getUserSkinDirectory() {
  File directory = getUserDirectory().getChildFile(kSkinFolderName);
  if (!directory.exists())
    directory.createDirectory();
  return directory;
}

File LoadSave::getUserSampleDirectory() {
  File directory = getUserDirectory().getChildFile(kSampleFolderName);
  if (!directory.exists())
    directory.createDirectory();
  return directory;
}

File LoadSave::getUserLfoDirectory() {
  File directory = getUserDirectory().getChildFile(kLfoFolderName);
  if (!directory.exists())
    directory.createDirectory();
  return directory;
}

File LoadSave::getUserFxDirectory() {
  File directory = getUserDirectory().getChildFile(kFxFolderName);
  if (!directory.exists())
    directory.createDirectory();
  return directory;
}

File LoadSave::getUserIrDirectory() {
  File directory = getUserDirectory().getChildFile(kIrFolderName);
  if (!directory.exists())
    directory.createDirectory();
  return directory;
}

File LoadSave::getDownloadsDirectory() {
  return File::getSpecialLocation(File::userHomeDirectory).getChildFile("Downloads");
}

int LoadSave::scanDownloadsForPresets() {
  File downloads = getDownloadsDirectory();
  if (!downloads.exists() || !downloads.isDirectory())
    return 0;

  Array<File> found;
  downloads.findChildFiles(found, File::findFiles, false, vital::kPresetExtensionsList);

  File destination = getUserPresetDirectory();
  int imported = 0;
  for (const File& preset : found) {
    File target = destination.getChildFile(preset.getFileName());
    if (target.exists())
      continue;

    if (preset.copyFileTo(target))
      ++imported;
    else
      writeErrorLog("Failed to copy downloaded preset: " + preset.getFullPathName());
  }

  return imported;
}

void LoadSave::getAllFilesOfTypeInDirectories(Array<File>& files, const String& extensions,
                                              const std::vector<File>& directories) {
  files.clear();
  for (const File& directory : directories) {
    if (directory.exists() && directory.isDirectory())
      directory.findChildFiles(files, File::findFiles, true, extensions);
  }
}

void LoadSave::getAllPresets(Array<File>& presets) {
  getAllFilesOfTypeInDirectories(presets, vital::kPresetExtensionsList, getPresetDirectories());
}

void LoadSave::getAllWavetables(Array<File>& wavetables) {
  getAllFilesOfTypeInDirectories(wavetables, vital::kWavetableExtensionsList, getWavetableDirectories());
}

void LoadSave::getAllSkins(Array<File>& skins) {
  getAllFilesOfTypeInDirectories(skins, String("*.") + vital::kSkinExtension, getSkinDirectories());
}

void LoadSave::getAllLfos(Array<File>& lfos) {
  getAllFilesOfTypeInDirectories(lfos, vital::kLfoExtensionsList, getLfoDirectories());
}

void LoadSave::getAllSamples(Array<File>& samples) {
  getAllFilesOfTypeInDirectories(samples, vital::kSampleExtensionsList, getSampleDirectories());
}

void LoadSave::getAllFx(Array<File>& fx) {
  getAllFilesOfTypeInDirectories(fx, String("*.") + vital::kFxExtension, getFxDirectories());
}

void LoadSave::getAllUserPresets(Array<File>& presets) {
  std::vector<File> directories = {
    getDataDirectory().getChildFile(kPresetFolderName),
    getUserPresetDirectory()
  };
  getAllFilesOfTypeInDirectories(presets, vital::kPresetExtensionsList, directories);
}

void LoadSave::getAllUserWavetables(Array<File>& wavetables) {
  std::vector<File> directories = {
    getDataDirectory().getChildFile(kWavetableFolderName),
    getUserWavetableDirectory()
  };
  getAllFilesOfTypeInDirectories(wavetables, vital::kWavetableExtensionsList, directories);
}

void LoadSave::getAllUserLfos(Array<File>& lfos) {
  std::vector<File> directories = {
    getDataDirectory().getChildFile(kLfoFolderName),
    getUserLfoDirectory()
  };
  getAllFilesOfTypeInDirectories(lfos, vital::kLfoExtensionsList, directories);
}

void LoadSave::getAllUserSamples(Array<File>& samples) {
  std::vector<File> directories = {
    getDataDirectory().getChildFile(kSampleFolderName),
    getUserSampleDirectory()
  };
  getAllFilesOfTypeInDirectories(samples, vital::kSampleExtensionsList, directories);
}

void LoadSave::getAllUserFx(Array<File>& fx) {
  std::vector<File> directories = {
    getDataDirectory().getChildFile(kFxFolderName),
    getUserFxDirectory()
  };
  getAllFilesOfTypeInDirectories(fx, String("*.") + vital::kFxExtension, directories);
}

void LoadSave::getAllIrs(Array<File>& irs) {
  getAllFilesOfTypeInDirectories(irs, vital::kIrExtensionsList, getIrDirectories());
}

void LoadSave::getAllUserIrs(Array<File>& irs) {
  std::vector<File> directories = {
    getDataDirectory().getChildFile(kIrFolderName),
    getUserIrDirectory()
  };
  getAllFilesOfTypeInDirectories(irs, vital::kIrExtensionsList, directories);
}

int LoadSave::compareFeatureVersionStrings(String a, String b) {
  a.trim();
  b.trim();

  return compareVersionStrings(a.upToLastOccurrenceOf(".", false, true),
                               b.upToLastOccurrenceOf(".", false, true));
}

int LoadSave::compareVersionStrings(String a, String b) {
  a.trim();
  b.trim();

  if (a.isEmpty() && b.isEmpty())
    return 0;

  String major_version_a = a.upToFirstOccurrenceOf(".", false, true);
  String major_version_b = b.upToFirstOccurrenceOf(".", false, true);

  if (!major_version_a.containsOnly("0123456789"))
    major_version_a = "0";
  if (!major_version_b.containsOnly("0123456789"))
    major_version_b = "0";

  int major_value_a = major_version_a.getIntValue();
  int major_value_b = major_version_b.getIntValue();

  if (major_value_a > major_value_b)
    return 1;
  else if (major_value_a < major_value_b)
    return -1;
  return compareVersionStrings(a.fromFirstOccurrenceOf(".", false, true),
                               b.fromFirstOccurrenceOf(".", false, true));
}

File LoadSave::getShiftedFile(const String directory_name, const String& extensions,
                              const std::string& additional_folders_name, const File& current_file, int shift) {
  FileSorterAscending file_sorter;

  std::vector<File> directories = getDirectories(directory_name);
  const bool current_is_atlas_content = current_file.existsAsFile() &&
      std::any_of(directories.begin(), directories.end(), [&current_file](const File& directory) {
        return current_file == directory || current_file.isAChildOf(directory);
      });

  // Samples always cycle within their current folder, including Factory/User
  // subfolders. Other asset types retain their existing library navigation.
  if (current_file.existsAsFile() && (directory_name == String(kSampleFolderName) || !current_is_atlas_content)) {
    Array<File> sibling_files;
    current_file.getParentDirectory().findChildFiles(sibling_files, File::findFiles, false, extensions);
    sibling_files.sort(file_sorter);
    const int sibling_index = sibling_files.indexOf(current_file);
    if (sibling_index >= 0 && !sibling_files.isEmpty())
      return sibling_files[(sibling_index + shift + sibling_files.size()) % sibling_files.size()];
  }

  std::vector<std::string> additional = getAdditionalFolders(additional_folders_name);
  for (const std::string& path : additional)
    directories.push_back(File(path));

  Array<File> all_files;
  getAllFilesOfTypeInDirectories(all_files, extensions, directories);
  if (all_files.isEmpty())
    return File();

  all_files.sort(file_sorter);
  int index = all_files.indexOf(current_file);
  if (index < 0)
    return all_files[0];
  return all_files[(index + shift + all_files.size()) % all_files.size()];
}
