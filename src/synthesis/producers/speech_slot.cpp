// SPDX-License-Identifier: GPL-3.0-or-later
#include "speech_slot.h"

namespace vital {
  void SpeechSlot::setPhrase(std::shared_ptr<const speech::Phrase> phrase) {
    retired_ = std::move(phrase_);
    phrase_ = std::move(phrase);
    generation_++;
    rewind();
  }

  int SpeechSlot::takeSyllable(double offset) {
    const speech::Phrase* phrase = phrase_.get();
    if (phrase == nullptr || phrase->numSyllables() == 0)
      return -1;
    const int count = phrase->numSyllables();
    const double start = clock_ + offset;
    if (last_syllable_ >= 0 && last_syllable_ < count && start - last_start_ < kChordSeconds)
      return last_syllable_;
    if (next_syllable_ < 0 || next_syllable_ >= count)
      next_syllable_ = 0;
    last_syllable_ = next_syllable_;
    last_start_ = start;
    next_syllable_ = (next_syllable_ + 1) % count;
    return last_syllable_;
  }
} // namespace vital
