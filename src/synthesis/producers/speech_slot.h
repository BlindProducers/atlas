// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "openevv_engine.h"

#include <atomic>
#include <cstdint>
#include <memory>

namespace vital {
  // The speech of one oscillator slot, shared by every voice copy of its
  // module: the rendered text, and which piece of it the next key plays.
  class SpeechSlot {
    public:
      // Notes starting within this long of the last one play the same piece,
      // so a chord sings one syllable.
      static constexpr double kChordSeconds = 0.03;

      // Message thread, under the synth's lock. Notes playing the previous
      // utterance go on with it (the slot keeps it until the next change);
      // the next key starts the new text from its first piece.
      // A `ticket` lower than the last one's is a stale render and is dropped.
      // Returns what the slot let go of, for the caller to free outside the lock.
      std::shared_ptr<const atlas::openevv::Utterance> setUtterance(
          std::shared_ptr<const atlas::openevv::Utterance> utterance, uint64_t ticket = 0);
      std::shared_ptr<const atlas::openevv::Utterance> utterance() const { return utterance_; }
      // Any thread: the next key plays the first piece again.
      void requestRestart() { restart_.store(true); }
      // Any thread: the last piece a key played (-1 before any) and of how many.
      int lastPlayed() const { return last_played_.load(); }
      int lastPlayedOf() const { return last_played_of_.load(); }
      int lastPlayedSplit() const { return last_played_split_.load(); }
      uint32_t lastPlayedGeneration() const { return last_played_generation_.load(); }

      // ---- Audio thread ----
      const atlas::openevv::Utterance* get() const { return utterance_.get(); }
      uint32_t generation() const { return generation_; }
      // The utterance a voice started at `generation`, or null once it has gone.
      const atlas::openevv::Utterance* get(uint32_t generation) const {
        if (generation == generation_)
          return utterance_.get();
        if (generation + 1 == generation_)
          return retired_.get();
        return nullptr;
      }

      // Once per block, before the voices (SpeechClock).
      void advanceClock(double seconds) {
        block_time_ = time_;
        time_ += seconds;
      }
      double blockTime() const { return block_time_; }

      // The piece a key starting at `time` plays, -1 when the text has ended
      // and `loop` is off. Advances to the next piece.
      int takeUnit(int split, bool loop, double time);
      void keyDown() { ++keys_held_; }
      // `restart` starts the text again once no key is held.
      void keyUp(bool restart);

    private:
      std::shared_ptr<const atlas::openevv::Utterance> utterance_;
      std::shared_ptr<const atlas::openevv::Utterance> retired_;
      uint32_t generation_ = 0;
      uint64_t ticket_ = 0;
      std::atomic<bool> restart_ { false };

      int next_ = 0;
      int last_unit_ = -1;
      int last_split_ = -1;
      uint32_t last_generation_ = 0;
      double last_time_ = -1.0;
      int keys_held_ = 0;
      double time_ = 0.0;
      double block_time_ = 0.0;

      std::atomic<int> last_played_ { -1 };
      std::atomic<int> last_played_of_ { 0 };
      std::atomic<int> last_played_split_ { 0 };
      std::atomic<uint32_t> last_played_generation_ { 0 };
  };
} // namespace vital
