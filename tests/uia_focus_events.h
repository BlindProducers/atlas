#pragma once

// Windows only: a UI Automation client, as NVDA is, that records the name of
// every focus-changed event this process raises while it lives. Defined in
// uia_focus_events_windows.cpp, away from JUCE's headers.

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace atlas::uia_test {
  class FocusEventRecorder {
    public:
      // UI Automation calls back into this process's elements while it
      // registers and unregisters, so `pump` runs the message loop meanwhile.
      explicit FocusEventRecorder(std::function<void()> pump);
      ~FocusEventRecorder();

      bool isListening() const;
      // The focused elements' names, in the order their events came.
      std::vector<std::wstring> names() const;

    private:
      struct Impl;
      std::unique_ptr<Impl> impl_;
  };
}
