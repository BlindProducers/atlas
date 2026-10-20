#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later
// What NVDA reads from a JUCE text field through UI Automation, for the tests
// (uia_text_reading_windows.cpp). Windows only.

#include <string>

namespace atlas::uia_test {
  // What NVDA says Backspace deleted, worked out as NVDA does before passing
  // the key on: its caret is the start of the field's first selection range,
  // and a copy moved back one character and expanded to it is read. Empty when
  // NVDA would find nothing before the caret. False when the field offers no
  // UI Automation text.
  bool backspaceReading(void* juce_native_element, std::wstring& deleted);

  // The character before NVDA's caret as its review cursor's previous
  // character reads it (expanding to a character before moving back).
  bool reviewPreviousCharacter(void* juce_native_element, std::wstring& character);

  // The character `count` characters away from NVDA's caret, as a review of
  // the field moves to it, and how many characters it moved.
  bool characterAwayFromCaret(void* juce_native_element, int count, std::wstring& character, int& moved);
}
