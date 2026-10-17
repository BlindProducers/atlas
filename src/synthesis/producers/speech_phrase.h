// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace vital {
namespace speech {
  // One pitch mark of a syllable: the centre of a grain and its half width,
  // both in source samples. Voiced marks sit one pitch period apart at the
  // same point of each period; unvoiced marks are evenly spaced.
  struct Mark {
    int32_t position = 0;
    int32_t half_width = 0;
    bool voiced = false;
  };

  // A syllable espeak-ng spoke in a monotone, cut from its phrase. A note
  // plays it from the start up to its hold point, holds the vowel there while
  // the key is down and plays the rest after the key is released.
  struct Syllable {
    std::vector<float> audio;
    std::vector<Mark> marks;
    // Source samples into the vowel, or -1 when there is no vowel to hold
    // and the syllable plays through.
    int32_t hold_point = -1;
    // The held vowel as one steady grain: two periods, windowed, centred,
    // with the vowel's average spectrum and the phase of the grain at the
    // hold point. Empty when the vowel has no voiced periods.
    std::vector<float> steady_grain;
    // The syllable whispered: noise with the audio's spectral envelope,
    // aligned with it and as loud. Breathiness mixes it in.
    std::vector<float> breath;
    // The vowel's middle, where a held note reads its breath (the whole
    // syllable when it has no vowel).
    int32_t breath_from = 0;
    int32_t breath_to = 0;
    // The syllable's phonemes in IPA, for the editor ("hˈæ").
    std::string phonemes;
    // The word the syllable belongs to and its position in it.
    int word = 0;
    int index_in_word = 0;

    int length() const { return static_cast<int>(audio.size()); }
  };

  // The phrase spoken at one espeak-ng pitch.
  struct Layer {
    // The monotone's pitch period in source samples, and its frequency.
    float period = 0.0f;
    float frequency = 0.0f;
    std::vector<Syllable> syllables;
  };

  // What one note takes from the text: the next syllable, the next word or
  // the whole text (the speech_split parameter).
  enum Split { kSplitSyllable, kSplitWord, kSplitPhrase, kNumSplits };

  // Everything one Speech slot sings: its text as syllables, in order,
  // spoken at several pitches. Every layer has the same syllables; a note
  // sings from the layer nearest its pitch.
  struct Phrase {
    double sample_rate = 22050.0;
    // Lowest pitch first.
    std::vector<Layer> layers;
    // The layer espeak-ng spoke at the voice's own pitch.
    int reference_layer = 0;
    // What was rendered.
    std::string text;
    int language = 0;
    int variant = 0;
    // Rendered with espeak-ng's intonation (Spoken mode), not a monotone.
    bool spoken = false;
    // The first syllable of each word, in order (findWords).
    std::vector<int> word_starts;

    int numSyllables() const { return layers.empty() ? 0 : static_cast<int>(layers.front().syllables.size()); }
    int numWords() const { return static_cast<int>(word_starts.size()); }
    // Fills word_starts from the syllables' words.
    void findWords();
    // How many notes it takes to sing the text once with `split`.
    int numUnits(int split) const;
    // The syllables a note taking `unit` sings, first to last; false when
    // there are none.
    bool unitRange(int split, int unit, int& first, int& last) const;
    // The layer nearest `frequency`, by interval.
    int layerFor(float frequency) const;
    // The layer espeak-ng spoke at the voice's own pitch (labels, tests).
    const Layer* referenceLayer() const;
  };

  // One phoneme espeak-ng reported: its IPA name ("ˈæ", empty for a pause),
  // where it starts in the phrase's audio and the word it belongs to.
  struct PhonemeEvent {
    std::string ipa;
    int64_t sample = 0;
    int word = -1;
  };

  // Places each syllable's pitch marks and hold point. `nucleus_start` and
  // `nucleus_end` (one entry per syllable, source samples into the
  // syllable, negative when it has no vowel) mark the vowel.
  // A `spoken` layer has espeak-ng's intonation: its marks follow a pitch
  // that moves around the layer's period.
  void analyseLayer(Layer& layer, double sample_rate, const std::vector<int32_t>& nucleus_start,
                    const std::vector<int32_t>& nucleus_end, bool spoken = false);

  // The monotone voice's pitch period over voiced audio, in samples, or 0.
  float estimatePeriod(const float* audio, int length, double sample_rate);
} // namespace speech
} // namespace vital
