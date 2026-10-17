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

    int numSyllables() const { return layers.empty() ? 0 : static_cast<int>(layers.front().syllables.size()); }
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
  void analyseLayer(Layer& layer, double sample_rate, const std::vector<int32_t>& nucleus_start,
                    const std::vector<int32_t>& nucleus_end);

  // The monotone voice's pitch period over voiced audio, in samples, or 0.
  float estimatePeriod(const float* audio, int length, double sample_rate);
} // namespace speech
} // namespace vital
