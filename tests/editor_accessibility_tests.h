#pragma once

#if JUCE_MAC
void focusNativeAccessibilityElement(juce::AccessibilityHandler& handler);
juce::StringArray nativeAccessibilityChildTitles(juce::AccessibilityHandler& handler);
bool nativeAccessibilityFocusSettles(juce::AccessibilityHandler& handler);
#endif

namespace {
  void pumpEditorEvents() {
    MessageManager::getInstance()->runDispatchLoopUntil(150);
  }

  // Pumps the message loop until `done` holds, then a little longer so trailing
  // async work (focus restoration, rebuild flags) settles. Gives up after `timeout_ms`.
  template <typename Predicate>
  void pumpUntil(Predicate done, int timeout_ms = 2000) {
    const uint32 start = Time::getMillisecondCounter();
    while (!done() && Time::getMillisecondCounter() - start < static_cast<uint32>(timeout_ms))
      MessageManager::getInstance()->runDispatchLoopUntil(10);
    MessageManager::getInstance()->runDispatchLoopUntil(50);
  }

  Component* editorSection(Component& parent, const String& section, int slot = -1) {
    if (parent.getProperties()["SectionName"].toString() == section &&
        (slot < 0 || (parent.getProperties().contains("FxSlot") &&
                     static_cast<int>(parent.getProperties()["FxSlot"]) == slot)))
      return &parent;
    for (auto* child : parent.getChildren()) {
      if (auto* found = editorSection(*child, section, slot))
        return found;
    }
    return nullptr;
  }

  Component* fxBusControls(Component& parent, const String& section) {
    if (parent.getTitle() == "Bus controls" && parent.getProperties()["SectionName"].toString() == section)
      return &parent;
    for (auto* child : parent.getChildren()) {
      if (auto* found = fxBusControls(*child, section))
        return found;
    }
    return nullptr;
  }

  // Component deletion can synchronously ask the editor for a new focus target.
  // Check the lists at that exact point, before freed memory happens to be reused.
  class FocusDeletionAudit : private ComponentListener {
    public:
      explicit FocusDeletionAudit(SynthEditor& editor) : editor_(editor) {
        std::set<Component*> controls;
        for (auto* control : editor.getKeyboardFocusOrder()) controls.insert(control);
        for (auto* control : editor.getAccessibleFocusOrder()) controls.insert(control);
        for (auto* control : controls) {
          watched_.emplace_back(control);
          control->addComponentListener(this);
        }
      }

      ~FocusDeletionAudit() override {
        for (auto& control : watched_)
          if (control != nullptr) control->removeComponentListener(this);
      }

      int deletions = 0;
      int stale_references = 0;

    private:
      void componentBeingDeleted(Component& component) override {
        ++deletions;
        for (const auto* order : { &editor_.getKeyboardFocusOrder(), &editor_.getAccessibleFocusOrder() })
          if (std::find(order->begin(), order->end(), &component) != order->end())
            ++stale_references;
      }

      SynthEditor& editor_;
      std::vector<Component::SafePointer<Component>> watched_;
  };

  void testEditorRebuildFocus() {
    SynthPlugin synth;
    for (int bus = 0; bus <= vital::kNumBuses; ++bus) {
      const std::string prefix = bus == 0 ? "" : "bus_" + std::to_string(bus) + "_";
      synth.parameterChanged(prefix + "fx_slot_1_type", vital::constants::kDelay + 1);
      synth.parameterChanged(prefix + "fx_slot_2_type", vital::constants::kUtility + 1);
    }
    auto editor = std::make_unique<SynthEditor>(synth);
    editor->updateFullGui();
    editor->addToDesktop(0);
    editor->setVisible(true);
    editor->toFront(true);
    pumpEditorEvents();

    for (int bus = 0; bus <= vital::kNumBuses; ++bus) {
      const String section = (bus == 0 ? String() : "Bus " + String(bus) + " - ") + "Chain and routing";
      const std::string prefix = bus == 0 ? "" : "bus_" + std::to_string(bus) + "_";
      int slot = 0;
      for (int key : { int(']'), int('['), KeyPress::backspaceKey }) {
        auto* header = editorSection(*editor, section, slot);
        expect(header != nullptr, "Focused effect header exists before rebuilding " + section.toStdString());
        if (!header) continue;
        editor->ensureComponentVisible(header);
        header->grabKeyboardFocus();
        expect(header->hasKeyboardFocus(false), "Effect header receives real keyboard focus");
        const String expected_title = key == KeyPress::backspaceKey ? "Add effect"
            : "Slot " + String(key == ']' ? 2 : 1) + " (Delay)";
        Component::SafePointer<Component> old_header(header);
        FocusDeletionAudit audit(*editor);
        expect(header->keyPressed(KeyPress(key)), "Effect move/remove shortcut is handled");
        pumpUntil([&] { return old_header == nullptr && Component::getCurrentlyFocusedComponent() != nullptr; });
        expect(old_header == nullptr && audit.deletions > 0, "Effect action rebuilds and destroys its old controls");
        expect(audit.stale_references == 0, "Destroyed controls must leave both focus lists before deletion");
        auto* focused = Component::getCurrentlyFocusedComponent();
        expect(focused && focused->getTitle() == expected_title,
               "Focus returns to the moved effect or Add effect after removal");
        for (bool keyboard : {false, true}) {
          auto traverser = keyboard ? editor->createKeyboardFocusTraverser() : editor->createFocusTraverser();
          const auto controls = traverser->getAllComponents(editor.get());
          expect(!controls.empty(), "Keyboard and accessibility traversal still work after rebuilding");
          for (auto* control : controls)
            expect(editor->isParentOf(control), "Focus traversal contains only current editor controls");
        }
        slot = key == ']' ? 1 : 0;
      }
      close(synth.getControls().at(prefix + "fx_slot_1_type")->value(), 0,
            "Removing the focused effect updates the correct bus");
    }

    // Host-driven full refresh and closing an editor with a focused dynamic
    // control exercise the other entrances to the same component teardown.
    auto* destination = titled(*editor, "Filter 2 destination");
    expect(destination != nullptr, "Restored filter destination remains available");
    if (destination) {
      editor->ensureComponentVisible(destination);
      destination->grabKeyboardFocus();
      FocusDeletionAudit audit(*editor);
      editor->updateFullGui();
      expect(audit.deletions > 0 && audit.stale_references == 0, "Full refresh invalidates old focus references");
    }
    pumpEditorEvents();
    if (auto* control = titled(*editor, "Filter 2 destination")) {
      editor->ensureComponentVisible(control);
      control->grabKeyboardFocus();
    }
    editor.reset();
    pumpEditorEvents();
  }

  void testFxBusControlsAndFocus() {
    SynthPlugin synth;
    for (int bus = 0; bus <= vital::kNumBuses; ++bus) {
      const std::string prefix = bus == 0 ? "" : "bus_" + std::to_string(bus) + "_";
      synth.parameterChanged(prefix + "fx_slot_1_type", vital::constants::kDelay + 1);
      synth.parameterChanged(prefix + "fx_slot_20_type", vital::constants::kUtility + 1);
    }
    SynthEditor editor(synth);
    editor.updateFullGui();
    editor.addToDesktop(0);
    editor.setVisible(true);
    editor.toFront(true);
    pumpEditorEvents();
    for (int bus = 0; bus <= vital::kNumBuses; ++bus) {
      const String section = (bus == 0 ? String() : "Bus " + String(bus) + " - ") + "Chain and routing";
      const std::string prefix = bus == 0 ? "" : "bus_" + std::to_string(bus) + "_";
      auto* group = fxBusControls(editor, section);
      expect(group != nullptr, section.toStdString() + " has Bus controls");
      if (!group)
        continue;
      auto* add = titled(*group->getParentComponent(), "Add effect");
      expect(add && add->getY() >= group->getBottom() &&
                 add->getExplicitFocusOrder() > group->getExplicitFocusOrder(),
             "Bus controls precedes Add effect in layout and keyboard order");
      auto* handler = group->getAccessibilityHandler();
      expect(handler && handler->getRole() == AccessibilityRole::group,
             "Bus controls is a VoiceOver group");
      if (handler) {
        const auto children = handler->getChildren();
        const StringArray titles { "Bus enabled", "Clear bus", "Load bus FX preset", "Save bus FX preset" };
        expect(children.size() == static_cast<size_t>(titles.size()), "Bus controls exposes four actions to VoiceOver");
        for (int index = 0; index < jmin(titles.size(), static_cast<int>(children.size())); ++index)
          expect(children[index]->getTitle() == titles[index], "Bus actions follow the announced order");
        expect(!children.empty() && children[0]->getRole() == AccessibilityRole::toggleButton &&
                   children[0]->getCurrentState().isCheckable() && children[0]->getCurrentState().isChecked(),
               "The bus bypass control is a single checked toggle while its effects are enabled");
      }
      // The toggle follows per-effect changes made elsewhere, not just its own presses.
      {
        auto* toggle = dynamic_cast<Button*>(titled(*group, "Bus enabled"));
        expect(toggle && toggle->getToggleState(), "Bus toggle starts enabled");
        for (int slot : { 1, 20 })
          synth.parameterChanged(prefix + "fx_slot_" + std::to_string(slot) + "_enabled", 0.0f);
        pumpEditorEvents();
        expect(toggle && !toggle->getToggleState(), "Bus toggle shows bypassed once every effect is off");
        synth.parameterChanged(prefix + "fx_slot_20_enabled", 1.0f);
        pumpEditorEvents();
        expect(toggle && toggle->getToggleState(), "Bus toggle shows enabled while any effect is on");
        synth.parameterChanged(prefix + "fx_slot_1_enabled", 1.0f);
        pumpEditorEvents();
      }
      struct BusAction {
        String title;
        int expected_enabled;
      };
      for (const BusAction action : { BusAction { "Bus enabled", 0 }, BusAction { "Bus enabled", 1 },
                                      BusAction { "Clear bus", 1 } }) {
        group = fxBusControls(editor, section);
        auto* button = group ? dynamic_cast<Button*>(titled(*group, action.title)) : nullptr;
        expect(button != nullptr, "Bus action exists after rebuild");
        if (!button)
          continue;
        editor.ensureComponentVisible(button);
        button->grabKeyboardFocus();
        button->getAccessibilityHandler()->grabFocus();
        Component::SafePointer<Component> pressed(button);
        expect(button->getAccessibilityHandler()->getActions().invoke(AccessibilityActionType::press),
               "VoiceOver can activate " + action.title.toStdString());
        // The rebuild is queued behind the editor's refresh tick, which can take over 100 ms
        // in the all-sections view; wait for the new control to take focus rather than a fixed time.
        pumpUntil([&] { return pressed == nullptr && Component::getCurrentlyFocusedComponent() != nullptr; });
        group = fxBusControls(editor, section);
        auto* restored = group ? titled(*group, action.title) : nullptr;
        auto* focused_now = Component::getCurrentlyFocusedComponent();
        expect(restored && focused_now == restored && restored->getAccessibilityHandler()->hasFocus(false),
               section.toStdString() + " retains keyboard and VoiceOver focus after " + action.title.toStdString() +
                   " (expected enabled " + std::to_string(action.expected_enabled) + ", focused: " +
                   (focused_now ? focused_now->getTitle().toStdString() : std::string("none")) +
                   ", restored found: " + (restored ? "yes" : "no") + ")");
        if (action.title == "Bus enabled") {
          auto* toggle = dynamic_cast<Button*>(restored);
          expect(toggle && toggle->getToggleState() == (action.expected_enabled != 0),
                 "Rebuilt bus toggle reflects the new bus state");
        }
        for (int slot : { 1, 20 }) {
          const auto slot_prefix = prefix + "fx_slot_" + std::to_string(slot) + "_";
          if (action.title == "Clear bus")
            close(synth.getControls().at(slot_prefix + "type")->value(), 0, "Clear bus removes its effects");
          else
            close(synth.getControls().at(slot_prefix + "enabled")->value(), action.expected_enabled,
                  "Bus toggle updates every effect's enabled state");
        }
        for (int other = bus + 1; other <= vital::kNumBuses; ++other) {
          const auto other_prefix = "bus_" + std::to_string(other) + "_fx_slot_1_";
          close(synth.getControls().at(other_prefix + "enabled")->value(), 1, "Other buses remain enabled");
          close(synth.getControls().at(other_prefix + "type")->value(), vital::constants::kDelay + 1,
                "Other buses retain their effects");
        }
      }
    }
    editor.removeFromDesktop();
  }

  // Finds a menu item by text, searching submenus too. Returns nullptr when absent.
  const PopupMenu::Item* menuItem(const PopupMenu& menu, const String& text) {
    for (PopupMenu::MenuItemIterator it(menu, true); it.next();) {
      if (it.getItem().text == text)
        return &it.getItem();
    }
    return nullptr;
  }

  StringArray submenuTexts(const PopupMenu& menu, const String& submenu) {
    StringArray texts;
    if (auto* parent = menuItem(menu, submenu)) {
      if (parent->subMenu != nullptr) {
        for (PopupMenu::MenuItemIterator it(*parent->subMenu); it.next();)
          texts.add(it.getItem().text);
      }
    }
    return texts;
  }

  // Builds the slot header's context menu exactly as the header shows it, then runs one item.
  // The real popup cannot stay open in a background test process, so items run directly.
  bool runSlotContextMenuItem(SynthEditor& editor, const String& section, int slot, const String& item) {
    auto* header = editorSection(editor, section, slot);
    expect(header != nullptr, "Effect slot header exists for " + section.toStdString());
    if (!header)
      return false;
    editor.ensureComponentVisible(header);
    header->grabKeyboardFocus();
    auto* handler = header->getAccessibilityHandler();
    expect(handler && handler->getActions().contains(AccessibilityActionType::showMenu),
           "Effect slot header offers a VoiceOver context menu");
    PopupMenu menu = editor.createEffectSlotContextMenu(section, slot, header->getTitle());
    auto* found = menuItem(menu, item);
    expect(found != nullptr && found->isEnabled && found->action != nullptr,
           "Slot context menu offers " + item.toStdString());
    if (!found || !found->isEnabled || !found->action)
      return false;
    Component::SafePointer<Component> old_header(header);
    found->action();
    if (item.startsWith("Copy "))
      pumpEditorEvents();
    else
      pumpUntil([&] { return old_header == nullptr && Component::getCurrentlyFocusedComponent() != nullptr; });
    return true;
  }

  int modulationIndex(SynthPlugin& synth, const std::string& source, const std::string& destination) {
    for (auto* route : synth.getModulationConnections()) {
      if (route && route->source_name == source && route->destination_name == destination)
        return synth.getConnectionIndex(source, destination);
    }
    return -1;
  }

  void testEffectSlotContextMenu() {
    const String main_section = "Chain and routing";
    const String bus_1_section = "Bus 1 - Chain and routing";
    const std::string source = "fx_slot_1_";
    const std::string pasted = "bus_1_fx_slot_1_";

    SynthPlugin synth;
    synth.parameterChanged(source + "type", vital::constants::kDelay + 1);
    synth.parameterChanged(source + "delay_feedback", 0.8f);
    synth.parameterChanged(source + "enabled", 0.0f);
    synth.connectModulation("lfo_1", source + "delay_feedback");
    const int source_route = modulationIndex(synth, "lfo_1", source + "delay_feedback");
    expect(source_route >= 0, "Test modulation route connects to the source effect");
    if (source_route >= 0)
      synth.parameterChanged("modulation_" + std::to_string(source_route + 1) + "_amount", 0.5f);

    SynthEditor editor(synth);
    editor.updateFullGui();
    editor.addToDesktop(0);
    editor.setVisible(true);
    editor.toFront(true);
    pumpEditorEvents();

    {
      auto* header = editorSection(editor, main_section, 0);
      PopupMenu menu = editor.createEffectSlotContextMenu(main_section, 0, header ? header->getTitle() : String());
      StringArray texts;
      for (PopupMenu::MenuItemIterator it(menu); it.next();)
        if (!it.getItem().isSeparator) texts.add(it.getItem().text);
      expect(texts == StringArray { "Copy Delay", "Copy Delay with modulations", "Paste to", "Replace Delay with",
                                    "Remove Delay" },
             "Slot context menu lists copy, paste, replace and remove: " + texts.joinIntoString(", ").toStdString());
      auto* paste = menuItem(menu, "Paste to");
      expect(paste && !paste->isEnabled, "Paste is unavailable before anything is copied");
      auto* same_effect = menuItem(menu, "Delay");
      expect(same_effect && !same_effect->isEnabled, "Replace does not offer the slot's current effect");
      expect(menuItem(menu, "Reverb") && menuItem(menu, "Reverb")->isEnabled, "Replace offers other effects");
      // Effects are grouped into categories; Utility stays at the top level of the Replace submenu.
      auto* replace = menuItem(menu, "Replace Delay with");
      StringArray top;
      if (replace && replace->subMenu)
        for (PopupMenu::MenuItemIterator it(*replace->subMenu); it.next();) top.add(it.getItem().text);
      for (const char* category : { "Analogue", "Dynamics", "Space", "Modulation", "Distortion and character", "Filter and EQ", "Spectral", "Utility" })
        expect(top.contains(category), std::string("Replace submenu lists ") + category + ": " + top.joinIntoString(", ").toStdString());
      expect(!top.contains("Reverb") && submenuTexts(*replace->subMenu, "Space").contains("Reverb"),
             "Reverb sits inside Space rather than at the top level");
      expect(submenuTexts(*replace->subMenu, "Analogue").contains("Analogue Wah"), "Analogue effects are grouped");
      expect(submenuTexts(*replace->subMenu, "Analogue").contains("Tape") &&
             !submenuTexts(*replace->subMenu, "Distortion and character").contains("Tape"),
             "Tape sits in the Analogue category");
    }

    // Copy with modulations, then paste into Bus 1. The paste submenu must skip the source bus.
    if (runSlotContextMenuItem(editor, main_section, 0, "Copy Delay with modulations")) {
      auto* header = editorSection(editor, main_section, 0);
      PopupMenu menu = editor.createEffectSlotContextMenu(main_section, 0, header ? header->getTitle() : String());
      const StringArray targets = submenuTexts(menu, "Paste Delay to");
      expect(targets == StringArray { "Bus 1", "Bus 2", "Bus 3" },
             "Paste offers every bus except the one the effect was copied from: " +
                 targets.joinIntoString(", ").toStdString());
    }
    if (runSlotContextMenuItem(editor, main_section, 0, "Bus 1")) {
      close(synth.getControls().at(pasted + "type")->value(), vital::constants::kDelay + 1,
            "Paste adds the copied effect to the chosen bus");
      close(synth.getControls().at(pasted + "delay_feedback")->value(), 0.8f, "Paste carries the effect's settings");
      close(synth.getControls().at(pasted + "enabled")->value(), 0.0f, "Paste keeps the effect's enabled state");
      close(synth.getControls().at(source + "type")->value(), vital::constants::kDelay + 1,
            "Paste leaves the source effect in place");
      for (int bus : { 2, 3 })
        close(synth.getControls().at("bus_" + std::to_string(bus) + "_fx_slot_1_type")->value(), 0,
              "Paste touches only the chosen bus");
      const int route = modulationIndex(synth, "lfo_1", pasted + "delay_feedback");
      expect(route >= 0, "Paste with modulations recreates the route on the pasted effect");
      if (route >= 0)
        close(synth.getControls().at("modulation_" + std::to_string(route + 1) + "_amount")->value(), 0.5f,
              "Pasted modulation keeps its amount");
      expect(editorSection(editor, bus_1_section, 0) != nullptr, "Bus 1 shows the pasted slot after rebuilding");
      auto* focused = Component::getCurrentlyFocusedComponent();
      expect(focused && focused->getTitle() == "Slot 1 (Delay)", "Focus stays on the source slot after pasting");
    }

    // From Bus 1 the paste submenu must now exclude Bus 1 instead.
    {
      auto* header = editorSection(editor, bus_1_section, 0);
      PopupMenu menu = editor.createEffectSlotContextMenu(bus_1_section, 0, header ? header->getTitle() : String());
      expect(submenuTexts(menu, "Paste Delay to") == StringArray { "Bus 1", "Bus 2", "Bus 3" },
             "Paste targets depend on where the effect was copied from, not where the menu opens");
      if (runSlotContextMenuItem(editor, bus_1_section, 0, "Copy Delay")) {
        menu = editor.createEffectSlotContextMenu(bus_1_section, 0, header ? header->getTitle() : String());
        expect(submenuTexts(menu, "Paste Delay to") == StringArray { "Main effects bus", "Bus 2", "Bus 3" },
               "Copying from Bus 1 excludes Bus 1 from the paste targets");
      }
    }

    // Replace the pasted Delay with a Reverb.
    if (runSlotContextMenuItem(editor, bus_1_section, 0, "Reverb")) {
      const float type = synth.getControls().at(pasted + "type")->value();
      close(type, vital::constants::kReverb + 1, "Replace swaps the slot's effect type in place");
      close(synth.getControls().at(pasted + "delay_feedback")->value(),
            vital::Parameters::getDetails(pasted + "delay_feedback").default_value,
            "Replace resets the previous effect's settings");
      expect(modulationIndex(synth, "lfo_1", pasted + "delay_feedback") < 0,
             "Replace drops modulation routes into the previous effect");
      auto* focused = Component::getCurrentlyFocusedComponent();
      expect(focused && focused->getTitle() == "Slot 1 (Reverb)", "Focus moves to the replaced slot");
    }

    // Remove the source effect from the main bus.
    if (runSlotContextMenuItem(editor, main_section, 0, "Remove Delay")) {
      close(synth.getControls().at(source + "type")->value(), 0, "Remove clears the slot");
      expect(modulationIndex(synth, "lfo_1", source + "delay_feedback") < 0,
             "Remove drops modulation routes into the removed effect");
      expect(editorSection(editor, main_section, 0) == nullptr, "Removed slot disappears from the editor");
    }
    editor.removeFromDesktop();
  }

  // Each modulation slot announces its own route, not every route of its source.
  void testModulationSlotTitlesDescribeOneRoute() {
    SynthPlugin synth;
    synth.connectModulation("lfo_2", "osc_1_distortion_amount");
    synth.connectModulation("lfo_2", "filter_1_cutoff");
    synth.connectModulation("env_2", "filter_2_cutoff");
    const int first = synth.getConnectionIndex("lfo_2", "osc_1_distortion_amount");
    const int second = synth.getConnectionIndex("lfo_2", "filter_1_cutoff");
    const int third = synth.getConnectionIndex("env_2", "filter_2_cutoff");
    expect(first == 0 && second == 1 && third == 2, "routes occupy slots 1 to 3");
    synth.parameterChanged("modulation_1_amount", 0.3f);
    synth.parameterChanged("modulation_2_amount", -0.25f);
    synth.parameterChanged("modulation_3_bypass", 1.0f);
    SynthEditor editor(synth);
    editor.updateFullGui();
    editor.addToDesktop(0);
    editor.setVisible(true);
    editor.toFront(true);
    pumpEditorEvents();
    auto slotHeader = [&](int slot) -> Component* {
      std::function<Component*(Component&)> find = [&](Component& parent) -> Component* {
        if (parent.getProperties().contains("ModulationSlot") &&
            static_cast<int>(parent.getProperties()["ModulationSlot"]) == slot)
          return &parent;
        for (auto* child : parent.getChildren())
          if (auto* found = find(*child)) return found;
        return nullptr;
      };
      return find(editor);
    };
    auto slotTitle = [&](int slot) -> String {
      auto* header = slotHeader(slot);
      return header ? header->getTitle() : String("<missing>");
    };
    auto clearButton = [&](int slot) -> Button* {
      auto* header = slotHeader(slot);
      return header ? dynamic_cast<Button*>(titled(*header, "Clear modulation slot")) : nullptr;
    };
    expect(slotTitle(1) == "Modulation one, LFO 2 to Oscillator 1 Distortion Amount, +30%, route 1 of 2 from LFO 2",
           "slot 1 names its own route, amount and position: " + slotTitle(1).toStdString());
    expect(slotTitle(2) == "Modulation two, LFO 2 to Filter 1 Cutoff, -25%, route 2 of 2 from LFO 2",
           "slot 2 names its own route: " + slotTitle(2).toStdString());
    expect(slotTitle(3) == "Modulation three, Envelope 2 to Filter 2 Cutoff, 0%, bypassed",
           "a single route from a source has no position, and bypass is announced: " + slotTitle(3).toStdString());
    expect(slotTitle(4) == "Modulation four, empty", "unused slots read as empty: " + slotTitle(4).toStdString());

    // The slot's Amount row reads -100% to 100%.
    auto* amount = slotHeader(2) ? dynamic_cast<Slider*>(titled(*slotHeader(2), "Amount")) : nullptr;
    expect(amount != nullptr && amount->getTextFromValue(amount->getValue()) == "-25%",
           "a modulation amount reads as a percentage");

    // Only slots with something assigned offer to clear it.
    for (int slot = 1; slot <= 3; ++slot)
      expect(clearButton(slot) != nullptr, "assigned slot " + std::to_string(slot) + " has a Clear modulation slot button");
    expect(clearButton(4) == nullptr, "an empty slot has no Clear modulation slot button");
    if (auto* clear = clearButton(2)) {
      clear->grabKeyboardFocus();
      clear->triggerClick();
      pumpUntil([&] { return clearButton(2) == nullptr; });
    }
    expect(synth.getConnectionIndex("lfo_2", "filter_1_cutoff") < 0, "clearing slot 2 disconnects its route");
    expect(synth.getControls().at("modulation_2_amount")->value() == 0.0f, "clearing a slot resets its amount");
    expect(slotTitle(2) == "Modulation two, empty", "a cleared slot reads as empty: " + slotTitle(2).toStdString());
    expect(clearButton(2) == nullptr, "a cleared slot no longer offers Clear");
    expect(slotTitle(1) == "Modulation one, LFO 2 to Oscillator 1 Distortion Amount, +30%",
           "the other routes stay: " + slotTitle(1).toStdString());
    auto* focused = Component::getCurrentlyFocusedComponent();
    expect(focused != nullptr && focused == slotHeader(2),
           "focus lands on the cleared slot's heading: " +
           (focused ? focused->getTitle().toStdString() : std::string("nothing")));
    editor.removeFromDesktop();
  }

  // A preset load changes effect types behind an open editor; the refreshed
  // slot groups must contain their parameter rows without reopening the editor.
  // The Wavetable filter model shows its frame row and wavetable controls in
  // the voice filters and in a Filter FX slot, and hides Drive and Blend.
  void testWavetableFilterRows() {
    SynthPlugin synth;
    SynthEditor editor(synth);
    synth.getControls().at("filter_1_model")->set(float(vital::constants::kFilterTable));
    synth.getControls().at("fx_slot_1_type")->set(vital::constants::kFilterFx + 1);
    synth.getControls().at("fx_slot_1_filter_fx_model")->set(float(vital::constants::kFilterTable));
    editor.updateFullGui();
    expect(titled(editor, "Wavetable frame") != nullptr, "Filter 1 shows its Wavetable frame row");
    expect(titled(editor, "Filter 1 filter wavetable controls") != nullptr || titled(editor, "Filter wavetable controls") != nullptr,
           "Filter 1 shows the filter wavetable controls group");
    expect(titled(editor, "Load wavetable") != nullptr, "the group offers Load wavetable");
    expect(titled(editor, "Phase mode") != nullptr, "Style reads as Phase mode for the Wavetable model");
    if (auto* slot = editorSection(editor, "Chain and routing", 0)) {
      // Slot rows take the parameter's display name.
      expect(titled(*slot, "Wavetable Frame") != nullptr, "the Filter FX slot shows its Wavetable Frame row");
      expect(titled(*slot, "Load wavetable") != nullptr, "the Filter FX slot shows the wavetable controls");
      expect(titled(*slot, "Drive") == nullptr, "the Filter FX slot hides Drive for the Wavetable model");
    }
    else expect(false, "the Filter FX slot exists");
    // Back to Analog: the rows disappear.
    synth.getControls().at("filter_1_model")->set(float(vital::constants::kAnalog));
    synth.getControls().at("fx_slot_1_filter_fx_model")->set(float(vital::constants::kAnalog));
    editor.updateFullGui();
    expect(titled(editor, "Wavetable frame") == nullptr, "other models hide the Wavetable frame row");
  }

  // Loading a wavetable into a filter with Previous/Next (or the browser) must
  // not rebuild the rows: the pressed control stays alive and focused, in the
  // voice filter section and in a Filter FX slot, and the "Current wavetable"
  // control takes the new name.
  void testWavetableFilterLoadKeepsFocus() {
    const File table = File(ATLAS_FILTER_TABLE_FACTORY).getChildFile("Lowpass Slopes.wav");
    expect(table.existsAsFile(), "a factory filter wavetable exists for the focus test");
    if (!table.existsAsFile()) return;
    SynthPlugin synth;
    SynthEditor editor(synth);
    editor.addToDesktop(0);
    editor.setVisible(true);
    editor.toFront(true);
    synth.getControls().at("filter_1_model")->set(float(vital::constants::kFilterTable));
    synth.getControls().at("fx_slot_1_type")->set(vital::constants::kFilterFx + 1);
    synth.getControls().at("fx_slot_1_filter_fx_model")->set(float(vital::constants::kFilterTable));
    editor.updateFullGui();
    // A window that was just shown takes focus back to itself while it is being
    // activated (within about 50 ms, with or without any action); wait that out
    // so the cases below measure the load, not the window.
    pumpUntil([] { return false; }, 300);
    struct Case { const char* prefix; const char* section; int slot; const char* button; };
    for (const Case& c : { Case{ "filter_1", "Filter 1", -1, "Next wavetable" }, Case{ "filter_1", "Filter 1", -1, "Previous wavetable" },
                           Case{ "fx_slot_1_filter_fx", "Chain and routing", 0, "Next wavetable" } }) {
      Component* scope = editorSection(editor, c.section, c.slot);
      if (scope == nullptr) scope = &editor;
      auto* button = dynamic_cast<Button*>(titled(*scope, c.button));
      expect(button != nullptr, std::string(c.section) + " has a " + c.button + " control");
      if (!button) continue;
      editor.ensureComponentVisible(button);
      button->grabKeyboardFocus();
      pumpUntil([&] { return button->hasKeyboardFocus(true); });
      expect(button->hasKeyboardFocus(true), std::string(c.section) + ": the " + c.button + " control can take focus before loading");
      Component::SafePointer<Component> pressed(button);
      // What the button's click does, minus the folder lookup.
      editor.loadFilterTableFile(c.prefix, table);
      // Let any asynchronous rebuild or focus move happen, then check nothing did.
      pumpUntil([] { return false; }, 400);
      auto* focused = Component::getCurrentlyFocusedComponent();
      std::cout << "  focus after loading into " << c.section << " via " << c.button << ": "
                << (focused ? focused->getTitle().toStdString() : std::string("none"))
                << (pressed != nullptr ? " (same control)" : " (control was rebuilt)") << '\n';
      expect(pressed != nullptr, std::string(c.section) + ": loading a wavetable does not rebuild the " + c.button + " control");
      expect(focused != nullptr && focused == pressed.getComponent(),
             std::string(c.section) + ": focus stays on " + c.button + " after loading a wavetable (focused: " +
                 (focused ? focused->getTitle().toStdString() : std::string("none")) + ")");
      auto* current = titled(*scope, "Current wavetable: " + table.getFileNameWithoutExtension());
      expect(current != nullptr, std::string(c.section) + ": the Current wavetable control shows the loaded name");
    }
    expect(synth.getFilterTableCreator("filter_1", false) != nullptr && synth.getFilterTableCreator("fx_slot_1_filter_fx", false) != nullptr,
           "both filters received the wavetable");
  }

  void testFullGuiRefreshRebuildsEffectRows() {
    SynthPlugin synth;
    SynthEditor editor(synth);
    editor.updateFullGui();
    expect(editorSection(editor, "Chain and routing", 0) == nullptr, "no effect slot before the change");
    // What a preset load does: controls change underneath, then the editor is told to refresh.
    synth.getControls().at("fx_slot_1_type")->set(vital::constants::kDelay + 1);
    synth.getControls().at("bus_2_fx_slot_1_type")->set(vital::constants::kReverb + 1);
    editor.updateFullGui();
    for (const String section : { "Chain and routing", "Bus 2 - Chain and routing" }) {
      auto* group = editorSection(editor, section, 0);
      expect(group != nullptr, section.toStdString() + " shows the new slot");
      if (!group) continue;
      expect(titled(*group, "Enabled") != nullptr, section.toStdString() + " slot has its Enabled row after a refresh");
      // Rows are the slot's grandchildren (row container, then control); labels end in " label".
      int rows = 0;
      std::function<void(Component&)> count = [&](Component& c) {
        if (c.getTitle().isNotEmpty() && !c.getTitle().endsWith(" label") && &c != group) ++rows;
        for (auto* child : c.getChildren()) count(*child);
      };
      count(*group);
      expect(rows >= 5, section.toStdString() + " slot has its effect parameter rows after a refresh (" + std::to_string(rows) + ")");
      expect(titled(*group, "Mix") != nullptr, section.toStdString() + " slot shows the effect's own controls, such as Mix");
    }
  }

  void testPresetGroupWorksWithoutKeyboardFocus() {
    SynthPlugin synth;
    SynthEditor editor(synth);
    editor.addToDesktop(0);
    editor.setVisible(true);
    editor.toFront(true);

    auto* summary = dynamic_cast<Label*>(titled(editor, "Preset summary"));
    expect(summary && !summary->getText().contains("not loaded yet"),
           "The visible preset browser starts loading before any keyboard focus");

    for (int pass = 0; pass < 2; ++pass) {
      if (pass != 0)
        editor.updateFullGui();
      auto* presets = titled(editor, "Presets");
      auto* distant_control = editorSection(editor, "Oscillator 3");
      expect(presets && distant_control, "Preset and oscillator groups exist");
      if (!presets || !distant_control)
        continue;
      editor.ensureComponentVisible(distant_control);
      distant_control->grabKeyboardFocus();
      auto* handler = presets->getAccessibilityHandler();
      expect(handler != nullptr, "Presets group remains accessible outside the viewport");
      if (!handler)
        continue;

      // Prime the same empty children query that VoiceOver can make while the
      // preset controls are outside the outer scrolling area.
      handler->getChildren();
#if JUCE_MAC
      expect(nativeAccessibilityFocusSettles(*handler),
             "Entering Presets with VoiceOver settles after its initial focus notification");
      const auto titles = nativeAccessibilityChildTitles(*handler);
#else
      handler->getActions().invoke(AccessibilityActionType::focus);
      handler->grabFocus();
      StringArray titles;
      for (auto* child : handler->getChildren())
        titles.add(child->getTitle());
#endif
      expect(Component::getCurrentlyFocusedComponent() == presets,
             "VoiceOver reveals Presets while focus stays on the group, before Tab into its children");
      for (const auto* title : { "Preset menu", "Preset search", "Collection", "Library", "Bank",
                                 "Category", "Tags", "Preset results table", "Autoload preset when scrolling" })
        expect(titles.contains(title), std::string("VoiceOver exposes ") + title + " without Tab");
#if JUCE_MAC
      const auto position = presets->getScreenPosition();
      expect(nativeAccessibilityFocusSettles(*handler),
             "Repeated VoiceOver focus requests settle without a layout/focus notification loop");
      expect(presets->getScreenPosition() == position,
             "Repeated VoiceOver focus requests do not scroll the preset browser back and forth");
#endif
      auto* table = dynamic_cast<TableListBox*>(titled(editor, "Preset results table"));
      auto* table_handler = table ? table->getAccessibilityHandler() : nullptr;
      expect(table_handler && table_handler->getRole() == AccessibilityRole::table &&
                 table_handler->getTableInterface() != nullptr,
             "Preset results retain native table navigation");
    }
    auto* collection = dynamic_cast<TableListBox*>(titled(editor, "Collection"));
    expect(collection != nullptr, "Preset collection table exists");
    if (collection) {
      for (int attempt = 0; attempt < 34 && collection->getNumRows() == 0; ++attempt)
        pumpEditorEvents();
      auto* handler = collection->getAccessibilityHandler();
      auto* table = handler ? handler->getTableInterface() : nullptr;
      expect(table && table->getNumRows() == 2 && table->getRowHandler(0) != nullptr,
             "All presets and Favourites populate without keyboard focus inside the preset browser");
    }
    editor.removeFromDesktop();
  }

  void testWavetableEditorReturnsFocusToItsOscillator() {
    SynthPlugin synth;
    SynthEditor editor(synth);
    // Initialise the timer's cached control state before posting button clicks;
    // an unrelated first-tick rebuild can otherwise discard a pending click.
    editor.updateFullGui();
    editor.addToDesktop(0);
    editor.setVisible(true);
    editor.toFront(true);
    pumpEditorEvents();

    const auto toggle = [&](int oscillator, bool visible) {
      const String section = "Oscillator " + String(oscillator);
      auto* group = editorSection(editor, section);
      auto* button = group ? dynamic_cast<Button*>(titled(*group, visible ? "Show wavetable editor"
                                                                                       : "Hide wavetable editor")) : nullptr;
      expect(button != nullptr, section.toStdString() + " exposes the wavetable toggle");
      if (!button)
        return;
      editor.ensureComponentVisible(button);
      button->grabKeyboardFocus();
      button->getAccessibilityHandler()->grabFocus();
      expect(button->getAccessibilityHandler()->getActions().invoke(AccessibilityActionType::press),
             "VoiceOver can activate the wavetable toggle");
      pumpEditorEvents();

      group = editorSection(editor, section);
      auto* restored = group ? titled(*group, visible ? "Hide wavetable editor" : "Show wavetable editor") : nullptr;
      expect(restored != nullptr && Component::getCurrentlyFocusedComponent() == restored,
             section.toStdString() + " keeps keyboard focus after " + (visible ? "showing" : "hiding"));
      expect(restored != nullptr && restored->getAccessibilityHandler()->hasFocus(false),
             section.toStdString() + " keeps accessibility focus after " + (visible ? "showing" : "hiding"));
    };

    // Duplicate Show and Hide labels must both resolve to the invoking oscillator.
    toggle(1, true);
    // Only wavetable-type oscillators (slots 1-4 by default) have a wavetable editor.
    for (int oscillator = 2; oscillator <= 4; ++oscillator) {
      toggle(oscillator, true);
      toggle(oscillator, false);
    }
    toggle(1, false);
    toggle(3, true);
    toggle(3, false);
    editor.removeFromDesktop();
  }

  // Oscillator slots Phase 2: every slot type shows the distortion rows it can
  // use, headers name the type, arp destinations name the slot's type and the
  // Sample/Granular playheads are offered as modulation sources.
  void testSlotTypeRowsFollowTheType() {
    SynthPlugin synth;
    SynthEditor editor(synth);
    editor.updateFullGui();
    pumpEditorEvents();
    const auto& controls = synth.getControls();

    // Oscillator 5 is a Sample slot: FM and ring modulation, FM first after None.
    auto* mode5 = dynamic_cast<Slider*>(titled(*editorSection(editor, "Oscillator 5"), "5 Distortion mode"));
    expect(mode5 != nullptr, "a Sample slot shows the Distortion mode row");
    if (mode5 != nullptr) {
      expect(mode5->getDescription().contains("playback rate"), "the Sample distortion help explains playback-rate FM");
      mode5->keyPressed(KeyPress(KeyPress::backspaceKey));
      expect(mode5->getTextFromValue(mode5->getValue()) == "None", "the Sample distortion mode starts at None");
      mode5->keyPressed(KeyPress(KeyPress::rightKey));
      expect(controls.at("osc_5_distortion_type")->value() == float(vital::SynthOscillator::kFmOscillatorA),
             "right arrow from None chooses FM from the first other oscillator");
      expect(mode5->getTextFromValue(mode5->getValue()) == "FM from Oscillator 1", "the FM mode names its source slot");
      for (int i = 0; i < 7; ++i)
        mode5->keyPressed(KeyPress(KeyPress::rightKey));
      expect(controls.at("osc_5_distortion_type")->value() == float(vital::SynthOscillator::kRmOscillatorA),
             "the ring modulation sources follow the seven FM sources");
      expect(mode5->getTextFromValue(mode5->getValue()) == "Ring Mod from Oscillator 1", "the RM mode names its source slot");
      mode5->keyPressed(KeyPress(KeyPress::backspaceKey));
    }
    expect(titled(*editorSection(editor, "Oscillator 5"), "5 Distortion Amount") != nullptr,
           "a Sample slot shows the distortion amount");
    expect(titled(*editorSection(editor, "Oscillator 5"), "5 Secondary distortion mode") != nullptr,
           "a Sample slot shows the secondary distortion mode");
    expect(titled(*editorSection(editor, "Oscillator 5"), "5 Distortion Spread") == nullptr,
           "a Sample slot hides the unison-only distortion spread");

    // Oscillator 8 is a Noise slot: ring modulation only.
    auto* mode8 = dynamic_cast<Slider*>(titled(*editorSection(editor, "Oscillator 8"), "8 Distortion mode"));
    expect(mode8 != nullptr, "a Noise slot shows the Distortion mode row");
    if (mode8 != nullptr) {
      expect(mode8->getDescription().contains("cannot be frequency modulated"), "the Noise distortion help says FM is unavailable");
      mode8->keyPressed(KeyPress(KeyPress::backspaceKey));
      mode8->keyPressed(KeyPress(KeyPress::rightKey));
      expect(controls.at("osc_8_distortion_type")->value() == float(vital::SynthOscillator::kRmOscillatorA),
             "right arrow from None on a Noise slot chooses ring modulation directly");
      expect(mode8->getTextFromValue(mode8->getValue()) == "Ring Mod from Oscillator 1", "the Noise RM mode names its source");
      mode8->keyPressed(KeyPress(KeyPress::backspaceKey));
    }

    // Section headers name the type instead of calling every non-wavetable slot analog.
    std::function<Component*(Component&, const String&)> titledPrefix = [&](Component& parent, const String& prefix) -> Component* {
      if (parent.getTitle().startsWith(prefix))
        return &parent;
      for (auto* child : parent.getChildren())
        if (auto* found = titledPrefix(*child, prefix))
          return found;
      return nullptr;
    };
    expect(titledPrefix(editor, "Oscillator 5, sample, current sample: ") != nullptr,
           "the Sample slot header names its type and sample");
    expect(titled(editor, "Oscillator 8, noise") != nullptr, "the Noise slot header names its type");

    // Arpeggiator destinations say what each slot plays.
    auto* destinations = editorSection(editor, "Arpeggiator 1 destinations");
    expect(destinations != nullptr && titled(*destinations, "Oscillator 1, wavetable") != nullptr,
           "arp destination rows name a wavetable slot's type");
    expect(destinations != nullptr && titled(*destinations, "Oscillator 6, granular") != nullptr,
           "arp destination rows name a granular slot's type");

    // Playheads: offered for Sample and Granular slots only.
    std::map<int, String> choices;
    PopupMenu menu = editor.createModulationSourceSubmenu("filter_1_cutoff", choices, 1);
    StringArray playheads;
    bool found_group = false;
    for (PopupMenu::MenuItemIterator it(menu, false); it.next();) {
      const auto& item = it.getItem();
      if (item.text != "Oscillator playheads" || item.subMenu == nullptr)
        continue;
      found_group = true;
      for (PopupMenu::MenuItemIterator sub(*item.subMenu, false); sub.next();)
        playheads.add(sub.getItem().text);
    }
    expect(found_group, "parameter rows offer an Oscillator playheads source group");
    expect(playheads.contains("Oscillator 5 playhead") && playheads.contains("Oscillator 6 playhead"),
           "the Sample and Granular slots offer their playheads");
    expect(!playheads.contains("Oscillator 7 playhead") && !playheads.contains("Oscillator 8 playhead") &&
           !playheads.contains("Oscillator 1 playhead"),
           "Spectral, Noise and wavetable slots have no playhead to offer");
    bool playhead_source_listed = false;
    for (const auto& id : choices)
      playhead_source_listed = playhead_source_listed || id.second == "osc_5_playhead";
    expect(playhead_source_listed, "choosing the playhead connects the osc_5_playhead source");
  }

  // Any slot switched to Wavetable gets the wavetable actions and editor, not just slots 1-4.
  void testWavetableEditingOnASlotSwitchedToWavetable() {
    SynthPlugin synth;
    SynthEditor editor(synth);
    editor.updateFullGui();
    editor.addToDesktop(0);
    editor.setVisible(true);
    editor.toFront(true);
    pumpEditorEvents();

    expect(titled(*editorSection(editor, "Oscillator 7"), "Load wavetable") == nullptr,
           "a Spectral slot has no wavetable actions");
    auto* type = dynamic_cast<ComboBox*>(titled(*editorSection(editor, "Oscillator 7"), "7 Type"));
    expect(type != nullptr, "Oscillator 7 exposes its Type menu");
    if (type == nullptr) {
      editor.removeFromDesktop();
      return;
    }
    // An arrow steps the type without opening the menu; each step rebuilds the rows.
    editor.ensureComponentVisible(type);
    type->grabKeyboardFocus();
    expect(type->getText() == "Spectral", "the Type menu shows the slot's type");
    const int spectral_index = type->getSelectedItemIndex();
    Component::SafePointer<Component> stepped_type(type);
    type->keyPressed(KeyPress(KeyPress::leftKey));
    expect(!type->isPopupActive(), "an arrow does not open the Type menu");
    expect(type->getSelectedItemIndex() == spectral_index - 1, "an arrow steps to the previous type");
    expect(synth.getControls().at("osc_7_type")->value() != float(vital::source_types::kSpectral),
           "an arrow changes the type");
    pumpUntil([&] {
      auto* focused = Component::getCurrentlyFocusedComponent();
      return stepped_type == nullptr && focused != nullptr && focused->getTitle() == "7 Type";
    });
    type = dynamic_cast<ComboBox*>(Component::getCurrentlyFocusedComponent());
    expect(type != nullptr && type->getTitle() == "7 Type",
           "after an arrow step rebuilds the rows, focus is on the new Type menu");
    if (type == nullptr) {
      editor.removeFromDesktop();
      return;
    }
    type->keyPressed(KeyPress(KeyPress::spaceKey));
    expect(type->isPopupActive(), "Space opens the Type menu");
    type->keyPressed(KeyPress(KeyPress::rightKey));
    expect(type->getSelectedItemIndex() == spectral_index - 1, "an arrow forwarded by the open menu keeps the choice");
    PopupMenu::dismissAllActiveMenus();
    pumpEditorEvents();
    type->setSelectedItemIndex(vital::source_types::kWavetable, sendNotificationSync);
    expect(synth.getControls().at("osc_7_type")->value() == 0.0f,  // Wavetable
           "choosing Wavetable from the menu applies it");
    pumpUntil([&] {
      auto* section = editorSection(editor, "Oscillator 7");
      return section != nullptr && titled(*section, "Load wavetable") != nullptr;
    });
    auto* refocused = Component::getCurrentlyFocusedComponent();
    auto* rebuilt_section = editorSection(editor, "Oscillator 7");
    expect(refocused != nullptr && refocused->getTitle() == "7 Type" && rebuilt_section != nullptr &&
               rebuilt_section->isParentOf(refocused),
           "after the rebuild, focus returns to Oscillator 7's Type menu");
    auto* refocused_handler = refocused != nullptr ? refocused->getAccessibilityHandler() : nullptr;
    expect(refocused_handler != nullptr && refocused_handler->hasFocus(false),
           "after the rebuild, the screen reader's focus is on the Type menu");
    auto* section = editorSection(editor, "Oscillator 7");
    expect(section != nullptr && titled(*section, "Load wavetable") != nullptr, "a slot switched to Wavetable shows Load wavetable");
    expect(section != nullptr && titled(*section, "Next wavetable") != nullptr, "a slot switched to Wavetable shows Next wavetable");
    expect(editorSection(editor, "Oscillator 7 wavetable controls") != nullptr, "the wavetable controls group belongs to slot 7");
    auto* show = section ? dynamic_cast<Button*>(titled(*section, "Show wavetable editor")) : nullptr;
    expect(show != nullptr, "a slot switched to Wavetable offers the wavetable editor");
    if (show != nullptr) {
      editor.ensureComponentVisible(show);
      show->grabKeyboardFocus();
      expect(show->getAccessibilityHandler()->getActions().invoke(AccessibilityActionType::press),
             "VoiceOver can open slot 7's wavetable editor");
      pumpUntil([&] { return editorSection(editor, "Oscillator 7 wavetable editor") != nullptr; });
      expect(editorSection(editor, "Oscillator 7 wavetable editor") != nullptr, "slot 7 gets its own wavetable editor group");
      auto* hide = editorSection(editor, "Oscillator 7") ?
                   titled(*editorSection(editor, "Oscillator 7"), "Hide wavetable editor") : nullptr;
      expect(hide != nullptr && Component::getCurrentlyFocusedComponent() == hide,
             "slot 7 keeps keyboard focus on its wavetable toggle after showing the editor");
    }
    editor.removeFromDesktop();
  }

  // Moving a slot whose rows depend on a variant (Dimension Expander mode) used
  // to race the 20 Hz timer: the tick saw the swapped mode, queued its own
  // rebuild with the stale title and the moved slot lost focus to the Editor menu.
  void testEffectMoveKeepsFocusAcrossTimerTick() {
    SynthPlugin synth;
    synth.parameterChanged("fx_slot_1_type", vital::constants::kDimensionExpander + 1);
    synth.parameterChanged("fx_slot_1_dimension_expander_mode", 1.0f);
    synth.parameterChanged("fx_slot_2_type", vital::constants::kDelay + 1);
    SynthEditor editor(synth);
    editor.updateFullGui();
    editor.addToDesktop(0);
    editor.setVisible(true);
    editor.toFront(true);
    pumpEditorEvents();

    const String section = "Chain and routing";
    for (int pass = 0; pass < 2; ++pass) {
      const int key = pass == 0 ? ']' : '[';
      const int slot = pass == 0 ? 0 : 1;
      auto* header = editorSection(editor, section, slot);
      expect(header != nullptr && header->getTitle() == "Slot " + String(slot + 1) + " (Dimension Expander)",
             "the Dimension Expander header is at the expected position");
      if (header == nullptr)
        break;
      editor.ensureComponentVisible(header);
      header->grabKeyboardFocus();
      Component::SafePointer<Component> old_header(header);
      expect(header->keyPressed(KeyPress(key)), "the move shortcut is handled");
      // A timer tick lands before the queued rebuild runs.
      editor.pollStateNow();
      pumpUntil([&] { return old_header == nullptr && Component::getCurrentlyFocusedComponent() != nullptr; });
      auto* focused = Component::getCurrentlyFocusedComponent();
      const String expected = "Slot " + String(pass == 0 ? 2 : 1) + " (Dimension Expander)";
      expect(focused != nullptr && focused->getTitle() == expected,
             "keyboard focus follows the moved Dimension Expander despite the timer tick (pass " +
             std::to_string(pass) + ")");
      expect(focused != nullptr && focused->getAccessibilityHandler() != nullptr &&
             focused->getAccessibilityHandler()->hasFocus(false),
             "accessibility focus follows the moved slot too");
      expect(focused == nullptr || focused->getTitle() != "Editor menu", "focus must not fall back to the Editor menu");
    }
    editor.removeFromDesktop();
  }

  // Oscillator sections group their rows: output rows at the top level, then
  // Analog controls, Pitch and unison (with Scale lock inside), Warp and the
  // type's own group, depending on the slot's type.
  // The wavetable editor edits layers, sources, modifiers and keyframes, and
  // its blend modes change the table instead of flattening it.
  void testWavetableEditorLayersAndInterpolation() {
    auto openEditor = [&](SynthEditor& editor) {
      editor.updateFullGui();
      editor.addToDesktop(0);
      editor.setVisible(true);
      pumpEditorEvents();
      auto* show = dynamic_cast<Button*>(titled(*editorSection(editor, "Oscillator 1"), "Show wavetable editor"));
      expect(show != nullptr, "Oscillator 1 offers the wavetable editor");
      if (show != nullptr && show->onClick)
        show->onClick();
      pumpUntil([&] { return editorSection(editor, "Oscillator 1 wavetable editor") != nullptr; });
      return editorSection(editor, "Oscillator 1 wavetable editor");
    };
    auto press = [&](SynthEditor& editor, const String& title) {
      auto* section = editorSection(editor, "Oscillator 1 wavetable editor");
      auto* button = section ? dynamic_cast<Button*>(titled(*section, title)) : nullptr;
      expect(button != nullptr, ("the wavetable editor has " + title).toStdString());
      if (button != nullptr && button->onClick)
        button->onClick();
      pumpEditorEvents();
    };
    auto find = [&](SynthEditor& editor, const String& title) -> Component* {
      auto* section = editorSection(editor, "Oscillator 1 wavetable editor");
      return section ? titled(*section, title) : nullptr;
    };

    {
      SynthPlugin synth;
      SynthEditor editor(synth);
      openEditor(editor);
      auto* creator = synth.getWavetableCreator(0);
      expect(find(editor, "Layer") != nullptr && find(editor, "Add layer") != nullptr, "layer rows are offered");
      expect(find(editor, "Layer 1 source: Wave source") != nullptr, "the source has its own group");
      press(editor, "Add frame");
      auto* source = atlas::wavetable_edit::waveSourceOfLayer(creator, 0);
      expect(source != nullptr && source->numFrames() == 2 && source->getFrameAt(1)->position() == 255,
             "Add frame after the last frame goes to the end of the timeline");
      pumpUntil([&] { return find(editor, "Spread frames evenly") != nullptr; });
      auto* interpolation = dynamic_cast<ComboBox*>(find(editor, "Frame interpolation"));
      expect(interpolation != nullptr, "the wave source offers Frame interpolation");
      if (interpolation != nullptr)
        interpolation->setSelectedId(3, sendNotificationSync);   // Spectral blend
      source = atlas::wavetable_edit::waveSourceOfLayer(creator, 0);
      expect(creator->numGroups() == 1 && source != nullptr && source->numFrames() == 2 &&
             source->getInterpolationMode() == WaveSource::kFrequency &&
             source->getInterpolationStyle() == WavetableComponent::kLinear,
             "choosing Spectral blend sets it on the source without touching its frames");

      auto* type = dynamic_cast<ComboBox*>(find(editor, "New layer source"));
      expect(type != nullptr, "a new layer's source can be chosen");
      if (type != nullptr)
        type->setSelectedId(2, sendNotificationSync);   // Line source
      press(editor, "Add layer");
      pumpUntil([&] { return find(editor, "Source line point") != nullptr; });
      expect(creator->numGroups() == 2, "a line source layer is added");
      expect(find(editor, "Source line point") != nullptr && find(editor, "Source point level") != nullptr,
             "the line layer exposes its points");
      expect(find(editor, "Frame interpolation") == nullptr, "only wave source layers show frame interpolation");

      auto* modifier = dynamic_cast<ComboBox*>(find(editor, "Add modifier"));
      if (modifier != nullptr)
        modifier->setSelectedId(1, sendNotificationSync);   // Phase Shift
      press(editor, "Add selected modifier");
      pumpUntil([&] { return find(editor, "Phase Shift 1 keyframe") != nullptr; });
      expect(creator->getGroup(1)->numComponents() == 2, "the modifier joins the selected layer");
      expect(find(editor, "Phase Shift 1 keyframe") != nullptr && find(editor, "Phase Shift 1 phase") != nullptr,
             "the modifier exposes its keyframes and values");
      press(editor, "Phase Shift 1 add keyframe");
      pumpUntil([&] { return find(editor, "Phase Shift 1 remove keyframe") != nullptr; });
      expect(creator->getGroup(1)->getComponent(1)->numFrames() == 2, "the modifier gains a keyframe");
      auto* phase = dynamic_cast<Slider*>(find(editor, "Phase Shift 1 phase"));
      if (phase != nullptr)
        phase->setValue(90.0, sendNotificationSync);
      auto* phase_modifier = dynamic_cast<PhaseModifier*>(creator->getGroup(1)->getComponent(1));
      expect(phase_modifier != nullptr && std::abs(phase_modifier->getKeyframe(1)->getPhase() - vital::kPi / 2) < 1.0e-3,
             "the phase row edits the selected, second keyframe");
      editor.removeFromDesktop();
    }

    {
      // An imported audio table keeps its audio layer when its blend changes.
      SynthPlugin synth;
      auto* creator = synth.getWavetableCreator(0);
      std::vector<float> audio(44100);
      for (int i = 0; i < 44100; ++i)
        audio[i] = 0.5f * std::sin(2.0f * float(M_PI) * 220.0f * i / 44100.0f) * (1.0f - i / 44100.0f);
      creator->initFromAudioFile(audio.data(), int(audio.size()), 44100, WavetableCreator::kWavetableSplice,
                                 FileSource::kTimeInterpolate);
      SynthEditor editor(synth);
      openEditor(editor);
      expect(find(editor, "Frame interpolation") == nullptr, "an audio layer has no wave frame interpolation");
      auto* blend = dynamic_cast<ComboBox*>(find(editor, "Source frame blend"));
      expect(blend != nullptr, "an audio layer offers its frame blend");
      if (blend != nullptr)
        blend->setSelectedId(4, sendNotificationSync);   // Spectral
      auto* file = dynamic_cast<FileSource*>(creator->getGroup(0)->getComponent(0));
      expect(creator->numGroups() == 1 && file != nullptr && file->getFadeStyle() == FileSource::kFreqInterpolate,
             "choosing Spectral sets the audio layer's blend and keeps the audio");
      auto* keyframe = dynamic_cast<Slider*>(find(editor, "Source keyframe"));
      auto* start = dynamic_cast<Slider*>(find(editor, "Source start position"));
      expect(keyframe != nullptr && start != nullptr, "an audio layer exposes keyframes and start positions");
      if (keyframe != nullptr && start != nullptr && file != nullptr) {
        keyframe->setValue(2.0, sendNotificationSync);
        expect(std::abs(start->getValue() - file->getKeyframe(1)->getStartPosition()) < 1.0,
               "the start position follows the selected keyframe");
        start->setValue(1000.0, sendNotificationSync);
        expect(std::abs(file->getKeyframe(1)->getStartPosition() - 1000.0) < 1.0, "the start position crops the audio");
      }
      expect(find(editor, "Flatten wavetable to editable frames") != nullptr, "flattening is an explicit action");
      editor.removeFromDesktop();
    }
  }

  // Autoload waits for the arrows to stop and loads the preset the selection
  // rests on; a refresh keeps the list on screen while it rescans.
  void testPresetAutoloadAndRefresh() {
    SynthPlugin synth;
    SynthEditor editor(synth);
    editor.updateFullGui();
    editor.refreshPresetLibrary();
    pumpUntil([&] { return !editor.isPresetLibraryBusy(); }, 60000);
    if (editor.presetLibrarySize() < 3) {
      std::cout << "preset autoload test skipped: fewer than three presets installed\n";
      return;
    }
    auto* table = dynamic_cast<TableListBox*>(titled(editor, "Preset results table"));
    auto* autoload = dynamic_cast<Button*>(titled(editor, "Autoload preset when scrolling"));
    expect(table != nullptr && autoload != nullptr, "the preset table and autoload toggle exist");
    if (table == nullptr || autoload == nullptr)
      return;
    const bool autoload_was_on = autoload->getToggleState();
    autoload->setToggleState(true, dontSendNotification);
    // A single press loads at once.
    MessageManager::getInstance()->runDispatchLoopUntil(300);
    table->selectRow(1);
    expect(synth.getActiveFile() == editor.presetFileAtRow(1), "a single press loads the preset at once");
    // Holding the arrow: key repeats every 30 ms keep loading, and the last one always loads.
    MessageManager::getInstance()->runDispatchLoopUntil(300);
    int loads = 0;
    File active = synth.getActiveFile();
    const int rows = jmin(editor.presetLibrarySize(), 22);
    for (int row = 2; row < rows; ++row) {
      table->selectRow(row);
      MessageManager::getInstance()->runDispatchLoopUntil(30);
      if (synth.getActiveFile() != active) { ++loads; active = synth.getActiveFile(); }
    }
    pumpUntil([&] { return synth.getActiveFile() == editor.presetFileAtRow(rows - 1); }, 3000);
    expect(rows < 12 || loads >= 2, "holding an arrow keeps loading presets, got " + std::to_string(loads));
    expect(synth.getActiveFile() == editor.presetFileAtRow(rows - 1), "the preset the selection rests on loads last");
    expect(table->getSelectedRow() == rows - 1, "loading keeps the selection on the loaded preset");

    editor.refreshPresetLibrary();
    expect(table->getNumRows() > 0 && editor.presetLibrarySize() > 0, "a refresh keeps the current list on screen");
    pumpUntil([&] { return !editor.isPresetLibraryBusy(); }, 60000);
    expect(table->getNumRows() > 0, "the refreshed list is shown");
    autoload->setToggleState(autoload_was_on, dontSendNotification);
  }

  // Convolution Reverb predelay: Free shows milliseconds, synced modes show the division.
  void testConvolutionPredelayRows() {
    SynthPlugin synth;
    synth.parameterChanged("fx_slot_1_type", vital::constants::kConvolutionReverb + 1);
    SynthEditor editor(synth);
    editor.updateFullGui();
    pumpEditorEvents();
    auto rowTitles = [&] {
      StringArray titles;
      if (auto* slot = editorSection(editor, "Chain and routing", 0))
        for (auto* control : editor.getKeyboardFocusOrder())
          if (slot->isParentOf(control))
            titles.add(control->getTitle());
      return titles.joinIntoString(" | ");
    };
    String rows = rowTitles();
    expect(rows.containsIgnoreCase("Predelay sync") && rows.containsIgnoreCase("Fade in") &&
           rows.containsIgnoreCase("Fade out") && rows.containsIgnoreCase("Tone"),
           ("the reverb offers predelay, fades and tone: " + rows).toStdString());
    expect(rows.contains("| Predelay |"), "Free predelay shows the milliseconds row");
    expect(rows.indexOf("Fade In") < rows.indexOf("Predelay Sync") && rows.indexOf("IR End") < rows.indexOf("Fade In"),
           "the fades sit with the impulse response controls");
    expect(!rows.containsIgnoreCase("Predelay tempo"), "Free predelay hides the division row");
    synth.parameterChanged("fx_slot_1_convolution_reverb_predelay_sync", 1.0f);
    pumpUntil([&] { return rowTitles().containsIgnoreCase("Predelay tempo"); });
    rows = rowTitles();
    expect(rows.containsIgnoreCase("Predelay tempo"), ("synced predelay shows the division row: " + rows).toStdString());
  }

  // A modulation amount typed in destination units keeps fractions on stepped
  // controls: 0.5 semitones on Transpose used to round to nothing.
  void testFractionalModulationAmountOnTranspose() {
    SynthPlugin synth;
    SynthEditor editor(synth);
    editor.updateFullGui();
    pumpEditorEvents();
    const String summary = editor.assignModulation("lfo_1", "osc_1_transpose", "0.5");
    pumpEditorEvents();
    const int connection = synth.getConnectionIndex("lfo_1", "osc_1_transpose");
    expect(connection >= 0, "LFO 1 is routed to Oscillator 1 Transpose");
    if (connection < 0)
      return;
    const auto& controls = synth.getControls();
    const float amount = controls.at("modulation_" + std::to_string(connection + 1) + "_amount")->value();
    expect(std::abs(amount * 96.0f - 0.5f) < 1.0e-3f,
           "an amount of 0.5 semitones is 0.5 semitones of the 96-semitone range, got " + std::to_string(amount * 96.0f));
    const bool bipolar = controls.at("modulation_" + std::to_string(connection + 1) + "_bipolar")->value() >= 0.5f;
    expect(bipolar, "an LFO route is bipolar by default");
    expect(summary == "amount 0.5 semitones, bipolar: 0.25 semitones each way",
           ("the confirmation names the swing: " + summary).toStdString());
  }

  // Typing a value, adding modulation and removing it all close or rebuild
  // around the focused control; focus must come back to it, not the top of the window.
  void testModulationPromptsKeepFocus() {
    SynthPlugin synth;
    SynthEditor editor(synth);
    editor.updateFullGui();
    editor.addToDesktop(0);
    editor.setVisible(true);
    editor.toFront(true);
    pumpEditorEvents();

    const String id = "filter_1_cutoff";
    auto focusRow = [&]() -> Component* {
      auto* control = editor.controlForParameter(id);
      if (control != nullptr) {
        editor.ensureComponentVisible(control);
        control->grabKeyboardFocus();
      }
      return control;
    };
    auto focusedName = [] {
      auto* focused = Component::getCurrentlyFocusedComponent();
      return focused != nullptr ? focused->getTitle().toStdString() : std::string("nothing");
    };
    auto typeValue = [&](Component& control, const String& text, std::function<void()> whileOpen = {}) {
      control.keyPressed(KeyPress(KeyPress::returnKey));
      auto* field = dynamic_cast<TextEditor*>(Component::getCurrentlyFocusedComponent());
      expect(field != nullptr, "Return on a slider opens the value field, focused " + focusedName());
      if (field == nullptr)
        return;
      if (whileOpen)
        whileOpen();
      field->setText(text, dontSendNotification);
      field->keyPressed(KeyPress(KeyPress::returnKey));
      pumpEditorEvents();
    };

    auto* cutoff = focusRow();
    expect(cutoff != nullptr, "Filter 1 Cutoff has a row");
    if (cutoff == nullptr) {
      editor.removeFromDesktop();
      return;
    }
    typeValue(*cutoff, "1000 Hz");
    expect(editor.focusedRowParameterId() == id, "a typed value returns focus to its control, focused " + focusedName());

    // The rows are rebuilt while the field is open: focus finds the new row.
    if ((cutoff = focusRow()) != nullptr)
      typeValue(*cutoff, "2000 Hz", [&] { editor.updateFullGui(); });
    expect(editor.focusedRowParameterId() == id,
           "a value typed while the rows were rebuilt returns focus to the new control, focused " + focusedName());

    // A new route adds a slot, which rebuilds the rows.
    if ((cutoff = focusRow()) != nullptr)
      editor.assignModulation("lfo_1", id, "50%", cutoff);
    pumpEditorEvents();
    expect(synth.getConnectionIndex("lfo_1", id.toStdString()) >= 0, "LFO 1 modulates Filter 1 Cutoff");
    expect(editor.focusedRowParameterId() == id, "adding modulation returns focus to its control, focused " + focusedName());

    // A typed value after adding modulation, as the next edit would be.
    if ((cutoff = focusRow()) != nullptr)
      typeValue(*cutoff, "500 Hz");
    expect(editor.focusedRowParameterId() == id,
           "a value typed after adding modulation returns focus to its control, focused " + focusedName());

    if ((cutoff = focusRow()) != nullptr)
      editor.removeModulation("lfo_1", id, *cutoff);
    pumpEditorEvents();
    expect(synth.getConnectionIndex("lfo_1", id.toStdString()) < 0, "the route is removed");
    expect(editor.focusedRowParameterId() == id, "removing modulation returns focus to its control, focused " + focusedName());
    editor.removeFromDesktop();
  }

  void testOscillatorRowGroups() {
    SynthPlugin synth;
    SynthEditor editor(synth);
    editor.updateFullGui();
    pumpEditorEvents();

    // The nearest enclosing group key of a control.
    auto groupOf = [](Component* control) -> String {
      for (auto* parent = control ? control->getParentComponent() : nullptr; parent != nullptr;
           parent = parent->getParentComponent()) {
        const String key = parent->getProperties()["SectionName"].toString();
        if (key.isNotEmpty())
          return key;
      }
      return {};
    };
    // Rows carry their oscillator's number, not its name: "1 Level".
    auto row = [&](int oscillator, const String& title) -> Component* {
      auto* section = editorSection(editor, "Oscillator " + String(oscillator));
      return section != nullptr ? titled(*section, String(oscillator) + " " + title) : nullptr;
    };
    auto check = [&](int oscillator, const String& title, const String& expected_group) {
      auto* control = row(oscillator, title);
      expect(control != nullptr, "Oscillator " + std::to_string(oscillator) + " " + title.toStdString() + " exists");
      if (control != nullptr)
        expect(groupOf(control) == expected_group,
               title.toStdString() + " sits in " + expected_group.toStdString() + " (found " +
               groupOf(control).toStdString() + ")");
    };
    check(1, "Switch", "Oscillator 1");
    check(1, "Type", "Oscillator 1");
    check(1, "Level", "Oscillator 1");
    check(1, "Pan", "Oscillator 1");
    check(1, "Solo", "Oscillator 1");
    check(1, "Transpose", "Oscillator 1 pitch and unison");
    check(1, "Unison Voices", "Oscillator 1 pitch and unison");
    check(1, "Transpose Quantize Key", "Oscillator 1 scale lock");
    check(1, "Distortion mode", "Oscillator 1 warp");
    check(1, "Secondary Distortion Spread", "Oscillator 1 warp");
    check(1, "Secondary Frequency Morph Spread", "Oscillator 1 warp");
    check(1, "Frequency Morph Type", "Oscillator 1 warp");
    check(1, "Wave Frame", "Oscillator 1 wavetable controls");
    auto* scale = editorSection(editor, "Oscillator 1 scale lock");
    expect(scale != nullptr && groupOf(scale) == "Oscillator 1 pitch and unison", "Scale lock nests inside Pitch and unison");
    expect(scale != nullptr && scale->getTitle() == "Scale lock", "the Scale lock group is titled");
    auto* pitch = editorSection(editor, "Oscillator 1 pitch and unison");
    expect(pitch != nullptr && pitch->getTitle() == "Pitch and unison" && groupOf(pitch) == "Oscillator 1",
           "Pitch and unison is a group of the oscillator section");
    expect(editorSection(editor, "Oscillator 1 analog controls") == nullptr, "a wavetable slot has no Analog controls group");

    // Sample, Granular and Noise slots.
    check(5, "Sample Start", "Oscillator 5 sample controls");
    check(5, "Sample Transpose", "Oscillator 5 pitch and unison");
    auto* sample_scale = editorSection(editor, "Oscillator 5 scale lock");
    expect(sample_scale != nullptr && groupOf(sample_scale) == "Oscillator 5 pitch and unison",
           "a Sample slot's scale lock rows nest in its Pitch and unison group");
    check(6, "Granular Density", "Oscillator 6 granular controls");
    check(6, "Granular Transpose", "Oscillator 6 pitch and unison");
    expect(editorSection(editor, "Oscillator 6 sample controls") != nullptr, "a Granular slot keeps the sample loading group");
    check(8, "Noise Width", "Oscillator 8 noise controls");
    expect(editorSection(editor, "Oscillator 8 pitch and unison") == nullptr, "a Noise slot has no pitch rows, so no group");
    expect(editorSection(editor, "Oscillator 8 warp") != nullptr, "a Noise slot keeps its Warp group");
    expect(titled(*editorSection(editor, "Oscillator 8"), "8 Secondary Distortion Spread") == nullptr,
           "unison spreads are hidden on a Noise slot");

    // Keyboard order follows the document: output rows, then the groups in order.
    const auto& order = editor.getKeyboardFocusOrder();
    auto position = [&](Component* control) {
      return int(std::find(order.begin(), order.end(), control) - order.begin());
    };
    auto pos = [&](const String& title) { return position(row(1, title)); };
    expect(pos("Switch") < pos("Solo") && pos("Solo") < pos("Type") &&
           pos("Type") < pos("Level") && pos("Level") < pos("Pan"),
           "output rows read switch, solo, type, level, pan");
    expect(pos("Pan") < position(pitch) && position(pitch) < pos("Transpose") &&
           pos("Transpose") < position(scale) && position(scale) < pos("Transpose Quantize Key") &&
           pos("Transpose Quantize Key") < pos("Distortion mode"),
           "groups follow the output rows in order: pitch, scale lock, warp");

    // Every type reads the same way: output rows, controls, pitch, warp.
    SynthPlugin typed;
    typed.parameterChanged("osc_2_type", float(vital::source_types::kAnalog));
    SynthEditor typed_editor(typed);
    typed_editor.updateFullGui();
    pumpEditorEvents();
    const auto& typed_order = typed_editor.getKeyboardFocusOrder();
    auto at = [&](Component* control) {
      const auto found = std::find(typed_order.begin(), typed_order.end(), control);
      return found == typed_order.end() ? -1 : int(found - typed_order.begin());
    };
    const char* controls_keys[] = { "wavetable controls", "analog controls", "wavetable controls", "wavetable controls",
                                    "sample controls", "sample controls", "spectral controls", "noise controls" };
    for (int oscillator = 1; oscillator <= 8; ++oscillator) {
      const String name = "Oscillator " + String(oscillator);
      auto* section = editorSection(typed_editor, name);
      expect(section != nullptr, (name + " exists").toStdString());
      if (section == nullptr)
        continue;
      std::vector<std::pair<String, int>> sequence;
      for (const String row : { "Switch", "Solo", "Type", "Level", "Pan" })
        sequence.push_back({ row, at(titled(*section, String(oscillator) + " " + row)) });
      sequence.push_back({ "Destination", at(titled(*section, String(oscillator) + " Destination")) });
      sequence.push_back({ "controls", at(editorSection(typed_editor, name + " " + controls_keys[oscillator - 1])) });
      if (oscillator == 6)
        sequence.push_back({ "granular controls", at(editorSection(typed_editor, name + " granular controls")) });
      if (auto* pitch_group = editorSection(typed_editor, name + " pitch and unison"))
        sequence.push_back({ "pitch", at(pitch_group) });
      sequence.push_back({ "warp", at(editorSection(typed_editor, name + " warp")) });
      bool ordered = true;
      String found;
      for (size_t i = 0; i < sequence.size(); ++i) {
        found << sequence[i].first << "=" << sequence[i].second << " ";
        ordered = ordered && sequence[i].second >= 0 && (i == 0 || sequence[i - 1].second < sequence[i].second);
      }
      expect(ordered, (name + " reads switch, solo, type, level, pan, destination, controls, pitch, warp: " + found).toStdString());
    }
  }

  // Each arpeggiator's destinations are a Destinations group VoiceOver can
  // enter, holding one row per oscillator named "Oscillator N, <type>".
  void testArpeggiatorDestinationsAreAGroup() {
    SynthPlugin synth;
    SynthEditor editor(synth);
    editor.updateFullGui();
    pumpEditorEvents();
    const auto& order = editor.getKeyboardFocusOrder();
    auto position = [&](Component* control) {
      const auto found = std::find(order.begin(), order.end(), control);
      return found == order.end() ? -1 : int(found - order.begin());
    };
    const char* types[] = { "wavetable", "wavetable", "wavetable", "wavetable", "sample", "granular", "spectral", "noise" };
    for (int arp = 1; arp <= 2; ++arp) {
      const String name = "Arpeggiator " + String(arp);
      auto* section = editorSection(editor, name);
      auto* group = editorSection(editor, name + " destinations");
      expect(section != nullptr && group != nullptr, (name + " has a Destinations group").toStdString());
      if (section == nullptr || group == nullptr)
        continue;
      expect(group->getTitle() == "Destinations" && section->isParentOf(group) &&
                 group->isFocusContainer(),
             (name + "'s Destinations group is a focus container inside the arpeggiator").toStdString());
      const auto handler = group->createAccessibilityHandler();
      expect(handler != nullptr && handler->getRole() == AccessibilityRole::group, "Destinations reads as a group");

      int previous = position(group);
      expect(previous > position(titled(*section, "Output channel")), "Destinations follow the arpeggiator's other rows");
      for (int oscillator = 1; oscillator <= 8; ++oscillator) {
        const String title = "Oscillator " + String(oscillator) + ", " + types[oscillator - 1];
        auto* row = titled(*group, title);
        expect(row != nullptr && group->isParentOf(row), (name + " groups " + title).toStdString());
        expect(position(row) > previous, (name + " reads " + title + " in order").toStdString());
        previous = position(row);
      }
      // Nothing is left outside the group, and no row still says Destination.
      std::function<int(Component&)> count = [&](Component& parent) {
        int found = parent.getTitle().startsWith("Destination oscillator") ? 1 : 0;
        if (&parent != group && parent.getTitle().startsWith("Oscillator ") && parent.getWantsKeyboardFocus())
          ++found;
        for (auto* child : parent.getChildren())
          if (child != group) found += count(*child);
        return found;
      };
      expect(count(*section) == 0, (name + " keeps every destination inside its group").toStdString());
    }
  }

  // An oscillator's section names it, so its rows do not repeat the name but
  // keep its number, for every type: "Oscillator 1 Sample Loop" reads "1 Sample Loop".
  void testOscillatorRowsNameTheirOscillatorNumber() {
    for (int type = 0; type < vital::source_types::kNumTypes; ++type) {
      SynthPlugin synth;
      synth.parameterChanged("osc_1_type", float(type));
      SynthEditor editor(synth);
      editor.updateFullGui();
      pumpEditorEvents();
      auto* section = editorSection(editor, "Oscillator 1");
      const std::string type_name = strings::kOscillatorTypeNames[type];
      expect(section != nullptr, "Oscillator 1 section exists for type " + type_name);
      if (section == nullptr)
        continue;
      int rows = 0;
      StringArray misnamed;
      std::function<void(Component&)> visit = [&](Component& component) {
        const bool row_control = dynamic_cast<Slider*>(&component) != nullptr ||
                                 dynamic_cast<ComboBox*>(&component) != nullptr ||
                                 dynamic_cast<ToggleButton*>(&component) != nullptr;
        if (row_control && component.getWantsKeyboardFocus() && component.getTitle().isNotEmpty()) {
          ++rows;
          if (!component.getTitle().startsWith("1 ") || component.getTitle().contains("Oscillator 1"))
            misnamed.add(component.getTitle());
        }
        for (auto* child : component.getChildren())
          visit(*child);
      };
      visit(*section);
      expect(rows > 10, type_name + " shows its controls");
      expect(misnamed.isEmpty(), type_name + " rows read the oscillator number, not its name: " +
                                     misnamed.joinIntoString(", ").toStdString());
      expect(titled(*section, "1 Switch") != nullptr && titled(*section, "1 Solo") != nullptr &&
                 titled(*section, "1 Level") != nullptr && titled(*section, "1 Destination") != nullptr,
             type_name + " reads 1 Switch, 1 Solo, 1 Level and 1 Destination");
      if (type == vital::source_types::kSample)
        expect(titled(*section, "1 Sample Loop") != nullptr && titled(*section, "1 Sample Bounce") != nullptr,
               "a Sample oscillator reads 1 Sample Loop and 1 Sample Bounce");
    }
  }

  void testGranularSizeModeNamesBothChoices() {
    SynthPlugin synth;
    SynthEditor editor(synth);
    pumpEditorEvents();
    const auto valueText = [&]() {
      auto* control = dynamic_cast<Slider*>(titled(editor, "6 Granular size mode"));
      expect(control != nullptr, "Granular size mode must be a named choice, not an on/off toggle");
      auto handler = control ? control->createAccessibilityHandler() : nullptr;
      auto* value = handler ? handler->getValueInterface() : nullptr;
      return value ? value->getCurrentValueAsString() : String();
    };
    expect(valueText() == "Milliseconds", "Granular size mode reads Milliseconds by default");
    synth.parameterChanged(slot_ids::id("granular_size_mode"), 1.0f);
    pumpEditorEvents();
    expect(valueText() == "Grid steps", "Granular size mode reads Grid steps when selected");
  }

  // Levels and amounts read as percentages of their range, 0% to 100% or
  // -100% to 100% for modulation amounts, and take percentages typed back.
  void testLevelsAndAmountsReadAsPercent() {
    SynthPlugin synth;
    std::map<String, ValueBridge*> bridges;
    for (auto* parameter : synth.getParameters())
      if (auto* bridge = dynamic_cast<ValueBridge*>(parameter))
        bridges[bridge->getParameterId()] = bridge;
    const auto text = [&](const String& id, float engine) {
      auto* bridge = bridges[id];
      return bridge ? bridge->getText(bridge->convertToPluginValue(engine), 128) : String("<missing " + id + ">");
    };
    const auto typed = [&](const String& id, const String& entry) {
      auto* bridge = bridges[id];
      return bridge ? bridge->convertToEngineValue(bridge->getValueForText(entry)) : -99.0f;
    };
    expect(text("osc_1_level", 0.5f) == "50%", "a level reads as the percent of its control: " +
                                                   text("osc_1_level", 0.5f).toStdString());
    expect(text("osc_1_level", 0.70710678f) == "70.7%", "the default level reads 70.7%");
    expect(text("env_1_sustain", 1.0f) == "100%", "envelope sustain reads as a percentage");
    expect(text("modulation_1_amount", -1.0f) == "-100%" && text("modulation_1_amount", 0.255f) == "25.5%" &&
               text("modulation_1_amount", 0.0f) == "0%",
           "modulation amounts read from -100% to 100%: " + text("modulation_1_amount", 0.255f).toStdString());
    expect(text("volume", 5473.0404f).endsWith("dB"), "Global Gain keeps its decibels");
    expect(!text("lfo_1_phase", 0.25f).endsWith("%"), "an LFO phase is a position, not an amount");
    close(typed("osc_1_level", "50%"), 0.5f, "typing 50% sets half the level");
    close(typed("osc_1_level", "25"), 0.25f, "a number beyond 1 is a percentage");
    close(typed("modulation_1_amount", "-25%"), -0.25f, "typing -25% sets a negative amount");
    close(typed("modulation_1_amount", "0.5"), 0.5f, "a fraction up to 1 keeps its old meaning");
    close(typed("modulation_1_amount", "250%"), 1.0f, "typed amounts stop at 100%");

    // Keys move these rows one percent at a time, on both ranges.
    SynthEditor editor(synth);
    editor.updateFullGui();
    std::function<Component*(Component&)> slotOne = [&](Component& parent) -> Component* {
      if (parent.getProperties().contains("ModulationSlot") && static_cast<int>(parent.getProperties()["ModulationSlot"]) == 1)
        return &parent;
      for (auto* child : parent.getChildren())
        if (auto* found = slotOne(*child)) return found;
      return nullptr;
    };
    auto* slot = slotOne(editor);
    auto* amount = slot ? dynamic_cast<ParameterSlider*>(titled(*slot, "Amount")) : nullptr;
    expect(amount != nullptr, "modulation slot one has an Amount row");
    if (amount != nullptr) {
      amount->keyPressed(KeyPress(KeyPress::upKey));
      close(synth.getControls().at("modulation_1_amount")->value(), 0.01f, "Up raises a modulation amount by 1%");
      expect(amount->getTextFromValue(amount->getValue()) == "1%", "the Amount row reads 1%");
      amount->keyPressed(KeyPress(KeyPress::backspaceKey));
    }
    auto* level = dynamic_cast<ParameterSlider*>(titled(*editorSection(editor, "Oscillator 1"), "1 Level"));
    expect(level != nullptr, "Oscillator 1 has a Level row");
    if (level != nullptr) {
      level->keyPressed(KeyPress(KeyPress::endKey));
      level->keyPressed(KeyPress(KeyPress::upKey));
      close(synth.getControls().at("osc_1_level")->value(), 0.01f, "Up raises a level by 1%");
      expect(level->getTextFromValue(level->getValue()) == "1%", "the Level row reads 1%");
      level->keyPressed(KeyPress(KeyPress::backspaceKey));
    }
  }

  void testFilterFxModelControlsAndValues() {
    SynthPlugin synth;
    for (int chain = 0; chain <= vital::kNumBuses; ++chain) {
      const std::string prefix = chain == 0 ? "" : "bus_" + std::to_string(chain) + "_";
      for (int slot : { 19, 20 }) {
        const auto slot_prefix = prefix + "fx_slot_" + std::to_string(slot) + "_";
        synth.parameterChanged(slot_prefix + "type", vital::constants::kFilterFx + 1);
        synth.parameterChanged(slot_prefix + "filter_fx_model", vital::constants::kAnalog);
        synth.parameterChanged(slot_prefix + "filter_fx_style", 0);
      }
    }
    SynthEditor editor(synth);
    pumpEditorEvents();

    const auto slider = [&](Component* group, const String& title) {
      auto* result = group ? dynamic_cast<Slider*>(titled(*group, title)) : nullptr;
      expect(result != nullptr, "Filter FX exposes " + title.toStdString());
      return result;
    };
    const auto valueText = [&](Slider* control) {
      auto handler = control ? control->createAccessibilityHandler() : nullptr;
      auto* value = handler ? handler->getValueInterface() : nullptr;
      expect(value != nullptr, "Filter FX exposes an accessible value");
      return value ? value->getCurrentValueAsString() : String();
    };
    // Model and style change which rows the slot shows, so they are menus.
    const auto menu = [&](Component* group, const String& title) {
      auto* result = group ? dynamic_cast<ComboBox*>(titled(*group, title)) : nullptr;
      expect(result != nullptr, "Filter FX offers " + title.toStdString() + " as a menu");
      return result;
    };
    const auto checkStyles = [&](Component* group, const StringArray& names) {
      auto* style = menu(group, "Filter style");
      if (!style)
        return;
      StringArray items;
      for (int i = 0; i < style->getNumItems(); ++i)
        items.add(style->getItemText(i));
      expect(items == names, "The style menu lists only this model's choices: " +
                                 items.joinIntoString(", ").toStdString());
      style->setSelectedItemIndex(names.size() - 1, sendNotificationSync);
      expect(style->getText() == names[names.size() - 1], "Filter FX announces the chosen style");
    };

    for (int chain = 0; chain <= vital::kNumBuses; ++chain) {
      const std::string prefix = chain == 0 ? "" : "bus_" + std::to_string(chain) + "_";
      const String section = chain == 0 ? String("Chain and routing")
                                         : "Bus " + String(chain) + " - Chain and routing";
      const auto filter_prefix = prefix + "fx_slot_20_filter_fx_";
      auto* group = editorSection(editor, section, 19);
      expect(group != nullptr, "Every effects chain exposes its Filter FX slot");
      if (!group)
        continue;
      expect(!titled(*group, "Comb Blend Offset") && !titled(*group, "Formant X"),
             "Analog hides comb and vowel controls");
      checkStyles(group, { "12dB", "24dB", "Notch Blend", "Notch Spread", "B/P/N" });
      auto* blend = slider(group, "Filter blend, low pass to high pass");
      if (blend) {
        for (int i = 0; i < 3; ++i) {
          blend->setValue(i * 0.5, sendNotificationSync);
          expect(valueText(blend) == StringArray { "Low pass", "Band pass", "High pass" }[i],
                 "Filter FX blend announces the pass type");
        }
        blend->setValue(0.25, sendNotificationSync);
        expect(valueText(blend) == "Between low pass and band pass, 0.50",
               "Intermediate blend announces the value and surrounding pass types");
      }

      // This change goes through the accessible row; let the normal timer refresh it.
      auto* model = menu(group, "Filter model");
      if (!model)
        continue;
      expect(model->getNumItems() == vital::constants::kNumFilterModels, "The model menu lists every filter model");
      model->setSelectedItemIndex(vital::constants::kComb, sendNotificationSync);
      expect(synth.getControls().at(filter_prefix + "model")->value() == float(vital::constants::kComb),
             "Choosing Comb from the model menu applies it");
      pumpEditorEvents();
      group = editorSection(editor, section, 19);
      expect(group && titled(*group, "Comb Blend Offset") && !titled(*group, "Formant X") &&
                 !titled(*group, "Drive"), "Comb shows its offset and hides drive and vowel controls");
      checkStyles(group, { "Low High Comb", "Low High Flange+", "Low High Flange-",
                           "Band Spread Comb", "Band Spread Flange+", "Band Spread Flange-" });
      blend = slider(group, "Comb blend");
      if (blend) {
        blend->setValue(0, sendNotificationSync);
        expect(valueText(blend) == "Low comb", "Comb blend announces its low endpoint");
        blend->setValue(0.5, sendNotificationSync);
        expect(valueText(blend) == "Low to high comb, 1.00", "Comb blend announces intermediate values");
        blend->setValue(1, sendNotificationSync);
        expect(valueText(blend) == "High comb", "Comb blend announces its high endpoint");
      }

      // Simulate automation changing the model while the editor remains open.
      synth.parameterChanged(filter_prefix + "model", vital::constants::kFormant);
      pumpEditorEvents();
      group = editorSection(editor, section, 19);
      expect(group && titled(*group, "Formant X") && titled(*group, "Formant Y") &&
                 titled(*group, "Formant Transpose") && !titled(*group, "Comb Blend Offset") &&
                 !titled(*group, "Cutoff") && !titled(*group, "Drive") && !titled(*group, "Comb blend"),
             "Automating the vowel model refreshes its controls without reopening the editor");
      checkStyles(group, { "AOIE", "AIUO" });

      synth.parameterChanged(filter_prefix + "model", vital::constants::kDiode);
      pumpEditorEvents();
      group = editorSection(editor, section, 19);
      checkStyles(group, { "Low Shelf", "Low Cut" });
      expect(group && !titled(*group, "Formant X") && !titled(*group, "Comb Blend Offset"),
             "Leaving vowel mode removes its controls");

      synth.parameterChanged(filter_prefix + "model", vital::constants::kPhase);
      pumpEditorEvents();
      group = editorSection(editor, section, 19);
      checkStyles(group, { "Positive", "Negative" });
      synth.parameterChanged(filter_prefix + "model", vital::constants::kReverbFilter);
      pumpEditorEvents();
      group = editorSection(editor, section, 19);
      for (const char* name : { "Reverb Cutoff", "Resonance", "Damp", "Bass Preserve", "Mix", "Output" }) {
        auto* control = slider(group, name);
        expect(control && control->isVisible() && control->getWidth() > 0,
               std::string("Reverb FX has a visible control: ") + name);
        expect(valueText(control).isNotEmpty(), "Every Reverb control has an accessible value");
      }
      expect(group && !titled(*group, "Filter style") && !titled(*group, "Drive") &&
                 !titled(*group, "Filter blend, low pass to high pass"),
             "Reverb hides unrelated slope, drive and pass blend controls");
      if (auto* cutoff = slider(group, "Reverb Cutoff")) {
        cutoff->keyPressed(KeyPress(KeyPress::endKey));
        expect(valueText(cutoff).contains("8") && valueText(cutoff).contains("Hz"),
               "Reverb cutoff reaches and announces 8 Hz");
      }
      synth.parameterChanged(filter_prefix + "model", vital::constants::kScreamFilter);
      pumpEditorEvents();
      group = editorSection(editor, section, 19);
      for (const char* name : { "Cutoff", "Scream", "Resonance", "Input Gain", "Mix", "Output", "Autogain" }) {
        auto* control = titled(*group, name);
        expect(control && control->isVisible() && control->getWidth() > 0,
               std::string("Scream Filter FX exposes its control: ") + name);
      }
      expect(!titled(*group, "Gate Release") && !titled(*group, "Damp") && !titled(*group, "Drive"),
             "Scream filter hides effect gate and unrelated filter controls");
      auto* untouched = editorSection(editor, section, 18);
      expect(untouched && !titled(*untouched, "Comb Blend Offset") && !titled(*untouched, "Formant X"),
             "Changing one filter preserves another filter in the same chain");
      auto* untouched_style = menu(untouched, "Filter style");
      expect(untouched_style != nullptr && untouched_style->getText() == "12dB",
             "Each Filter FX slot uses its own model and style");
    }
    for (int voice_filter : {1, 2}) {
      const std::string prefix = "filter_" + std::to_string(voice_filter) + "_";
      synth.parameterChanged(prefix + "model", vital::constants::kReverbFilter);
      pumpEditorEvents();
      auto* group = editorSection(editor, "Filter " + String(voice_filter));
      expect(group != nullptr, "Voice filter section exists");
      for (const char* name : { "Reverb Cutoff", "Resonance", "Damp", "Bass Preserve", "Mix", "Output" }) {
        auto* control = slider(group, "Filter " + String(voice_filter) + " " + name);
        expect(control && control->isVisible() && control->getWidth() > 0 && valueText(control).isNotEmpty(),
               std::string("Voice Reverb has a visible accessible control: ") + name);
      }
      synth.parameterChanged(prefix + "model", vital::constants::kScreamFilter);
      pumpEditorEvents();
      group = editorSection(editor, "Filter " + String(voice_filter));
      for (const char* name : { "Cutoff", "Scream", "Resonance", "Input Gain", "Mix", "Output", "Autogain", "Key Track" }) {
        auto* control = titled(*group, "Filter " + String(voice_filter) + " " + name);
        expect(control && control->isVisible() && control->getWidth() > 0 && control->getWantsKeyboardFocus(),
               std::string("Voice Scream exposes a keyboard-accessible control: ") + name);
      }
      expect(!titled(*group, "Filter " + String(voice_filter) + " Gate Release"),
             "Voice Scream relies on Env 1, with no gate release control");
      synth.parameterChanged(prefix + "model", vital::constants::kAnalog);
      pumpEditorEvents();
      group = editorSection(editor, "Filter " + String(voice_filter));
      expect(group && !titled(*group, "Filter " + String(voice_filter) + " Damp") &&
                 !titled(*group, "Filter " + String(voice_filter) + " Output") &&
                 titled(*group, "Filter " + String(voice_filter) + " Drive"),
             "Switching away from Reverb restores the previous model's controls");
    }
  }
}
