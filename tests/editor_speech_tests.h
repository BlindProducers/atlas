#pragma once

#include "screen_reader_speech.h"
#if JUCE_WINDOWS
#include "windows_screen_reader.h"
#endif

namespace {
  // Captures what Atlas would say. Announcements go to the observer instead of
  // the screen reader, and the default speech settings replace the saved ones.
  struct SpokenMessages {
    SpokenMessages() {
      atlas::speech::useDefaultSettingsForTesting();
      atlas::speech::setObserverForTesting([this](const String& message) { messages.add(message); });
    }
    ~SpokenMessages() { atlas::speech::setObserverForTesting(nullptr); }

    StringArray messages;
  };

  KeyPress characterKey(juce_wchar character) {
    return KeyPress(static_cast<int>(character), ModifierKeys(), character);
  }

  // Each navigation key in a keyboard editor says where the cursor landed:
  // exactly one message, containing `expected`.
  void expectNavigationSpoken(Component& editor, SpokenMessages& spoken, const KeyPress& key,
                              const String& expected, const String& what) {
    spoken.messages.clear();
    const String name = what + " " + key.getTextDescription();
    expect(editor.keyPressed(key), (name + " is handled").toStdString());
    std::cout << "  " << name << ": " << spoken.messages.joinIntoString(" | ") << '\n';
    expect(spoken.messages.size() == 1 && spoken.messages[0].contains(expected),
           (name + " announces " + expected + " (heard: " + spoken.messages.joinIntoString(" | ") + ")").toStdString());
  }

  // The LFO, Trance gate and wavetable keyboard editors have no other spoken
  // feedback, so moving between their points, steps, frames and harmonics is
  // announced with the default settings.
  void testKeyboardEditorsAnnounceNavigation() {
    SpokenMessages spoken;
    SynthPlugin synth;
    SynthEditor editor(synth);
    synth.getControls().at("fx_slot_1_type")->set(vital::constants::kTranceGate + 1);
    editor.updateFullGui();
    editor.addToDesktop(0);
    editor.setVisible(true);
    pumpEditorEvents();

    auto* lfo = titled(editor, "LFO Editor");
    expect(lfo != nullptr, "the editor has the LFO keyboard editor");
    if (lfo != nullptr) {
      expectNavigationSpoken(*lfo, spoken, KeyPress(KeyPress::rightKey), "Bar 1", "LFO editor");
      expectNavigationSpoken(*lfo, spoken, KeyPress(KeyPress::leftKey), "Bar 1 beat 1 0%", "LFO editor");
      expectNavigationSpoken(*lfo, spoken, characterKey('.'), "Bar ", "LFO editor next point");
      expectNavigationSpoken(*lfo, spoken, characterKey(','), "Bar ", "LFO editor previous point");
      expectNavigationSpoken(*lfo, spoken, characterKey('='), "Grid ", "LFO editor finer grid");
      // KeyPress == ignores keys with a modifier held, so Shift's 5% step was never reached.
      expectNavigationSpoken(*lfo, spoken, KeyPress(KeyPress::upKey, ModifierKeys::shiftModifier, 0), "5%",
                             "LFO editor five percent up");
    }

    auto* slot = editorSection(editor, "Chain and routing", 0);
    Component::SafePointer<Component> gate(slot != nullptr ? titled(*slot, "Trance gate Editor") : nullptr);
    expect(gate != nullptr, "the Trance gate slot has its keyboard editor");
    if (gate != nullptr) {
      expectNavigationSpoken(*gate, spoken, KeyPress(KeyPress::rightKey), "Step 2,", "Trance gate editor");
      expectNavigationSpoken(*gate, spoken, KeyPress(KeyPress::rightKey), "Step 3,", "Trance gate editor");
      expectNavigationSpoken(*gate, spoken, KeyPress(KeyPress::leftKey), "Step 2,", "Trance gate editor");
      expectNavigationSpoken(*gate, spoken, KeyPress(KeyPress::upKey), "", "Trance gate editor value");
      expectNavigationSpoken(*gate, spoken, characterKey('h'), "", "Trance gate editor step shape");
    }

    auto* show = dynamic_cast<Button*>(titled(*editorSection(editor, "Oscillator 1"), "Show wavetable editor"));
    expect(show != nullptr && show->onClick != nullptr, "Oscillator 1 offers the wavetable editor");
    if (show != nullptr && show->onClick)
      show->onClick();
    pumpUntil([&] { return editorSection(editor, "Oscillator 1 wavetable editor") != nullptr; });
    auto* wavetable_section = editorSection(editor, "Oscillator 1 wavetable editor");
    Component::SafePointer<Component> wavetable(
        wavetable_section != nullptr ? titled(*wavetable_section, "Wavetable keyboard editor") : nullptr);
    expect(wavetable != nullptr, "the wavetable editor has its keyboard editor");
    if (wavetable != nullptr) {
      expectNavigationSpoken(*wavetable, spoken, KeyPress(KeyPress::endKey), "Frame 1 of 1", "Wavetable editor");
      expectNavigationSpoken(*wavetable, spoken, KeyPress(KeyPress::rightKey), "Frame 1 of 1", "Wavetable editor");
      expectNavigationSpoken(*wavetable, spoken, characterKey('.'), "Harmonic 2", "Wavetable editor next harmonic");
      expectNavigationSpoken(*wavetable, spoken, characterKey(','), "Harmonic 1", "Wavetable editor previous harmonic");
      expectNavigationSpoken(*wavetable, spoken, KeyPress(KeyPress::pageUpKey), "Harmonic 11", "Wavetable editor ten harmonics up");
    }

    // Use speech feedback off silences them all.
    atlas::speech::settings().speech_feedback = false;
    spoken.messages.clear();
    if (lfo != nullptr) lfo->keyPressed(KeyPress(KeyPress::rightKey));
    if (gate != nullptr) gate->keyPressed(KeyPress(KeyPress::rightKey));
    if (wavetable != nullptr) wavetable->keyPressed(KeyPress(KeyPress::leftKey));
    expect(spoken.messages.isEmpty(), "Use speech feedback off silences the keyboard editors");
    atlas::speech::settings().speech_feedback = true;
  }

  // A list's selection keys say the row on Windows, where JUCE leaves NVDA
  // silent. VoiceOver reads the row itself, so macOS adds nothing.
  void expectListRowSpoken(Component& list, SpokenMessages& spoken, const KeyPress& key,
                           const String& expected, const String& what) {
#if JUCE_WINDOWS
    expectNavigationSpoken(list, spoken, key, expected, what);
#else
    spoken.messages.clear();
    const String name = what + " " + key.getTextDescription();
    expect(list.keyPressed(key), (name + " is handled").toStdString());
    expect(spoken.messages.isEmpty(), (name + " leaves the row to VoiceOver").toStdString());
#endif
  }

  void testListsSayTheSelectedRow() {
    SpokenMessages spoken;
    SynthPlugin synth;
    SynthEditor editor(synth);
    editor.updateFullGui();
    editor.addToDesktop(0);
    editor.setVisible(true);
    editor.refreshPresetLibrary();
    pumpUntil([&] { return !editor.isPresetLibraryBusy(); }, 60000);

    auto* autoload = dynamic_cast<Button*>(titled(editor, "Autoload preset when scrolling"));
    const bool autoload_was_on = autoload != nullptr && autoload->getToggleState();
    if (autoload != nullptr)
      autoload->setToggleState(false, dontSendNotification);

    auto* collection = dynamic_cast<TableListBox*>(titled(editor, "Collection"));
    expect(collection != nullptr && collection->getNumRows() == 2,
           "the preset browser lists All presets and Favourites");
    if (collection != nullptr && collection->getNumRows() == 2) {
      collection->selectRow(0);
      expectListRowSpoken(*collection, spoken, KeyPress(KeyPress::downKey), "Favourites, 2 of 2", "Collection");
      expectListRowSpoken(*collection, spoken, KeyPress(KeyPress::downKey), "Favourites, 2 of 2", "Collection at its end");
      expectListRowSpoken(*collection, spoken, KeyPress(KeyPress::homeKey), "All presets, 1 of 2", "Collection");
    }

    auto* results = dynamic_cast<TableListBox*>(titled(editor, "Preset results table"));
    expect(results != nullptr, "the preset browser has its results table");
    const int rows = results != nullptr ? results->getNumRows() : 0;
    if (rows >= 2) {
      auto rowText = [&](int row) {
        return editor.presetFileAtRow(row).getFileNameWithoutExtension() + ", " + String(row + 1) + " of " + String(rows);
      };
      results->selectRow(0);
      expectListRowSpoken(*results, spoken, KeyPress(KeyPress::downKey), rowText(1), "Preset results");
      expectListRowSpoken(*results, spoken, KeyPress(KeyPress::endKey), rowText(rows - 1), "Preset results");
      expectListRowSpoken(*results, spoken, KeyPress(KeyPress::upKey), rowText(rows - 2), "Preset results");
    }
    else {
      std::cout << "  fewer than two presets installed; skipped the preset results rows\n";
    }

    // Use speech feedback off silences them too.
    atlas::speech::settings().speech_feedback = false;
    spoken.messages.clear();
    if (collection != nullptr) collection->keyPressed(KeyPress(KeyPress::endKey));
    expect(spoken.messages.isEmpty(), "Use speech feedback off silences the lists");
    atlas::speech::settings().speech_feedback = true;

    if (autoload != nullptr)
      autoload->setToggleState(autoload_was_on, dontSendNotification);
    editor.removeFromDesktop();
  }

  ToggleButton* findToggle(Component& parent, const std::function<bool(ToggleButton&)>& matches) {
    if (auto* toggle = dynamic_cast<ToggleButton*>(&parent))
      if (matches(*toggle))
        return toggle;
    for (auto* child : parent.getChildren())
      if (auto* found = findToggle(*child, matches))
        return found;
    return nullptr;
  }

  // Flipping a toggle says its new state on Windows, where NVDA never speaks a
  // check box's new value; VoiceOver reads the state itself, so macOS adds nothing.
  void testTogglesSayTheirState() {
    SpokenMessages spoken;
    SynthPlugin synth;
    synth.getControls().at("fx_slot_1_type")->set(vital::constants::kTranceGate + 1);
    SynthEditor editor(synth);
    editor.updateFullGui();
    editor.addToDesktop(0);
    editor.setVisible(true);
    pumpEditorEvents();

    Component::SafePointer<ToggleButton> slot_toggle(findToggle(editor, [](ToggleButton& toggle) {
      return toggle.getProperties()["accessibleOnText"].toString() == "Enabled";
    }));
    expect(slot_toggle != nullptr, "the effect slot has its Enabled toggle");
    if (slot_toggle != nullptr) {
      const bool was_on = slot_toggle->getToggleState();
      const String expected = was_on ? "Bypassed" : "Enabled";
      spoken.messages.clear();
      Component& focused_toggle = *slot_toggle;   // Button::keyPressed is protected
      expect(focused_toggle.keyPressed(KeyPress(KeyPress::returnKey)), "Enter flips the effect slot toggle");
      pumpUntil([&] { return spoken.messages.contains(expected); }, 1000);
      std::cout << "  Effect slot toggle: " << spoken.messages.joinIntoString(" | ") << '\n';
#if JUCE_WINDOWS
      expect(spoken.messages.contains(expected), ("the toggle says " + expected).toStdString());
#else
      expect(!spoken.messages.contains(expected), "VoiceOver reads the toggle's state itself");
#endif
    }
    editor.removeFromDesktop();
  }

  // Without NVDA, other screen readers get a UI Automation notification raised
  // on the focused control's JUCE element. Check that JUCE's element accepts it.
  void testUiaNotificationOnJuceElement() {
#if JUCE_WINDOWS
    TextButton control("Notification target");
    control.setSize(120, 30);
    control.addToDesktop(0);
    control.setVisible(true);
    pumpEditorEvents();
    auto* handler = control.getAccessibilityHandler();
    expect(handler != nullptr && handler->getNativeImplementation() != nullptr,
           "a JUCE control on the desktop has a native accessibility element");
    if (handler != nullptr)
      expect(atlas::windows_screen_reader::raiseNotification(handler->getNativeImplementation(),
                                                             L"Atlas notification test", false),
             "a UI Automation notification can be raised on a JUCE control");
    control.removeFromDesktop();
#endif
  }

  // With NVDA running, an announcement goes to NVDA itself (never SAPI).
  void testAnnouncementsReachNvda() {
    if (!atlas::speech::isNvdaRunning()) {
      std::cout << "  NVDA is not running; skipped the NVDA speech check\n";
      return;
    }
    const auto route = atlas::speech::announce("Atlas speech test",
                                               AccessibilityHandler::AnnouncementPriority::high);
    expect(route == atlas::speech::Route::kNvda, "with NVDA running, announcements are spoken by NVDA");
    std::cout << "  NVDA spoke the test announcement\n";
  }
}
