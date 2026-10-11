// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>

namespace vital {
  // MIDI controller state the multi-sample oscillators read: the latest value
  // of every CC (omni) and the CC events of the current block, which trigger
  // CC regions such as piano pedal noises. Owned by the SoundEngine and
  // touched only on the audio thread.
  struct MultiSampleMidiState {
    static constexpr int kNumCcs = 128;
    static constexpr int kMaxEvents = 256;

    struct Event {
      int cc = 0;
      float previous = 0.0f;
      float value = 0.0f;
      // Host-rate sample offset in the block.
      int offset = 0;
      // Whether `previous` is a value received from MIDI.
      bool had_previous = false;
    };

    std::array<float, kNumCcs> values {};
    std::array<bool, kNumCcs> received {};
    std::array<Event, kMaxEvents> events {};
    int num_events = 0;

    void controlChange(int cc, float value, int offset) {
      if (cc < 0 || cc >= kNumCcs)
        return;
      if (num_events < kMaxEvents) {
        Event& event = events[num_events++];
        event.cc = cc;
        event.previous = values[cc];
        event.had_previous = received[cc];
        event.value = value;
        event.offset = offset;
      }
      values[cc] = value;
      received[cc] = true;
    }

    // All Controllers Off: every CC returns to the instrument's default.
    void resetControllers() {
      received.fill(false);
    }

    void endBlock() { num_events = 0; }
  };
} // namespace vital
