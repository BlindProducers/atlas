#pragma once

// The amount prompt of Assign modulation has a slider after its text box. The slider moves through
// the values the modulation can take its destination to, steps and reads them
// as the destination's own slider does (Filter 1 Cutoff in Hz, 10 Hz a step),
// and the route plays each amount at once, to be heard. Return on it keeps
// the amount, as OK does; anything else puts back the one the route had.
// Included into macro_modulation_tests.cpp
// after editor_accessibility_tests.h and release_hardening_tests.h, whose
// helpers it uses.

namespace {
  // What a slider says its value is, as a screen reader reads it.
  String spokenValue(Component& component) {
    auto* handler = component.getAccessibilityHandler();
    auto* value = handler != nullptr ? handler->getValueInterface() : nullptr;
    return value != nullptr ? value->getCurrentValueAsString() : String();
  }

  int positionIn(const std::vector<Component*>& order, const Component* component) {
    const auto found = std::find(order.begin(), order.end(), component);
    return found == order.end() ? -1 : static_cast<int>(found - order.begin());
  }

  float routeAmount(SynthPlugin& synth, const String& source, const String& destination) {
    const int connection = synth.getConnectionIndex(source.toStdString(), destination.toStdString());
    if (connection < 0)
      return std::numeric_limits<float>::quiet_NaN();
    return synth.getControls().at("modulation_" + std::to_string(connection + 1) + "_amount")->value();
  }

  double hertz(const String& text) {
    return text.endsWith(" Hz") ? text.dropLastCharacters(3).getDoubleValue() : -1.0;
  }

  void testModulationAmountSlider() {
    TemporaryConfig config;
    atlas::speech::useDefaultSettingsForTesting();
    atlas::speech::settings().frequency_values_in_hz = true;
    StringArray said;
    atlas::speech::setObserverForTesting([&said](const String& message) { said.add(message); });

    SynthPlugin synth;
    SynthEditor editor(synth);
    editor.updateFullGui();
    // JUCE gives a control its accessibility handler, the one a screen reader
    // reads, only on a window.
    editor.addToDesktop(0);
    editor.setVisible(true);
    pumpEditorEvents();
    auto& field = editor.modulationAmountEditorForTesting();
    auto& slider = editor.modulationAmountSliderForTesting();
    const String cutoff = "filter_1_cutoff";
    auto* cutoff_bridge = parameter(synth, cutoff.toStdString());
    expect(cutoff_bridge != nullptr, "Filter 1 Cutoff is a host parameter");
    if (cutoff_bridge == nullptr) {
      editor.removeFromDesktop();
      return;
    }
    // A new route rebuilds the rows: Cutoff's slider is looked up each time.
    auto cutoffRow = [&] { return editor.controlForParameter(cutoff); };
    auto cutoffText = [&](double value) {
      auto* row = dynamic_cast<ParameterSlider*>(cutoffRow());
      return row != nullptr ? row->getTextFromValue(value) : String();
    };
    auto closePrompt = [&] {
      editor.keyPressed(KeyPress(KeyPress::escapeKey));
      pumpEditorEvents();
    };

    // The slider follows the box, in the Tab order too, and reads in Hz, as
    // Cutoff's own slider does, starting where Cutoff is now.
    editor.openModulationAmountPromptForTesting("lfo_2", cutoff, cutoffRow());
    expect(slider.isVisible(), "the prompt shows the slider");
    for (const auto* order : { &editor.getKeyboardFocusOrder(), &editor.getAccessibleFocusOrder() }) {
      const int box = positionIn(*order, &field);
      expect(box >= 0 && positionIn(*order, &slider) == box + 1, "the slider comes right after the text box");
    }
    expect(slider.getTitle() == "Modulation amount", "the slider is called Modulation amount");
    pumpUntil([&] { return !said.isEmpty(); });
    expect(said.joinIntoString(" | ").contains("Type the value the modulation moves to, or Tab to its slider"),
           "the prompt names the slider: " + said.joinIntoString(" | ").toStdString());
    auto* row = dynamic_cast<ParameterSlider*>(cutoffRow());
    const String now = row != nullptr ? spokenValue(*row) : String();
    expect(now.endsWith(" Hz") && spokenValue(slider) == now && field.getText() == now,
           "the slider starts at Cutoff's value, read as Cutoff reads it: " + spokenValue(slider).toStdString() +
               " (Cutoff " + now.toStdString() + ", box " + field.getText().toStdString() + ")");
    expect(row != nullptr && row->getAdjustmentStep() == slider.getAdjustmentStep() && slider.getAdjustmentStep() == 10.0,
           "the slider steps as Cutoff's own slider does, 10 Hz");
    row = nullptr;

    // Each key moves it as on Cutoff: 10 Hz, 100 Hz with Page Up, 1 Hz with
    // Shift. The box says the value each time, ready for OK, and the route
    // plays the new amount at once, so it can be heard.
    const double start_hz = hertz(spokenValue(slider));
    auto playsTheSlider = [&](const String& source, const String& destination, const std::string& when) {
      auto* bridge = parameter(synth, destination.toStdString());
      close(routeAmount(synth, source, destination),
            bridge != nullptr ? static_cast<float>(slider.getValue() - bridge->getValue()) : -2.0f,
            when + ", the route plays the slider's amount", 1.0e-5f);
    };
    const struct {
      KeyPress key;
      double change;
      const char* name;
    } steps[] = { { KeyPress(KeyPress::rightKey), 10.0, "Right" },
                  { KeyPress(KeyPress::pageUpKey), 100.0, "Page Up" },
                  { KeyPress(KeyPress::leftKey, ModifierKeys::shiftModifier, 0), -1.0, "Shift Left" } };
    double expected_hz = start_hz;
    for (const auto& step : steps) {
      slider.keyPressed(step.key);
      expected_hz += step.change;
      const double heard = hertz(spokenValue(slider));
      expect(std::abs(heard - expected_hz) <= 1.0,
             std::string(step.name) + " moves the slider to " + std::to_string(expected_hz) + " Hz, read " +
                 spokenValue(slider).toStdString());
      expect(field.getText() == spokenValue(slider), std::string("after ") + step.name + " the box says the slider's value");
      playsTheSlider("lfo_2", cutoff, std::string("after ") + step.name);
    }

    // Return on the slider sets the amount that takes Cutoff there.
    const double target = slider.getValue();
    const String target_text = spokenValue(slider);
    slider.keyPressed(KeyPress(KeyPress::returnKey));
    pumpEditorEvents();
    expect(!editor.isModulationAmountPromptOpen(), "Return on the slider closes the prompt");
    close(routeAmount(synth, "lfo_2", cutoff), static_cast<float>(target - cutoff_bridge->getValue()),
          "Return on the slider sets the amount it moved to", 1.0e-5f);
    const String peak_text = cutoffText(cutoff_bridge->getValue() + routeAmount(synth, "lfo_2", cutoff));
    expect(peak_text == target_text, "the modulation takes Cutoff to " + target_text.toStdString() + ", reaches " +
                                         peak_text.toStdString());

    // Opened again, the slider starts at the value the modulation reaches.
    // A value typed into the box moves the slider there, and OK sets it:
    // typing plays nothing until then, as half-typed values would jump about.
    editor.openModulationAmountPromptForTesting("lfo_2", cutoff, cutoffRow());
    expect(spokenValue(slider) == target_text && field.getText() == target_text,
           "the slider starts at the value the modulation reaches, " + spokenValue(slider).toStdString());
    const float before_typing = routeAmount(synth, "lfo_2", cutoff);
    field.setText("1000 Hz", true);
    pumpEditorEvents();
    expect(spokenValue(slider) == "1000 Hz", "typing 1000 Hz moves the slider there, read " +
                                                 spokenValue(slider).toStdString());
    expect(routeAmount(synth, "lfo_2", cutoff) == before_typing, "typing changes nothing until OK");
    field.keyPressed(KeyPress(KeyPress::returnKey));
    pumpEditorEvents();
    const String typed_peak = cutoffText(cutoff_bridge->getValue() + routeAmount(synth, "lfo_2", cutoff));
    expect(typed_peak == "1000 Hz", "OK sets the typed value, 1000 Hz, reaches " + typed_peak.toStdString());

    // Opened and closed again without a change: the amount stays as it was,
    // though its text is rounded to the hertz (half a hertz here is 7e-5).
    const float before_ok = routeAmount(synth, "lfo_2", cutoff);
    editor.openModulationAmountPromptForTesting("lfo_2", cutoff, cutoffRow());
    field.keyPressed(KeyPress(KeyPress::returnKey));
    pumpEditorEvents();
    close(routeAmount(synth, "lfo_2", cutoff), before_ok, "OK with nothing changed keeps the amount", 1.0e-6f);
    const float kept = routeAmount(synth, "lfo_2", cutoff);

    // Backspace goes back to where Cutoff is now (no modulation); Home and
    // End reach either end of its range; Escape puts back the amount the
    // route had, whatever the slider played, and leaves nothing to undo.
    const size_t steps_before_escape = synth.undoHistory().numUndoSteps();
    editor.openModulationAmountPromptForTesting("lfo_2", cutoff, cutoffRow());
    slider.keyPressed(KeyPress(KeyPress::backspaceKey));
    expect(spokenValue(slider) == now && field.getText() == now,
           "Backspace returns the slider to Cutoff's value now, " + spokenValue(slider).toStdString());
    close(routeAmount(synth, "lfo_2", cutoff), 0.0f, "after Backspace the route plays no modulation", 1.0e-6f);
    slider.keyPressed(KeyPress(KeyPress::homeKey));
    expect(slider.getValue() == 1.0, "Home reaches Cutoff's maximum, " + spokenValue(slider).toStdString());
    playsTheSlider("lfo_2", cutoff, "after Home");
    slider.keyPressed(KeyPress(KeyPress::endKey));
    expect(slider.getValue() == 0.0, "End reaches Cutoff's minimum, " + spokenValue(slider).toStdString());
    playsTheSlider("lfo_2", cutoff, "after End");
    expect(slider.keyPressed(KeyPress(KeyPress::escapeKey)), "Escape on the slider is Cancel");
    pumpEditorEvents();
    expect(!editor.isModulationAmountPromptOpen() && routeAmount(synth, "lfo_2", cutoff) == kept,
           "Escape closes the prompt and puts back the amount the route had");
    expect(synth.undoHistory().numUndoSteps() == steps_before_escape,
           "and leaves no step of its own to undo (" + std::to_string(synth.undoHistory().numUndoSteps()) + " steps, " +
               std::to_string(steps_before_escape) + " before)");

    // A stepped destination steps as its control does: Transpose, a semitone.
    const String transpose = "osc_1_transpose";
    auto* transpose_bridge = parameter(synth, transpose.toStdString());
    editor.openModulationAmountPromptForTesting("lfo_1", transpose, editor.controlForParameter(transpose));
    const double transpose_start = slider.getValue();
    slider.keyPressed(KeyPress(KeyPress::rightKey));
    expect(std::abs((slider.getValue() - transpose_start) * 96.0 - 1.0) < 1.0e-6,
           "Right moves Transpose's slider one semitone");
    expect(transpose_bridge != nullptr &&
               spokenValue(slider) == transpose_bridge->getText(static_cast<float>(slider.getValue()), 128),
           "and it reads as Transpose does: " + spokenValue(slider).toStdString());
    slider.keyPressed(KeyPress(KeyPress::returnKey));
    pumpEditorEvents();
    close(routeAmount(synth, "lfo_1", transpose) * 96.0f, 1.0f, "the amount takes Transpose one semitone up");

    // A level steps one percent.
    const String resonance = "filter_1_resonance";
    editor.openModulationAmountPromptForTesting("lfo_1", resonance, editor.controlForParameter(resonance));
    const String resonance_start = spokenValue(slider);
    slider.keyPressed(KeyPress(KeyPress::rightKey));
    expect(resonance_start.endsWith("%") &&
               std::abs(spokenValue(slider).getDoubleValue() - resonance_start.getDoubleValue() - 1.0) < 0.01,
           "Right moves Resonance's slider one percent: " + resonance_start.toStdString() + " to " +
               spokenValue(slider).toStdString());
    playsTheSlider("lfo_1", resonance, "after Right on Resonance");
    closePrompt();
    expect(routeAmount(synth, "lfo_1", resonance) == 0.0f, "Escape takes a new route's played amount back to none");

    // The other prompts that share the box have no slider.
    if (auto* cutoff_control = cutoffRow()) {
      cutoff_control->keyPressed(KeyPress(KeyPress::returnKey));
      expect(!editor.isModulationAmountPromptOpen() && field.getTitle().endsWith(" value") &&
                 field.getTitle() != "Modulation amount, as a destination value",
             "Return on Cutoff opens its value prompt, " + field.getTitle().toStdString());
      expect(!slider.isVisible() && positionIn(editor.getKeyboardFocusOrder(), &slider) < 0,
             "the value prompt has no modulation slider");
      closePrompt();
    }

    pumpEditorEvents();
    editor.removeFromDesktop();
    atlas::speech::setObserverForTesting({});
  }

  // What the slider plays, a key at a time as the user presses them, is part
  // of the one step undo takes back with the route. Unless OK sets it, the
  // route gets back the amount it had: Escape, another prompt in its place,
  // or the window closing.
  void testPlayedAmountIsOneStepOrNone() {
    TemporaryConfig config;
    atlas::speech::useDefaultSettingsForTesting();
    atlas::speech::setObserverForTesting([](const String&) {});
    SynthPlugin synth;
    const String cutoff = "filter_1_cutoff";
    float kept = 0.0f;
    {
      SynthEditor editor(synth);
      editor.updateFullGui();
      editor.addToDesktop(0);
      editor.setVisible(true);
      pumpEditorEvents();
      auto& slider = editor.modulationAmountSliderForTesting();
      auto& history = synth.undoHistory();
      auto openPrompt = [&] {
        editor.openModulationAmountPromptForTesting("lfo_1", cutoff, editor.controlForParameter(cutoff));
        pumpEditorEvents();
      };
      auto press = [&](int key) {
        slider.keyPressed(KeyPress(key));
        pumpEditorEvents();
      };

      const size_t steps_before = history.numUndoSteps();
      openPrompt();
      for (int i = 0; i < 4; ++i)
        press(KeyPress::rightKey);
      const float played = routeAmount(synth, "lfo_1", cutoff);
      expect(played > 0.0f, "four steps up play a positive amount while the prompt is open");
      press(KeyPress::returnKey);
      close(routeAmount(synth, "lfo_1", cutoff), played, "Return keeps the amount the slider played", 1.0e-6f);
      expect(history.numUndoSteps() == steps_before + 1,
             "the route and every amount the slider played are one step (" + std::to_string(history.numUndoSteps()) +
                 " steps, " + std::to_string(steps_before) + " before)");
      history.undo();
      pumpEditorEvents();
      expect(synth.getConnectionIndex("lfo_1", cutoff.toStdString()) < 0, "one undo takes the route away");
      history.redo();
      pumpEditorEvents();
      close(routeAmount(synth, "lfo_1", cutoff), played, "redo brings it back with the amount the slider set", 1.0e-6f);

      // Escape: the played amount goes back, and nothing is left to undo.
      kept = routeAmount(synth, "lfo_1", cutoff);
      const size_t steps = history.numUndoSteps();
      openPrompt();
      press(KeyPress::rightKey);
      press(KeyPress::rightKey);
      expect(routeAmount(synth, "lfo_1", cutoff) != kept, "the slider plays a new amount");
      editor.keyPressed(KeyPress(KeyPress::escapeKey));
      pumpEditorEvents();
      expect(routeAmount(synth, "lfo_1", cutoff) == kept, "Escape puts back the amount the route had");
      expect(history.numUndoSteps() == steps, "and leaves the history as it was (" +
                                                  std::to_string(history.numUndoSteps()) + " steps, " +
                                                  std::to_string(steps) + " before)");

      // Another prompt opening in its place puts the amount back too.
      openPrompt();
      press(KeyPress::rightKey);
      if (auto* row = editor.controlForParameter(cutoff))
        row->keyPressed(KeyPress(KeyPress::returnKey));
      expect(routeAmount(synth, "lfo_1", cutoff) == kept,
             "the value prompt opening in its place puts the amount back");
      editor.keyPressed(KeyPress(KeyPress::escapeKey));
      pumpEditorEvents();

      // So does closing the window with the prompt open.
      openPrompt();
      press(KeyPress::rightKey);
      expect(routeAmount(synth, "lfo_1", cutoff) != kept, "the slider plays again");
      editor.removeFromDesktop();
    }
    expect(routeAmount(synth, "lfo_1", cutoff) == kept,
           "closing the editor with the prompt open puts back the amount the route had");
    atlas::speech::setObserverForTesting({});
  }

  float routeBipolar(SynthPlugin& synth, const String& source, const String& destination) {
    const int connection = synth.getConnectionIndex(source.toStdString(), destination.toStdString());
    if (connection < 0)
      return std::numeric_limits<float>::quiet_NaN();
    return synth.getControls().at("modulation_" + std::to_string(connection + 1) + "_bipolar")->value();
  }

  // Bipolar follows the slider in the prompt. It starts as the route is, sets
  // the route's polarity at once, is kept by OK and put back by Escape, and
  // joins the prompt's one undo step. A macro's prompt has none.
  void testBipolarToggle() {
    TemporaryConfig config;
    atlas::speech::useDefaultSettingsForTesting();
    atlas::speech::setObserverForTesting([](const String&) {});
    SynthPlugin synth;
    SynthEditor editor(synth);
    editor.updateFullGui();
    editor.addToDesktop(0);
    editor.setVisible(true);
    pumpEditorEvents();
    auto& slider = editor.modulationAmountSliderForTesting();
    auto& toggle = editor.modulationBipolarToggleForTesting();
    auto& history = synth.undoHistory();
    const String cutoff = "filter_1_cutoff";
    auto openPrompt = [&](const String& source) {
      editor.openModulationAmountPromptForTesting(source, cutoff, editor.controlForParameter(cutoff));
      pumpEditorEvents();
    };

    const size_t steps_before = history.numUndoSteps();
    openPrompt("lfo_1");
    expect(toggle.isVisible() && toggle.getTitle() == "Bipolar", "the prompt has a Bipolar toggle");
    for (const auto* order : { &editor.getKeyboardFocusOrder(), &editor.getAccessibleFocusOrder() }) {
      const int at = positionIn(*order, &slider);
      expect(at >= 0 && positionIn(*order, &toggle) == at + 1, "Bipolar comes right after the slider");
    }
    const bool lfo_default = routeBipolar(synth, "lfo_1", cutoff) >= 0.5f;
    expect(toggle.getToggleState() == lfo_default, "Bipolar starts as the new route is");
    toggle.triggerClick();
    pumpEditorEvents();
    expect((routeBipolar(synth, "lfo_1", cutoff) >= 0.5f) != lfo_default, "switching it sets the route at once");
    editor.keyPressed(KeyPress(KeyPress::escapeKey));
    pumpEditorEvents();
    expect((routeBipolar(synth, "lfo_1", cutoff) >= 0.5f) == lfo_default, "Escape puts the polarity back");

    openPrompt("lfo_1");
    toggle.triggerClick();
    pumpEditorEvents();
    slider.keyPressed(KeyPress(KeyPress::returnKey));
    pumpEditorEvents();
    expect((routeBipolar(synth, "lfo_1", cutoff) >= 0.5f) != lfo_default, "OK keeps the polarity switched");
    expect(history.numUndoSteps() == steps_before + 1,
           "the route and its polarity are one step (" + std::to_string(history.numUndoSteps()) + " steps, " +
               std::to_string(steps_before) + " before)");
    history.undo();
    pumpEditorEvents();
    expect(synth.getConnectionIndex("lfo_1", cutoff.toStdString()) < 0, "one undo takes the route away");

    openPrompt("macro_control_1");
    expect(editor.isModulationAmountPromptOpen() && !toggle.isVisible() &&
               positionIn(editor.getKeyboardFocusOrder(), &toggle) < 0,
           "a macro's prompt has no Bipolar: the macro's own setting decides");
    editor.keyPressed(KeyPress(KeyPress::escapeKey));
    pumpEditorEvents();
    editor.removeFromDesktop();
    atlas::speech::setObserverForTesting({});
  }

  int runModulationAmountSliderTests() {
    testModulationAmountSlider();
    testPlayedAmountIsOneStepOrNone();
    testBipolarToggle();
    if (failures == 0)
      std::cout << "Atlas modulation amount slider tests passed\n";
    return failures == 0 ? 0 : 1;
  }
}
