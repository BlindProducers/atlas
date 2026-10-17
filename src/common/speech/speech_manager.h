// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "JuceHeader.h"
#include "speech_phrase.h"
#include "espeak_speech.h"

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace vital {
namespace speech {
  struct SlotStatus {
    enum State { kEmpty, kRendering, kReady, kFailed };
    State state = kEmpty;
    int syllables = 0;
    // The syllables' phonemes, words separated by spaces and syllables by dots.
    std::string phonemes;
    std::string error;
    // Increases on every change, for the editor to notice.
    uint32_t revision = 0;
  };

  // Renders the oscillator slots' text with espeak-ng on a background
  // thread and hands the phrases to the engine on the message thread. A
  // slot renders while its type is Speech and it has text; the language and
  // variant come from its parameters, which a timer watches, so host
  // automation and preset loads re-render as edits in the editor do.
  class Manager : private juce::AsyncUpdater, private juce::Timer {
    public:
      static constexpr int kNumSlots = 8;
      static constexpr int kMaxTextLength = 4000;
      // espeak-ng pitches each phrase is spoken at, about four semitones
      // apart across its range; the middle one is the voice's own.
      static constexpr int kNumLayers = 5;
      static constexpr int kLayerPitches[kNumLayers] = { 6, 28, 50, 72, 94 };
      static constexpr int kReferenceLayer = 2;

      struct Voice {
        bool active = false;
        int language = 0;
        int variant = 0;
      };
      // Message thread (or the audio thread offline): give a slot its phrase.
      using ApplyCallback = std::function<void(int slot, std::shared_ptr<const Phrase> phrase)>;
      // The slot's current type and voice parameters.
      using VoiceCallback = std::function<Voice(int slot)>;

      Manager(ApplyCallback apply, VoiceCallback voice);
      ~Manager() override;

      // Starts watching the slots' parameters, once the voice callback can read them.
      void startWatching();

      // Message thread. Text longer than kMaxTextLength bytes is cut.
      void setText(int slot, const std::string& text);
      std::string text(int slot) const;
      void clearAll();
      // Starts the renders the slots' text and voices call for.
      void refresh();

      SlotStatus status(int slot) const;
      std::shared_ptr<const Phrase> phrase(int slot) const;

      bool hasPendingWork() const;
      // Tests and offline rendering: wait for renders, then apply them here.
      // The audio thread passes `blocking` false, as Multi-sample's does.
      bool waitForPendingWork(int timeout_ms);
      void processPendingResults(bool blocking = true);

    private:
      struct Key {
        bool active = false;
        std::string text;
        int language = -1;
        int variant = -1;
        bool operator==(const Key& other) const {
          return active == other.active && text == other.text && language == other.language &&
                 variant == other.variant;
        }
      };

      struct Slot {
        std::string text;
        // What the slot plays or is rendering.
        Key key;
        uint64_t request = 0;
        std::shared_ptr<const Phrase> current;
        std::shared_ptr<const Phrase> pending;
        bool has_pending = false;
        SlotStatus status;
      };

      void timerCallback() override { refresh(); }
      void handleAsyncUpdate() override { processPendingResults(); }
      void render(int slot, uint64_t request, Key key);
      void setStatus(Slot& slot, SlotStatus::State state, const std::string& error = {});

      ApplyCallback apply_;
      VoiceCallback voice_;
      mutable std::mutex mutex_;
      std::array<Slot, kNumSlots> slots_;
      std::atomic<uint64_t> next_request_ { 1 };
      std::atomic<int> rendering_ { 0 };
      std::atomic<bool> shutting_down_ { false };
      std::mutex apply_mutex_;
      juce::ThreadPool pool_ { 1 };

      JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Manager)
  };
} // namespace speech
} // namespace vital
