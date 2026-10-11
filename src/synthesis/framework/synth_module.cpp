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

#include "synth_module.h"

#include <algorithm>

#include "synth_constants.h"
#include "smooth_value.h"
#include "value_switch.h"

namespace vital {

  namespace { thread_local effect_control_inputs* prepared_effect_inputs = nullptr; }
  EffectControlPreparation::EffectControlPreparation(effect_control_inputs& inputs) :
      previous_(prepared_effect_inputs) { prepared_effect_inputs = &inputs; }
  EffectControlPreparation::~EffectControlPreparation() { prepared_effect_inputs = previous_; }
  effect_control_inputs* EffectControlPreparation::current() { return prepared_effect_inputs; }

  namespace {
    thread_local std::vector<ControlSpec>* control_description = nullptr;
    thread_local const control_map* external_controls = nullptr;
  }
  ControlDescriptionScope::ControlDescriptionScope(std::vector<ControlSpec>& specs) :
      previous_(control_description) { control_description = &specs; }
  ControlDescriptionScope::~ControlDescriptionScope() { control_description = previous_; }
  std::vector<ControlSpec>* ControlDescriptionScope::current() { return control_description; }
  ExternalControlScope::ExternalControlScope(const control_map& controls) :
      previous_(external_controls) { external_controls = &controls; }
  ExternalControlScope::~ExternalControlScope() { external_controls = previous_; }
  const control_map* ExternalControlScope::current() { return external_controls; }

  Value* SynthModule::createBaseControl(std::string name, bool audio_rate, bool smooth_value) {
    if (prepared_effect_inputs)
      smooth_value = false; // The permanent slot control already smooths this value.
    if (control_description)
      control_description->push_back({ name, audio_rate, smooth_value });
    if (external_controls) {
      const auto external = external_controls->find(name);
      if (external != external_controls->end() && external->second != nullptr) {
        // The host owns (and smooths) this value; just read it.
        data_->controls[name] = external->second;
        return external->second;
      }
    }
    mono_float default_value = Parameters::getDetails(name).default_value;

    Value* val = nullptr;
    if (audio_rate) {
      if (smooth_value) {
        val = new SmoothValue(default_value);
        addMonoProcessor(val, false);
      }
      else {
        val = new Value(default_value);
        addIdleMonoProcessor(val);
      }
    }
    else {
      if (smooth_value) {
        val = new cr::SmoothValue(default_value);
        addMonoProcessor(val, false);
      }
      else {
        val = new cr::Value(default_value);
        addIdleMonoProcessor(val);
      }
    }

    data_->controls[name] = val;
    if (prepared_effect_inputs)
      (*prepared_effect_inputs)[name] = { val, false };
    return val;
  }

  Output* SynthModule::createBaseModControl(std::string name, bool audio_rate,
                                            bool smooth_value, Output* internal_modulation) {
    if (prepared_effect_inputs) {
      VITAL_ASSERT(internal_modulation == nullptr);
      Value* value = createBaseControl(name, audio_rate, false);
      (*prepared_effect_inputs)[name] = { value, true };
      return value->output();
    }
    Processor* base_val = createBaseControl(name, audio_rate, smooth_value);

    Processor* mono_total = nullptr;
    if (audio_rate)
      mono_total = new ModulationSum();
    else
      mono_total = new cr::VariableAdd();

    mono_total->plugNext(base_val);
    addMonoProcessor(mono_total, false);
    data_->mono_mod_destinations[name] = mono_total;
    data_->mono_modulation_readout[name] = mono_total->output();

    ValueSwitch* control_switch = new ValueSwitch(0.0f);
    control_switch->plugNext(base_val);
    control_switch->plugNext(mono_total);

    if (internal_modulation)
      mono_total->plugNext(internal_modulation);
    else
      control_switch->addProcessor(mono_total);

    addIdleMonoProcessor(control_switch);
    if (smooth_value || internal_modulation)
      control_switch->set(1);
    else
      control_switch->set(0);
    data_->mono_modulation_switches[name] = control_switch;

    return control_switch->output(ValueSwitch::kSwitch);
  }

  Output* SynthModule::createMonoModControl(std::string name, bool audio_rate,
                                            bool smooth_value, Output* internal_modulation) {
    ValueDetails details = Parameters::getDetails(name);
    Output* control_rate_total = createBaseModControl(name, audio_rate, smooth_value, internal_modulation);
    if (audio_rate)
      return control_rate_total;

    if (details.value_scale == ValueDetails::kQuadratic) {
      Processor* scale = nullptr;
      if (details.post_offset)
        scale = new cr::Quadratic(details.post_offset);
      else
        scale = new cr::Square();

      scale->plug(control_rate_total);
      addMonoProcessor(scale);
      control_rate_total = scale->output();
    }
    else if (details.value_scale == ValueDetails::kCubic) {
      Processor* scale = nullptr;
      VITAL_ASSERT(details.post_offset == 0.0f);
      if (details.post_offset)
        scale = new cr::Cubic(details.post_offset);
      else
        scale = new cr::Cube();

      scale->plug(control_rate_total);
      addMonoProcessor(scale);
      control_rate_total = scale->output();
    }
    else if (details.value_scale == ValueDetails::kQuartic) {
      Processor* scale = nullptr;
      VITAL_ASSERT(details.post_offset == 0.0f);
      if (details.post_offset)
        scale = new cr::Quartic(details.post_offset);
      else
        scale = new cr::Quart();

      scale->plug(control_rate_total);
      addMonoProcessor(scale);
      control_rate_total = scale->output();
    }
    else if (details.value_scale == ValueDetails::kExponential) {
      cr::ExponentialScale* exponential = new cr::ExponentialScale(details.min, details.max, 2.0f);
      exponential->plug(control_rate_total);
      addMonoProcessor(exponential);
      control_rate_total = exponential->output();
    }
    else if (details.value_scale == ValueDetails::kSquareRoot) {
      cr::Root* root = new cr::Root(details.post_offset);
      root->plug(control_rate_total);
      addMonoProcessor(root);
      control_rate_total = root->output();
    }

    return control_rate_total;
  }

  Output* SynthModule::createPolyModControl(std::string name, bool audio_rate,
                                            bool smooth_value, Output* internal_modulation, Input* reset) {
    ValueDetails details = Parameters::getDetails(name);
    Output* base_control = createBaseModControl(name, audio_rate, smooth_value);

    Processor* poly_total;
    if (audio_rate) {
      poly_total = new ModulationSum();
      if (reset)
        poly_total->useInput(reset, ModulationSum::kReset);
    }
    else
      poly_total = new cr::VariableAdd();
    addProcessor(poly_total);
    data_->poly_mod_destinations[name] = poly_total;

    Processor* modulation_total;
    if (audio_rate)
      modulation_total = new Add();
    else
      modulation_total = new cr::Add();

    modulation_total->plug(base_control, 0);
    modulation_total->plug(poly_total, 1);
    addProcessor(modulation_total);

    data_->poly_modulation_readout[name] = poly_total->output();

    ValueSwitch* control_switch = new ValueSwitch(0.0f);
    control_switch->plugNext(base_control);
    control_switch->plugNext(modulation_total);

    if (internal_modulation) {
      poly_total->plugNext(internal_modulation);
      control_switch->set(1);
    }
    else {
      control_switch->addProcessor(poly_total);
      control_switch->addProcessor(modulation_total);
      control_switch->set(0);
    }
    addIdleProcessor(control_switch);
    data_->poly_modulation_switches[name] = control_switch;

    Output* control_rate_total = control_switch->output(ValueSwitch::kSwitch);
    if (audio_rate)
      return control_rate_total;

    if (details.value_scale == ValueDetails::kQuadratic) {
      Processor* scale = nullptr;
      if (details.post_offset)
        scale = new cr::Quadratic(details.post_offset);
      else
        scale = new cr::Square();

      scale->plug(control_rate_total);
      addProcessor(scale);
      control_rate_total = scale->output();
    }
    else if (details.value_scale == ValueDetails::kCubic) {
      Processor* scale = nullptr;
      VITAL_ASSERT(details.post_offset == 0.0f);
      if (details.post_offset)
        scale = new cr::Cubic(details.post_offset);
      else
        scale = new cr::Cube();

      scale->plug(control_rate_total);
      addProcessor(scale);
      control_rate_total = scale->output();
    }
    else if (details.value_scale == ValueDetails::kQuartic) {
      Processor* scale = nullptr;
      VITAL_ASSERT(details.post_offset == 0.0f);
      if (details.post_offset)
        scale = new cr::Quartic(details.post_offset);
      else
        scale = new cr::Quart();

      scale->plug(control_rate_total);
      addProcessor(scale);
      control_rate_total = scale->output();
    }
    else if (details.value_scale == ValueDetails::kExponential) {      
      Processor* exponential = new cr::ExponentialScale(details.min, details.max, 2.0f, details.post_offset);
      exponential->plug(control_rate_total);
      addProcessor(exponential);
      control_rate_total = exponential->output();
    }
    else if (details.value_scale == ValueDetails::kSquareRoot) {      
      cr::Root* root = new cr::Root(details.post_offset);
      root->plug(control_rate_total);
      addProcessor(root);
      control_rate_total = root->output();
    }

    return control_rate_total;
  }

  Output* SynthModule::createAudioRateExponentialPolyModControl(std::string name, Input* reset) {
    const ValueDetails details = Parameters::getDetails(name);
    VITAL_ASSERT(details.value_scale == ValueDetails::kExponential);

    Output* exponent = createPolyModControl(name, true, false, nullptr, reset);
    ExponentialScale* exponential = new ExponentialScale(details.min, details.max, 2.0f);
    exponential->plug(exponent);
    addProcessor(exponential);
    return exponential->output();
  }

  Output* SynthModule::createTempoSyncSwitch(std::string name, Processor* frequency,
                                             const Output* beats_per_second, bool poly, Input* midi) {
    Output* tempo = nullptr;
    if (poly)
      tempo = createPolyModControl(name + "_tempo");
    else
      tempo = createMonoModControl(name + "_tempo");

    Value* sync;
    if (prepared_effect_inputs) {
      sync = createBaseControl(name + "_sync");
      sync->set(1.0f);
    }
    else {
      sync = new cr::Value(1.0f);
      data_->controls[name + "_sync"] = sync;
      addIdleProcessor(sync);
    }

    TempoChooser* tempo_chooser = new TempoChooser(!frequency->isControlRate());
    tempo_chooser->plug(sync, TempoChooser::kSync);
    tempo_chooser->plug(tempo, TempoChooser::kTempoIndex);
    tempo_chooser->plug(frequency, TempoChooser::kFrequency);
    tempo_chooser->plug(beats_per_second, TempoChooser::kBeatsPerSecond);

    if (midi) {
      Output* keytrack_transpose = nullptr;
      Output* keytrack_tune = nullptr;
      if (poly) {
        keytrack_transpose = createPolyModControl(name + "_keytrack_transpose");
        keytrack_tune = createPolyModControl(name + "_keytrack_tune");
      }
      else {
        keytrack_transpose = createMonoModControl(name + "_keytrack_transpose");
        keytrack_tune = createMonoModControl(name + "_keytrack_tune");
      }

      tempo_chooser->plug(keytrack_transpose, TempoChooser::kKeytrackTranspose);
      tempo_chooser->plug(keytrack_tune, TempoChooser::kKeytrackTune);
      tempo_chooser->useInput(midi, TempoChooser::kMidi);
    }

    if (poly)
      addProcessor(tempo_chooser);
    else
      addMonoProcessor(tempo_chooser);

    return tempo_chooser->output();
  }

  void SynthModule::createStatusOutput(std::string name, Output* source) {
    data_->status_outputs[name] = std::make_unique<StatusOutput>(source);
  }

  control_map SynthModule::getControls() {
    control_map all_controls = data_->controls;
    for (SynthModule* sub_module : data_->sub_modules) {
      control_map sub_controls = sub_module->getControls();
      all_controls.insert(sub_controls.begin(), sub_controls.end());
    }

    return all_controls;
  }

  Output* SynthModule::getModulationSource(std::string name) {
    if (data_->mod_sources.count(name))
      return data_->mod_sources[name];

    for (SynthModule* sub_module : data_->sub_modules) {
      Output* source = sub_module->getModulationSource(name);
      if (source)
        return source;
    }

    return nullptr;
  }

  const StatusOutput* SynthModule::getStatusOutput(std::string name) const {
    if (data_->status_outputs.count(name))
      return data_->status_outputs.at(name).get();

    for (SynthModule* sub_module : data_->sub_modules) {
      const StatusOutput* source = sub_module->getStatusOutput(name);
      if (source)
        return source;
    }

    return nullptr;
  }

  Processor* SynthModule::getModulationDestination(std::string name, bool poly) {
    Processor* poly_destination = getPolyModulationDestination(name);

    if (poly && poly_destination)
      return poly_destination;

    return getMonoModulationDestination(name);
  }

  Processor* SynthModule::getMonoModulationDestination(std::string name) {
    if (data_->mono_mod_destinations.count(name))
      return data_->mono_mod_destinations[name];

    for (SynthModule* sub_module : data_->sub_modules) {
      Processor* destination = sub_module->getMonoModulationDestination(name);
      if (destination)
        return destination;
    }

    return nullptr;
  }

  Processor* SynthModule::getPolyModulationDestination(std::string name) {
    if (data_->poly_mod_destinations.count(name))
      return data_->poly_mod_destinations[name];

    for (SynthModule* sub_module : data_->sub_modules) {
      Processor* destination = sub_module->getPolyModulationDestination(name);
      if (destination)
        return destination;
    }

    return nullptr;
  }

  ValueSwitch* SynthModule::getModulationSwitch(std::string name, bool poly) {
    if (poly)
      return getPolyModulationSwitch(name);
    return getMonoModulationSwitch(name);
  }

  ValueSwitch* SynthModule::getMonoModulationSwitch(std::string name) {
    if (data_->mono_modulation_switches.count(name))
      return data_->mono_modulation_switches[name];

    for (SynthModule* sub_module : data_->sub_modules) {
      ValueSwitch* value_switch = sub_module->getMonoModulationSwitch(name);
      if (value_switch)
        return value_switch;
    }

    return nullptr;
  }

  ValueSwitch* SynthModule::getPolyModulationSwitch(std::string name) {
    if (data_->poly_modulation_switches.count(name))
      return data_->poly_modulation_switches[name];

    for (SynthModule* sub_module : data_->sub_modules) {
      ValueSwitch* value_switch = sub_module->getPolyModulationSwitch(name);
      if (value_switch)
        return value_switch;
    }

    return nullptr;
  }

  void SynthModule::updateAllModulationSwitches() {
    for (auto& mod_switch : data_->mono_modulation_switches) {
      bool enable = data_->mono_mod_destinations[mod_switch.first]->connectedInputs() > 1;
      if (data_->poly_mod_destinations.count(mod_switch.first))
        enable = enable || data_->poly_mod_destinations[mod_switch.first]->connectedInputs() > 0;
      mod_switch.second->set(enable ? 1.0f : 0.0f);
    }

    for (auto& mod_switch : data_->poly_modulation_switches)
      mod_switch.second->set(data_->poly_mod_destinations[mod_switch.first]->connectedInputs() > 0);

    for (SynthModule* sub_module : data_->sub_modules)
      sub_module->updateAllModulationSwitches();
  }

  output_map& SynthModule::getModulationSources() {
    output_map& all_sources = data_->mod_sources;
    for (SynthModule* sub_module : data_->sub_modules) {
      output_map& sub_sources = sub_module->getModulationSources();
      all_sources.insert(sub_sources.begin(), sub_sources.end());
    }

    return all_sources;
  }

  input_map& SynthModule::getMonoModulationDestinations() {
    input_map& all_destinations = data_->mono_mod_destinations;
    for (SynthModule* sub_module : data_->sub_modules) {
      input_map& sub_destinations = sub_module->getMonoModulationDestinations();
      all_destinations.insert(sub_destinations.begin(), sub_destinations.end());
    }

    return all_destinations;
  }

  input_map& SynthModule::getPolyModulationDestinations() {
    input_map& all_destinations = data_->poly_mod_destinations;
    for (SynthModule* sub_module : data_->sub_modules) {
      input_map& sub_destinations = sub_module->getPolyModulationDestinations();
      all_destinations.insert(sub_destinations.begin(), sub_destinations.end());
    }

    return all_destinations;
  }

  output_map& SynthModule::getMonoModulations() {
    output_map& all_readouts = data_->mono_modulation_readout;
    for (SynthModule* sub_module : data_->sub_modules) {
      output_map& sub_readouts = sub_module->getMonoModulations();
      all_readouts.insert(sub_readouts.begin(), sub_readouts.end());
    }

    return all_readouts;
  }

  output_map& SynthModule::getPolyModulations() {
    output_map& all_readouts = data_->poly_modulation_readout;
    for (SynthModule* sub_module : data_->sub_modules) {
      output_map& sub_readouts = sub_module->getPolyModulations();
      all_readouts.insert(sub_readouts.begin(), sub_readouts.end());
    }

    return all_readouts;
  }

  void SynthModule::enableOwnedProcessors(bool enable) {
    for (Processor* processor : data_->owned_mono_processors)
      processor->enable(enable);
    for (SynthModule* sub_module : data_->sub_modules)
      sub_module->enable(enable);
  }

  void SynthModule::enable(bool enable) {
    if (enabled() == enable)
      return;
      
    ProcessorRouter::enable(enable);
    enableOwnedProcessors(enable);
  }

  void SynthModule::addMonoProcessor(Processor* processor, bool own) {
    getMonoRouter()->addProcessor(processor);
    data_->mono_router_processors.push_back(processor);
    if (own)
      data_->owned_mono_processors.push_back(processor);
  }

  void SynthModule::addIdleMonoProcessor(Processor* processor) {
    getMonoRouter()->addIdleProcessor(processor);
    data_->idle_mono_processors.push_back(processor);
  }

  void SynthModule::detachMonoProcessors() {
    for (SynthModule* sub_module : data_->sub_modules)
      sub_module->detachMonoProcessors();
    ProcessorRouter* mono_router = getMonoRouter();
    for (Processor* processor : data_->mono_router_processors) {
      if (processor->router() == mono_router) {
        // removeProcessor hands ownership back to the caller.
        mono_router->removeProcessor(processor);
        delete processor;
      }
    }
    for (Processor* processor : data_->idle_mono_processors) {
      if (processor->router() == mono_router)
        mono_router->removeIdleProcessor(processor);
    }
    data_->mono_router_processors.clear();
    data_->owned_mono_processors.clear();
    data_->idle_mono_processors.clear();
  }

  void SynthModule::removeSubmodule(SynthModule* module) {
    auto& sub_modules = data_->sub_modules;
    sub_modules.erase(std::remove(sub_modules.begin(), sub_modules.end(), module), sub_modules.end());
  }
} // namespace vital
