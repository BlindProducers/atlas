// SPDX-License-Identifier: GPL-3.0-or-later
// What NVDA reads from a JUCE text field through UI Automation. Built on its
// own, outside the test's JUCE translation unit, because it needs <windows.h>
// and UI Automation next to each other (as windows_screen_reader.cpp does).
#include "uia_text_reading.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <oleauto.h>
#include <uiautomation.h>
#pragma comment(lib, "oleaut32.lib")

namespace atlas::uia_test {
  namespace {
    template <typename Interface>
    struct Holder {
      Interface* pointer = nullptr;
      Holder() = default;
      Holder(const Holder&) = delete;
      Holder& operator=(const Holder&) = delete;
      ~Holder() {
        if (pointer != nullptr)
          pointer->Release();
      }
      Interface** out() { return &pointer; }
      Interface* operator->() const { return pointer; }
      explicit operator bool() const { return pointer != nullptr; }
    };

    // JUCE 8's AccessibilityNativeHandle derives first from
    // IRawElementProviderSimple, so the handle starts with an IUnknown.
    bool textProviderOf(void* juce_native_element, Holder<ITextProvider>& text) {
      if (juce_native_element == nullptr)
        return false;
      Holder<IRawElementProviderSimple> element;
      if (FAILED(static_cast<IUnknown*>(juce_native_element)->QueryInterface(IID_PPV_ARGS(element.out()))))
        return false;
      Holder<IUnknown> pattern;
      if (FAILED(element->GetPatternProvider(UIA_TextPatternId, pattern.out())) || !pattern)
        return false;
      return SUCCEEDED(pattern->QueryInterface(IID_PPV_ARGS(text.out()))) && text;
    }

    std::wstring textOf(ITextRangeProvider* range) {
      BSTR text = nullptr;
      if (FAILED(range->GetText(-1, &text)) || text == nullptr)
        return {};
      std::wstring result(text, SysStringLen(text));
      SysFreeString(text);
      return result;
    }

    // NVDA's caret (NVDAObjects/UIA UIATextInfo, POSITION_CARET): the first
    // selection range, collapsed to its start.
    bool nvdaCaret(ITextProvider* text, Holder<ITextRangeProvider>& caret) {
      SAFEARRAY* ranges = nullptr;
      if (FAILED(text->GetSelection(&ranges)) || ranges == nullptr)
        return false;
      LONG index = 0;
      IUnknown* first = nullptr;
      const bool found = SUCCEEDED(SafeArrayGetElement(ranges, &index, &first)) && first != nullptr;
      SafeArrayDestroy(ranges);
      if (!found)
        return false;
      Holder<ITextRangeProvider> selection;
      const HRESULT queried = first->QueryInterface(IID_PPV_ARGS(selection.out()));
      first->Release();
      if (FAILED(queried) || !selection || FAILED(selection->Clone(caret.out())) || !caret)
        return false;
      return SUCCEEDED(caret->MoveEndpointByRange(TextPatternRangeEndpoint_End, caret.pointer,
                                                  TextPatternRangeEndpoint_Start));
    }

    // NVDA's UIATextInfo.move: "Some Implementations of Move and moveEndpointByUnit
    // return a positive number even if the direction is negative."
    int nvdaMove(ITextRangeProvider* range, TextUnit unit, int direction) {
      int moved = 0;
      if (FAILED(range->Move(unit, direction, &moved)))
        return 0;
      return direction < 0 && moved > 0 ? -moved : moved;
    }
  }

  bool backspaceReading(void* juce_native_element, std::wstring& deleted) {
    deleted.clear();
    Holder<ITextProvider> text;
    Holder<ITextRangeProvider> test;
    if (!textProviderOf(juce_native_element, text) || !nvdaCaret(text.pointer, test))
      return false;
    // EditableText._backspaceScriptHelper: a copy of the caret moved back one
    // character and expanded to it is what NVDA says Backspace deletes.
    if (nvdaMove(test.pointer, TextUnit_Character, -1) < 0) {
      test->ExpandToEnclosingUnit(TextUnit_Character);
      deleted = textOf(test.pointer);
    }
    return true;
  }

  bool reviewPreviousCharacter(void* juce_native_element, std::wstring& character) {
    character.clear();
    Holder<ITextProvider> text;
    Holder<ITextRangeProvider> review;
    if (!textProviderOf(juce_native_element, text) || !nvdaCaret(text.pointer, review))
      return false;
    // globalCommands review_previousCharacter: expand to a character, collapse,
    // move back one, expand again.
    review->ExpandToEnclosingUnit(TextUnit_Character);
    review->MoveEndpointByRange(TextPatternRangeEndpoint_End, review.pointer, TextPatternRangeEndpoint_Start);
    if (nvdaMove(review.pointer, TextUnit_Character, -1) < 0) {
      review->ExpandToEnclosingUnit(TextUnit_Character);
      character = textOf(review.pointer);
    }
    return true;
  }

  bool characterAwayFromCaret(void* juce_native_element, int count, std::wstring& character, int& moved) {
    character.clear();
    moved = 0;
    Holder<ITextProvider> text;
    Holder<ITextRangeProvider> review;
    if (!textProviderOf(juce_native_element, text) || !nvdaCaret(text.pointer, review))
      return false;
    moved = nvdaMove(review.pointer, TextUnit_Character, count);
    review->ExpandToEnclosingUnit(TextUnit_Character);
    character = textOf(review.pointer);
    return true;
  }
}
