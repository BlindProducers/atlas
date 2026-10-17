// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "espeak_speech.h"
#include "speech_phrase.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace vital {
namespace speech {
  // A syllable as a span of the phrase's audio, from espeak-ng's phonemes.
  struct SyllableSpan {
    int64_t start = 0;
    int64_t end = 0;
    // The vowel (or syllabic consonant), or -1 when the syllable has none.
    int64_t nucleus_start = -1;
    int64_t nucleus_end = -1;
    std::string phonemes;
    int word = 0;
    int index_in_word = 0;
  };

  // Whether an IPA phoneme espeak-ng reported is a syllable's nucleus: a
  // vowel, a diphthong, or a consonant marked syllabic ("n̩").
  bool isNucleus(const std::string& ipa);

  // Splits each word's phonemes into syllables. Every vowel is a nucleus;
  // consonants before a word's first vowel and after its last stay with it,
  // a single consonant between two vowels starts the next syllable, and of
  // several the first closes the syllable before and the rest start the
  // next ("mon-ster"). A word without a vowel is one syllable.
  std::vector<SyllableSpan> syllabify(const std::vector<PhonemeEvent>& phonemes, int64_t total_samples);

  // Cuts each recording (the same text at rising pitches) into the
  // syllables, normalised, with pitch marks: one layer per recording that
  // splits into the same syllables as `reference`, the voice's own pitch.
  std::shared_ptr<Phrase> buildPhrase(const std::vector<Recording>& recordings, int reference);
  // One layer.
  std::shared_ptr<Phrase> buildPhrase(const Recording& recording);
} // namespace speech
} // namespace vital
