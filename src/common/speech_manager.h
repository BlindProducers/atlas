// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "openevv_engine.h"
#include "synth_constants.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace atlas::openevv {
  // The text of each Speech oscillator slot and its renders, for one plugin
  // instance. A thread of its own watches each Speech slot's text and voice
  // controls and renders again when they change, so dragging Head size or
  // automating Breathiness is heard on the next key. A render's frames go to
  // the slot's engine through `deliver`.
  class SpeechManager {
    public:
      enum class State { kEmpty, kRendering, kReady, kFailed, kUnavailable };

      struct Status {
        State state = State::kEmpty;
        std::string error;
        // The render the slot plays, null before the first.
        std::shared_ptr<const Utterance> utterance;
        // Counts finished renders, so the editor can tell a new one.
        uint32_t renders = 0;
        // The newest render this status describes.
        uint64_t ticket = 0;
      };

      // The render a slot asks for now, or nothing when it is not a Speech slot.
      using RequestReader = std::function<std::optional<RenderRequest>(int slot, const std::string& text)>;
      // Called without the manager's locks held, possibly out of order: a
      // render with a lower ticket than one already delivered is stale.
      using Deliver = std::function<void(int slot, std::shared_ptr<const Utterance> utterance, uint64_t ticket)>;

      SpeechManager(RequestReader reader, Deliver deliver);
      ~SpeechManager();

      // Any thread. A change is rendered by the worker within a few milliseconds.
      void setText(int slot, const std::string& text);
      std::string text(int slot) const;
      Status status(int slot) const;
      // Renders, on the calling thread, every slot whose text or controls have
      // changed (a loaded preset, before it plays). Not the audio thread.
      void renderPendingNow();
      // Wakes the worker to look at the controls again.
      void poke();

    private:
      void run();
      // Renders `slot` if what it asks for differs from what it has.
      void update(int slot);

      RequestReader reader_;
      Deliver deliver_;

      mutable std::mutex mutex_;
      std::array<std::string, vital::kNumOscillators> texts_;
      std::array<Status, vital::kNumOscillators> statuses_;
      std::array<std::optional<RenderRequest>, vital::kNumOscillators> rendered_;
      std::array<uint64_t, vital::kNumOscillators> tickets_ {};
      // Held while a render runs, so the worker and renderPendingNow never render the same slot twice.
      std::mutex render_mutex_;

      std::condition_variable wake_;
      bool stop_ = false;
      bool poked_ = false;
      std::thread thread_;
  };
} // namespace atlas::openevv
