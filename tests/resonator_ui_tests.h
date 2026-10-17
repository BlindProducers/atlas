#pragma once

// A rebuilt slot's predecessor lingers until a later message; use the one on show.
Component* visibleEffectSlot(Component& editor, const String& section, int slot) {
  Component* result = nullptr;
  std::function<void(Component&)> visit = [&](Component& component) {
    if (result != nullptr || (&component != &editor && !component.isVisible())) return;
    if (component.getProperties()["SectionName"].toString() == section &&
        component.getProperties().contains("FxSlot") && static_cast<int>(component.getProperties()["FxSlot"]) == slot)
      result = &component;
    for (auto* child : component.getChildren()) visit(*child);
  };
  visit(editor);
  return result;
}

int testResonatorUi(SynthPlugin& synth, SynthPlugin& restored) {
  const char* names[] = { "model", "quality", "decay", "material", "brightness", "inharmonics", "ratio", "radius",
      "opening", "hit", "listen_left", "listen_right", "midi_pitch", "tune", "transpose", "fine", "spread", "glide",
      "note_priority", "off_decay", "lfo_amount", "lfo_shape", "lfo_sync", "lfo_frequency", "lfo_tempo",
      "lfo_stereo_phase", "lfo_spin", "lfo_offset", "filter", "filter_frequency", "filter_bandwidth", "bleed",
      "width", "excitation", "wet_gain", "limiter", "output_gain", "mix" };
  // Membrane, a 2:1 side ratio, 220 Hz, 3 s decay, synced LFO, Lowest priority.
  const std::pair<const char*, float> settings[] = {
    { "model", 3 }, { "ratio", 2.0f }, { "tune", float(std::log2(220.0)) }, { "decay", float(std::log2(3.0)) },
    { "lfo_sync", 1 }, { "note_priority", 1 }, { "mix", 0.6f }, { "fine", -0.25f }
  };
  for (int chain = 0; chain <= vital::kNumBuses; ++chain) {
    const std::string cp = chain == 0 ? "" : "bus_" + std::to_string(chain) + "_";
    synth.clearFxAutomationForPrefix(cp + "fx_slot_");
    for (int slot = 1; slot <= vital::constants::kMaxEffectSlots; ++slot) {
      const auto prefix = cp + "fx_slot_" + std::to_string(slot) + "_resonator_";
      for (const char* name : names)
        if (!synth.getControls().count(prefix + name)) return fail("Resonator missing from a bus/slot: " + prefix + name);
      // REAPER-only controls are replaced by Atlas's MIDI handling.
      for (const char* name : { "midi_channel", "bend_range", "panic" })
        if (synth.getControls().count(prefix + name)) return fail("Resonator exposes a REAPER-only control");
      synth.parameterChanged(cp + "fx_slot_" + std::to_string(slot) + "_type", 0);
    }
    for (int slot : { 1, 20 }) {
      const auto prefix = cp + "fx_slot_" + std::to_string(slot) + "_";
      synth.parameterChanged(prefix + "type", vital::constants::kResonator + 1);
      synth.parameterChanged(prefix + "enabled", 1);
      for (const char* name : names)
        synth.parameterChanged(prefix + "resonator_" + name,
                               vital::Parameters::getDetails(prefix + "resonator_" + name).default_value);
    }
    for (const auto& setting : settings)
      synth.parameterChanged(cp + "fx_slot_1_resonator_" + setting.first, setting.second);
    const std::string tune = cp + "fx_slot_1_resonator_tune";
    const int assigned = synth.assignFxParameterToAutomationSlot(tune);
    auto* proxy = proxyNamed(synth, "FX " + (chain == 0 ? String("Main") : "Bus " + String(chain)) + " Param " + String(assigned + 1));
    if (assigned < 0 || !proxy) return fail("Resonator Tune could not use an FX automation slot");
    const auto& details = vital::Parameters::getDetails(tune);
    proxy->setValue(float((std::log2(440.0) - details.min) / (details.max - details.min)));
    if (std::abs(synth.getControls().at(tune)->value() - std::log2(440.0f)) > 1e-4f || !proxy->getText(proxy->getValue(), 128).contains("440"))
      return fail("Host automation of Resonator Tune is not in Hz");
    proxy->setValue(float((std::log2(220.0) - details.min) / (details.max - details.min)));
  }

  MemoryBlock state;
  synth.getStateInformation(state);
  restored.setStateInformation(state.getData(), int(state.getSize()));
  SynthEditor editor(restored);
  // Let the editor's 20 Hz timer notice the model and finish its queued rebuild.
  const auto pump = [] { for (int i = 0; i < 4; ++i) MessageManager::getInstance()->runDispatchLoopUntil(150); };
  for (int chain = 0; chain <= vital::kNumBuses; ++chain) {
    const std::string cp = chain == 0 ? "" : "bus_" + std::to_string(chain) + "_";
    const String group = chain == 0 ? String("Effects") : "Bus " + String(chain);
    const String section = chain == 0 ? String("Chain and routing") : group + " - Chain and routing";
    const auto prefix = cp + "fx_slot_1_resonator_";
    for (const auto& setting : settings) {
      if (std::abs(restored.getControls().at(prefix + setting.first)->value() - setting.second) > 1e-4f)
        return fail("Resonator settings lost on state reload");
    }
    if (restored.fxAutomationSlotNameForParameter(prefix + "tune").isEmpty())
      return fail("Resonator automation assignment lost on state reload");
    if (!selectEditorItem(editor, "Editor group", group) || !selectEditorItem(editor, "Editor element", "Chain and routing"))
      return fail("Resonator bus editor missing");
    auto* slot = visibleEffectSlot(editor, section, 0);
    if (!slot || !controlsInOrder(*slot, { "Enabled", "Model", "Quality", "Decay", "Material", "Brightness",
          "Inharmonics", "Ratio", "Hit Position", "Listen Left", "Listen Right", "MIDI Pitch", "Tune", "Transpose",
          "Fine Tune", "Spread", "Glide", "Note Priority", "Off Decay", "LFO Amount", "LFO Shape", "LFO Time Mode",
          "LFO Free Rate", "LFO Sync Rate", "LFO Stereo Phase", "LFO Spin", "LFO Offset", "Filter",
          "Filter Frequency", "Filter Bandwidth", "Bleed", "Width", "Excitation", "Wet Gain", "Limiter",
          "Output Gain", "Mix" }))
      return fail("Resonator accessible controls missing or out of order");
    if (componentWithTitle(*slot, "Radius") || componentWithTitle(*slot, "Opening"))
      return fail("Membrane shows Pipe and Tube controls");
    if (!continuousKeyboardControls(*slot)) return fail("Resonator keyboard/accessibility increments differ from Atlas");

    auto* tune = dynamic_cast<ParameterSlider*>(componentWithTitle(*slot, "Tune"));
    if (!tune || tune->getAdjustmentStep() != 1) return fail("Tune must move in semitones");
    {
      // The handler must go before a rebuild deletes the slider: on Windows
      // its destructor reports the removal to the slider's parent.
      auto handler = tune->createAccessibilityHandler();
      auto* value = handler->getValueInterface();
      value->setValueAsString("440 Hz");
      if (std::abs(value->getCurrentValue() - 69.0) > 1e-3 || !value->getCurrentValueAsString().contains("440"))
        return fail("Tune text entry or MIDI note value is wrong");
      tune->keyPressed(KeyPress(KeyPress::rightKey));
      if (std::abs(std::exp2(restored.getControls().at(prefix + "tune")->value()) - 440.0 * std::exp2(1.0 / 12.0)) > 0.05)
        return fail("Tune arrows must change one semitone");
      value->setValueAsString("2 kHz");
      if (std::abs(std::exp2(restored.getControls().at(prefix + "tune")->value()) - 2000.0) > 0.5)
        return fail("Tune text entry must accept kHz");
    }

    auto* priority = dynamic_cast<ParameterSlider*>(componentWithTitle(*slot, "Note Priority"));
    if (!priority || !priority->getTextFromValue(priority->getValue()).contains("Lowest"))
      return fail("Note Priority must be a named Last/Lowest choice, not On/Off");
    auto* filter = dynamic_cast<ToggleButton*>(componentWithTitle(*slot, "Filter"));
    if (!filter || filter->getToggleState()) return fail("Resonator Filter must be an accessible toggle");
    filter->setToggleState(true, sendNotificationSync);
    if (restored.getControls().at(prefix + "filter")->value() != 1) return fail("Filter toggle not connected");

    auto* fine = dynamic_cast<ParameterSlider*>(componentWithTitle(*slot, "Fine Tune"));
    if (!fine) return fail("Fine Tune control missing");
    fine->keyPressed(KeyPress(KeyPress::rightKey, ModifierKeys::shiftModifier, 0));
    const String fine_text = fine->getTextFromValue(fine->getValue());
    if (!fine_text.contains("24.9")) return fail("Fine Tune must show a Shift-arrow tenth of a cent: " + fine_text.toStdString());

    // Plate shares Membrane's rows, so the slot is not rebuilt (focus stays).
    pump();  // settle any rebuild queued by the edits above
    slot = visibleEffectSlot(editor, section, 0);
    // The model can change the slot's rows, so it is a menu of the models.
    auto* model = slot ? dynamic_cast<ComboBox*>(componentWithTitle(*slot, "Model")) : nullptr;
    if (!model || model->getNumItems() != 7) return fail("Model must be a menu of the seven models");
    slot->getProperties().set("resonator_test_marker", true);
    model->setSelectedItemIndex(model->getSelectedItemIndex() + 1, sendNotificationSync);
    pump();
    slot = visibleEffectSlot(editor, section, 0);
    if (!slot || !slot->getProperties().contains("resonator_test_marker") || !componentWithTitle(*slot, "Ratio"))
      return fail("Moving between models that share rows must not rebuild the slot");
    // Choosing Pipe replaces the solid-body rows with Pipe's own.
    model->setSelectedItemIndex(model->getSelectedItemIndex() + 1, sendNotificationSync);
    if (restored.getControls().at(prefix + "model")->value() != 5) return fail("The model menu must choose models in order");
    pump();
    slot = visibleEffectSlot(editor, section, 0);
    if (!slot || !componentWithTitle(*slot, "Radius") || !componentWithTitle(*slot, "Opening") ||
        componentWithTitle(*slot, "Ratio") || componentWithTitle(*slot, "Hit Position") ||
        componentWithTitle(*slot, "Quality") || componentWithTitle(*slot, "Bleed"))
      return fail("Resonator rows did not follow the Pipe model");
    restored.getControls().at(prefix + "model")->set(6);
    pump();
    slot = visibleEffectSlot(editor, section, 0);
    if (!slot || !componentWithTitle(*slot, "Radius") || componentWithTitle(*slot, "Opening"))
      return fail("Tube must show Radius without Opening");

    if (restored.getControls().at(cp + "fx_slot_20_resonator_model")->value() != 0 ||
        restored.getControls().at(cp + "fx_slot_20_resonator_filter")->value() != 0)
      return fail("editing Resonator affects a second instance");
  }
  // Capture the Membrane layout at both ends of the slot for visual QA.
  restored.getControls().at("fx_slot_1_resonator_model")->set(3);
  selectEditorItem(editor, "Editor group", "Effects");
  selectEditorItem(editor, "Editor element", "Chain and routing");
  pump();
  auto* slot = visibleEffectSlot(editor, "Chain and routing", 0);
  auto* viewport = slot ? slot->findParentComponentOfClass<Viewport>() : nullptr;
  if (!viewport) return fail("Resonator is absent from the FX viewport");
  for (bool bottom : { false, true }) {
    const auto bounds = viewport->getViewedComponent()->getLocalArea(slot, slot->getLocalBounds());
    viewport->setViewPosition(0, bottom ? bounds.getBottom() - viewport->getHeight() + 12 : bounds.getY() - 8);
    File path = File::getSpecialLocation(File::tempDirectory).getChildFile(bottom ? "atlas-resonator-bottom.png" : "atlas-resonator-top.png");
    FileOutputStream stream(path); stream.setPosition(0); stream.truncate();
    PNGImageFormat png;
    if (!png.writeImageToStream(editor.createComponentSnapshot(editor.getLocalBounds()), stream)) return fail("Resonator screenshot failed");
    std::cout << "Resonator editor capture: " << path.getFullPathName() << '\n';
  }
  return 0;
}
