#include "JuceHeader.h"
#include "slot_test_ids.h"
#include "fx_automation_parameter.h"
#include "synth_plugin.h"
#include "synth_editor.h"
#include "parameter_slider.h"
#include "sound_engine.h"
#include "delay.h"
#include "tape_controls.h"

#include <cmath>
#include <iostream>
#include <map>
#include <set>
#include <string>

namespace {
  int fail(const std::string& message) {
    std::cerr << "FX automation test failed: " << message << '\n';
    return 1;
  }

  // A modulatable effect parameter is a modulation destination once a route
  // asks for it: an effect its slot does not select builds the wiring then,
  // as connectModulation and preset loading do.
  bool isModulationDestination(SynthPlugin& synth, const std::string& id) {
    synth.prepareModulationDestination(id);
    return synth.getEngine()->getMonoModulationDestination(id) != nullptr;
  }

  std::string firstAutomatableControl(SynthPlugin& synth, const std::string& prefix) {
    for (const auto& control : synth.getControls()) {
      if (control.second != nullptr && control.first.rfind(prefix, 0) == 0 &&
          control.first != prefix + "type" && vital::Parameters::isParameter(control.first))
        return control.first;
    }
    return {};
  }

  FxAutomationParameter* proxyNamed(SynthPlugin& synth, const String& name) {
    for (auto* parameter : synth.getParameters()) {
      if (parameter != nullptr && parameter->getName(128) == name)
        return dynamic_cast<FxAutomationParameter*>(parameter);
    }
    return nullptr;
  }

  float engineValueForNormalized(const vital::ValueDetails& details, float normalized) {
    float span = details.max - details.min;
    if (details.value_scale == vital::ValueDetails::kIndexed)
      span = std::round(span);
    float value = normalized * span + details.min;
    if (details.value_scale == vital::ValueDetails::kIndexed)
      value = std::round(value);
    return jlimit(details.min, details.max, value);
  }

  Component* componentWithTitle(Component& parent, const String& title) {
    if (parent.getTitle() == title)
      return &parent;
    for (auto* child : parent.getChildren()) {
      if (auto* found = componentWithTitle(*child, title))
        return found;
    }
    return nullptr;
  }

  bool selectEditorItem(Component& editor, const String& title, const String& text) {
    auto* selector = dynamic_cast<ComboBox*>(componentWithTitle(editor, title));
    if (selector == nullptr)
      return false;
    for (int i = 0; i < selector->getNumItems(); ++i) {
      if (selector->getItemText(i) == text) {
        selector->setSelectedItemIndex(i, sendNotificationSync);
        return true;
      }
    }
    return false;
  }

  bool continuousKeyboardControls(Component& parent) {
    for (auto* child : parent.getChildren()) {
      if (auto* slider = dynamic_cast<ParameterSlider*>(child); slider != nullptr && slider->getInterval() == 0.0) {
        const double saved = slider->getValue();
        const double min = slider->getMinimum();
        const double range = slider->getMaximum() - min;
        slider->setValue(min + range * 0.4, sendNotificationSync);
        double expected = slider->adjustmentValue(slider->getValue());
        const double step = slider->getAdjustmentStep();
        const auto check = [&](const KeyPress& key, double delta) {
          expected += step * delta;
          return slider->keyPressed(key) &&
                 std::abs(slider->adjustmentValue(slider->getValue()) - expected) < step * 0.001;
        };
        if (!check(KeyPress(KeyPress::rightKey), 1.0) ||
            !check(KeyPress(KeyPress::upKey, ModifierKeys::shiftModifier, 0), 0.1) ||
            !check(KeyPress(KeyPress::pageUpKey), 10.0) ||
            !check(KeyPress(KeyPress::pageDownKey), -10.0) ||
            !check(KeyPress(KeyPress::leftKey, ModifierKeys::shiftModifier, 0), -0.1) ||
            !check(KeyPress(KeyPress::downKey), -1.0) ||
            !slider->keyPressed(KeyPress(KeyPress::homeKey)) || slider->getValue() != slider->getMaximum() ||
            !slider->keyPressed(KeyPress(KeyPress::endKey)) || slider->getValue() != slider->getMinimum()) {
          std::cerr << "Inconsistent slider keyboard control: " << slider->getTitle() << '\n';
          return false;
        }
        auto handler = slider->createAccessibilityHandler();
        auto* value = handler ? handler->getValueInterface() : nullptr;
        if (value == nullptr || std::abs(value->getRange().getInterval() - step) > step * 0.001)
          return false;
        slider->setValue(saved, sendNotificationSync);
      }
      else if (!continuousKeyboardControls(*child))
        return false;
    }
    return true;
  }

  bool frequencyKeyboardControls(Component* component) {
    auto* slider = dynamic_cast<ParameterSlider*>(component);
    if (slider == nullptr || !slider->toAdjustmentValue || !slider->fromAdjustmentValue)
      return false;
    const double saved = slider->getValue();
    auto handler = slider->createAccessibilityHandler();
    auto* value = handler ? handler->getValueInterface() : nullptr;
    if (value == nullptr || value->getRange().getInterval() <= 0.0)
      return false;
    // The test uses the user's display preference, without changing settings.
    // Hz is the preference used for the reported issue. Semitone mode keeps
    // the established normalized increments and still has a nonzero AX step.
    if (value->getRange().getMaximumValue() <= 1.0)
      return true;
    const auto closeTo = [&](double hz) {
      return std::abs(value->getCurrentValue() - hz) < 0.02 &&
             std::abs(slider->getTextFromValue(slider->getValue()).getDoubleValue() - hz) < 0.6;
    };
    for (double start : { 200.0, 500.0, 1200.0, 20400.0 }) {
      value->setValueAsString(String(start) + " Hz");
      if (!closeTo(start)) return false;
      slider->keyPressed(KeyPress(KeyPress::rightKey));
      if (!closeTo(start + 10.0)) return false;
      slider->keyPressed(KeyPress(KeyPress::leftKey));
      if (!closeTo(start)) return false;
      slider->keyPressed(KeyPress(KeyPress::upKey, ModifierKeys::shiftModifier, 0));
      if (!closeTo(start + 1.0)) return false;
      slider->keyPressed(KeyPress(KeyPress::pageUpKey));
      if (!closeTo(start + 101.0)) return false;
      slider->keyPressed(KeyPress(KeyPress::pageDownKey));
      if (!closeTo(start + 1.0)) return false;
      // Same call sequence as JUCE's macOS accessibility increment/decrement.
      value->setValue(value->getCurrentValue() + value->getRange().getInterval());
      if (!closeTo(start + 11.0)) return false;
      value->setValue(value->getCurrentValue() - value->getRange().getInterval());
      if (!closeTo(start + 1.0)) return false;
    }
    slider->keyPressed(KeyPress(KeyPress::homeKey));
    slider->keyPressed(KeyPress(KeyPress::rightKey));
    if (slider->getValue() != slider->getMaximum()) return false;
    slider->keyPressed(KeyPress(KeyPress::endKey));
    slider->keyPressed(KeyPress(KeyPress::leftKey));
    if (slider->getValue() != slider->getMinimum()) return false;
    slider->setValue(saved, sendNotificationSync);
    return true;
  }

  bool controlsInOrder(Component& parent, std::initializer_list<const char*> titles) {
    int previous_y = -1;
    for (const auto* title : titles) {
      auto* control = componentWithTitle(parent, title);
      if (control == nullptr)
        return false;
      const int y = parent.getLocalArea(control, control->getLocalBounds()).getY();
      if (y <= previous_y)
        return false;
      previous_y = y;
    }
    return true;
  }

  Component* effectSlotComponent(Component& parent, const String& section, int slot = 19) {
    if (parent.getProperties()["SectionName"].toString() == section &&
        parent.getProperties().contains("FxSlot") && static_cast<int>(parent.getProperties()["FxSlot"]) == slot)
      return &parent;
    for (auto* child : parent.getChildren()) {
      if (auto* found = effectSlotComponent(*child, section, slot))
        return found;
    }
    return nullptr;
  }
}

#include "fx_bus_preset_tests.h"
#include "spectral_lock_ui_tests.h"
#include "resonator_ui_tests.h"

int main() {
  ScopedJuceInitialiser_GUI juce;
  if (const int result = testFxBusPresets())
    return result;
  SynthPlugin synth;

  std::map<std::string, String> original_host_ids;
  int original_host_index = 0;
  bool reached_spectral_gate_parameters = false;
  for (int i = 0; i < vital::Parameters::getNumParameters(); ++i) {
    const auto* details = vital::Parameters::getDetails(i);
    const bool added_for_gate = details->name.find("spectral_gate_") != std::string::npos ||
                               String(details->name).endsWith("effect_chain_slot_18");
    reached_spectral_gate_parameters |= added_for_gate;
    if (reached_spectral_gate_parameters && !added_for_gate && details->version_added <= 0x020101)
      return fail("new spectral gate parameters shifted an existing host parameter index");
    // Legacy fixed-source names are aliases for oscillator slot controls and keep
    // their number; the slot's own later entry must not take it over.
    std::string engine_name = details->name;
    if (synth.getControls().count(engine_name) == 0)
      engine_name = SynthPlugin::legacyAliasTarget(engine_name);
    if (!engine_name.empty() && synth.getControls().count(engine_name) != 0)
      original_host_ids.emplace(engine_name, String(original_host_index++));
  }
  std::set<int> original_vst3_hashes;
  for (int index = 0; index < original_host_index; ++index)
    original_vst3_hashes.insert(String(index).hashCode() & 0x7fffffff);

  int proxy_count = 0;
  std::set<int> vst3_parameter_hashes;
  for (auto* parameter : synth.getParameters()) {
    const auto* hosted_parameter = dynamic_cast<const HostedAudioProcessorParameter*>(parameter);
    if (hosted_parameter == nullptr)
      return fail("a host parameter has no stable parameter ID");
    const int vst3_hash = hosted_parameter->getParameterID().hashCode() & 0x7fffffff;
    if (!vst3_parameter_hashes.insert(vst3_hash).second)
      return fail("two host parameter IDs collide in VST3");

    if (dynamic_cast<FxAutomationParameter*>(parameter) != nullptr) {
      if (original_vst3_hashes.count(vst3_hash) != 0)
        return fail("a new FX proxy collides with an old host parameter ID");
      ++proxy_count;
    }

    if (auto* bridge = dynamic_cast<ValueBridge*>(parameter)) {
      const String id = bridge->getParameterId();
      const auto expected_id = original_host_ids.find(id.toStdString());
      const auto* hosted = dynamic_cast<const HostedAudioProcessorParameter*>(bridge);
      if (expected_id == original_host_ids.end() || hosted == nullptr ||
          hosted->getParameterID() != expected_id->second)
        return fail("a surviving parameter did not retain its original host ID: " + id.toStdString() +
                    " expected " + (expected_id == original_host_ids.end() ? String("<none>") : expected_id->second).toStdString() +
                    " actual " + (hosted ? hosted->getParameterID().toStdString() : std::string("<none>")));
      if (id == "effect_chain_order" || id == "bus_1_effect_chain_order" ||
          id == "delay_mix" || id == "bus_1_delay_mix")
        return fail("an internal FX parameter is still exposed to the host: " + id.toStdString());
    }
  }

  const int expected_proxies = SynthPlugin::kNumFxAutomationChains * SynthPlugin::kFxAutomationSlotsPerChain +
                               SynthPlugin::kNumOscAutomationChains * SynthPlugin::kOscAutomationSlotsPerOscillator;
  if (proxy_count != expected_proxies)
    return fail("expected exactly " + std::to_string(expected_proxies) + " automation proxy parameters (FX chains and oscillator slots), found " + std::to_string(proxy_count));

  for (int chain = 0; chain < SynthPlugin::kNumFxAutomationChains; ++chain) {
    const String chain_name = chain == 0 ? String("Main") : "Bus " + String(chain);
    for (int slot = 0; slot < SynthPlugin::kFxAutomationSlotsPerChain; ++slot) {
      if (proxyNamed(synth, "FX " + chain_name + " Param " + String(slot + 1)) == nullptr)
        return fail("missing a stable proxy in " + chain_name.toStdString());
    }
  }

  const std::string main_target = firstAutomatableControl(synth, "fx_slot_1_");
  if (main_target.empty())
    return fail("no main-chain dynamic FX target was found");
  if (synth.assignFxParameterToAutomationSlot(main_target) != 0)
    return fail("the first main-chain target did not use slot 1");
  if (synth.fxAutomationSlotNameForParameter(main_target) != "FX Main Param 1")
    return fail("main-chain assignment name is incorrect");

  auto* main_proxy = proxyNamed(synth, "FX Main Param 1");
  if (main_proxy == nullptr)
    return fail("main proxy cannot be found");
  constexpr float kHostValue = 0.73f;
  main_proxy->setValue(kHostValue);
  const auto& main_details = vital::Parameters::getDetails(main_target);
  const float expected_engine_value = engineValueForNormalized(main_details, kHostValue);
  if (std::abs(synth.getControls()[main_target]->value() - expected_engine_value) > 1.0e-4f)
    return fail("host automation was not forwarded to the internal FX control");

  constexpr float kInternalNormalized = 0.29f;
  const float internal_engine_value = engineValueForNormalized(main_details, kInternalNormalized);
  synth.parameterChanged(main_target, internal_engine_value);
  if (std::abs(main_proxy->getValue() - kInternalNormalized) > 1.0e-4f &&
      main_details.value_scale != vital::ValueDetails::kIndexed)
    return fail("an internal FX edit was not reflected by its proxy");

  MemoryBlock state;
  synth.getStateInformation(state);
  SynthPlugin restored;
  restored.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
  if (restored.fxAutomationSlotNameForParameter(main_target) != "FX Main Param 1")
    return fail("the proxy assignment did not survive state reload");
  if (std::abs(restored.getControls()[main_target]->value() - internal_engine_value) > 1.0e-4f)
    return fail("the automated target value did not survive state reload");

  const std::string second_target = "fx_slot_2_" + main_target.substr(std::string("fx_slot_1_").size());
  restored.swapFxAutomationPrefixes("fx_slot_1_", "fx_slot_2_");
  if (restored.fxAutomationSlotNameForParameter(main_target).isNotEmpty() ||
      restored.fxAutomationSlotNameForParameter(second_target) != "FX Main Param 1")
    return fail("moving an effect did not move its automation assignment");
  restored.clearFxAutomationForPrefix("fx_slot_2_");
  if (restored.fxAutomationSlotNameForParameter(second_target).isNotEmpty())
    return fail("removing an effect did not free its automation assignment");

  const std::string bus_target = firstAutomatableControl(restored, "bus_1_fx_slot_1_");
  if (bus_target.empty() || restored.assignFxParameterToAutomationSlot(bus_target) != 0 ||
      restored.fxAutomationSlotNameForParameter(bus_target) != "FX Bus 1 Param 1")
    return fail("bus automation was not assigned independently");

  // Spectral Gate must participate in the same slot state, modulation, and
  // proxy machinery on the main chain and every bus, including the last slot.
  synth.clearFxAutomationForPrefix("fx_slot_");
  for (int chain = 0; chain <= vital::kNumBuses; ++chain) {
    const std::string prefix = (chain == 0 ? "" : "bus_" + std::to_string(chain) + "_") +
                               std::string("fx_slot_20_");
    for (const auto* suffix : { "cutoff", "balance", "tilt", "mix", "tilt_enabled" }) {
      const auto id = prefix + "spectral_gate_" + suffix;
      if (synth.getControls().count(id) == 0 || !vital::Parameters::isParameter(id))
        return fail("missing spectral gate control: " + id);
    }
    synth.parameterChanged(prefix + "type", vital::constants::kSpectralGate + 1);
    synth.parameterChanged(prefix + "enabled", 1.0f);
    // An effect's parameters become modulation destinations once its slot selects it.
    for (const auto* suffix : { "cutoff", "balance", "tilt", "mix" }) {
      const auto id = prefix + "spectral_gate_" + suffix;
      if (!isModulationDestination(synth, id))
        return fail("spectral gate parameter is not a modulation destination: " + id);
    }
    if (synth.assignFxParameterToAutomationSlot(prefix + "spectral_gate_tilt") != 0)
      return fail("spectral gate could not use a chain's first automation proxy");
    auto* proxy = proxyNamed(synth, "FX " + (chain == 0 ? String("Main") : "Bus " + String(chain)) + " Param 1");
    proxy->setValue(0.25f);
    if (std::abs(synth.getControls().at(prefix + "spectral_gate_tilt")->value() + 0.5f) > 1.0e-6f)
      return fail("spectral gate bipolar tilt did not accept host automation");
  }
  MemoryBlock gate_state;
  synth.getStateInformation(gate_state);
  restored.setStateInformation(gate_state.getData(), static_cast<int>(gate_state.getSize()));
  for (int chain = 0; chain <= vital::kNumBuses; ++chain) {
    const std::string prefix = (chain == 0 ? "" : "bus_" + std::to_string(chain) + "_") +
                               std::string("fx_slot_20_");
    if (restored.getControls().at(prefix + "type")->value() != vital::constants::kSpectralGate + 1 ||
        std::abs(restored.getControls().at(prefix + "spectral_gate_tilt")->value() + 0.5f) > 1.0e-6f ||
        restored.fxAutomationSlotNameForParameter(prefix + "spectral_gate_tilt").isEmpty())
      return fail("spectral gate slot or automation did not survive state reload: " + prefix +
                  " type=" + std::to_string(restored.getControls().at(prefix + "type")->value()) +
                  " tilt=" + std::to_string(restored.getControls().at(prefix + "spectral_gate_tilt")->value()) +
                  " assignment=" + restored.fxAutomationSlotNameForParameter(prefix + "spectral_gate_tilt").toStdString());
  }

  // Exercise the actual editor's slot rows and keyboard controls, not just its
  // parameter metadata. No native window or screen reader is required here.
  {
    SynthEditor editor(restored);
    for (const String group : { String("Effects"), String("Bus 1"), String("Bus 2"), String("Bus 3") }) {
      if (!selectEditorItem(editor, "Editor group", group) ||
          !selectEditorItem(editor, "Editor element", "Chain and routing"))
        return fail("spectral gate effects navigation is missing for " + group.toStdString());
      auto* slot_component = effectSlotComponent(editor, group == "Effects" ? String("Chain and routing") :
                                                                   group + " - Chain and routing");
      if (slot_component == nullptr)
        return fail("spectral gate slot component is missing for " + group.toStdString());
      for (const char* title : { "Cutoff", "Strong Balance", "Enable Tilt", "Tilt", "Mix" }) {
        auto* control = componentWithTitle(*slot_component, title);
        if (control == nullptr || !control->isAccessible() || control->createAccessibilityHandler() == nullptr)
          return fail("spectral gate accessible control is missing: " + std::string(title));
      }
      auto* cutoff = componentWithTitle(*slot_component, "Cutoff");
      const int chain = group == "Effects" ? 0 : group.getTrailingIntValue();
      const std::string id = (chain == 0 ? "" : "bus_" + std::to_string(chain) + "_") +
                            std::string("fx_slot_20_spectral_gate_cutoff");
      const float before = restored.getControls().at(id)->value();
      if (!cutoff->keyPressed(KeyPress(KeyPress::rightKey)) ||
          restored.getControls().at(id)->value() <= before)
        return fail("spectral gate Cutoff cannot be increased with the keyboard");
    }
  }

  // New JSFX effects: independent saved controls and stable proxy assignments
  // on all chains, including a named two-mode Dimension selector.
  for (int chain = 0; chain <= vital::kNumBuses; ++chain) {
    const std::string prefix = chain == 0 ? "" : "bus_" + std::to_string(chain) + "_";
    synth.clearFxAutomationForPrefix(prefix + "fx_slot_");
    const std::string dimension = prefix + "fx_slot_18_";
    const std::string erosion = prefix + "fx_slot_19_";
    const std::string floaty = prefix + "fx_slot_20_";
    synth.parameterChanged(dimension + "type", vital::constants::kDimensionExpander + 1);
    synth.parameterChanged(erosion + "type", vital::constants::kErosion + 1);
    synth.parameterChanged(floaty + "type", vital::constants::kFloatyDelay + 1);
    synth.parameterChanged(dimension + "dimension_expander_amount", 0.28f);
    synth.parameterChanged(dimension + "dimension_expander_quorax_size", 65.0f);
    synth.parameterChanged(dimension + "dimension_expander_quorax_low_cut", 340.0f);
    synth.parameterChanged(dimension + "dimension_expander_quorax_high_cut", 8000.0f);
    synth.parameterChanged(dimension + "dimension_expander_quorax_slope", 3.0f);
    synth.parameterChanged(erosion + "erosion_frequency", 6300.0f);
    synth.parameterChanged(floaty + "floaty_delay_sync", 17.0f);
    if (synth.assignFxParameterToAutomationSlot(dimension + "dimension_expander_mode") != 0 ||
        synth.assignFxParameterToAutomationSlot(erosion + "erosion_amount") != 1 ||
        synth.assignFxParameterToAutomationSlot(floaty + "floaty_delay_playback_rate") != 2)
      return fail("new JSFX parameters could not be assigned to automation proxies");
    const String proxy_prefix = "FX " + (chain == 0 ? String("Main") : "Bus " + String(chain)) + " Param ";
    auto* mode_proxy = proxyNamed(synth, proxy_prefix + "1");
    if (mode_proxy->getText(0, 100) != "Mode 1" || mode_proxy->getText(1, 100) != "Mode 2")
      return fail("Dimension mode proxy lost its named choices");
    mode_proxy->setValue(1);
    proxyNamed(synth, proxy_prefix + "2")->setValue(0.63f);
    proxyNamed(synth, proxy_prefix + "3")->setValue(0.125f);
    for (const auto& id : { dimension + "dimension_expander_quorax_size", erosion + "erosion_frequency",
                             floaty + "floaty_delay_warp_amount" })
      if (!isModulationDestination(synth, id))
        return fail("new JSFX modulation destination is missing: " + id);
  }
  MemoryBlock jsfx_state;
  synth.getStateInformation(jsfx_state);
  restored.setStateInformation(jsfx_state.getData(), static_cast<int>(jsfx_state.getSize()));
  for (int chain = 0; chain <= vital::kNumBuses; ++chain) {
    const std::string prefix = chain == 0 ? "" : "bus_" + std::to_string(chain) + "_";
    const auto& controls = restored.getControls();
    if (controls.at(prefix + "fx_slot_18_dimension_expander_mode")->value() != 1 ||
        std::abs(controls.at(prefix + "fx_slot_18_dimension_expander_amount")->value() - 0.28f) > 1.0e-6f ||
        controls.at(prefix + "fx_slot_18_dimension_expander_quorax_size")->value() != 65 ||
        controls.at(prefix + "fx_slot_18_dimension_expander_quorax_slope")->value() != 3 ||
        std::abs(controls.at(prefix + "fx_slot_19_erosion_amount")->value() - 126) > 1.0e-4f ||
        controls.at(prefix + "fx_slot_20_floaty_delay_playback_rate")->value() != -1.5f ||
        controls.at(prefix + "fx_slot_20_floaty_delay_sync")->value() != 17 ||
        restored.fxAutomationSlotNameForParameter(prefix + "fx_slot_20_floaty_delay_playback_rate").isEmpty())
      return fail("new JSFX state or automation assignment did not survive reload");
  }
  {
    SynthEditor editor(restored);
    for (int chain = 0; chain <= vital::kNumBuses; ++chain) {
      const String group = chain == 0 ? String("Effects") : "Bus " + String(chain);
      const String section = chain == 0 ? String("Chain and routing") : group + " - Chain and routing";
      if (!selectEditorItem(editor, "Editor group", group) || !selectEditorItem(editor, "Editor element", "Chain and routing"))
        return fail("new JSFX effect navigation is missing");
      auto* dimension = effectSlotComponent(editor, section, 17);
      auto* erosion = effectSlotComponent(editor, section, 18);
      auto* floaty = effectSlotComponent(editor, section, 19);
      if (!dimension || !erosion || !floaty) return fail("new JSFX effect slots are missing from the editor");
      for (const char* title : { "Mode", "Size", "Mix", "Output", "Motion Rate",
                                 "Wet Low Cut", "Wet High Cut", "Wet Filter Slope" })
        if (!componentWithTitle(*dimension, title)) return fail("missing accessible Mode 2 control: " + std::string(title));
      if (componentWithTitle(*dimension, "Amount") != nullptr)
        return fail("Mode 1 controls are still exposed while Mode 2 is active");
      for (const char* title : { "Mode", "Frequency", "Width", "Amount", "Stereo" })
        if (!componentWithTitle(*erosion, title)) return fail("missing accessible Erosion control: " + std::string(title));
      for (const char* title : { "Time", "Mix", "Feedback", "Warp Rate", "Warp Amount", "Filter Cutoff", "Filter Resonance",
                                 "Playback Rate", "Time Sync", "Tempo", "L/R Offset", "Pre-Filter Clamp" })
        if (!componentWithTitle(*floaty, title)) return fail("missing accessible Floaty control: " + std::string(title));
      if (!erosion->getTitle().contains("Erosion") || erosion->getTitle().contains("BP A-"))
        return fail("Erosion still exposes its old display name");
      if (!controlsInOrder(*dimension, { "Enabled", "Mode", "Size", "Motion Rate", "Wet Low Cut",
                                        "Wet High Cut", "Wet Filter Slope", "Mix", "Output" }) ||
          !controlsInOrder(*erosion, { "Enabled", "Mode", "Frequency", "Width", "Amount", "Stereo" }) ||
          !controlsInOrder(*floaty, { "Enabled", "Time Sync", "Tempo", "Time", "Playback Rate", "Feedback", "Warp Rate",
                                     "Warp Amount", "Filter Cutoff", "Filter Resonance", "Pre-Filter Clamp",
                                     "L/R Offset", "Mix" }))
        return fail("effect parameters do not follow signal flow order");
      if (!continuousKeyboardControls(*dimension) || !continuousKeyboardControls(*erosion) ||
          !continuousKeyboardControls(*floaty))
        return fail("new effects do not share Delay's keyboard adjustment");
      auto* sync = dynamic_cast<ToggleButton*>(componentWithTitle(*floaty, "Time Sync"));
      auto* tempo = dynamic_cast<Slider*>(componentWithTitle(*floaty, "Tempo"));
      if (!sync || !tempo || !sync->getToggleState())
        return fail("Floaty must expose a Free/Sync toggle reflecting the restored duration");
      const std::string sync_id = (chain == 0 ? "" : "bus_" + std::to_string(chain) + "_") +
                                  std::string("fx_slot_20_floaty_delay_sync");
      tempo->setValue(tempo->getValueFromText("1/4"), sendNotificationSync);
      if (restored.getControls().at(sync_id)->value() != 8 || tempo->getTextFromValue(tempo->getValue()) != "1/4")
        return fail("Floaty quarter note must map to its original eight thirty-second notes");
      sync->setToggleState(false, sendNotificationSync);
      if (restored.getControls().at(sync_id)->value() != 0)
        return fail("Floaty Free must disable sync through the legacy parameter");
      sync->setToggleState(true, sendNotificationSync);
      if (restored.getControls().at(sync_id)->value() != 8)
        return fail("Floaty Sync must restore the chosen duration");
      tempo->setValue(tempo->getValueFromText("1/8d"), sendNotificationSync);
      if (restored.getControls().at(sync_id)->value() != 6)
        return fail("Floaty dotted durations must match the original DSP length");
      tempo->keyPressed(KeyPress(KeyPress::upKey));
      if (tempo->getTextFromValue(tempo->getValue()) != "7/32")
        return fail("Floaty arrows must retain every original thirty-second-note duration");
      tempo->keyPressed(KeyPress(KeyPress::upKey));
      if (tempo->getTextFromValue(tempo->getValue()) != "1/4")
        return fail("Floaty arrow navigation must display the next musical duration");
      tempo->keyPressed(KeyPress(KeyPress::pageUpKey));
      if (restored.getControls().at(sync_id)->value() != 18)
        return fail("Floaty Page Up must move ten duration steps");
      tempo->keyPressed(KeyPress(KeyPress::pageDownKey));
      {
        // The handler must go before updateFullGui below rebuilds the slot: on
        // Windows its destructor reports the removal to the slider's parent.
        auto handler = tempo->createAccessibilityHandler();
        if (!handler || !handler->getValueInterface() || handler->getValueInterface()->getCurrentValueAsString() != "1/4")
          return fail("Floaty accessibility must announce musical duration values");
      }
      for (int value = 0; value <= 64; ++value) {
        tempo->setValue(value / 64.0, sendNotificationSync);
        const auto label = tempo->getTextFromValue(tempo->getValue());
        if (std::abs(tempo->getValueFromText(label) - value / 64.0) > 1e-6 || label.contains("32nd notes"))
          return fail("Every saved Floaty sync value must have a round-trippable musical name");
      }
      auto* playback = dynamic_cast<Slider*>(componentWithTitle(*floaty, "Playback Rate"));
      if (!playback || playback->getInterval() != 0.0 ||
          !playback->keyPressed(KeyPress(KeyPress::rightKey)))
        return fail("Floaty playback must use the same continuous keyboard adjustment as Delay");
      const std::string prefix = chain == 0 ? "" : "bus_" + std::to_string(chain) + "_";
      if (std::abs(restored.getControls().at(prefix + "fx_slot_20_floaty_delay_playback_rate")->value() + 1.46f) > 1.0e-5f)
        return fail("Floaty playback keyboard edit did not reach the DSP parameter");
      // The mode switches the slot's rows, so it is a menu of its two named modes.
      auto* mode = dynamic_cast<ComboBox*>(componentWithTitle(*dimension, "Mode"));
      if (!mode || mode->getNumItems() != 2 || !mode->getText().contains("Mode 2"))
        return fail("Dimension mode must be a menu of named modes, not an On/Off toggle");
      mode->setSelectedItemIndex(0, sendNotificationSync);
      editor.updateFullGui();
      dimension = effectSlotComponent(editor, section, 17);
      if (!dimension || !componentWithTitle(*dimension, "Amount") || componentWithTitle(*dimension, "Size"))
        return fail("switching to Mode 1 did not update accessible controls");
      mode = dynamic_cast<ComboBox*>(componentWithTitle(*dimension, "Mode"));
      if (!mode) return fail("Dimension mode menu missing after switching to Mode 1");
      mode->setSelectedItemIndex(1, sendNotificationSync);
      editor.updateFullGui();
    }
  }

  // New controls must survive state restoration and be reachable through the
  // same editor/proxy paths as existing controls, in every effects chain.
  for (int chain = 0; chain <= vital::kNumBuses; ++chain) {
    const std::string prefix = chain == 0 ? "" : "bus_" + std::to_string(chain) + "_";
    const std::string gate = prefix + "fx_slot_17_";
    const std::string utility = prefix + "fx_slot_16_";
    const std::string dimension = prefix + "fx_slot_18_";
    restored.parameterChanged(gate + "type", vital::constants::kSpectralGate + 1);
    restored.parameterChanged(utility + "type", vital::constants::kUtility + 1);
    restored.parameterChanged(gate + "enabled", 1.0f);
    restored.parameterChanged(utility + "enabled", 1.0f);
    restored.parameterChanged(dimension + "dimension_expander_mode", 0.0f);
    restored.parameterChanged(dimension + "dimension_expander_delay", 17.5f);
    restored.parameterChanged(utility + "utility_filter_slope", 6.0f);
    restored.parameterChanged(utility + "utility_input_gain", 3.0f);
    const int assigned = restored.assignFxParameterToAutomationSlot(gate + "spectral_gate_fft_size");
    if (assigned < 0) return fail("FFT Size could not be assigned to an FX automation proxy");
    const String name = "FX " + (chain == 0 ? String("Main") : "Bus " + String(chain)) + " Param " + String(assigned + 1);
    auto* proxy = proxyNamed(restored, name);
    proxy->setValue(1.0f);
    if (!proxy->getText(1.0f, 128).contains("8192") || restored.getControls().at(gate + "spectral_gate_fft_size")->value() != 5.0f)
      return fail("FFT automation maximum is not 8192");
    if (!isModulationDestination(restored, dimension + "dimension_expander_delay"))
      return fail("Mode 1 Delay is not a modulation destination");
  }
  restored.parameterChanged(slot_ids::id("sample_filter_slope"), 7.0f);
  restored.parameterChanged(slot_ids::id("noise_filter_slope"), 5.0f);
  MemoryBlock options_state;
  restored.getStateInformation(options_state);
  synth.setStateInformation(options_state.getData(), static_cast<int>(options_state.getSize()));
  if (synth.getControls().at(slot_ids::id("sample_filter_slope"))->value() != 7.0f ||
      synth.getControls().at(slot_ids::id("noise_filter_slope"))->value() != 5.0f)
    return fail("oscillator slopes did not survive patch restoration");
  {
    SynthEditor editor(synth);
    for (int chain = 0; chain <= vital::kNumBuses; ++chain) {
      const String group = chain == 0 ? String("Effects") : "Bus " + String(chain);
      const String section = chain == 0 ? String("Chain and routing") : group + " - Chain and routing";
      const std::string prefix = chain == 0 ? "" : "bus_" + std::to_string(chain) + "_";
      if (synth.getControls().at(prefix + "fx_slot_18_dimension_expander_delay")->value() != 17.5f ||
          synth.getControls().at(prefix + "fx_slot_17_spectral_gate_fft_size")->value() != 5.0f ||
          synth.getControls().at(prefix + "fx_slot_16_utility_filter_slope")->value() != 6.0f ||
          synth.getControls().at(prefix + "fx_slot_16_utility_input_gain")->value() != 3.0f)
        return fail("new FX values did not survive patch restoration");
      if (!selectEditorItem(editor, "Editor group", group) || !selectEditorItem(editor, "Editor element", "Chain and routing"))
        return fail("new FX option pages are unavailable");
      auto* dimension = effectSlotComponent(editor, section, 17);
      auto* gate = effectSlotComponent(editor, section, 16);
      auto* utility = effectSlotComponent(editor, section, 15);
      if (!dimension || !gate || !utility ||
          !controlsInOrder(*dimension, { "Enabled", "Mode", "Delay", "Amount", "Low Cut", "High Cut" }) ||
          !controlsInOrder(*gate, { "Enabled", "FFT Size", "Cutoff", "Strong Balance", "Enable Tilt", "Tilt", "Mix" }))
        return fail("new FX options are not in logical keyboard order");
      if (componentWithTitle(*utility, "Input Gain") || !componentWithTitle(*utility, "Output Gain"))
        return fail("Utility must expose only its output gain while retaining legacy preset gain values");
      if (!continuousKeyboardControls(*dimension)) return fail("Delay does not follow standard slider keyboard adjustment");
      for (const char* title : { "Low Cut", "High Cut" })
        if (!frequencyKeyboardControls(componentWithTitle(*utility, title)))
          return fail("Utility frequency keyboard/accessibility adjustment is incorrect");
      auto* fft = dynamic_cast<Slider*>(componentWithTitle(*gate, "FFT Size"));
      auto* slope = dynamic_cast<Slider*>(componentWithTitle(*utility, "Filter Slope"));
      if (!fft || !slope) return fail("FFT Size or Utility slope is not an indexed accessible slider");
      fft->keyPressed(KeyPress(KeyPress::homeKey));
      if (!fft->getTextFromValue(fft->getValue()).contains("8192")) return fail("FFT Home goes beyond 8192");
      fft->keyPressed(KeyPress(KeyPress::endKey));
      if (!fft->getTextFromValue(fft->getValue()).contains("256")) return fail("FFT End is not the minimum size");
      fft->keyPressed(KeyPress(KeyPress::upKey));
      if (!fft->getTextFromValue(fft->getValue()).contains("512")) return fail("FFT arrows do not select the next size");
      slope->keyPressed(KeyPress(KeyPress::homeKey));
      if (!slope->getTextFromValue(slope->getValue()).contains("48 dB")) return fail("Utility cannot select 48 dB/octave");
    }
    // The Sample and Noise sources are oscillator slots 5 and 8 now; their rows sit in those sections.
    std::function<Component*(Component&, const String&)> sectionNamed = [&](Component& parent, const String& name) -> Component* {
      if (parent.getProperties()["SectionName"].toString() == name && parent.getNumChildComponents() > 0)
        return &parent;
      for (auto* child : parent.getChildren())
        if (auto* found = sectionNamed(*child, name)) return found;
      return nullptr;
    };
    const auto oscillatorRow = [&](int oscillator, const String& title) -> Component* {
      auto* section = sectionNamed(editor, "Oscillator " + String(oscillator));
      return section != nullptr ? componentWithTitle(*section, String(oscillator) + " " + title) : nullptr;
    };
    for (const auto& [oscillator, title] : std::initializer_list<std::pair<int, const char*>> {
             { 5, "Sample Low Cutoff" }, { 5, "Sample High Cutoff" }, { 8, "Noise Low Cutoff" }, { 8, "Noise High Cutoff" } })
      if (!frequencyKeyboardControls(oscillatorRow(oscillator, title)))
        return fail(std::string("incorrect oscillator frequency keyboard/accessibility adjustment: ") + title);
    for (const auto& [oscillator, title] : std::initializer_list<std::pair<int, const char*>> {
             { 5, "Sample Filter Slope" }, { 8, "Noise Filter Slope" } }) {
      auto* slope = dynamic_cast<Slider*>(oscillatorRow(oscillator, title));
      if (!slope) return fail(std::string("missing oscillator choice: ") + title);
      slope->keyPressed(KeyPress(KeyPress::homeKey));
      if (!slope->getTextFromValue(slope->getValue()).contains("48 dB")) return fail("oscillator slope maximum is not 48 dB/octave");
      slope->keyPressed(KeyPress(KeyPress::endKey));
      if (!slope->getTextFromValue(slope->getValue()).contains("6 dB")) return fail("oscillator slope minimum is not 6 dB/octave");
    }
  }

  // Spectral Compressor retains its source macro semantics and uses the same
  // accessible rows and stable automation proxies in every chain.
  for (int chain = 0; chain <= vital::kNumBuses; ++chain) {
    const std::string chain_prefix = chain == 0 ? "" : "bus_" + std::to_string(chain) + "_";
    synth.clearFxAutomationForPrefix(chain_prefix + "fx_slot_");
    for (int slot = 1; slot <= vital::constants::kMaxEffectSlots; ++slot) {
      const std::string prefix = chain_prefix + "fx_slot_" + std::to_string(slot) + "_spectral_compressor_";
      for (const char* suffix : { "compression", "downward", "upward", "target", "attack", "release",
                                   "smooth", "curve", "tilt", "makeup", "mix" }) {
        if (synth.getControls().count(prefix + suffix) == 0)
          return fail("Spectral Compressor parameter missing in a main/bus slot: " + prefix + suffix);
        if (String(suffix) != "compression" && String(suffix) != "curve" &&
            !isModulationDestination(synth, prefix + suffix))
          return fail("Spectral Compressor DSP slider is not a modulation destination: " + prefix + suffix);
      }
    }
    const std::string slot = chain_prefix + "fx_slot_15_";
    const std::string prefix = slot + "spectral_compressor_";
    synth.parameterChanged(slot + "type", vital::constants::kSpectralCompressor + 1);
    synth.parameterChanged(slot + "enabled", 1);
    for (const char* suffix : { "compression", "downward", "upward", "curve" })
      if (synth.assignFxParameterToAutomationSlot(prefix + suffix) < 0)
        return fail("Spectral Compressor automation assignment failed");
    auto* macro = proxyNamed(synth, "FX " + (chain == 0 ? String("Main") : "Bus " + String(chain)) + " Param 1");
    if (!macro) return fail("Compression macro automation proxy missing");
    macro->setValue(0.63f);
    for (const char* suffix : { "compression", "downward", "upward" })
      if (std::abs(synth.getControls().at(prefix + suffix)->value() - 63) > 1.0e-5f)
        return fail("host Compression macro did not move both source amounts");
    synth.valueChangedThroughMidi(prefix + "compression", 42);
    if (synth.getControls().at(prefix + "downward")->value() != 42 ||
        synth.getControls().at(prefix + "upward")->value() != 42)
      return fail("MIDI Compression macro did not move both source amounts");
    synth.parameterChanged(prefix + "downward", 17);
    synth.parameterChanged(prefix + "compression", 42);
    if (synth.getControls().at(prefix + "downward")->value() != 17)
      return fail("repeated unchanged Compression automation must preserve independent amounts");
    synth.parameterChanged(prefix + "curve", 2);
    if (synth.getControls().at(prefix + "upward")->value() != 42 ||
        synth.getControls().at(prefix + "compression")->value() != 42)
      return fail("Downward must remain independently editable after Compression");
  }
  MemoryBlock compressor_state;
  synth.getStateInformation(compressor_state);
  restored.setStateInformation(compressor_state.getData(), static_cast<int>(compressor_state.getSize()));
  {
    SynthEditor editor(restored);
    for (int chain = 0; chain <= vital::kNumBuses; ++chain) {
      const String group = chain == 0 ? String("Effects") : "Bus " + String(chain);
      const String section = chain == 0 ? String("Chain and routing") : group + " - Chain and routing";
      const std::string prefix = (chain == 0 ? std::string() : "bus_" + std::to_string(chain) + "_") +
                                 "fx_slot_15_spectral_compressor_";
      if (restored.getControls().at(prefix + "downward")->value() != 17 ||
          restored.getControls().at(prefix + "upward")->value() != 42 ||
          restored.getControls().at(prefix + "curve")->value() != 2 ||
          restored.getControls().at(prefix + "compression")->value() != 42 ||
          restored.fxAutomationSlotNameForParameter(prefix + "compression").isEmpty())
        return fail("Spectral Compressor state restore overwrote independent amounts or lost automation");
      if (!selectEditorItem(editor, "Editor group", group) || !selectEditorItem(editor, "Editor element", "Chain and routing"))
        return fail("Spectral Compressor chain page unavailable");
      auto* compressor = effectSlotComponent(editor, section, 14);
      if (!compressor || !controlsInOrder(*compressor, { "Enabled", "Compression", "Downward", "Upward", "Target",
                                                        "Attack", "Release", "Smooth", "Curve", "Tilt", "Makeup", "Mix" }))
        return fail("Spectral Compressor must have all source controls in logical accessible row order");
      if (!continuousKeyboardControls(*compressor))
        return fail("Spectral Compressor does not use standard Atlas slider/accessibility adjustment");
      auto* curve = dynamic_cast<Slider*>(componentWithTitle(*compressor, "Curve"));
      if (!curve || !curve->createAccessibilityHandler()) return fail("Curve has no accessible value control");
      curve->keyPressed(KeyPress(KeyPress::endKey));
      if (curve->getTextFromValue(curve->getValue()) != "Flat") return fail("minimum Curve must be Flat");
      curve->keyPressed(KeyPress(KeyPress::upKey));
      if (curve->getTextFromValue(curve->getValue()) != "Smooth") return fail("second Curve must be Smooth");
      curve->keyPressed(KeyPress(KeyPress::homeKey));
      if (curve->getTextFromValue(curve->getValue()) != "OTT") return fail("maximum Curve must be OTT");
      auto* macro = dynamic_cast<Slider*>(componentWithTitle(*compressor, "Compression"));
      macro->setValue(macro->getValueFromText("81%"), sendNotificationSync);
      if (restored.getControls().at(prefix + "downward")->value() != 81 ||
          restored.getControls().at(prefix + "upward")->value() != 81)
        return fail("accessible Compression slider did not move both DSP amounts");
    }
  }

  // Both ping-pong directions must be available in every chain, through host
  // automation and accessible keyboard controls, and survive a state reload.
  for (int chain = 0; chain <= vital::kNumBuses; ++chain) {
    const std::string prefix = (chain == 0 ? std::string() : "bus_" + std::to_string(chain) + "_") +
                               "fx_slot_20_";
    synth.clearFxAutomationForPrefix(chain == 0 ? "fx_slot_" : "bus_" + std::to_string(chain) + "_fx_slot_");
    synth.parameterChanged(prefix + "type", vital::constants::kDelay + 1);
    if (synth.assignFxParameterToAutomationSlot(prefix + "delay_style") != 0)
      return fail("Delay Style could not be assigned to automation");
    auto* style_proxy = proxyNamed(synth, "FX " + (chain == 0 ? String("Main") : "Bus " + String(chain)) + " Param 1");
    if (!style_proxy || style_proxy->getText(0.5f, 100) != "Ping Pong R-L" ||
        style_proxy->getText(1.0f, 100) != "Ping Pong L-R")
      return fail("Delay Style automation is missing its directional names");
    style_proxy->setValue(chain % 2 == 0 ? 1.0f : 0.5f);
  }
  MemoryBlock delay_state;
  synth.getStateInformation(delay_state);
  restored.setStateInformation(delay_state.getData(), static_cast<int>(delay_state.getSize()));
  {
    SynthEditor editor(restored);
    for (int chain = 0; chain <= vital::kNumBuses; ++chain) {
      const String group = chain == 0 ? String("Effects") : "Bus " + String(chain);
      const String section = chain == 0 ? String("Chain and routing") : group + " - Chain and routing";
      const std::string id = (chain == 0 ? std::string() : "bus_" + std::to_string(chain) + "_") +
                             "fx_slot_20_delay_style";
      const int expected = chain % 2 == 0 ? vital::StereoDelay::kPingPongLR : vital::StereoDelay::kPingPong;
      if (restored.getControls().at(id)->value() != expected || restored.fxAutomationSlotNameForParameter(id).isEmpty())
        return fail("Delay direction or automation assignment did not survive state reload");
      if (!selectEditorItem(editor, "Editor group", group) || !selectEditorItem(editor, "Editor element", "Chain and routing"))
        return fail("Delay chain page unavailable");
      auto* slot = effectSlotComponent(editor, section);
      auto* style = slot ? dynamic_cast<Slider*>(componentWithTitle(*slot, "Style")) : nullptr;
      if (!style || !style->isAccessible() || !style->createAccessibilityHandler())
        return fail("Delay Style has no accessible control");
      style->keyPressed(KeyPress(KeyPress::homeKey));
      if (style->getTextFromValue(style->getValue()) != "Mid Ping Pong" ||
          restored.getControls().at(id)->value() != vital::StereoDelay::kMidPingPong)
        return fail("Mid Ping Pong must be last in presentation order and retain saved ID 3");
      style->keyPressed(KeyPress(KeyPress::downKey));
      if (style->getTextFromValue(style->getValue()) != "Ping Pong L-R" ||
          restored.getControls().at(id)->value() != vital::StereoDelay::kPingPongLR)
        return fail("Ping Pong L-R must precede Mid Ping Pong and retain saved ID 4");
      style->keyPressed(KeyPress(KeyPress::downKey));
      if (style->getTextFromValue(style->getValue()) != "Ping Pong R-L" ||
          restored.getControls().at(id)->value() != vital::StereoDelay::kPingPong)
        return fail("Keyboard cannot select Ping Pong R-L");
    }
  }

  // Scream preserves all nine non-LFO controls in every chain/slot, and uses
  // the ordinary Atlas automation, modulation and accessibility paths.
  const char* scream_names[] = { "cutoff", "scream", "resonance", "input_gain", "wet", "output_gain",
                                 "gate_release", "autogain", "keytracking" };
  for (int chain = 0; chain <= vital::kNumBuses; ++chain) {
    const std::string chain_prefix = chain == 0 ? std::string() : "bus_" + std::to_string(chain) + "_";
    synth.clearFxAutomationForPrefix(chain_prefix + "fx_slot_");
    for (int slot : { 1, 10, 20 }) {
      const std::string prefix = chain_prefix + "fx_slot_" + std::to_string(slot) + "_scream_";
      for (int i = 0; i < 9; ++i) {
        const auto id = prefix + scream_names[i];
        if (synth.getControls().count(id) == 0) return fail("Scream parameter missing: " + id);
        if (i < 7 && !isModulationDestination(synth, id))
          return fail("Scream slider missing modulation destination: " + id);
      }
      for (const auto& control : synth.getControls())
        if (control.first.find(prefix + "lfo") == 0) return fail("Scream must use Atlas LFOs");
    }
    const std::string slot = chain_prefix + "fx_slot_20_";
    const std::string prefix = slot + "scream_";
    synth.parameterChanged(slot + "type", vital::constants::kScream + 1);
    synth.parameterChanged(slot + "enabled", 1);
    for (int i = 0; i < 9; ++i)
      if (synth.assignFxParameterToAutomationSlot(prefix + scream_names[i]) != i)
        return fail("Scream automation assignment failed");
    const String proxy_prefix = "FX " + (chain == 0 ? String("Main") : "Bus " + String(chain)) + " Param ";
    auto* cutoff = proxyNamed(synth, proxy_prefix + "1");
    auto* gain = proxyNamed(synth, proxy_prefix + "4");
    auto* release = proxyNamed(synth, proxy_prefix + "7");
    if (!cutoff || !gain || !release || cutoff->getText(0.0f, 128) != "20 Hz" ||
        cutoff->getText(1.0f, 128) != "20000 Hz" || gain->getText(0.75f, 128) != "0 dB" ||
        release->getText(0.0f, 128) != "5 ms" || release->getText(1.0f, 128) != "500 ms")
      return fail("Scream automation display ranges differ from BP Scream");
    cutoff->setValue(cutoff->getValueForText("2 kHz"));
    gain->setValue(gain->getValueForText("-12 dB"));
    release->setValue(release->getValueForText("123 ms"));
    synth.parameterChanged(prefix + "autogain", 0);
    synth.parameterChanged(prefix + "keytracking", 1);
  }
  MemoryBlock scream_state;
  synth.getStateInformation(scream_state);
  restored.setStateInformation(scream_state.getData(), static_cast<int>(scream_state.getSize()));
  {
    SynthEditor editor(restored);
    for (int chain = 0; chain <= vital::kNumBuses; ++chain) {
      const String group = chain == 0 ? String("Effects") : "Bus " + String(chain);
      const String section = chain == 0 ? String("Chain and routing") : group + " - Chain and routing";
      const std::string prefix = (chain == 0 ? std::string() : "bus_" + std::to_string(chain) + "_") +
                                 "fx_slot_20_scream_";
      if (std::abs(restored.getControls().at(prefix + "input_gain")->value() - 0.625f) > 1.0e-6f ||
          std::abs(restored.getControls().at(prefix + "gate_release")->value() - 123.0f) > 1.0e-4f ||
          restored.getControls().at(prefix + "autogain")->value() != 0 ||
          restored.getControls().at(prefix + "keytracking")->value() != 1)
        return fail("Scream state did not retain gains, release or switches");
      for (const char* name : scream_names)
        if (restored.fxAutomationSlotNameForParameter(prefix + name).isEmpty())
          return fail("Scream automation assignment did not survive state restore");
      if (!selectEditorItem(editor, "Editor group", group) || !selectEditorItem(editor, "Editor element", "Chain and routing"))
        return fail("Scream chain page unavailable");
      auto* slot = effectSlotComponent(editor, section);
      if (!slot || !controlsInOrder(*slot, { "Enabled", "Cutoff", "Scream", "Resonance", "Input Gain", "Wet",
                                           "Output Gain", "Gate Release", "Autogain", "MIDI Keytracking" }))
        return fail("Scream controls are missing or in the wrong accessible order");
      if (!continuousKeyboardControls(*slot)) return fail("Scream controls do not support Atlas keyboard navigation");
      auto* cutoff = dynamic_cast<Slider*>(componentWithTitle(*slot, "Cutoff"));
      auto* release = dynamic_cast<Slider*>(componentWithTitle(*slot, "Gate Release"));
      auto* autogain = dynamic_cast<ToggleButton*>(componentWithTitle(*slot, "Autogain"));
      auto* keytracking = dynamic_cast<ToggleButton*>(componentWithTitle(*slot, "MIDI Keytracking"));
      if (!cutoff || !release || !autogain || !keytracking || !autogain->createAccessibilityHandler() ||
          !keytracking->createAccessibilityHandler())
        return fail("Scream sliders or accessible switches unavailable");
      auto cutoff_handler = cutoff->createAccessibilityHandler();
      auto* cutoff_value = cutoff_handler ? cutoff_handler->getValueInterface() : nullptr;
      if (!cutoff_value || cutoff_value->getRange().getInterval() != 10.0)
        return fail("Scream Cutoff must expose a 10 Hz accessibility step");
      cutoff_value->setValueAsString("500 Hz");
      cutoff->keyPressed(KeyPress(KeyPress::rightKey));
      if (std::abs(cutoff_value->getCurrentValue() - 510.0) > 0.02)
        return fail("Scream Cutoff arrow key does not change 10 Hz");
      cutoff->keyPressed(KeyPress(KeyPress::upKey, ModifierKeys::shiftModifier, 0));
      if (std::abs(cutoff_value->getCurrentValue() - 511.0) > 0.02)
        return fail("Scream Cutoff Shift-arrow does not change 1 Hz");
      cutoff_value->setValue(cutoff_value->getCurrentValue() + cutoff_value->getRange().getInterval());
      if (std::abs(cutoff_value->getCurrentValue() - 521.0) > 0.02)
        return fail("Scream Cutoff VoiceOver increment does not change 10 Hz");
      cutoff->setValue(cutoff->getValueFromText("2 kHz"), sendNotificationSync);
      release->setValue(release->getValueFromText("47 ms"), sendNotificationSync);
      if (std::abs(restored.getControls().at(prefix + "cutoff")->value() - std::log2(100.0f) / 10.0f) > 1.0e-6f ||
          std::abs(restored.getControls().at(prefix + "gate_release")->value() - 47.0f) > 1.0e-4f)
        return fail("Scream accessible text entry differs from source parameter mapping");
    }
  }

  // Tremolo Pan is available in every slot and chain, with display-unit
  // keyboard/AX steps and named timing/shape sliders in musical order.
  const char* tremolo_names[] = { "sync", "frequency", "tempo", "shape", "stereo_phase", "offset", "depth", "smoothing", "mix" };
  const float tremolo_values[] = { 0, 2.5f, 9, 1, 0.5f, 0.25f, 0.73f, 3.5f, 0.81f };
  for (int chain = 0; chain <= vital::kNumBuses; ++chain) {
    const std::string chain_prefix = chain == 0 ? "" : "bus_" + std::to_string(chain) + "_";
    synth.clearFxAutomationForPrefix(chain_prefix + "fx_slot_");
    for (int slot = 1; slot <= vital::constants::kMaxEffectSlots; ++slot) {
      const std::string prefix = chain_prefix + "fx_slot_" + std::to_string(slot) + "_tremolo_pan_";
      for (const char* name : tremolo_names) {
        const auto id = prefix + name;
        if (synth.getControls().count(id) == 0 || vital::Parameters::getDetails(id).version_added != 0x020106)
          return fail("Tremolo Pan parameter missing or has an incorrect host version: " + id);
        if (String(name) != "sync" && String(name) != "shape" && !isModulationDestination(synth, id))
          return fail("Tremolo Pan modulation destination missing: " + id);
      }
    }
    const std::string slot = chain_prefix + "fx_slot_14_";
    synth.parameterChanged(slot + "type", vital::constants::kTremoloPan + 1);
    synth.parameterChanged(slot + "enabled", 1);
    for (int i = 0; i < 9; ++i) {
      const auto id = slot + "tremolo_pan_" + tremolo_names[i];
      synth.parameterChanged(id, tremolo_values[i]);
      if (synth.assignFxParameterToAutomationSlot(id) != i)
        return fail("Tremolo Pan automation assignment failed: " + id);
    }
    const String proxies = "FX " + (chain == 0 ? String("Main") : "Bus " + String(chain)) + " Param ";
    if (proxyNamed(synth, proxies + "1")->getText(0, 100) != "Free" ||
        proxyNamed(synth, proxies + "1")->getText(1, 100) != "Triplet" ||
        proxyNamed(synth, proxies + "4")->getText(1, 100) != "Ramp Down")
      return fail("Tremolo Pan automation lost named timing or shape choices");
    auto* rate_proxy = proxyNamed(synth, proxies + "2");
    rate_proxy->setValue(rate_proxy->getValueForText("2.5 Hz"));
    if (std::abs(synth.getControls().at(slot + "tremolo_pan_frequency")->value() - 2.5f) > 1.0e-5)
      return fail("Tremolo Pan host automation rate mapping failed");
  }
  MemoryBlock tremolo_state;
  synth.getStateInformation(tremolo_state);
  restored.setStateInformation(tremolo_state.getData(), static_cast<int>(tremolo_state.getSize()));
  {
    SynthEditor editor(restored);
    for (int chain = 0; chain <= vital::kNumBuses; ++chain) {
      const String group = chain == 0 ? String("Effects") : "Bus " + String(chain);
      const String section = chain == 0 ? String("Chain and routing") : group + " - Chain and routing";
      const std::string prefix = (chain == 0 ? "" : "bus_" + std::to_string(chain) + "_") +
                                 "fx_slot_14_tremolo_pan_";
      for (int i = 0; i < 9; ++i) {
        const auto id = prefix + tremolo_names[i];
        if (std::abs(restored.getControls().at(id)->value() - tremolo_values[i]) > 1.0e-5 ||
            restored.fxAutomationSlotNameForParameter(id).isEmpty())
          return fail("Tremolo Pan state or automation did not survive reload: " + id);
      }
      if (!selectEditorItem(editor, "Editor group", group) || !selectEditorItem(editor, "Editor element", "Chain and routing"))
        return fail("Tremolo Pan chain page unavailable");
      auto* slot = effectSlotComponent(editor, section, 13);
      if (!slot || !slot->getTitle().contains("Tremolo Pan") ||
          !controlsInOrder(*slot, { "Enabled", "Time Mode", "Free Rate", "Sync Rate", "Shape", "Stereo Phase",
                                   "Offset", "Depth", "Smoothing", "Mix" }))
        return fail("Tremolo Pan controls are missing or in the wrong accessible order");
      if (!continuousKeyboardControls(*slot))
        return fail("Tremolo Pan sliders do not share Atlas keyboard/accessibility navigation");
      const char* titles[] = { "Free Rate", "Stereo Phase", "Offset", "Depth", "Smoothing", "Mix" };
      const char* suffixes[] = { "frequency", "stereo_phase", "offset", "depth", "smoothing", "mix" };
      const double raw_steps[] = { 0.1, 1.0 / 360.0, 1.0 / 360.0, 0.01, 0.1, 0.01 };
      const double display_steps[] = { 0.1, 1, 1, 1, 0.1, 1 };
      for (int i = 0; i < 6; ++i) {
        auto* slider = dynamic_cast<ParameterSlider*>(componentWithTitle(*slot, titles[i]));
        if (!slider || slider->getSliderStyle() != Slider::LinearHorizontal)
          return fail("Tremolo Pan continuous control must be a horizontal keyboard slider");
        auto handler = slider->createAccessibilityHandler();
        auto* value = handler ? handler->getValueInterface() : nullptr;
        if (!value || std::abs(value->getRange().getInterval() - display_steps[i]) > 1.0e-5)
          return fail("Tremolo Pan has an unsuitable accessibility increment");
        const float before = restored.getControls().at(prefix + suffixes[i])->value();
        slider->keyPressed(KeyPress(KeyPress::rightKey));
        slider->keyPressed(KeyPress(KeyPress::upKey, ModifierKeys::shiftModifier, 0));
        if (std::abs(restored.getControls().at(prefix + suffixes[i])->value() - before - raw_steps[i] * 1.1) > 1.0e-5)
          return fail("Tremolo Pan arrow and fine-arrow increments are incorrect");
        value->setValue(value->getCurrentValue() + value->getRange().getInterval());
        if (std::abs(restored.getControls().at(prefix + suffixes[i])->value() - before - raw_steps[i] * 2.1) > 1.0e-5)
          return fail("Tremolo Pan VoiceOver increment differs from the keyboard");
      }
      for (const char* title : { "Time Mode", "Sync Rate", "Shape" }) {
        auto* slider = dynamic_cast<ParameterSlider*>(componentWithTitle(*slot, title));
        if (!slider || slider->getInterval() <= 0 || !slider->keyPressed(KeyPress(KeyPress::rightKey)))
          return fail("Tremolo Pan timing/shape must be named indexed sliders");
      }
      const char* text_titles[] = { "Free Rate", "Stereo Phase", "Offset", "Depth", "Smoothing", "Mix" };
      const char* texts[] = { "7.3 Hz", "90 degrees", "270 degrees", "43%", "4.7 ms", "62%" };
      const double typed_values[] = { 7.3, 0.25, 0.75, 0.43, 4.7, 0.62 };
      for (int i = 0; i < 6; ++i) {
        auto* slider = dynamic_cast<ParameterSlider*>(componentWithTitle(*slot, text_titles[i]));
        auto handler = slider->createAccessibilityHandler();
        handler->getValueInterface()->setValueAsString(texts[i]);
        if (std::abs(restored.getControls().at(prefix + suffixes[i])->value() - typed_values[i]) > 1.0e-5)
          return fail("Tremolo Pan typed display value did not reach DSP correctly");
      }
    }
  }

  // Tape must retain Atlas slot/preset/automation and accessible slider behavior.
  const char* tape_names[] = { "drive_amount", "wow", "flutter_rate", "noise_source", "noise_width", "noise_hp_slope", "noise_pitch", "motion_enabled", "noise_level", "global_width", "global_hp_cutoff", "global_lp_slope" };
  const float tape_values[] = { 4.3f, .27f, 7.2f, 26, 1.5f, 3, -12, 1, -18, .8f, 320, 2 };
  for (int chain = 0; chain <= vital::kNumBuses; ++chain) {
    const std::string cp = chain == 0 ? "" : "bus_" + std::to_string(chain) + "_";
    synth.clearFxAutomationForPrefix(cp + "fx_slot_");
    for (int slot = 1; slot <= vital::constants::kMaxEffectSlots; ++slot) {
      for (const auto& control : vital::tape::controls) {
        const auto id = cp + "fx_slot_" + std::to_string(slot) + "_tape_" + control.suffix;
        if (synth.getControls().count(id) == 0 || vital::Parameters::getDetails(id).version_added != (String(control.suffix).startsWith("global_") ? 0x020108 : 0x020107))
          return fail("Tape parameter missing or has wrong host version: " + id);
        if (!control.indexed && !isModulationDestination(synth, id))
          return fail("Tape continuous modulation destination missing: " + id);
      }
    }
    for (const char* removed : { "drive_width", "drive_hp_enabled", "drive_hp_cutoff", "drive_hp_slope", "drive_lp_enabled", "drive_lp_cutoff", "drive_lp_slope",
                                 "motion_width", "motion_hp_enabled", "motion_hp_cutoff", "motion_hp_slope", "motion_lp_enabled", "motion_lp_cutoff", "motion_lp_slope" }) {
      const auto id = cp + "fx_slot_15_tape_" + removed;
      if (synth.getControls().count(id) || vital::Parameters::isParameter(id))
        return fail("Removed per-section Tape parameter still exists: " + id);
    }
    const std::string slot = cp + "fx_slot_15_";
    synth.parameterChanged(slot + "type", vital::constants::kTape + 1);
    synth.parameterChanged(slot + "enabled", 1);
    for (int i = 0; i < 12; ++i) {
      const auto id = slot + "tape_" + tape_names[i];
      synth.parameterChanged(id, tape_values[i]);
      if (synth.assignFxParameterToAutomationSlot(id) != i) return fail("Tape automation assignment: " + id);
    }
    const String proxies = "FX " + (chain == 0 ? String("Main") : "Bus " + String(chain)) + " Param ";
    if (proxyNamed(synth, proxies + "4")->getText(1, 100) != "Vinyl 27" ||
        proxyNamed(synth, proxies + "6")->getText(1, 100) != "48 dB/oct")
      return fail("Tape automation must announce named noise sounds and slopes");
  }
  MemoryBlock tape_state;
  synth.getStateInformation(tape_state);
  restored.setStateInformation(tape_state.getData(), static_cast<int>(tape_state.getSize()));
  {
    SynthEditor editor(restored);
    for (int chain = 0; chain <= vital::kNumBuses; ++chain) {
      const String group = chain == 0 ? String("Effects") : "Bus " + String(chain);
      const String section = chain == 0 ? String("Chain and routing") : group + " - Chain and routing";
      const std::string prefix = (chain == 0 ? "" : "bus_" + std::to_string(chain) + "_") + "fx_slot_15_tape_";
      for (int i = 0; i < 12; ++i) {
        const auto id = prefix + tape_names[i];
        if (std::abs(restored.getControls().at(id)->value() - tape_values[i]) > 1e-5 || restored.fxAutomationSlotNameForParameter(id).isEmpty())
          return fail("Tape state/automation did not survive reload: " + id);
      }
      if (!selectEditorItem(editor, "Editor group", group) || !selectEditorItem(editor, "Editor element", "Chain and routing"))
        return fail("Tape chain editor missing");
      auto* slot = effectSlotComponent(editor, section, 14);
      if (!slot || !slot->getTitle().contains("Tape") ||
          !controlsInOrder(*slot, { "Enabled", "Drive Enabled", "Drive", "Drive Tone", "Drive Mix", "Drive Output",
              "Wow / Flutter Enabled", "Wow Depth", "Wow Rate", "Flutter Depth", "Flutter Rate", "Dropout Depth", "Dropout Density",
              "Noise Enabled", "Noise Sound", "Noise Level", "Noise Follow", "Noise Pitch", "Noise Tone", "Noise Width",
              "Noise High-pass Enabled", "Noise High-pass", "Noise High-pass Slope", "Noise Low-pass Enabled", "Noise Low-pass", "Noise Low-pass Slope",
              "Global Width", "Global High-pass Enabled", "Global High-pass", "Global High-pass Slope", "Global Low-pass Enabled", "Global Low-pass", "Global Low-pass Slope" }))
        return fail("Tape slider order/labels do not match its three processing sections");
      if (!continuousKeyboardControls(*slot)) return fail("Tape keyboard and accessibility controls disagree");
      for (const char* title : { "Drive Enabled", "Wow / Flutter Enabled", "Noise Enabled", "Noise Sound", "Noise High-pass Slope", "Noise Low-pass Slope", "Global High-pass Enabled", "Global Low-pass Enabled", "Global High-pass Slope", "Global Low-pass Slope" }) {
        auto* slider = dynamic_cast<ParameterSlider*>(componentWithTitle(*slot, title));
        if (!slider || slider->getSliderStyle() != Slider::LinearHorizontal || slider->getInterval() <= 0)
          return fail("Every Tape indexed control must be a named horizontal slider");
      }
      const char* titles[] = { "Drive", "Wow Rate", "Flutter Rate", "Noise Level", "Noise Pitch", "Noise Width", "Noise High-pass", "Noise Low-pass", "Global Width", "Global High-pass", "Global Low-pass" };
      const char* suffixes[] = { "drive_amount", "wow_rate", "flutter_rate", "noise_level", "noise_pitch", "noise_width", "noise_hp_cutoff", "noise_lp_cutoff", "global_width", "global_hp_cutoff", "global_lp_cutoff" };
      const char* texts[] = { "57%", "0.63 Hz", "8.2 Hz", "-21.4 dB", "-7 semitones", "137%", "850 Hz", "8 kHz", "76%", "120 Hz", "12 kHz" };
      const float values[] = { 5.7f, .63f, 8.2f, -21.4f, -7, 1.37f, 850, 8000, .76f, 120, 12000 };
      for (int i = 0; i < 11; ++i) {
        auto* slider = dynamic_cast<ParameterSlider*>(componentWithTitle(*slot, titles[i]));
        if (!slider) return fail("Tape slider missing for typed entry");
        auto handler = slider->createAccessibilityHandler();
        handler->getValueInterface()->setValueAsString(texts[i]);
        if (std::abs(restored.getControls().at(prefix + suffixes[i])->value() - values[i]) > .002)
          return fail("Tape typed value failed: " + std::string(texts[i]));
      }
    }
  }

  // Defuse: all slots/buses, source parameter ranges, host state, keyboard and AX edits.
  const char* defuse_names[] = { "stages", "frequency", "resonance", "spread", "spread_style", "automation_precision" };
  const float defuse_values[] = { 127, 1230, 1.23f, -2.31f, 1, .73f };
  for (int chain = 0; chain <= vital::kNumBuses; ++chain) {
    synth.clearFxAutomationForPrefix(chain == 0 ? "fx_slot_" : "bus_" + std::to_string(chain) + "_fx_slot_");
    const std::string cp = chain == 0 ? "" : "bus_" + std::to_string(chain) + "_";
    for (int slot = 1; slot <= vital::constants::kMaxEffectSlots; ++slot) {
      for (const auto* name : defuse_names) {
        const auto id = cp + "fx_slot_" + std::to_string(slot) + "_defuse_" + name;
        if (!vital::Parameters::isParameter(id) || synth.getControls().count(id) == 0 ||
            vital::Parameters::getDetails(id).version_added != 0x020108)
          return fail("Defuse is missing a versioned slot parameter: " + id);
        if (String(name) != "stages" && String(name) != "spread_style" &&
            !isModulationDestination(synth, id))
          return fail("Defuse modulation destination missing: " + id);
      }
    }
    for (int slot = 1; slot <= vital::constants::kMaxEffectSlots; ++slot) {
      const auto id = cp + "fx_slot_" + std::to_string(slot) + "_defuse_bypass";
      if (vital::Parameters::isParameter(id) || synth.getControls().count(id) != 0 ||
          synth.assignFxParameterToAutomationSlot(id) >= 0)
        return fail("Removed Defuse bypass must not exist in controls or automation");
    }
    const auto prefix = cp + "fx_slot_20_";
    synth.parameterChanged(prefix + "type", vital::constants::kDefuse + 1);
    synth.parameterChanged(prefix + "enabled", 1);
    for (int i = 0; i < 6; ++i) {
      const auto id = prefix + "defuse_" + defuse_names[i];
      synth.parameterChanged(id, defuse_values[i]);
      if (synth.assignFxParameterToAutomationSlot(id) != i) return fail("Defuse automation assignment failed");
    }
    const String proxies = "FX " + (chain == 0 ? String("Main") : "Bus " + String(chain)) + " Param ";
    auto* stages = proxyNamed(synth, proxies + "1");
    if (!stages) return fail("Defuse stage proxy missing");
    // Atlas host proxies keep a stable continuous type as assignments change.
    // Their engine mapping must still round to all 513 integer stage values.
    for (int count = 0; count <= 512; ++count) {
      stages->setValue(float(count) / 512.0f);
      if (synth.getControls().at(prefix + "defuse_stages")->value() != count)
        return fail("Defuse host automation must reach every integer stage count");
    }
    stages->setValue(127.0f / 512.0f);
    auto* frequency = proxyNamed(synth, proxies + "2");
    frequency->setValue(frequency->getValueForText("1.23 kHz"));
    if (std::abs(synth.getControls().at(prefix + "defuse_frequency")->value() - 1230) > .002)
      return fail("Defuse host frequency entry must understand kHz");
    if (proxyNamed(synth, proxies + "5")->getText(0, 100) != "Octaves" ||
        proxyNamed(synth, proxies + "5")->getText(1, 100) != "Linear")
      return fail("Defuse spread styles must retain their names in host automation");
  }
  MemoryBlock defuse_state;
  synth.getStateInformation(defuse_state);
  restored.setStateInformation(defuse_state.getData(), static_cast<int>(defuse_state.getSize()));
  {
    SynthEditor editor(restored);
    for (int chain = 0; chain <= vital::kNumBuses; ++chain) {
      const String group = chain == 0 ? String("Effects") : "Bus " + String(chain);
      const String section = chain == 0 ? String("Chain and routing") : group + " - Chain and routing";
      const std::string prefix = (chain == 0 ? "" : "bus_" + std::to_string(chain) + "_") + "fx_slot_20_defuse_";
      for (int i = 0; i < 6; ++i) {
        const auto id = prefix + defuse_names[i];
        if (std::abs(restored.getControls().at(id)->value() - defuse_values[i]) > 1e-5 ||
            restored.fxAutomationSlotNameForParameter(id).isEmpty())
          return fail("Defuse values or host assignments did not survive preset reload");
      }
      if (!selectEditorItem(editor, "Editor group", group) || !selectEditorItem(editor, "Editor element", "Chain and routing"))
        return fail("Defuse chain editor missing");
      auto* slot = effectSlotComponent(editor, section, 19);
      if (!slot || !slot->getTitle().contains("Defuse") ||
          !controlsInOrder(*slot, { "Enabled", "Stages", "Frequency", "Resonance", "Spread", "Spread Style", "Automation Precision" }))
        return fail("Defuse slider labels or order are incorrect");
      if (!continuousKeyboardControls(*slot)) return fail("Defuse keyboard and accessibility controls disagree");
      const char* titles[] = { "Frequency", "Resonance", "Spread", "Automation Precision" };
      const char* suffixes[] = { "frequency", "resonance", "spread", "automation_precision" };
      const double steps[] = { 10, .01, .1, 1 };
      const char* typed[] = { "1.23 kHz", "1.23", "-2.31 octaves", "73%" };
      const double values[] = { 1230, 1.23, -2.31, .73 };
      for (int i = 0; i < 4; ++i) {
        auto* slider = dynamic_cast<ParameterSlider*>(componentWithTitle(*slot, titles[i]));
        if (!slider || slider->getSliderStyle() != Slider::LinearHorizontal || slider->getAdjustmentStep() != steps[i])
          return fail("Defuse must use horizontal Atlas sliders with useful display-unit increments");
        auto handler = slider->createAccessibilityHandler();
        auto* value = handler->getValueInterface();
        if (std::abs(value->getRange().getInterval() - steps[i]) > 1e-5)
          return fail("Defuse VoiceOver increment differs from its keyboard increment");
        value->setValueAsString(typed[i]);
        if (std::abs(restored.getControls().at(prefix + suffixes[i])->value() - values[i]) > .002)
          return fail("Defuse typed units did not reach the DSP");
        const double before = value->getCurrentValue();
        slider->keyPressed(KeyPress(KeyPress::rightKey));
        slider->keyPressed(KeyPress(KeyPress::upKey, ModifierKeys::shiftModifier, 0));
        slider->keyPressed(KeyPress(KeyPress::pageUpKey));
        if (std::abs(value->getCurrentValue() - before - steps[i] * 11.1) > steps[i] * .001)
          return fail("Defuse arrow, fine-arrow or Page Up increment is incorrect");
        value->setValue(value->getCurrentValue() + value->getRange().getInterval());
        if (std::abs(value->getCurrentValue() - before - steps[i] * 12.1) > steps[i] * .001)
          return fail("Defuse accessibility increment does not match its keyboard increment");
        slider->keyPressed(KeyPress(KeyPress::backspaceKey));
        if (std::abs(restored.getControls().at(prefix + suffixes[i])->value() - vital::Parameters::getDetails(prefix + suffixes[i]).default_value) > .002)
          return fail("Defuse Backspace must restore Diopser's default");
      }
      for (const char* title : { "Stages", "Spread Style" }) {
        auto* slider = dynamic_cast<ParameterSlider*>(componentWithTitle(*slot, title));
        if (!slider || slider->getSliderStyle() != Slider::LinearHorizontal || slider->getInterval() <= 0)
          return fail("Defuse stage/style choices must be keyboard sliders");
      }
      auto* frequency_slider = dynamic_cast<ParameterSlider*>(componentWithTitle(*slot, "Frequency"));
      auto frequency_handler = frequency_slider->createAccessibilityHandler();
      auto* frequency_value = frequency_handler->getValueInterface();
      frequency_value->setValueAsString("5 Hz");
      frequency_slider->keyPressed(KeyPress(KeyPress::downKey));
      if (frequency_value->getRange().getMinimumValue() != 20 ||
          restored.getControls().at(prefix + "frequency")->value() != 20)
        return fail("Defuse text, keyboard and accessibility must enforce the 20 Hz minimum");
      if (componentWithTitle(*slot, "Bypass") != nullptr)
        return fail("Defuse must use only Atlas's Enabled toggle");
      auto* stages = dynamic_cast<ParameterSlider*>(componentWithTitle(*slot, "Stages"));
      stages->keyPressed(KeyPress(KeyPress::endKey));
      stages->keyPressed(KeyPress(KeyPress::rightKey));
      stages->keyPressed(KeyPress(KeyPress::pageUpKey));
      if (restored.getControls().at(prefix + "stages")->value() != 11) return fail("Defuse stage increments must be one and ten");
      stages->keyPressed(KeyPress(KeyPress::homeKey));
      if (restored.getControls().at(prefix + "stages")->value() != 512) return fail("Defuse must expose the full Diopser stage range");
    }
  }

  const char* suppressor_names[] = { "amount", "smoothing", "release", "frequency", "width", "slope", "delta" };
  const float suppressor_values[] = { 63, 46, 3, 244, 61, 37, 1 };
  for (int chain = 0; chain <= vital::kNumBuses; ++chain) {
    const auto cp = chain == 0 ? std::string() : "bus_" + std::to_string(chain) + "_";
    synth.clearFxAutomationForPrefix(cp + "fx_slot_");
    for (int slot = 1; slot <= vital::constants::kMaxEffectSlots; ++slot) {
      for (const char* name : suppressor_names) {
        const auto id = cp + "fx_slot_" + std::to_string(slot) + "_suppressor_" + name;
        if (!vital::Parameters::isParameter(id) || synth.getControls().count(id) == 0)
          return fail("Suppressor parameter missing: " + id);
        if (String(name) != "delta" && !isModulationDestination(synth, id))
          return fail("Suppressor modulation destination missing: " + id);
      }
      for (const char* removed : { "sidechain", "spectrum", "spectrum_line", "silk", "bypass" })
        if (vital::Parameters::isParameter(cp + "fx_slot_" + std::to_string(slot) + "_suppressor_" + removed))
          return fail("Excluded Suppressor control must not be registered");
    }
    const auto prefix = cp + "fx_slot_20_";
    synth.parameterChanged(prefix + "type", vital::constants::kSuppressor + 1);
    synth.parameterChanged(prefix + "enabled", 1);
    for (int n = 0; n < 7; ++n) {
      const auto id = prefix + "suppressor_" + suppressor_names[n];
      synth.parameterChanged(id, suppressor_values[n]);
      if (synth.assignFxParameterToAutomationSlot(id) != n) return fail("Suppressor proxy assignment failed");
    }
    const String proxies = "FX " + (chain == 0 ? String("Main") : "Bus " + String(chain)) + " Param ";
    if (proxyNamed(synth, proxies + "2")->getText(.461f, 100) != "46.1%")
      return fail("Suppressor host text must expose fine percentage changes");
    auto* frequency = proxyNamed(synth, proxies + "4");
    const float value = frequency->getValueForText("1.23 kHz");
    if (std::abs(frequency->getText(value, 100).getFloatValue() - 1230) > .02)
      return fail("Suppressor host frequency entry must round trip Hz and kHz");
  }
  MemoryBlock suppressor_state;
  synth.getStateInformation(suppressor_state);
  restored.setStateInformation(suppressor_state.getData(), static_cast<int>(suppressor_state.getSize()));
  {
    SynthEditor editor(restored);
    for (int chain = 0; chain <= vital::kNumBuses; ++chain) {
      const String group = chain == 0 ? String("Effects") : "Bus " + String(chain);
      const String section = chain == 0 ? String("Chain and routing") : group + " - Chain and routing";
      const std::string prefix = (chain == 0 ? "" : "bus_" + std::to_string(chain) + "_") + "fx_slot_20_suppressor_";
      for (int n = 0; n < 7; ++n) {
        const auto id = prefix + suppressor_names[n];
        if (std::abs(restored.getControls().at(id)->value() - suppressor_values[n]) > 1e-5 ||
            restored.fxAutomationSlotNameForParameter(id).isEmpty())
          return fail("Suppressor state and automation must survive reload");
      }
      if (!selectEditorItem(editor, "Editor group", group) || !selectEditorItem(editor, "Editor element", "Chain and routing"))
        return fail("Suppressor chain editor missing");
      auto* slot = effectSlotComponent(editor, section, 19);
      if (!slot || !controlsInOrder(*slot, { "Enabled", "Suppressor Amount", "Smoothing", "Release", "Curve Frequency", "Curve Width", "Curve Slope", "Delta" }))
        return fail("Suppressor must expose all controls with the requested amount label and Atlas row order");
      if (!continuousKeyboardControls(*slot)) return fail("Suppressor keyboard and accessibility increments disagree");
      const char* titles[] = { "Suppressor Amount", "Smoothing", "Release", "Curve Frequency", "Curve Width", "Curve Slope" };
      const double steps[] = { 1, 1, .05, 10, 10, 1 };
      for (int n = 0; n < 6; ++n) {
        auto* slider = dynamic_cast<ParameterSlider*>(componentWithTitle(*slot, titles[n]));
        if (!slider || slider->getSliderStyle() != Slider::LinearHorizontal || slider->getAdjustmentStep() != steps[n])
          return fail("Suppressor must use Atlas horizontal sliders and display-unit steps");
      }
      auto* delta = dynamic_cast<ToggleButton*>(componentWithTitle(*slot, "Delta"));
      if (!delta || !delta->getToggleState() || delta->getButtonText() != "Delta")
        return fail("Suppressor Delta must be a recalled toggle with a consistent label");
      auto* smoothing = dynamic_cast<ParameterSlider*>(componentWithTitle(*slot, "Smoothing"));
      if (smoothing->getTextFromValue(smoothing->sliderValue(46.1)) != "46.1%")
        return fail("Suppressor slider must expose fine percentage changes");
      delta->setToggleState(false, sendNotificationSync);
      if (restored.getControls().at(prefix + "delta")->value() != 0) return fail("Delta toggle must change the DSP parameter");
    }
  }

  // Capture the actual Atlas layout with only Suppressor, for visual QA.
  for (int chain = 0; chain <= vital::kNumBuses; ++chain) {
    const auto cp = chain == 0 ? std::string() : "bus_" + std::to_string(chain) + "_";
    for (int n = 1; n <= vital::constants::kMaxEffectSlots; ++n)
      restored.parameterChanged(cp + "fx_slot_" + std::to_string(n) + "_type", 0);
  }
  restored.parameterChanged("fx_slot_1_type", vital::constants::kSuppressor + 1);
  restored.parameterChanged("fx_slot_1_enabled", 1);
  {
    SynthEditor editor(restored);
    if (!selectEditorItem(editor, "Editor group", "Effects") || !selectEditorItem(editor, "Editor element", "Chain and routing"))
      return fail("Suppressor-only editor unavailable");
    auto* module = effectSlotComponent(editor, "Chain and routing", 0);
    auto* viewport = module ? module->findParentComponentOfClass<Viewport>() : nullptr;
    if (!module || !viewport) return fail("Suppressor must be present in the visible FX viewport");
    viewport->setViewPosition(0, viewport->getViewedComponent()->getLocalArea(module, module->getLocalBounds()).getY() - 8);
    auto screenshot = File::getSpecialLocation(File::tempDirectory).getChildFile("atlas-suppressor-editor.png");
    FileOutputStream stream(screenshot);
    stream.setPosition(0); stream.truncate();
    PNGImageFormat png;
    if (!png.writeImageToStream(editor.createComponentSnapshot(editor.getLocalBounds()), stream))
      return fail("Suppressor layout capture failed");
    std::cout << "Suppressor editor capture: " << screenshot.getFullPathName() << std::endl;
  }

  if (const int result = testSpectralLockUi(synth, restored)) return result;
  if (const int result = testResonatorUi(synth, restored)) return result;

  std::cout << "Atlas FX automation proxy tests passed (80 stable host slots, "
            << synth.getParameters().size() << " total host parameters, down from "
            << original_host_index << ")\n";
  return 0;
}
