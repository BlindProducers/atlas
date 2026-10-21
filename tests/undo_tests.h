#pragma once

// Undo and redo: what the user changes in the editor is recorded by
// UndoHistory, and Command Z, Shift Command Z and Command Y (Ctrl on
// Windows) take it back and put it back.

namespace {
  KeyPress undoKey() {
    return KeyPress('z', ModifierKeys::commandModifier, 0);
  }

  KeyPress redoKey() {
    return KeyPress('z', ModifierKeys::commandModifier | ModifierKeys::shiftModifier, 0);
  }

  KeyPress redoWithYKey() {
    return KeyPress('y', ModifierKeys::commandModifier, 0);
  }

  float controlValue(SynthPlugin& synth, const std::string& name) {
    return synth.getControls().at(name)->value();
  }

  // The editor on screen, as the user has it.
  struct UndoTestEditor {
    explicit UndoTestEditor(SynthPlugin& synth) : editor(synth) {
      editor.updateFullGui();
      editor.addToDesktop(0);
      editor.setVisible(true);
      editor.toFront(true);
      pumpEditorEvents();
    }

    ~UndoTestEditor() {
      editor.removeFromDesktop();
    }

    void press(const KeyPress& key) {
      editor.keyPressed(key);
      pumpEditorEvents();
    }

    // Up on the control's row, as the arrow key does it.
    bool nudge(const std::string& id) {
      Component* control = editor.controlForParameter(String(id));
      if (control == nullptr)
        return false;
      control->keyPressed(KeyPress(KeyPress::upKey));
      pumpEditorEvents();
      return true;
    }

    SynthEditor editor;
  };

  // A preset library of its own in a temporary folder, so that Initialize
  // patch and Save patch as default never read or write the user's.
  struct TemporaryUndoLibrary {
    File folder = File::getSpecialLocation(File::tempDirectory)
                      .getNonexistentChildFile("AtlasUndoTestLibrary", "", false);
    TemporaryFile config { ".config" };

    TemporaryUndoLibrary() {
      folder.createDirectory();
      json contents = LoadSave::getConfigJson();
      if (!contents.is_object())
        contents = json::object();
      contents["data_directory"] = folder.getFullPathName().toStdString();
      // Never the user's community login, nor an old token an older Atlas
      // left in their config.
      for (const char* key : { "community_api_token", "community_account_username", "community_account_name",
                               "community_logins" })
        contents.erase(key);
      config.getFile().replaceWithText(contents.dump());
      LoadSave::setConfigFileForTesting(config.getFile());
    }

    ~TemporaryUndoLibrary() {
      LoadSave::setConfigFileForTesting({});
      folder.deleteRecursively();
    }

    bool isolated() const {
      return LoadSave::getDataDirectory() == folder;
    }
  };

  bool heard(const SpokenMessages& spoken, const String& words) {
    return spoken.messages.joinIntoString(" | ").contains(words);
  }

  std::string heardText(const SpokenMessages& spoken) {
    return spoken.messages.joinIntoString(" | ").toStdString();
  }

  // Arrow keys on a control, then Command Z: the control goes back, the quick
  // presses as one step, and Shift Command Z (or Command Y) puts it back.
  void testUndoMovesAValueBack() {
    SpokenMessages spoken;
    SynthPlugin synth;
    UndoTestEditor screen(synth);
    const std::string id = "filter_1_cutoff";
    const float before = controlValue(synth, id);
    bool nudged = true;
    for (int i = 0; i < 3; ++i)
      nudged = screen.nudge(id) && nudged;
    expect(nudged, "the editor has a Filter 1 Cutoff row");
    if (!nudged)
      return;
    const float after = controlValue(synth, id);
    expect(after != before, "the arrows move Filter 1 Cutoff");
    expect(synth.undoHistory().numUndoSteps() == 1,
           "three quick presses are one step, got " + std::to_string(synth.undoHistory().numUndoSteps()));

    spoken.messages.clear();
    screen.press(undoKey());
    close(controlValue(synth, id), before, "Command Z puts Filter 1 Cutoff back");
    expect(heard(spoken, "Undo") && heard(spoken, "Cutoff"), "undo says what it undid, heard: " + heardText(spoken));

    spoken.messages.clear();
    screen.press(redoKey());
    close(controlValue(synth, id), after, "Shift Command Z puts the change back");
    expect(heard(spoken, "Redo") && heard(spoken, "Cutoff"), "redo says what it redid, heard: " + heardText(spoken));

    screen.press(undoKey());
    screen.press(redoWithYKey());
    close(controlValue(synth, id), after, "Command Y redoes too");

    screen.press(undoKey());
    spoken.messages.clear();
    screen.press(undoKey());
    close(controlValue(synth, id), before, "with nothing left to undo the value stays");
    expect(heard(spoken, "Nothing to undo"), "an empty history says so, heard: " + heardText(spoken));
  }

  // Two controls changed one after the other are two steps, undone newest
  // first; a change made after an undo drops what redo would have done.
  void testUndoKeepsEachControlsChangeApart() {
    SynthPlugin synth;
    UndoTestEditor screen(synth);
    const std::string first = "filter_1_cutoff";
    const std::string second = "filter_1_resonance";
    const float first_before = controlValue(synth, first);
    const float second_before = controlValue(synth, second);
    const bool nudged = screen.nudge(first) && screen.nudge(second);
    expect(nudged, "the editor has Filter 1 Cutoff and Resonance rows");
    if (!nudged)
      return;
    const float first_after = controlValue(synth, first);
    expect(synth.undoHistory().numUndoSteps() == 2, "two controls are two steps");

    screen.press(undoKey());
    close(controlValue(synth, second), second_before, "the newest change is undone first");
    close(controlValue(synth, first), first_after, "the change before it stays");
    screen.press(undoKey());
    close(controlValue(synth, first), first_before, "then the one before");

    screen.press(redoKey());
    close(controlValue(synth, first), first_after, "redo puts the first change back");
    expect(synth.undoHistory().canRedo(), "the second change can still be redone");
    screen.nudge(second);
    expect(!synth.undoHistory().canRedo(), "a new change drops what redo would have done");
  }

  // The host's automation moves the starting point; it is never a step, and
  // undoing the user's change leaves the automated value alone.
  void testHostAutomationIsNotAStep() {
    SynthPlugin synth;
    UndoTestEditor screen(synth);
    const std::string edited = "filter_1_cutoff";
    const std::string automated = "filter_1_resonance";
    const float edited_before = controlValue(synth, edited);
    expect(screen.nudge(edited), "the editor has a Filter 1 Cutoff row");
    auto* host_parameter = parameter(synth, automated);
    expect(host_parameter != nullptr, "Filter 1 Resonance is a host parameter");
    if (host_parameter == nullptr)
      return;
    // As the host's automation sets it: no gesture of the editor's around it.
    host_parameter->setValue(0.9f);
    const float automated_value = controlValue(synth, automated);
    pumpEditorEvents();
    expect(synth.undoHistory().numUndoSteps() == 1, "the host's change is not a step");

    screen.press(undoKey());
    close(controlValue(synth, edited), edited_before, "undo takes back the user's change");
    close(controlValue(synth, automated), automated_value, "and leaves the automated value alone");
    expect(!synth.undoHistory().canUndo(), "nothing else is left to undo");
  }

  // Loading a preset is a step: undo brings back the patch, its name and the
  // file Save writes to; redo loads the preset again.
  void testPresetLoadIsUndone() {
    SpokenMessages spoken;
    SynthPlugin synth;
    TemporaryFile preset_file("." + String(vital::kPresetExtension));
    synth.parameterChanged("filter_1_cutoff", 40.0f);
    const bool written = synth.saveCopyToFile(preset_file.getFile());
    expect(written, "the test preset is written");
    if (!written)
      return;
    synth.parameterChanged("filter_1_cutoff", 100.0f);
    synth.setPresetName("Before the load");

    UndoTestEditor screen(synth);
    std::string error;
    expect(synth.loadFromFile(preset_file.getFile(), error), "the test preset loads");
    pumpEditorEvents();
    close(controlValue(synth, "filter_1_cutoff"), 40.0f, "the preset sets Filter 1 Cutoff");
    expect(synth.getActiveFile() == preset_file.getFile(), "Save now writes to the loaded preset");

    spoken.messages.clear();
    screen.press(undoKey());
    pumpUntil([&] { return heard(spoken, "Undo"); });
    close(controlValue(synth, "filter_1_cutoff"), 100.0f, "undo brings back the patch from before the load");
    expect(synth.getPresetName() == "Before the load",
           "and its name, got " + synth.getPresetName().toStdString());
    expect(synth.getActiveFile() == File(), "and Save no longer writes over the preset that was loaded");
    expect(heard(spoken, "Undo"), "undo says it, heard: " + heardText(spoken));

    screen.press(redoKey());
    close(controlValue(synth, "filter_1_cutoff"), 40.0f, "redo loads the preset again");
    expect(synth.getActiveFile() == preset_file.getFile(), "and Save writes to it again");
  }

  // Presets the browser autoloads one after another are one step: undo goes
  // back to the patch from before the browsing.
  void testAutoloadedPresetsAreOneStep() {
    SynthPlugin synth;
    TemporaryFile first("." + String(vital::kPresetExtension));
    TemporaryFile second("." + String(vital::kPresetExtension));
    synth.parameterChanged("filter_1_cutoff", 30.0f);
    bool written = synth.saveCopyToFile(first.getFile());
    synth.parameterChanged("filter_1_cutoff", 50.0f);
    written = synth.saveCopyToFile(second.getFile()) && written;
    expect(written, "the test presets are written");
    if (!written)
      return;
    synth.parameterChanged("filter_1_cutoff", 100.0f);

    UndoTestEditor screen(synth);
    for (const File& file : { first.getFile(), second.getFile() }) {
      // What the preset browser does for an autoload.
      synth.undoHistory().describeNextStep("load preset " + file.getFileNameWithoutExtension(), "autoload");
      std::string error;
      synth.loadFromFile(file, error);
      pumpEditorEvents();
    }
    close(controlValue(synth, "filter_1_cutoff"), 50.0f, "the second preset is loaded");
    expect(synth.undoHistory().numUndoSteps() == 1,
           "autoloaded presets are one step, got " + std::to_string(synth.undoHistory().numUndoSteps()));
    screen.press(undoKey());
    close(controlValue(synth, "filter_1_cutoff"), 100.0f, "undo goes back to the patch from before the browsing");
    expect(!synth.undoHistory().canUndo(), "in one step");
  }

  void testInitializePatchIsUndone() {
    TemporaryUndoLibrary library;
    expect(library.isolated(), "the test's library is its own temporary folder");
    if (!library.isolated())
      return;
    SynthPlugin synth;
    synth.parameterChanged("filter_1_cutoff", 30.0f);
    UndoTestEditor screen(synth);
    screen.editor.initializePatchForTesting();
    pumpEditorEvents();
    close(controlValue(synth, "filter_1_cutoff"), vital::Parameters::getDetails("filter_1_cutoff").default_value,
          "Initialize patch resets Filter 1 Cutoff");
    expect(synth.undoHistory().undoDescription() == "initialize patch",
           "the Editor menu names the step, got " + synth.undoHistory().undoDescription().toStdString());
    screen.press(undoKey());
    close(controlValue(synth, "filter_1_cutoff"), 30.0f, "undo brings back the patch from before");
  }

  // Save patch as default writes a file: undo takes away a default that did
  // not exist, or writes back the one it replaced, byte for byte; a default
  // something else wrote since is left alone.
  void testSavePatchAsDefaultIsUndone() {
    TemporaryUndoLibrary library;
    expect(library.isolated(), "the test's library is its own temporary folder");
    if (!library.isolated())
      return;
    SpokenMessages spoken;
    SynthPlugin synth;
    UndoTestEditor screen(synth);
    const File default_file = atlas::library::libraryDirectory(atlas::library::Library::kUser, LoadSave::getDataDirectory())
                                  .getChildFile("Default").withFileExtension(vital::kPresetExtension);
    expect(!default_file.exists(), "the temporary library starts without a default patch");

    screen.editor.savePatchAsDefaultForTesting();
    pumpEditorEvents();
    MemoryBlock first_default;
    expect(default_file.loadFileAsData(first_default), "Save patch as default writes the default patch");
    spoken.messages.clear();
    screen.press(undoKey());
    expect(!default_file.exists(), "undo takes away the default patch it made");
    expect(heard(spoken, "Undo save patch as default"), "undo says what it undid, heard: " + heardText(spoken));
    screen.press(redoKey());
    MemoryBlock redone;
    expect(default_file.loadFileAsData(redone) && redone == first_default, "redo writes it again");

    synth.parameterChanged("filter_1_cutoff", 33.0f);
    screen.editor.savePatchAsDefaultForTesting();
    pumpEditorEvents();
    MemoryBlock second_default;
    expect(default_file.loadFileAsData(second_default) && second_default != first_default,
           "saving the default again replaces it");
    screen.press(undoKey());
    MemoryBlock restored;
    expect(default_file.loadFileAsData(restored) && restored == first_default,
           "undo writes back the default it replaced");

    screen.press(redoKey());
    default_file.replaceWithText("{}");
    spoken.messages.clear();
    screen.press(undoKey());
    expect(default_file.loadFileAsString() == "{}", "undo leaves a default something else wrote since");
    expect(heard(spoken, "changed since"), "and says why, heard: " + heardText(spoken));
  }

  // An LFO edit is a step once the keys stop, and undo puts the shape back.
  void testLfoEditIsUndone() {
    SynthPlugin synth;
    UndoTestEditor screen(synth);
    Component::SafePointer<Component> lfo(titled(screen.editor, "LFO 1 accessible editor"));
    expect(lfo != nullptr, "the editor has LFO 1's keyboard editor");
    if (lfo == nullptr)
      return;
    const std::string before = synth.getLfoSource(0)->stateToJson().dump();
    lfo->keyPressed(KeyPress(KeyPress::upKey));
    pumpEditorEvents();
    const std::string edited = synth.getLfoSource(0)->stateToJson().dump();
    expect(edited != before, "Up moves LFO 1's point");
    expect(synth.undoHistory().numUndoSteps() == 0, "the edit waits for the keys to stop");

    screen.press(undoKey());
    expect(synth.getLfoSource(0)->stateToJson().dump() == before, "undo puts LFO 1's shape back");
    screen.press(redoKey());
    expect(synth.getLfoSource(0)->stateToJson().dump() == edited, "redo moves the point again");
  }

  // Removing an effect is a step: undo puts it back with its settings, and
  // the editor shows it again.
  void testEffectRemovalIsUndone() {
    SynthPlugin synth;
    const std::string slot = "fx_slot_1_";
    synth.parameterChanged(slot + "type", vital::constants::kDelay + 1);
    synth.parameterChanged(slot + "delay_feedback", 0.8f);
    UndoTestEditor screen(synth);
    const String section = "Chain and routing";
    const bool removed = runSlotContextMenuItem(screen.editor, section, 0, "Remove Delay");
    expect(removed, "the slot's menu removes the Delay");
    if (!removed)
      return;
    pumpEditorEvents();
    close(controlValue(synth, slot + "type"), 0.0f, "Remove empties the slot");
    expect(synth.undoHistory().undoDescription() == "remove Delay",
           "the step is named, got " + synth.undoHistory().undoDescription().toStdString());

    screen.press(undoKey());
    pumpEditorEvents();
    close(controlValue(synth, slot + "type"), static_cast<float>(vital::constants::kDelay + 1),
          "undo puts the Delay back");
    close(controlValue(synth, slot + "delay_feedback"), 0.8f, "with its settings");
    expect(editorSection(screen.editor, section, 0) != nullptr, "the editor shows the Delay again");

    screen.press(redoKey());
    pumpEditorEvents();
    close(controlValue(synth, slot + "type"), 0.0f, "redo removes it again");
  }

  // A route and the amount typed for it are undone together.
  void testModulationRouteIsUndone() {
    SynthPlugin synth;
    UndoTestEditor screen(synth);
    const std::string destination = "filter_1_cutoff";
    Component* target = screen.editor.controlForParameter(String(destination));
    expect(target != nullptr, "the editor has a Filter 1 Cutoff row");
    screen.editor.assignModulation("lfo_1", String(destination), "50", target);
    pumpEditorEvents();
    const int route = modulationIndex(synth, "lfo_1", destination);
    expect(route >= 0, "the route is made");
    if (route < 0)
      return;
    const std::string amount = "modulation_" + std::to_string(route + 1) + "_amount";
    const float amount_value = controlValue(synth, amount);
    expect(amount_value != 0.0f, "the route has the typed amount");

    screen.press(undoKey());
    pumpEditorEvents();
    expect(modulationIndex(synth, "lfo_1", destination) < 0, "undo takes the route away");
    screen.press(redoKey());
    pumpEditorEvents();
    const int redone = modulationIndex(synth, "lfo_1", destination);
    expect(redone >= 0, "redo makes the route again");
    if (redone >= 0)
      close(controlValue(synth, "modulation_" + std::to_string(redone + 1) + "_amount"), amount_value,
            "with its amount");
  }

  // The history lives in the plugin: closing and opening the editor keeps it.
  void testHistoryOutlivesTheEditor() {
    SynthPlugin synth;
    const std::string id = "filter_1_cutoff";
    const float before = controlValue(synth, id);
    {
      UndoTestEditor screen(synth);
      expect(screen.nudge(id), "the editor has a Filter 1 Cutoff row");
    }
    UndoTestEditor reopened(synth);
    reopened.press(undoKey());
    close(controlValue(synth, id), before, "undo in a reopened editor takes back a change made before");
  }

  // A state the host restores (a project, the host's own undo) clears the
  // history; changes after it are undone back to that state.
  void testHostStateClearsTheHistory() {
    SynthPlugin synth;
    UndoTestEditor screen(synth);
    const std::string id = "filter_1_cutoff";
    expect(screen.nudge(id), "the editor has a Filter 1 Cutoff row");
    expect(synth.undoHistory().canUndo(), "the change is a step");
    MemoryBlock state;
    synth.getStateInformation(state);
    synth.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    pumpEditorEvents();
    expect(!synth.undoHistory().canUndo(), "a state the host restores clears the history");
    const float restored = controlValue(synth, id);
    expect(screen.nudge(id), "the row is there after the restore");
    screen.press(undoKey());
    close(controlValue(synth, id), restored, "a change after it is undone back to the restored state");
  }

  // Presses on the same control after a pause are steps of their own.
  void testChangesAfterAPauseAreStepsOfTheirOwn() {
    SynthPlugin synth;
    UndoTestEditor screen(synth);
    const std::string id = "filter_1_cutoff";
    expect(screen.nudge(id), "the editor has a Filter 1 Cutoff row");
    MessageManager::getInstance()->runDispatchLoopUntil(static_cast<int>(UndoHistory::kMergeWindowMs) + 300);
    screen.nudge(id);
    expect(synth.undoHistory().numUndoSteps() == 2,
           "a pause starts a new step, got " + std::to_string(synth.undoHistory().numUndoSteps()));
  }

  // The Toolbar, the editor's first group: Settings, then Undo and Redo named
  // after the change they would undo or redo, then Show preset browser, on by
  // default, which leaves only the summary and Preset menu in Presets when
  // off. Its controls keep the focus through the rebuilds they cause.
  void testToolbar() {
    SpokenMessages spoken;
    SynthPlugin synth;
    {
      UndoTestEditor screen(synth);
      SynthEditor& editor = screen.editor;
      auto* toolbar = titled(editor, "Toolbar");
      expect(toolbar != nullptr && toolbar->getParentComponent() == &editor,
             "the Toolbar sits in the window, above the scrolling groups");
      if (toolbar == nullptr)
        return;

      const auto& keys = editor.getKeyboardFocusOrder();
      StringArray key_titles;
      for (size_t i = 0; i < std::min<size_t>(keys.size(), 6); ++i)
        key_titles.add(keys[i]->getTitle());
      expect(key_titles.size() == 6 && key_titles[0] == "Toolbar" && key_titles[1] == "Settings" &&
                 key_titles[2] == "Nothing to undo" && key_titles[3] == "Nothing to redo" &&
                 key_titles[4] == "Show preset browser" && key_titles[5] == "Presets",
             "Tab starts in the Toolbar, then Presets: " + key_titles.joinIntoString(" | ").toStdString());
      expect(!editor.getAccessibleFocusOrder().empty() && editor.getAccessibleFocusOrder().front() == toolbar,
             "a screen reader finds the Toolbar first among the window's groups");
      StringArray inside;
      if (auto* handler = toolbar->getAccessibilityHandler()) {
        expect(handler->getRole() == AccessibilityRole::group, "the Toolbar is a group");
        for (auto* child : handler->getChildren())
          inside.add(child->getTitle());
      }
      expect(inside.joinIntoString(" | ") == "Settings | Nothing to undo | Nothing to redo | Show preset browser",
             "the Toolbar holds its four controls: " + inside.joinIntoString(" | ").toStdString());

      auto* undo = dynamic_cast<Button*>(keys[2]);
      auto* redo = dynamic_cast<Button*>(keys[3]);
      auto* toggle = dynamic_cast<Button*>(keys[4]);
      if (undo == nullptr || redo == nullptr || toggle == nullptr)
        return;

      // Undo names the change once it is a step; the timer's refresh leaves
      // quick presses one step.
      const std::string id = "filter_1_cutoff";
      const float before = controlValue(synth, id);
      expect(screen.nudge(id) && screen.nudge(id), "the editor has a Filter 1 Cutoff row");
      const float after = controlValue(synth, id);
      pumpUntil([&] { return undo->getTitle() != "Nothing to undo"; });
      expect(synth.undoHistory().numUndoSteps() == 1,
             "two quick presses stay one step, got " + std::to_string(synth.undoHistory().numUndoSteps()));
      expect(undo->getTitle() == "Undo " + String(vital::Parameters::getDisplayName(id)) &&
                 undo->getButtonText() == undo->getTitle() && redo->getTitle() == "Nothing to redo",
             "Undo names the change: " + undo->getTitle().toStdString());

      undo->grabKeyboardFocus();
      spoken.messages.clear();
      undo->triggerClick();
      pumpEditorEvents();
      close(controlValue(synth, id), before, "the toolbar's Undo puts Filter 1 Cutoff back");
      expect(heard(spoken, "Undo"), "and says what it undid, heard: " + heardText(spoken));
      expect(undo->getTitle() == "Nothing to undo" && redo->getTitle().startsWith("Redo "),
             "the buttons follow the history: " + undo->getTitle().toStdString() + " | " +
                 redo->getTitle().toStdString());
      expect(undo->hasKeyboardFocus(false), "Undo keeps the focus");
      redo->triggerClick();
      pumpEditorEvents();
      close(controlValue(synth, id), after, "the toolbar's Redo puts the change back");

      // Show preset browser: on by default.
      auto presetTitles = [&] {
        StringArray titles;
        if (auto* presets = titled(editor, "Presets"))
          for (auto* child : presets->getChildren())
            titles.add(child->getTitle());
        return titles;
      };
      expect(toggle->getToggleState() && editor.isPresetBrowserShown() &&
                 presetTitles().contains("Preset results table") && presetTitles().contains("Preset search"),
             "Show preset browser is on, and Presets has the browser");
      toggle->grabKeyboardFocus();
      toggle->triggerClick();
      pumpEditorEvents();
      StringArray hidden = presetTitles();
      expect(!toggle->getToggleState() && !editor.isPresetBrowserShown(), "Show preset browser turns off");
      expect(hidden.contains("Preset summary") && hidden.contains("Preset menu"),
             "Presets keeps its summary and Preset menu: " + hidden.joinIntoString(", ").toStdString());
      for (const auto* title : { "Preset search", "Collection", "Library", "Bank", "Category", "Tags",
                                 "Preset results table", "Selected preset details", "Autoload preset when scrolling" })
        expect(!hidden.contains(title), std::string("the hidden browser leaves out ") + title);
      bool table_in_order = false;
      for (auto* component : editor.getKeyboardFocusOrder())
        table_in_order |= component->getTitle() == "Preset results table";
      expect(!table_in_order, "Tab skips the hidden browser");
      expect(toggle->hasKeyboardFocus(false), "the toggle keeps the focus");
      toggle->triggerClick();
      pumpEditorEvents();
      expect(editor.isPresetBrowserShown() && presetTitles().contains("Preset results table"),
             "turned on again, the browser is back");
      toggle->triggerClick();
      pumpEditorEvents();
    }
    // The instance remembers; another starts with the browser shown.
    SynthEditor reopened(synth);
    auto* toggle = dynamic_cast<Button*>(titled(reopened, "Show preset browser"));
    expect(!reopened.isPresetBrowserShown() && toggle != nullptr && !toggle->getToggleState(),
           "reopening the window keeps the browser hidden");
    SynthPlugin other;
    SynthEditor fresh(other);
    expect(fresh.isPresetBrowserShown(), "another instance shows the browser");
  }

  int runUndoTests() {
    testToolbar();
    testUndoMovesAValueBack();
    testUndoKeepsEachControlsChangeApart();
    testHostAutomationIsNotAStep();
    testPresetLoadIsUndone();
    testAutoloadedPresetsAreOneStep();
    testInitializePatchIsUndone();
    testSavePatchAsDefaultIsUndone();
    testLfoEditIsUndone();
    testEffectRemovalIsUndone();
    testModulationRouteIsUndone();
    testHistoryOutlivesTheEditor();
    testHostStateClearsTheHistory();
    testChangesAfterAPauseAreStepsOfTheirOwn();
    if (failures == 0)
      std::cout << "Atlas undo tests passed\n";
    return failures == 0 ? 0 : 1;
  }
}
