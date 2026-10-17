// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace atlas::openevv {
  // Words in a Klatt parameter frame (ATLAS_KLATT_FRAME_WORDS): 62 parameters
  // and the duration slot, in src/klatt/klatt_synth.c's order.
  constexpr int kFrameWords = 63;
  using Frame = std::array<int32_t, kFrameWords>;

  // Frame word indices, as KlattSynth reads them.
  enum FrameWord {
    kStep = 0, kF0 = 1, kAv = 2, kTl = 4, kAh = 7, kAf = 8, kF1 = 9, kDf1 = 11, kF2 = 13, kF3 = 15, kF4 = 17, kF5 = 19,
    kF6 = 21, kF7 = 23, kF8 = 25, kFnp = 27, kFnz = 29, kFtp = 31, kFtz = 33, kA1f = 35, kA8f = 42, kAb = 43,
    kAnv = 52, kAtv = 61
  };

  // The rate every render runs at: IBM's, the one Eloquence has always sounded like.
  constexpr int kEngineRate = 11025;
  constexpr int kWideRate = 22050;
  // Text longer than this is cut, so a render stays well under a second.
  constexpr int kMaxTextCharacters = 2000;
  // Frames kept from one render (5 ms each): five minutes of speech.
  constexpr int kMaxFrames = 60000;

  struct Language {
    const char* tag;
    unsigned int dialect;
    const char* name;
  };
  constexpr int kNumLanguages = 10;
  // In the order of the Language parameter (strings::kSpeechLanguageNames).
  extern const Language kLanguages[kNumLanguages];

  // The eight voice parameters, in the engine's own units (speed 0 to 250,
  // gender 0 or 1, the rest 0 to 100).
  struct VoiceSettings {
    int gender = 0;
    int head_size = 50;
    int pitch_baseline = 65;
    int pitch_fluctuation = 30;
    int roughness = 0;
    int breathiness = 0;
    int speed = 50;
    int volume = 92;

    bool operator==(const VoiceSettings& other) const;
    bool operator!=(const VoiceSettings& other) const { return !(*this == other); }
  };

  constexpr int kNumPresetVoices = 8;
  // The language modules' eight preset voices (every module has the same),
  // as `evv -l` lists them.
  extern const VoiceSettings kPresetVoices[kNumPresetVoices];
  extern const char* const kPresetVoiceNames[kNumPresetVoices];

  struct RenderRequest {
    std::string text;
    int language = 0;
    VoiceSettings voice;
    bool annotations = false;

    bool operator==(const RenderRequest& other) const;
    bool operator!=(const RenderRequest& other) const { return !(*this == other); }
  };

  // How the text is cut into the pieces one key plays.
  enum Split { kSyllables, kWords, kWholeText, kNumSplits };

  // One piece: frames [start, end). The nucleus is the frame a held key
  // sustains (the loudest vowel of a syllable, the last syllable's of a word
  // or of the whole text), and f0 its pitch in tenths of a hertz.
  struct Unit {
    int start = 0;
    int nucleus = 0;
    int end = 0;
    int word = 0;
    int part = 0;
    int parts = 1;
    int f0 = 0;
  };

  // The synthesiser's per-render voice: decibel offsets and formant count,
  // which the rules hand KlattSetConstParms.
  struct KlattVoice {
    int32_t add_20 = 0;
    int32_t add_24 = 0;
    int32_t add_28 = 0;
    int32_t add_2c = 0;
    int32_t n_formants = 5;

    bool operator==(const KlattVoice& other) const {
      return add_20 == other.add_20 && add_24 == other.add_24 && add_28 == other.add_28 &&
             add_2c == other.add_2c && n_formants == other.n_formants;
    }
  };

  // What a render made: every frame the engine synthesised, the samples it
  // spoke (for tests), and the text cut three ways.
  struct Utterance {
    RenderRequest request;
    std::vector<Frame> frames;
    std::vector<int32_t> volumes;
    std::vector<uint16_t> voice_of_frame;
    std::vector<KlattVoice> voices;
    std::vector<int16_t> samples;
    // Each word as the text spells it, and the frame it starts at.
    std::vector<std::string> words;
    std::vector<int> word_starts;
    std::array<std::vector<Unit>, kNumSplits> units;

    const std::vector<Unit>& unitsFor(int split) const {
      return units[static_cast<size_t>(split < 0 || split >= kNumSplits ? kSyllables : split)];
    }
  };

  // Whether this build has the engine (ATLAS_HAS_OPENEVV) and the language.
  bool available();
  bool languageAvailable(int language);

  // Speaks the request and records it. Any thread except the audio thread;
  // renders from every plugin instance in the process take turns. Returns
  // null with `error` set when the engine refused or is not built in.
  std::shared_ptr<const Utterance> render(const RenderRequest& request, std::string& error);

  // The text the engine is given: Windows-1252 bytes, control characters and
  // (without annotations) backquotes as spaces.
  std::string engineText(const std::string& utf8, bool annotations);

  // Cuts the frames into syllables, words and the whole text. `word_starts`
  // is the first frame of each word (from the engine's index marks).
  void segment(Utterance& utterance);

  // A frame that makes no sound: every source amplitude at zero.
  bool isSilent(const Frame& frame);
} // namespace atlas::openevv
