// SPDX-License-Identifier: GPL-3.0-or-later
#include "screen_reader_speech.h"
#include "load_save.h"

#if JUCE_WINDOWS
#include "windows_screen_reader.h"
#endif

namespace atlas::speech {
  namespace {
    // Version 2 turned the LFO, Trance gate and wavetable editor announcements
    // on, because those editors say nothing without them. Settings saved before
    // version 2 get them on once; turning them off again afterwards is kept.
    constexpr int kSettingsVersion = 2;

    struct SpeechState {
      Settings settings;
      bool loaded = false;
      bool testing = false;
      std::function<void(const String&)> observer;
    };

    SpeechState& speechState() {
      static SpeechState state;
      return state;
    }

    void loadSettings(Settings& settings) {
      json config = LoadSave::getConfigJson();
      if (!config.count("accessibility_speech") || !config["accessibility_speech"].is_object())
        return;

      const json& data = config["accessibility_speech"];
      auto load = [&data](const char* name, bool& value) {
        if (data.count(name) && data[name].is_boolean())
          value = data[name].get<bool>();
      };
      load("speech_feedback", settings.speech_feedback);
      load("navigation", settings.navigation);
      load("presets", settings.presets);
      load("lfo_editor", settings.lfo_editor);
      load("wavetable_editor", settings.wavetable_editor);
      load("sample_browser", settings.sample_browser);
      load("modulation", settings.modulation);
      load("parameters", settings.parameters);
      load("other", settings.other);
      load("frequency_values_in_hz", settings.frequency_values_in_hz);

      const int version = data.count("version") && data["version"].is_number_integer()
          ? data["version"].get<int>() : 1;
      if (version < 2) {
        settings.lfo_editor = true;
        settings.wavetable_editor = true;
      }
    }
  }

  Settings& settings() {
    auto& state = speechState();
    if (!state.loaded) {
      state.loaded = true;
      loadSettings(state.settings);
    }
    return state.settings;
  }

  void saveSettings() {
    if (speechState().testing)
      return;

    const auto& current = settings();
    json config = LoadSave::getConfigJson();
    json data;
    data["version"] = kSettingsVersion;
    data["speech_feedback"] = current.speech_feedback;
    data["navigation"] = current.navigation;
    data["presets"] = current.presets;
    data["lfo_editor"] = current.lfo_editor;
    data["wavetable_editor"] = current.wavetable_editor;
    data["sample_browser"] = current.sample_browser;
    data["modulation"] = current.modulation;
    data["parameters"] = current.parameters;
    data["other"] = current.other;
    data["frequency_values_in_hz"] = current.frequency_values_in_hz;
    config["accessibility_speech"] = data;
    LoadSave::saveJsonToConfig(config);
  }

  Route announce(const String& message, AccessibilityHandler::AnnouncementPriority priority) {
    if (message.trim().isEmpty())
      return Route::kNone;

    auto& state = speechState();
    if (state.observer) {
      state.observer(message);
      return Route::kObserver;
    }

#if JUCE_WINDOWS
    const wchar_t* text = message.toWideCharPointer();
    if (windows_screen_reader::speakWithNvda(text))
      return Route::kNvda;

    // Without NVDA, another screen reader reads a notification raised on the
    // focused control, the element it is already reading.
    void* element = nullptr;
    if (auto* focused = Component::getCurrentlyFocusedComponent())
      if (auto* handler = focused->getAccessibilityHandler())
        element = handler->getNativeImplementation();
    if (windows_screen_reader::raiseNotification(element, text,
                                                 priority == AccessibilityHandler::AnnouncementPriority::high))
      return Route::kUiaNotification;
    return Route::kNone;
#else
    AccessibilityHandler::postAnnouncement(message, priority);
    return Route::kVoiceOver;
#endif
  }

  bool isNvdaRunning() {
#if JUCE_WINDOWS
    return windows_screen_reader::isNvdaRunning();
#else
    return false;
#endif
  }

  void useDefaultSettingsForTesting() {
    auto& state = speechState();
    state.settings = {};
    state.loaded = true;
    state.testing = true;
  }

  void setObserverForTesting(std::function<void(const String&)> observer) {
    speechState().observer = std::move(observer);
  }
}
