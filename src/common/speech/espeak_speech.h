// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "JuceHeader.h"
#include "speech_phrase.h"

#include <string>
#include <vector>

namespace vital {
namespace speech {
  // What espeak-ng said: its audio and the phonemes it reported on the way.
  struct Recording {
    std::vector<float> audio;
    double sample_rate = 22050.0;
    std::vector<PhonemeEvent> phonemes;
  };

  // espeak-ng keeps one global state per process, which every plugin
  // instance shares: these calls serialise on a process-wide lock. Never call
  // them from the audio thread.
  namespace espeak {
    // The folder holding espeak-ng-data: the plugin bundle's Resources
    // folder, or the one setDataDirectory chose (tests) before first use.
    juce::File dataDirectory();
    void setDataDirectory(const juce::File& directory);

    // espeak-ng's pitch setting runs from 0 to 99; 50 is the voice's own.
    constexpr int kMaxPitch = 99;
    constexpr int kVoicePitch = 50;

    // Speaks `text` with `voice` ("gmw/en-US+klatt") at `pitch` (0 to
    // kMaxPitch): in a monotone, or with espeak-ng's own intonation when
    // `intonation` is set. Returns false with `error` set when espeak-ng
    // cannot run or the voice is missing.
    bool speak(const std::string& text, const std::string& voice, int pitch, Recording& recording,
               std::string& error, bool intonation = false);
  } // namespace espeak
} // namespace speech
} // namespace vital
