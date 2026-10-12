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

#include "source_slot_module.h"

#include "braids_module.h"
#include "granular_module.h"
#include "multisample_module.h"
#include "multisample_slot.h"
#include "noise_module.h"
#include "openwurli_module.h"
#include "oscillator_module.h"
#include "sample_module.h"
#include "sample_source.h"
#include "spectral_module.h"
#include "synth_constants.h"
#include "wavetable.h"

#include <algorithm>
#include <mutex>

namespace vital {
  namespace {
    // OscillatorModule names its controls "osc_N_x" from the prefix "osc_N".
    std::string oscillatorModulePrefix(const std::string& slot_prefix) {
      return slot_prefix.substr(0, slot_prefix.size() - 1);
    }
  }

  SourceSlotModule::SourceSlotModule(int slot, const Output* beats_per_second) :
      SynthModule(kNumInputs, kNumOutputs), slot_(slot), prefix_(source_types::slotPrefix(slot)),
      beats_per_second_(beats_per_second), wavetable_(std::make_shared<Wavetable>(kNumOscillatorWaveFrames)),
      sample_(std::make_shared<Sample>()), multisample_(std::make_shared<MultiSampleSlot>()),
      playhead_(std::make_shared<PlayheadOutput>()), modulation_output_(std::make_shared<ModulationOutput>()),
      was_on_(std::make_shared<bool>(true)), state_(std::make_shared<ActiveState>()),
      type_controls_(std::make_shared<std::array<TypeControls, source_types::kNumTypes>>()) {
    playhead_->output()->buffer[0] = 0.0f;
    // Written only while a route reads it; the engine enables it on connect.
    Output* modulation = modulation_output_->output();
    for (int i = 0; i < modulation->buffer_size; ++i)
      modulation->buffer[i] = 0.5f;
    modulation->trigger_value = 0.5f;
    modulation_output_->enable(false);
  }

  const std::vector<ControlSpec>& SourceSlotModule::describeType(int type) {
    // Each type is probed once per process: a throwaway module built standalone
    // (its own router, nothing shared) tells us which controls it creates and
    // how they are smoothed, so every slot can allocate matching permanent values.
    static std::array<std::vector<ControlSpec>, source_types::kNumTypes> descriptions;
    static std::array<std::once_flag, source_types::kNumTypes> flags;
    const int index = type < 0 || type >= source_types::kNumTypes ? source_types::kWavetable : type;
    std::call_once(flags[index], [index] {
      std::vector<ControlSpec> specs;
      {
        ControlDescriptionScope scope(specs);
        std::unique_ptr<SynthModule> probe(createTypeModuleFor(0, index, nullptr, nullptr, nullptr));
        probe->init();
      }
      // Store suffixes relative to the probe slot's prefix.
      const std::string prefix = source_types::slotPrefix(0);
      for (auto& spec : specs) {
        if (spec.name.rfind(prefix, 0) == 0)
          spec.name = spec.name.substr(prefix.size());
      }
      descriptions[index] = std::move(specs);
    });
    return descriptions[index];
  }

  SynthModule* SourceSlotModule::createTypeModuleFor(int slot, int type, std::shared_ptr<Wavetable> wavetable,
                                                     std::shared_ptr<Sample> sample,
                                                     const Output* beats_per_second,
                                                     std::shared_ptr<MultiSampleSlot> multisample) {
    switch (type) {
      case source_types::kMultiSample:
        return new MultiSampleModule(source_types::groupPrefix(slot, type), std::move(multisample),
                                     source_types::slotPrefix(slot));
      case source_types::kSample:
        return new SampleModule(source_types::groupPrefix(slot, type), std::move(sample),
                                source_types::slotPrefix(slot));
      case source_types::kGranular:
        return new GranularModule(source_types::groupPrefix(slot, type), std::move(sample), beats_per_second,
                                  source_types::slotPrefix(slot));
      case source_types::kSpectral:
        return new SpectralModule(source_types::groupPrefix(slot, type), std::move(sample),
                                  source_types::slotPrefix(slot));
      case source_types::kBraids:
        return new BraidsModule(source_types::groupPrefix(slot, type), source_types::slotPrefix(slot));
      case source_types::kOpenWurli:
        return new OpenWurliModule(source_types::groupPrefix(slot, type), source_types::slotPrefix(slot));
      case source_types::kNoise:
        return new NoiseModule(source_types::groupPrefix(slot, type), source_types::slotPrefix(slot));
      case source_types::kWavetable:
      case source_types::kAnalog:
      default:
        return new OscillatorModule(oscillatorModulePrefix(source_types::slotPrefix(slot)), std::move(wavetable));
    }
  }

  SynthModule* SourceSlotModule::createTypeModule(int type) const {
    return createTypeModuleFor(slot_, type, wavetable_, sample_, beats_per_second_, multisample_);
  }

  void SourceSlotModule::init() {
    on_ = createBaseControl(prefix_ + "on");
    type_ = createBaseControl(prefix_ + "type");
    for (int type = 0; type < source_types::kNumTypes; ++type)
      createPermanentControls(type);
    const auto find_control = [this](const std::string& name) -> Value* {
      const auto found = data_->controls.find(prefix_ + name);
      return found == data_->controls.end() ? nullptr : found->second;
    };
    distortion_type_ = find_control("distortion_type");
    secondary_distortion_type_ = find_control("secondary_distortion_type");
    multisample_amp_env_ = find_control("multisample_amp_env");
    wurli_amp_env_ = find_control("wurli_amp_env");
    SynthModule::init();
  }

  void SourceSlotModule::createPermanentControls(int type) {
    // Types sharing a parameter group (Wavetable and Analog) share controls.
    for (int other = 0; other < type; ++other) {
      if (source_types::shareModule(other, type)) {
        (*type_controls_)[type] = (*type_controls_)[other];
        return;
      }
    }

    TypeControls& controls = (*type_controls_)[type];
    for (const ControlSpec& spec : describeType(type)) {
      const std::string name = prefix_ + spec.name;
      const auto existing = data_->controls.find(name);
      if (existing != data_->controls.end()) {
        // A control shared with an earlier group (level, pan, distortion): this
        // type runs its smoother too while active.
        controls.controls[name] = existing->second;
        if (spec.smooth_value)
          controls.smoothers.push_back(existing->second);
        continue;
      }
      Value* value = createBaseControl(name, spec.audio_rate, spec.smooth_value);
      controls.controls[name] = value;
      if (spec.smooth_value)
        controls.smoothers.push_back(value);
    }
    controls.created = true;
    // Only the active type's smoothers need to run.
    for (Processor* smoother : controls.smoothers)
      smoother->enable(false);
  }

  void SourceSlotModule::setTypeControlsEnabled(int type, bool enabled) {
    if (type < 0 || type >= source_types::kNumTypes)
      return;
    for (Processor* smoother : (*type_controls_)[type].smoothers)
      smoother->enable(enabled);
  }

  const control_map& SourceSlotModule::typeControls(int type) const {
    static const control_map empty;
    if (type < 0 || type >= source_types::kNumTypes)
      return empty;
    return (*type_controls_)[type].controls;
  }

  int SourceSlotModule::requestedType() const {
    if (type_ == nullptr)
      return source_types::kWavetable;
    const int type = static_cast<int>(type_->value() + 0.5f);
    return type < 0 || type >= source_types::kNumTypes ? source_types::kWavetable : type;
  }

  void SourceSlotModule::connectTypeModule(SynthModule* module, int type) {
    switch (type) {
      case source_types::kSample: {
        auto* sample = static_cast<SampleModule*>(module);
        sample->useInput(input(kReset), SampleModule::kReset);
        sample->useInput(input(kRetrigger), SampleModule::kRetrigger);
        sample->useInput(input(kVoiceEvent), SampleModule::kVoiceEvent);
        sample->useInput(input(kMidi), SampleModule::kMidi);
        sample->useInput(input(kNoteCount), SampleModule::kNoteCount);
        sample->useInput(input(kNotePressed), SampleModule::kNotePressed);
        sample->useOutput(output(kRaw), SampleModule::kRaw);
        sample->useOutput(output(kLevelled), SampleModule::kLevelled);
        state_->module_phase = sample->getPhaseOutput();
        state_->modulation_target = sample;
        break;
      }
      case source_types::kGranular: {
        auto* granular = static_cast<GranularModule*>(module);
        granular->useInput(input(kReset), GranularModule::kReset);
        granular->useInput(input(kVoiceEvent), GranularModule::kVoiceEvent);
        granular->useInput(input(kMidi), GranularModule::kMidi);
        granular->useInput(input(kNoteCount), GranularModule::kNoteCount);
        granular->useOutput(output(kRaw), GranularModule::kRaw);
        granular->useOutput(output(kLevelled), GranularModule::kLevelled);
        state_->module_phase = granular->getPhaseOutput();
        state_->modulation_target = granular;
        break;
      }
      case source_types::kSpectral: {
        auto* spectral = static_cast<SpectralModule*>(module);
        spectral->useInput(input(kReset), SpectralModule::kReset);
        spectral->useInput(input(kRetrigger), SpectralModule::kRetrigger);
        spectral->useInput(input(kMidi), SpectralModule::kMidi);
        spectral->useInput(input(kNotePressed), SpectralModule::kNotePressed);
        spectral->useInput(input(kActiveVoices), SpectralModule::kActiveVoices);
        spectral->useOutput(output(kRaw), SpectralModule::kRaw);
        spectral->useOutput(output(kLevelled), SpectralModule::kLevelled);
        state_->modulation_target = spectral;
        break;
      }
      case source_types::kMultiSample: {
        auto* multisample = static_cast<MultiSampleModule*>(module);
        multisample->useInput(input(kReset), MultiSampleModule::kReset);
        multisample->useInput(input(kRetrigger), MultiSampleModule::kRetrigger);
        multisample->useInput(input(kVoiceEvent), MultiSampleModule::kVoiceEvent);
        multisample->useInput(input(kMidi), MultiSampleModule::kMidi);
        multisample->useInput(input(kNote), MultiSampleModule::kNote);
        multisample->useInput(input(kMidiKey), MultiSampleModule::kMidiKey);
        multisample->useInput(input(kVelocity), MultiSampleModule::kVelocity);
        multisample->useOutput(output(kRaw), MultiSampleModule::kRaw);
        multisample->useOutput(output(kLevelled), MultiSampleModule::kLevelled);
        multisample->useOutput(output(kHold), MultiSampleModule::kHold);
        state_->modulation_target = multisample;
        break;
      }
      case source_types::kBraids: {
        auto* braids = static_cast<BraidsModule*>(module);
        braids->useInput(input(kReset), BraidsModule::kReset);
        braids->useInput(input(kVoiceEvent), BraidsModule::kVoiceEvent);
        braids->useInput(input(kMidi), BraidsModule::kMidi);
        braids->useOutput(output(kRaw), BraidsModule::kRaw);
        braids->useOutput(output(kLevelled), BraidsModule::kLevelled);
        state_->modulation_target = braids;
        break;
      }
      case source_types::kOpenWurli: {
        auto* wurli = static_cast<OpenWurliModule*>(module);
        wurli->useInput(input(kReset), OpenWurliModule::kReset);
        wurli->useInput(input(kVoiceEvent), OpenWurliModule::kVoiceEvent);
        wurli->useInput(input(kMidi), OpenWurliModule::kMidi);
        wurli->useInput(input(kVelocity), OpenWurliModule::kVelocity);
        wurli->useOutput(output(kRaw), OpenWurliModule::kRaw);
        wurli->useOutput(output(kLevelled), OpenWurliModule::kLevelled);
        wurli->useOutput(output(kHold), OpenWurliModule::kHold);
        state_->modulation_target = wurli;
        break;
      }
      case source_types::kNoise: {
        auto* noise = static_cast<NoiseModule*>(module);
        noise->useInput(input(kReset), NoiseModule::kReset);
        noise->useInput(input(kNotePressed), NoiseModule::kNotePressed);
        noise->useOutput(output(kRaw), NoiseModule::kRaw);
        noise->useOutput(output(kLevelled), NoiseModule::kLevelled);
        state_->modulation_target = noise;
        break;
      }
      default: {
        auto* oscillator = static_cast<OscillatorModule*>(module);
        oscillator->useInput(input(kReset), OscillatorModule::kReset);
        oscillator->useInput(input(kRetrigger), OscillatorModule::kRetrigger);
        oscillator->useInput(input(kMidi), OscillatorModule::kMidi);
        oscillator->useInput(input(kActiveVoices), OscillatorModule::kActiveVoices);
        oscillator->useInput(input(kNotePressed), OscillatorModule::kNotePressed);
        oscillator->useOutput(output(kRaw), OscillatorModule::kRaw);
        oscillator->useOutput(output(kLevelled), OscillatorModule::kLevelled);
        state_->oscillator = oscillator;
        break;
      }
    }
  }

  bool SourceSlotModule::prepareActiveType() {
    const int requested = requestedType();
    ActiveState& state = *state_;
    if (state.module != nullptr && source_types::shareModule(state.active_type, requested)) {
      state.active_type = requested;
      return false;
    }

    destroyActiveModule();

    SynthModule* module = createTypeModule(requested);
    connectTypeModule(module, requested);
    {
      // The module reads the slot's permanent values instead of creating its own.
      control_map external = typeControls(requested);
      external[prefix_ + "on"] = on_;
      external[prefix_ + "type"] = type_;
      ExternalControlScope scope(external);
      addSubmodule(module);
      addProcessor(module);
      module->init();
    }
    // setSampleRate expects the base rate; a processor's stored rate already
    // includes its oversampling factor.
    module->setSampleRate(getSampleRate() / std::max(1, getOversampleAmount()));
    module->enable(true);
    if (state.oscillator != nullptr) {
      for (int position = 0; position < kNumModulatorPositions; ++position)
        state.oscillator->oscillator()->setModulatorOutput(position, state.modulators[position]);
    }
    if (state.modulation_target != nullptr) {
      for (int position = 0; position < kNumModulatorPositions; ++position)
        state.modulation_target->setModulatorOutput(position, state.modulators[position]);
    }
    setTypeControlsEnabled(requested, true);
    state.module = module;
    state.active_type = requested;
    *was_on_ = true;  // Clear the shared buffers once more if the slot is off now.
    clone_was_on_ = false;
    output(kRaw)->clearBuffer();
    output(kLevelled)->clearBuffer();
    output(kHold)->clearBuffer();
    playhead_->output()->buffer[0] = 0.0f;
    return true;
  }

  void SourceSlotModule::destroyActiveModule() {
    ActiveState& state = *state_;
    if (state.module == nullptr)
      return;

    setTypeControlsEnabled(state.active_type, false);
    SynthModule* module = state.module;
    state.module = nullptr;
    state.oscillator = nullptr;
    state.module_phase = nullptr;
    state.modulation_target = nullptr;
    state.active_type = -1;
    module->detachMonoProcessors();
    removeSubmodule(module);
    removeProcessor(module);
    delete module;
  }

  bool SourceSlotModule::holdsVoiceOpen() const {
    if (state_->module == nullptr)
      return false;
    if (state_->active_type == source_types::kMultiSample)
      return multisample_amp_env_ != nullptr && multisample_amp_env_->value() < 0.5f;
    if (state_->active_type == source_types::kOpenWurli)
      return wurli_amp_env_ != nullptr && wurli_amp_env_->value() < 0.5f;
    return false;
  }

  void SourceSlotModule::setModulatorOutput(int position, Output* output) {
    if (position < 0 || position >= kNumModulatorPositions)
      return;
    state_->modulators[static_cast<size_t>(position)] = output;
    if (state_->oscillator != nullptr)
      state_->oscillator->oscillator()->setModulatorOutput(position, output);
    if (state_->modulation_target != nullptr)
      state_->modulation_target->setModulatorOutput(position, output);
  }

  int SourceSlotModule::modulatorPosition(bool secondary) const {
    const Value* type_value = secondary ? secondary_distortion_type_ : distortion_type_;
    if (state_->module == nullptr || type_value == nullptr)
      return -1;
    const int type = utils::iclamp(static_cast<int>(type_value->value()), 0, SynthOscillator::kNumDistortionTypes - 1);
    const int position = SynthOscillator::modulatorPosition(type);
    if (position < 0)
      return -1;
    const bool fm = SynthOscillator::isFmType(type);
    if (fm ? !source_types::supportsFm(state_->active_type) : !source_types::supportsRm(state_->active_type))
      return -1;
    return position;
  }

  void SourceSlotModule::process(int num_samples) {
    ActiveState& state = *state_;
    const bool on = state.module != nullptr && on_->value() >= 0.5f;
    if (on) {
      if (!clone_was_on_ && state.active_type == source_types::kSpectral) {
        // A voice copy must have cloned the new module before it is looked up;
        // getLocalProcessor on an unsynced copy would plant an empty entry.
        if (shouldUpdate())
          updateAllProcessors();
        if (Processor* local = getLocalProcessor(state.module))
          local->hardReset();
      }
      SynthModule::process(num_samples);
      playhead_->output()->buffer[0] = state.module_phase != nullptr ? state.module_phase->buffer[0] : poly_float(0.0f);
    }
    else if (*was_on_) {
      output(kRaw)->clearBuffer();
      output(kLevelled)->clearBuffer();
      output(kHold)->clearBuffer();
      playhead_->output()->buffer[0] = 0.0f;
    }
    // Only a sounding Multi-sample or OpenWurli type holds the voice open.
    if (!on || (state.active_type != source_types::kMultiSample && state.active_type != source_types::kOpenWurli)) {
      Output* hold = output(kHold);
      if (poly_float::notEqual(hold->buffer[0], 0.0f).anyMask())
        hold->clearBuffer();
    }
    *was_on_ = on;
    clone_was_on_ = on;
    writeModulationOutput(num_samples);
  }

  void SourceSlotModule::writeModulationOutput(int num_samples) {
    if (!modulation_output_->enabled())
      return;

    // Modulation sources read 0 to 1. The raw audio, before Level, maps
    // -1..1 onto that range, so silence (or a switched-off slot) is the
    // neutral 0.5 and a bipolar route swings evenly around the destination.
    const poly_float* raw = output(kRaw)->buffer;
    Output* modulation = modulation_output_->output();
    poly_float* buffer = modulation->buffer;
    for (int i = 0; i < num_samples; ++i)
      buffer[i] = utils::clamp(raw[i] * 0.5f + 0.5f, 0.0f, 1.0f);
    modulation->trigger_value = buffer[0];
  }
} // namespace vital
