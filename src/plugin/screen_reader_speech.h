// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "JuceHeader.h"
#include <functional>

// Every message Atlas speaks goes through announce(). On Windows it goes to
// NVDA's speech and braille through NVDA's controller interface. With another
// screen reader running it is raised as a UI Automation notification. It never
// falls back to SAPI, the voice JUCE's AccessibilityHandler::postAnnouncement
// uses on Windows. On macOS it is posted to VoiceOver through JUCE.
namespace atlas::speech {
  // Which announcements the user hears, saved in the config file under
  // "accessibility_speech". The keyboard editors (LFO, Trance gate and
  // wavetable) have no other spoken feedback, so their announcements are on.
  struct Settings {
    bool speech_feedback = true;
    bool navigation = false;
    bool presets = false;
    bool lfo_editor = true;
    bool wavetable_editor = true;
    bool sample_browser = false;
    bool modulation = true;
    bool parameters = false;
    bool other = false;
    bool frequency_values_in_hz = false;
  };

  Settings& settings();
  void saveSettings();

  enum class Route { kNone, kObserver, kNvda, kUiaNotification, kVoiceOver };

  // Speaks `message` through the running screen reader and returns where it went.
  Route announce(const String& message, AccessibilityHandler::AnnouncementPriority priority);

  // True when NVDA answers on its controller interface (always false off Windows).
  bool isNvdaRunning();

  // Tests: use the default settings instead of the saved ones, never save them,
  // and send announcements to `observer` instead of the screen reader.
  void useDefaultSettingsForTesting();
  void setObserverForTesting(std::function<void(const String&)> observer);
}
