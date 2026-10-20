#pragma once

// NVDA reads the character Backspace deletes in a JUCE text field (the value
// and modulation amount prompts, a slider's text box) as it does in any
// Windows edit field: through UI Automation, before it passes the key on.
// JUCE's text range Move worked out the new position but never moved the
// range, and counted a backward move as a forward one, so NVDA read the
// character after the caret as the one deleted, the same letter again for
// every Backspace, or "blank" at the end of the text. With text selected it
// now says the selection Backspace deletes (third_party/patches/
// juce8-uia-text-reading.patch). Included into macro_modulation_tests.cpp,
// whose expect it uses.

#if JUCE_WINDOWS
#include "uia_text_reading.h"
#endif

namespace {
#if JUCE_WINDOWS
  std::string narrowText(const std::wstring& text) {
    return String(text.c_str()).toStdString();
  }

  void testBackspaceReadsTheDeletedCharacter() {
    TextEditor field;
    field.setSize(240, 30);
    field.addToDesktop(ComponentPeer::windowHasTitleBar);
    field.setVisible(true);
    MessageManager::getInstance()->runDispatchLoopUntil(100);
    auto* handler = field.getAccessibilityHandler();
    void* native = handler != nullptr ? handler->getNativeImplementation() : nullptr;
    expect(native != nullptr, "a text field on the desktop has a UI Automation element");
    if (native == nullptr) {
      field.removeFromDesktop();
      return;
    }

    auto nvdaReads = [&] {
      std::wstring deleted;
      expect(atlas::uia_test::backspaceReading(native, deleted), "the text field offers UI Automation text");
      return deleted;
    };
    auto deleteEach = [&](std::initializer_list<const wchar_t*> characters, const std::string& where) {
      for (const wchar_t* expected : characters) {
        const String before = field.getText();
        const std::wstring heard = nvdaReads();
        expect(heard == expected, "Backspace " + where + " in \"" + before.toStdString() + "\" reads \"" +
                                      narrowText(expected) + "\" (NVDA would read \"" + narrowText(heard) + "\")");
        field.keyPressed(KeyPress(KeyPress::backspaceKey));
      }
    };

    // From the end of the text, each Backspace reads the character it deletes:
    // not "blank", as it read at the end of every value before.
    field.setText("0.5 ms", dontSendNotification);
    field.moveCaretToEnd(false);
    deleteEach({ L"s", L"m", L" ", L"5", L".", L"0" }, "at the end");
    expect(field.getText().isEmpty(), "every character was deleted");
    expect(nvdaReads().empty(), "an empty field has nothing for Backspace to read");

    // In the middle, the character before the caret each time, never the one
    // after it again and again.
    field.setText("abcdef", dontSendNotification);
    field.setCaretPosition(4);
    deleteEach({ L"d", L"c", L"b", L"a" }, "in the middle");
    expect(field.getText() == "ef", "the characters after the caret stay");
    expect(nvdaReads().empty(), "at the start there is nothing before the caret to delete");

    // A highlighted letter: Backspace deletes it, and NVDA says it, though
    // NVDA takes a selection's start as its caret and would otherwise read the
    // letter before it, as in other Windows edit fields. The Backspaces after
    // it read their own characters again.
    field.setText("abcFgh", dontSendNotification);
    field.setHighlightedRegion({ 3, 4 });
    deleteEach({ L"F" }, "on a highlighted letter");
    expect(field.getText() == "abcgh", "Backspace deleted the highlighted letter");
    deleteEach({ L"c", L"b", L"a" }, "after deleting a highlighted letter");

    // A value prompt opens with its whole value selected: Backspace says the
    // value it deletes rather than "blank".
    field.setText("-12 dB", dontSendNotification);
    field.selectAll();
    deleteEach({ L"-12 dB" }, "on a selected value");
    expect(field.getText().isEmpty(), "Backspace deleted the selected value");

    // Reading back from the caret while text is selected, as NVDA's review
    // cursor does (it expands to a character first), still reads the character
    // before the selection: only Backspace's own reading spans it.
    field.setText("abcd", dontSendNotification);
    field.setHighlightedRegion({ 2, 3 });
    std::wstring before_selection;
    expect(atlas::uia_test::reviewPreviousCharacter(native, before_selection) && before_selection == L"b",
           "the review cursor's previous character is b (read \"" + narrowText(before_selection) + "\")");

    // Moving through the text a character at a time reaches each character,
    // and stops at the ends.
    field.setText("xyz", dontSendNotification);
    field.setCaretPosition(1);
    std::wstring character;
    int moved = 0;
    expect(atlas::uia_test::characterAwayFromCaret(native, 1, character, moved) && moved == 1 && character == L"z",
           "one character on from the caret is z (moved " + std::to_string(moved) + ", read \"" + narrowText(character) +
               "\")");
    expect(atlas::uia_test::characterAwayFromCaret(native, -1, character, moved) && moved == -1 && character == L"x",
           "one character back from the caret is x (moved " + std::to_string(moved) + ", read \"" +
               narrowText(character) + "\")");
    expect(atlas::uia_test::characterAwayFromCaret(native, -5, character, moved) && moved == -1 && character == L"x",
           "moving back stops at the start (moved " + std::to_string(moved) + ")");
    field.removeFromDesktop();
  }
#endif

  int runTextFieldReadingTests() {
#if JUCE_WINDOWS
    testBackspaceReadsTheDeletedCharacter();
#else
    std::cout << "  The text field reading test is for UI Automation, on Windows\n";
#endif
    if (failures == 0)
      std::cout << "Atlas text field reading tests passed\n";
    return failures == 0 ? 0 : 1;
  }
}
