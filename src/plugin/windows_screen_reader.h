// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Windows speech for Atlas's announcements. The header stays free of JUCE and
// <windows.h> so the plugin unity build can include it.
namespace atlas::windows_screen_reader {
  // Speaks `text` with NVDA and shows it on NVDA's braille display, through
  // NVDA's controller interface (third_party/nvda_controller). False when NVDA
  // is not running, is asleep in this application, or the text is empty.
  bool speakWithNvda(const wchar_t* text);

  // True when NVDA answers on its controller interface.
  bool isNvdaRunning();

  // Raises a UI Automation notification with `text` on a JUCE accessibility
  // element (AccessibilityHandler::getNativeImplementation()). Narrator, JAWS
  // and NVDA read these. False when the element is missing or Windows has no
  // notification support (before Windows 10 1709).
  bool raiseNotification(void* juceNativeElement, const wchar_t* text, bool important);
}
