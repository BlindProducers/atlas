#pragma once

int testSpectralLockUi(SynthPlugin& synth, SynthPlugin& restored) {
  const char* names[] = { "amount", "isolate", "shimmer", "bend", "bend_range", "range_high", "range_low",
      "mute_high", "mute_low", "spray", "tilt", "pitch", "root", "scale", "mix", "width", "follow",
      "mode", "transients", "reach" };
  // Filterbank mode keeps every row visible; Spectral mode hides the resynthesis rows.
  const float settings[] = { .71f, .32f, .28f, -.4f, 12, 3200, 140, 1, 0, .22f, .3f, -7, 9, 3, .65f, .4f, .5f, 0, .3f, 2 };
  for (int chain = 0; chain <= vital::kNumBuses; ++chain) {
    const std::string cp = chain == 0 ? "" : "bus_" + std::to_string(chain) + "_";
    synth.clearFxAutomationForPrefix(cp + "fx_slot_");
    for (int slot = 1; slot <= vital::constants::kMaxEffectSlots; ++slot) {
      const auto prefix = cp + "fx_slot_" + std::to_string(slot) + "_spectral_lock_";
      for (const char* name : names)
        if (!synth.getControls().count(prefix + name)) return fail("Spectral Lock missing from a bus/slot: " + prefix + name);
      for (const char* name : { "matrix0", "matrix1", "glide", "level", "midi", "freeze" })
        if (synth.getControls().count(prefix + name)) return fail("Spectral Lock exposes an unrequested control");
      synth.parameterChanged(cp + "fx_slot_" + std::to_string(slot) + "_type", 0);
    }
    for (int slot : { 1, 20 }) {
      const auto prefix = cp + "fx_slot_" + std::to_string(slot) + "_";
      synth.parameterChanged(prefix + "type", vital::constants::kSpectralLock + 1);
      synth.parameterChanged(prefix + "enabled", 1);
      for (int i = 0; i < 20; ++i)
        synth.parameterChanged(prefix + "spectral_lock_" + names[i], slot == 1 ? settings[i] : vital::Parameters::getDetails(prefix + "spectral_lock_" + names[i]).default_value);
    }
    for (int i = 0; i < 20; ++i)
      if (synth.assignFxParameterToAutomationSlot(cp + "fx_slot_1_spectral_lock_" + names[i]) != i)
        return fail("Spectral Lock parameter could not use a stable FX automation slot");
    auto* root = proxyNamed(synth, "FX " + (chain == 0 ? String("Main") : "Bus " + String(chain)) + " Param 13");
    if (!root) return fail("Spectral Lock Root host proxy missing");
    root->setValue(9.0f / 11.0f);
    if (synth.getControls().at(cp + "fx_slot_1_spectral_lock_root")->value() != 9)
      return fail("host automation does not select Root");
  }
  MemoryBlock state;
  synth.getStateInformation(state);
  restored.setStateInformation(state.getData(), int(state.getSize()));
  SynthEditor editor(restored);
  for (int chain = 0; chain <= vital::kNumBuses; ++chain) {
    const std::string cp = chain == 0 ? "" : "bus_" + std::to_string(chain) + "_";
    const String group = chain == 0 ? String("Effects") : "Bus " + String(chain);
    const String section = chain == 0 ? String("Chain and routing") : group + " - Chain and routing";
    const auto prefix = cp + "fx_slot_1_spectral_lock_";
    for (int i = 0; i < 20; ++i) {
      if (std::abs(restored.getControls().at(prefix + names[i])->value() - settings[i]) > 1e-5f ||
          restored.fxAutomationSlotNameForParameter(prefix + names[i]).isEmpty())
        return fail("Spectral Lock settings or automation assignments lost on state reload");
    }
    if (!selectEditorItem(editor, "Editor group", group) || !selectEditorItem(editor, "Editor element", "Chain and routing"))
      return fail("Spectral Lock bus editor missing");
    auto* slot = effectSlotComponent(editor, section, 0);
    if (!slot || !controlsInOrder(*slot, { "Enabled", "Amount", "Isolate", "Shimmer", "Bend", "Bend Range",
          "Range High", "Range Low", "Mute High", "Mute Low", "Spray", "Tilt", "Pitch", "Root", "Scale", "Mix" }))
      return fail("Spectral Lock accessible controls missing or out of order");
    if (!continuousKeyboardControls(*slot)) return fail("Spectral Lock keyboard/accessibility increments differ from Atlas");
    for (const char* name : { "Root", "Scale" }) {
      auto* combo = dynamic_cast<ComboBox*>(componentWithTitle(*slot, name));
      if (!combo || combo->getNumItems() != 12) return fail("Root and Scale must be twelve-choice combo boxes");
      auto handler = combo->createAccessibilityHandler();
      if (!handler || handler->getRole() != AccessibilityRole::comboBox || !handler->getValueInterface())
        return fail("Root/Scale combo boxes have no accessible role or selected value");
      combo->keyPressed(KeyPress(KeyPress::homeKey));
      combo->keyPressed(KeyPress(KeyPress::rightKey));
      if (combo->getSelectedItemIndex() != 1) return fail("Root/Scale keyboard choice did not advance");
      if (restored.getControls().at(prefix + String(name).toLowerCase().toStdString())->value() != 1)
        return fail("Root/Scale combo change not applied to the engine");
      combo->keyPressed(KeyPress(KeyPress::endKey));
      if (combo->getSelectedItemIndex() != 11) return fail("Root/Scale End shortcut did not select the last choice");
      combo->keyPressed(KeyPress(KeyPress::backspaceKey));
      if (combo->getSelectedItemIndex() != 0) return fail("Root/Scale default shortcut failed");
    }
    for (const char* name : { "Range High", "Range Low" }) {
      auto* slider = dynamic_cast<ParameterSlider*>(componentWithTitle(*slot, name));
      if (!slider || slider->getAdjustmentStep() != 10) return fail("Range controls must use 10 Hz steps");
      auto handler = slider->createAccessibilityHandler();
      auto* value = handler->getValueInterface();
      value->setValueAsString("700 Hz");
      slider->keyPressed(KeyPress(KeyPress::rightKey));
      slider->keyPressed(KeyPress(KeyPress::upKey, ModifierKeys::shiftModifier, 0));
      if (std::abs(value->getCurrentValue() - 711) > .01) return fail("Range keyboard/VoiceOver values are not in Hz");
    }
    for (const char* name : { "Pitch", "Bend Range" }) {
      auto* slider = dynamic_cast<ParameterSlider*>(componentWithTitle(*slot, name));
      if (!slider || slider->getInterval() <= 0) return fail("Pitch and Bend Range need semitone steps");
      slider->keyPressed(KeyPress(KeyPress::endKey));
      const float before = restored.getControls().at(prefix + (String(name) == "Pitch" ? "pitch" : "bend_range"))->value();
      slider->keyPressed(KeyPress(KeyPress::upKey));
      if (restored.getControls().at(prefix + (String(name) == "Pitch" ? "pitch" : "bend_range"))->value() != before + 1)
        return fail("Pitch/Bend Range arrows must change one semitone");
    }
    auto* mute = dynamic_cast<ToggleButton*>(componentWithTitle(*slot, "Mute High"));
    if (!mute || !mute->getToggleState()) return fail("Mute High must restore as an accessible toggle");
    mute->setToggleState(false, sendNotificationSync);
    if (restored.getControls().at(prefix + "mute_high")->value() != 0) return fail("Mute High toggle not connected");
    if (restored.getControls().at(cp + "fx_slot_20_spectral_lock_amount")->value() != 1 ||
        restored.getControls().at(cp + "fx_slot_20_spectral_lock_pitch")->value() != 0)
      return fail("editing Spectral Lock affects a second instance");
  }
  // Capture the shipped editor at both ends of the long FX panel for visual QA.
  selectEditorItem(editor, "Editor group", "Effects");
  selectEditorItem(editor, "Editor element", "Chain and routing");
  auto* slot = effectSlotComponent(editor, "Chain and routing", 0);
  auto* viewport = slot ? slot->findParentComponentOfClass<Viewport>() : nullptr;
  if (!viewport) return fail("Spectral Lock is absent from the FX viewport");
  for (bool bottom : { false, true }) {
    const auto bounds = viewport->getViewedComponent()->getLocalArea(slot, slot->getLocalBounds());
    viewport->setViewPosition(0, bottom ? bounds.getBottom() - viewport->getHeight() + 12 : bounds.getY() - 8);
    File path = File::getSpecialLocation(File::tempDirectory).getChildFile(bottom ? "atlas-spectral-lock-bottom.png" : "atlas-spectral-lock-top.png");
    FileOutputStream stream(path); stream.setPosition(0); stream.truncate();
    PNGImageFormat png;
    if (!png.writeImageToStream(editor.createComponentSnapshot(editor.getLocalBounds()), stream)) return fail("Spectral Lock screenshot failed");
    std::cout << "Spectral Lock editor capture: " << path.getFullPathName() << '\n';
  }
  return 0;
}
