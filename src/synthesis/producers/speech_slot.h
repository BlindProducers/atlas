// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "speech_phrase.h"

#include <cstdint>
#include <memory>

namespace vital {
  // The Speech state of one oscillator slot, shared by every voice copy of
  // its module: the phrase it sings and which syllable the next note takes.
  class SpeechSlot {
    public:
      // Notes that start within this many seconds of each other (a chord,
      // a strum) sing the same syllable.
      static constexpr double kChordSeconds = 0.04;

      // Message thread, holding the engine's lock. Notes playing the phrase
      // before this one finish with it; older phrases stop.
      void setPhrase(std::shared_ptr<const speech::Phrase> phrase);
      std::shared_ptr<const speech::Phrase> phrase() const { return phrase_; }
      std::shared_ptr<const speech::Phrase> retired() const { return retired_; }

      // ---- Audio thread ----
      uint32_t generation() const { return generation_; }
      // The phrase a note started at `generation` plays, or null once freed.
      const speech::Phrase* phraseFor(uint32_t generation) const {
        if (generation == generation_)
          return phrase_.get();
        if (generation + 1 == generation_)
          return retired_.get();
        return nullptr;
      }
      // Once per block, from the voice handler.
      void advanceClock(int num_samples, float sample_rate) {
        if (sample_rate > 0.0f)
          clock_ += num_samples / static_cast<double>(sample_rate);
      }
      // The syllable a note starting `offset` seconds into this block sings,
      // or -1 when there is none.
      int takeSyllable(double offset);
      // The next note sings the first syllable (transport start, all sounds off).
      void rewind() {
        next_syllable_ = 0;
        last_syllable_ = -1;
      }

    private:
      std::shared_ptr<const speech::Phrase> phrase_;
      std::shared_ptr<const speech::Phrase> retired_;
      uint32_t generation_ = 0;
      double clock_ = 0.0;
      double last_start_ = -1.0e9;
      int next_syllable_ = 0;
      int last_syllable_ = -1;
  };
} // namespace vital
