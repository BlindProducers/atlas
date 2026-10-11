#pragma once

#include "convolution_reverb_module.h"

namespace {
  int testFxBusPresets() {
    SynthPlugin synth;
    auto& controls = synth.getControls();
    auto value = [&](const std::string& id) { return controls.at(id)->value(); };
    for (int bus = 0; bus <= vital::kNumBuses; ++bus) {
      const std::string prefix = bus == 0 ? "" : "bus_" + std::to_string(bus) + "_";
      synth.parameterChanged(prefix + "fx_slot_1_type", vital::constants::kDelay + 1);
      synth.parameterChanged(prefix + "fx_slot_1_delay_feedback", 0.23f + 0.1f * bus);
      synth.parameterChanged(prefix + "fx_slot_20_type", vital::constants::kUtility + 1);
      synth.parameterChanged(prefix + "fx_slot_20_enabled", 0);
    }
    const std::string target = "bus_1_fx_slot_1_delay_feedback";
    const int binding = synth.assignFxParameterToAutomationSlot(target);
    synth.connectModulation("lfo_1", target);
    synth.connectModulation("lfo_2", "bus_2_fx_slot_1_delay_feedback");
    if (binding < 0 || !(synth.getConnectionIndex("lfo_1", target) >= 0))
      return fail("bus preset test modulation/automation setup failed");
    std::array<json, vital::kNumBuses + 1> before;
    for (int bus = 0; bus <= vital::kNumBuses; ++bus)
      before[bus] = synth.fxBusPreset(bus);
    synth.setFxBusEnabled(1, false);
    if (value("bus_1_fx_slot_1_enabled") != 0 || value("bus_1_fx_slot_20_enabled") != 0 ||
        value(target) != before[1]["slots"][0]["parameters"]["delay_feedback"].get<float>() ||
        synth.fxAutomationSlotNameForParameter(target).isEmpty() || !(synth.getConnectionIndex("lfo_1", target) >= 0))
      return fail("bypassing a bus must retain modules, parameters, and assignments");
    for (int bus : { 0, 2, 3 }) {
      if (synth.fxBusPreset(bus) != before[bus])
        return fail("bypassing a bus modified another chain");
    }
    synth.setFxBusEnabled(1, true);
    if (value("bus_1_fx_slot_1_enabled") != 1 || value("bus_1_fx_slot_20_enabled") != 1)
      return fail("enable bus must enable all occupied slots, including slot 20");
    synth.parameterChanged("bus_1_fx_slot_20_enabled", 0);

    synth.parameterChanged("bus_1_fx_slot_5_type", vital::constants::kConvolutionReverb + 1);
    vital::ConvolutionReverbModule::IrData ir;
    ir.name = "Portable bus impulse";
    ir.sample_rate = 48000;
    ir.left.resize(128, 0.0f);
    ir.left[0] = 0.5f;
    ir.right = ir.left;
    const json ir_data = vital::ConvolutionReverbModule::irToJson(ir);
    for (const auto& module : synth.getConvolutionReverbModules()) {
      if (module.first == "bus_1_fx_slot_5_" && !module.second->irStateFromJson(ir_data))
        return fail("bus preset impulse setup failed");
    }
    const json saved = synth.fxBusPreset(1);
    if (!saved["slots"][4].count("impulse_response"))
      return fail("bus presets must embed custom convolution audio");
    TemporaryFile file(".atfx");
    if (!file.getFile().replaceWithText(String(saved.dump(2))))
      return fail("could not write atfx round-trip fixture");
    const json from_file = json::parse(file.getFile().loadFileAsString().toStdString());
    String error;
    std::vector<json> invalid;
    invalid.push_back(json::parse("broken", nullptr, false));
    invalid.push_back({ { "type", "Atlas FX Preset" }, { "parameters", json::object() } });
    auto corrupt = [&](const std::function<void(json&)>& edit) {
      json data = saved;
      edit(data);
      invalid.push_back(std::move(data));
    };
    corrupt([](json& data) { data["version"] = 99; });
    corrupt([](json& data) { data["slots"].push_back(data["slots"][0]); });
    corrupt([](json& data) { data["slots"][0]["effect"] = "unknown_effect"; });
    corrupt([](json& data) { data["slots"][0]["enabled"] = "yes"; });
    corrupt([](json& data) { data["slots"][0]["parameters"]["delay_feedback"] = "bad"; });
    corrupt([](json& data) { data["slots"][0]["parameters"]["delay_feedback"] = 1e100; });
    corrupt([](json& data) { data["slots"][0]["parameters"]["delay_sync"] = 0.5; });
    corrupt([](json& data) { data["slots"][0]["parameters"]["osc_1_level"] = 0; });
    corrupt([](json& data) { data["slots"][4]["impulse_response"]["samples"] = "invalid"; });
    corrupt([](json& data) { data["slots"][4]["impulse_response"]["length"] = 1e15; });
    for (const auto& data : invalid) {
      if (synth.loadFxBusPreset(1, data, error) || error.isEmpty() || synth.fxBusPreset(1) != saved ||
          !(synth.getConnectionIndex("lfo_1", target) >= 0) || synth.fxAutomationSlotNameForParameter(target).isEmpty())
        return fail("invalid bus presets must fail before modifying the bus or its assignments");
    }

    const float destination = value("bus_2_destination");
    const float oscillator = value("osc_1_level");
    synth.assignFxParameterToAutomationSlot("bus_2_fx_slot_1_delay_feedback");
    synth.parameterChanged("bus_2_fx_slot_3_type", vital::constants::kDelay + 1);
    synth.parameterChanged("bus_2_fx_slot_3_delay_feedback", 0.75f);
    for (int bus : { 0, 2, 3 }) {
      if (!synth.loadFxBusPreset(bus, from_file, error))
        return fail("could not load a valid portable bus preset: " + error.toStdString());
      const json loaded = synth.fxBusPreset(bus);
      if (loaded != saved || !error.isEmpty())
        return fail("bus preset must retain slot order, bypass states, settings, and embedded impulse");
    }
    if (value("bus_2_destination") != destination || value("osc_1_level") != oscillator ||
        (synth.getConnectionIndex("lfo_2", "bus_2_fx_slot_1_delay_feedback") >= 0) ||
        synth.fxAutomationSlotNameForParameter("bus_2_fx_slot_1_delay_feedback").isNotEmpty() ||
        !(synth.getConnectionIndex("lfo_1", target) >= 0) || synth.fxAutomationSlotNameForParameter(target).isEmpty())
      return fail("loading a bus preset must replace only the selected bus and its assignments");
    if (value("bus_2_fx_slot_3_delay_feedback") !=
        vital::Parameters::getDetails("bus_2_fx_slot_3_delay_feedback").default_value)
      return fail("loading a bus preset must reset parameters from removed slots");

    synth.clearFxBus(1);
    for (const auto& control : controls) {
      if (control.first.rfind("bus_1_fx_slot_", 0) == 0 && vital::Parameters::isParameter(control.first) &&
          control.second->value() != vital::Parameters::getDetails(control.first).default_value)
        return fail("clear bus must remove all slots and reset their stored settings");
    }
    for (const auto& module : synth.getConvolutionReverbModules()) {
      if (module.first == "bus_1_fx_slot_5_" && module.second->hasCustomIr())
        return fail("clear bus must release the removed effects' custom impulses");
    }
    if ((synth.getConnectionIndex("lfo_1", target) >= 0) || synth.fxAutomationSlotNameForParameter(target).isNotEmpty())
      return fail("clear bus must remove obsolete modulation and automation assignments");
    for (int bus : { 0, 2, 3 }) {
      if (synth.fxBusPreset(bus) != saved)
        return fail("clear bus modified a different effects chain");
    }

    // Exercise every effect's parameter schema, not just delay and reverb.
    for (int effect = 0; effect < vital::constants::kNumEffects; ++effect) {
      synth.parameterChanged("bus_1_fx_slot_1_type", effect + 1);
      const json effect_preset = synth.fxBusPreset(1);
      if (!synth.loadFxBusPreset(2, effect_preset, error) || synth.fxBusPreset(2) != effect_preset)
        return fail("FX bus preset round-trip failed for effect " + std::to_string(effect) + ": " +
                    error.toStdString());
    }
    std::cout << "Atlas FX bus presets passed (isolation, bypass, clear, portable atfx, impulses, invalid files)\n";
    return 0;
  }
}
