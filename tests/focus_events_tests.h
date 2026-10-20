#pragma once

// Each Tab tells a screen reader the focus moved once, to the control Tab
// reached. JUCE also raised UI Automation's focus event from the control
// losing the focus, at a moment no control had it, and NVDA found the window
// instead: in the Settings window it said "Atlas Settings dialog" again before
// every control (third_party/patches/juce8-uia-focus-loss.patch). Included
// into macro_modulation_tests.cpp, whose expect and pumpEditorEvents it uses.

#if JUCE_WINDOWS
#include "uia_focus_events.h"
#endif

namespace {
#if JUCE_WINDOWS
  void testSettingsTabRaisesOneFocusEvent() {
    atlas::speech::useDefaultSettingsForTesting();
    atlas::speech::setObserverForTesting([](const String&) {});
    SynthPlugin synth;
    SynthEditor editor(synth);
    editor.updateFullGui();
    editor.addToDesktop(ComponentPeer::windowHasTitleBar);
    editor.setVisible(true);
    editor.toFront(true);
    pumpEditorEvents();
    Component::SafePointer<DialogWindow> window(editor.showSettingsForTesting());
    MessageManager::getInstance()->runDispatchLoopUntil(800);
    auto* focused = Component::getCurrentlyFocusedComponent();
    expect(window != nullptr && focused != nullptr && window->isParentOf(focused),
           "the Settings window opens with the focus in it");
    if (window == nullptr || focused == nullptr || !window->isParentOf(focused)) {
      editor.removeFromDesktop();
      atlas::speech::setObserverForTesting({});
      return;
    }

    // Every focus event a Tab raises names the control it reached, never the
    // one it left. (UI Automation also hands each event on through its
    // bridge to the older accessibility events, so a client can get one
    // twice; screen readers drop the repeat.)
    {
      atlas::uia_test::FocusEventRecorder recorder([] { MessageManager::getInstance()->runDispatchLoopUntil(10); });
      expect(recorder.isListening(), "a UI Automation client listens for focus events");
      for (int tab = 0; tab < 6; ++tab) {
        const size_t before = recorder.names().size();
        if (auto* current = Component::getCurrentlyFocusedComponent())
          current->moveKeyboardFocusToSibling(true);
        MessageManager::getInstance()->runDispatchLoopUntil(300);
        auto* now = Component::getCurrentlyFocusedComponent();
        auto* handler = now != nullptr ? now->getAccessibilityHandler() : nullptr;
        const std::wstring reached = handler != nullptr ? handler->getTitle().toWideCharPointer() : L"(nothing)";
        const auto events = recorder.names();
        String heard;
        bool only_reached = events.size() > before;
        for (size_t i = before; i < events.size(); ++i) {
          heard << (heard.isEmpty() ? "" : " | ") << String(events[i].c_str());
          only_reached = only_reached && events[i] == reached;
        }
        expect(only_reached, "Tab " + std::to_string(tab + 1) + " says the focus moved only to " +
                                 String(reached.c_str()).toStdString() + ", heard: " + heard.toStdString());
      }
    }

    if (window != nullptr)
      delete window.getComponent();
    pumpEditorEvents();
    editor.removeFromDesktop();
    atlas::speech::setObserverForTesting({});
  }
#endif

  int runFocusEventTests() {
#if JUCE_WINDOWS
    testSettingsTabRaisesOneFocusEvent();
#else
    std::cout << "  The focus event test is for UI Automation, on Windows\n";
#endif
    if (failures == 0)
      std::cout << "Atlas focus event tests passed\n";
    return failures == 0 ? 0 : 1;
  }
}
