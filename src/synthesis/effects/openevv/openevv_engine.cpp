// SPDX-License-Identifier: GPL-3.0-or-later
#include "openevv_engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <thread>

#if ATLAS_HAS_OPENEVV
#include "atlas_openevv.h"
#ifndef ECI_STATIC
#define ECI_STATIC 1
#endif
#include "eci.h"
#endif

namespace atlas::openevv {
  const Language kLanguages[kNumLanguages] = {
    { "enus", 0x00010000, "English (US)" },
    { "engb", 0x00010001, "English (UK)" },
    { "dede", 0x00040000, "German" },
    { "eses", 0x00020000, "Spanish (Spain)" },
    { "esus", 0x00020001, "Spanish (Mexico)" },
    { "frfr", 0x00030000, "French (France)" },
    { "frca", 0x00030001, "French (Canada)" },
    { "itit", 0x00050000, "Italian" },
    { "fifi", 0x00090000, "Finnish" },
    { "ptbr", 0x00070000, "Portuguese (Brazil)" },
  };

  const VoiceSettings kPresetVoices[kNumPresetVoices] = {
    { 0, 50, 65, 30, 0, 0, 50, 92 },
    { 1, 50, 81, 30, 0, 50, 50, 100 },
    { 1, 22, 93, 35, 0, 0, 50, 90 },
    { 0, 89, 52, 43, 0, 0, 50, 93 },
    { 0, 50, 69, 34, 0, 0, 70, 92 },
    { 1, 56, 89, 35, 0, 40, 70, 95 },
    { 1, 45, 68, 30, 3, 40, 50, 90 },
    { 0, 30, 61, 44, 18, 20, 50, 90 },
  };

  const char* const kPresetVoiceNames[kNumPresetVoices] = {
    "Adult Male 1", "Adult Female 1", "Child", "Adult Male 2", "Adult Male 3", "Adult Female 2",
    "Elderly Female", "Elderly Male"
  };

  bool VoiceSettings::operator==(const VoiceSettings& other) const {
    return gender == other.gender && head_size == other.head_size && pitch_baseline == other.pitch_baseline &&
           pitch_fluctuation == other.pitch_fluctuation && roughness == other.roughness &&
           breathiness == other.breathiness && speed == other.speed && volume == other.volume;
  }

  bool RenderRequest::operator==(const RenderRequest& other) const {
    return text == other.text && language == other.language && voice == other.voice &&
           annotations == other.annotations;
  }

  namespace {
    // Windows-1252's 0x80 to 0x9f, by code point.
    int windows1252(uint32_t code) {
      static const uint16_t kHigh[32] = {
        0x20ac, 0, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021, 0x02c6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017d, 0,
        0, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014, 0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0, 0x017e,
        0x0178
      };
      if (code < 0x80)
        return static_cast<int>(code);
      if (code >= 0xa0 && code <= 0xff)
        return static_cast<int>(code);
      for (int i = 0; i < 32; ++i) {
        if (kHigh[i] != 0 && kHigh[i] == code)
          return 0x80 + i;
      }
      return -1;
    }

    // The next code point of UTF-8 text, or U+FFFD for a malformed byte.
    uint32_t nextCodePoint(const std::string& text, size_t& at) {
      const auto byte = [&](size_t i) { return static_cast<unsigned char>(text[i]); };
      const unsigned char lead = byte(at++);
      if (lead < 0x80)
        return lead;
      int extra = lead >= 0xf0 ? 3 : lead >= 0xe0 ? 2 : lead >= 0xc0 ? 1 : -1;
      if (extra < 0)
        return 0xfffd;
      uint32_t code = lead & (0x3f >> extra);
      for (int i = 0; i < extra; ++i) {
        if (at >= text.size() || (byte(at) & 0xc0) != 0x80)
          return 0xfffd;
        code = (code << 6) | (byte(at++) & 0x3f);
      }
      return code;
    }

    bool isSpace(uint32_t code) {
      return code == ' ' || code == '\t' || code == '\n' || code == '\r' || code == 0xa0 || code == 0x2028 ||
             code == 0x2029 || code == 0x3000;
    }

    // The text's words, each as written (UTF-8), at most kMaxTextCharacters in all.
    std::vector<std::string> splitWords(const std::string& text) {
      std::vector<std::string> words;
      std::string word;
      size_t at = 0;
      int characters = 0;
      while (at < text.size() && characters < kMaxTextCharacters) {
        const size_t start = at;
        const uint32_t code = nextCodePoint(text, at);
        ++characters;
        if (isSpace(code) || code < 0x20) {
          if (!word.empty())
            words.push_back(std::move(word));
          word.clear();
        }
        else
          word.append(text, start, at - start);
      }
      if (!word.empty())
        words.push_back(std::move(word));
      return words;
    }

    // `vowel_ah` is the aspiration the rules mix into this voice's vowels.
    float sonority(const Frame& frame, int vowel_ah) {
      if (frame[kAv] <= 0)
        return 0.0f;
      // Open vowels are louder than the voiced consonants between them, and
      // a voiced frame with frication (a voiced fricative, or a vowel running
      // into a fricative) is no vowel.
      const float f1 = static_cast<float>(std::clamp(frame[kF1], 250, 950));
      float result = static_cast<float>(frame[kAv]) + 0.03f * (f1 - 250.0f) -
                     0.5f * static_cast<float>(std::max(0, frame[kAf]));
      // Nasals and voiced fricatives (voicing with none of the vowels'
      // aspiration), voiced closures (tilted voicing) and the breathy release
      // that ends a phrase (more aspiration than the vowels) are no vowels
      // however loud: a nasal held at the end of a phrase is no second syllable.
      constexpr float kConsonant = 15.0f;
      constexpr int kBreathy = 3;
      if (frame[kTl] > 0 || (vowel_ah > 0 && (2 * frame[kAh] < vowel_ah || frame[kAh] > vowel_ah + kBreathy)))
        result -= kConsonant;
      return result;
    }

    // The aspiration most of the utterance's open vowels carry, 0 when they
    // have none.
    int vowelAspiration(const Utterance& utterance) {
      std::array<int, 128> counts {};
      for (const Frame& frame : utterance.frames) {
        if (frame[kAv] > 0 && frame[kAf] <= 0 && frame[kTl] <= 0 && frame[kF1] >= 450 && frame[kAh] > 0 &&
            frame[kAh] < static_cast<int>(counts.size()))
          ++counts[static_cast<size_t>(frame[kAh])];
      }
      const auto most = std::max_element(counts.begin(), counts.end());
      return *most > 0 ? static_cast<int>(std::distance(counts.begin(), most)) : 0;
    }

    bool isObstruentOnset(const Frame& frame) {
      return frame[kAf] > 0 || (frame[kAh] > 0 && frame[kAv] <= 0);
    }

    // The frame whose pitch a unit is played against: its nucleus, or the
    // nearest voiced frame.
    int unitF0(const Utterance& utterance, const Unit& unit) {
      const auto voiced = [&](int i) {
        return i >= unit.start && i < unit.end && utterance.frames[static_cast<size_t>(i)][kAv] > 0 &&
               utterance.frames[static_cast<size_t>(i)][kF0] > 0;
      };
      for (int distance = 0; distance < unit.end - unit.start; ++distance) {
        if (voiced(unit.nucleus - distance))
          return utterance.frames[static_cast<size_t>(unit.nucleus - distance)][kF0];
        if (voiced(unit.nucleus + distance))
          return utterance.frames[static_cast<size_t>(unit.nucleus + distance)][kF0];
      }
      return 0;
    }

    // Drops a unit's leading silence and all but one frame of its trailing
    // silence. Answers false when nothing in it sounds.
    bool trim(const Utterance& utterance, Unit& unit) {
      int first = unit.start;
      while (first < unit.end && isSilent(utterance.frames[static_cast<size_t>(first)]))
        ++first;
      if (first >= unit.end)
        return false;
      int last = unit.end - 1;
      while (last > first && isSilent(utterance.frames[static_cast<size_t>(last)]))
        --last;
      unit.start = first;
      unit.end = std::min(unit.end, last + 2);
      unit.nucleus = std::clamp(unit.nucleus, unit.start, unit.end - 1);
      return true;
    }

    // The loudest frame of [start, end), for a unit with no vowel.
    int loudest(const Utterance& utterance, int start, int end) {
      int best = start;
      int best_level = -1;
      for (int i = start; i < end; ++i) {
        const Frame& frame = utterance.frames[static_cast<size_t>(i)];
        const int level = std::max({ frame[kAv], frame[kAh], frame[kAf] });
        if (level > best_level) {
          best_level = level;
          best = i;
        }
      }
      return best;
    }

    // The vowel nuclei of frames [start, end): sonority peaks at least 60 ms
    // apart at the usual speed (less when the voice speaks faster, `pace`
    // below 1) with a dip of 3 dB or more between them.
    std::vector<int> nuclei(const std::vector<float>& level, int start, int end, float pace) {
      const int window = std::max(2, static_cast<int>(std::lround(6.0f * pace)));
      constexpr float kVoiced = 35.0f;
      const int min_frames = std::max(2, static_cast<int>(std::lround(12.0f * pace)));
      constexpr float kMinDip = 4.0f;

      std::vector<int> peaks;
      for (int i = start; i < end; ++i) {
        if (level[static_cast<size_t>(i)] < kVoiced)
          continue;
        bool peak = true;
        // Neighbours beyond the range count too: a word's last frames rising
        // into the next word's vowel are no peak of their own.
        const int size = static_cast<int>(level.size());
        for (int j = std::max(0, i - window); j < std::min(size, i + window + 1) && peak; ++j) {
          if (j < i)
            peak = level[static_cast<size_t>(i)] > level[static_cast<size_t>(j)];
          else if (j > i)
            peak = level[static_cast<size_t>(i)] >= level[static_cast<size_t>(j)];
        }
        if (peak)
          peaks.push_back(i);
      }

      std::vector<int> kept;
      for (int peak : peaks) {
        if (kept.empty()) {
          kept.push_back(peak);
          continue;
        }
        const int previous = kept.back();
        float valley = level[static_cast<size_t>(previous)];
        for (int i = previous; i <= peak; ++i)
          valley = std::min(valley, level[static_cast<size_t>(i)]);
        const float lower = std::min(level[static_cast<size_t>(previous)], level[static_cast<size_t>(peak)]);
        if (peak - previous < min_frames || lower - valley < kMinDip) {
          if (level[static_cast<size_t>(peak)] > level[static_cast<size_t>(previous)])
            kept.back() = peak;
        }
        else
          kept.push_back(peak);
      }
      return kept;
    }

    // The frame a held key sustains: the middle of the vowel's steady part
    // around `peak` (within 1.5 dB of it), clear of the creaky onset the
    // engine gives a vowel (diplophonia, which would halve the held pitch).
    int steadyNucleus(const Utterance& utterance, const std::vector<float>& level, int peak, int start, int end) {
      constexpr int kDiplophonia = 6;
      const float floor = level[static_cast<size_t>(peak)] - 1.5f;
      int low = peak;
      while (low > start && level[static_cast<size_t>(low - 1)] >= floor)
        --low;
      int high = peak;
      while (high + 1 < end && level[static_cast<size_t>(high + 1)] >= floor)
        ++high;
      const auto steady = [&](int i) { return utterance.frames[static_cast<size_t>(i)][kDiplophonia] <= 0; };
      while (low < high && !steady(low))
        ++low;
      int nucleus = (low + high) / 2;
      while (nucleus < high && !steady(nucleus))
        ++nucleus;
      return nucleus;
    }

    // Where the syllable after nucleus `first` begins: in the dip before
    // nucleus `second`, at the first fricative or burst there (it starts the
    // next syllable), or else halfway through the dip.
    int syllableCut(const Utterance& utterance, const std::vector<float>& level, int first, int second) {
      float valley = level[static_cast<size_t>(first)];
      for (int i = first + 1; i < second; ++i)
        valley = std::min(valley, level[static_cast<size_t>(i)]);
      int low = -1;
      int high = -1;
      for (int i = first + 1; i < second; ++i) {
        if (level[static_cast<size_t>(i)] <= valley + 1.0f) {
          if (low < 0)
            low = i;
          high = i;
        }
      }
      if (low < 0)
        return (first + second + 1) / 2;
      for (int i = low; i <= high; ++i) {
        if (isObstruentOnset(utterance.frames[static_cast<size_t>(i)]))
          return i;
      }
      return (low + high + 1) / 2;
    }

    // Where the word whose index mark is `mark` begins, within frames
    // [low, high]: in the dip there that is deepest once each frame away from
    // the mark counts `distance` dB against it (every consonant and silence
    // counting as equally deep, so a pause after the word's own consonant
    // cannot pull the cut past the word); within that dip, where sound
    // resumes after a closure or a pause, else where a fricative or burst
    // starts, else at the frame nearest the mark.
    int wordCut(const Utterance& utterance, const std::vector<float>& level, int low, int high, int mark,
                float distance) {
      const int num_frames = static_cast<int>(level.size());
      const auto at = [&](int i) { return level[static_cast<size_t>(std::clamp(i, 0, num_frames - 1))]; };
      int nearest = -1;
      float best_score = 0.0f;
      for (int i = low; i <= high; ++i) {
        if (at(i) > at(i - 1) || at(i) > at(i + 1))
          continue;
        constexpr float kFloor = 25.0f;
        const float score = std::max(at(i), kFloor) + distance * static_cast<float>(std::abs(i - mark));
        if (nearest < 0 || score < best_score) {
          nearest = i;
          best_score = score;
        }
      }
      if (nearest < 0)
        return mark;
      const float valley = at(nearest);
      const auto quiet = [&](int i) { return i >= low && i <= high && at(i) <= valley + 1.0f; };
      int first = nearest;
      while (quiet(first - 1))
        --first;
      int last = nearest;
      while (quiet(last + 1))
        ++last;

      const auto silent = [&](int i) { return isSilent(utterance.frames[static_cast<size_t>(i)]); };
      const auto onset = [&](int i) { return isObstruentOnset(utterance.frames[static_cast<size_t>(i)]); };
      int best = -1;
      const auto consider = [&](int i) {
        if (best < 0 || std::abs(i - mark) < std::abs(best - mark))
          best = i;
      };
      for (int i = std::max(1, first); i <= std::min(last + 1, high); ++i) {
        if (silent(i - 1) && !silent(i))
          consider(i);
      }
      if (best >= 0)
        return best;
      for (int i = std::max(1, first); i <= last; ++i) {
        if (onset(i) && !onset(i - 1))
          consider(i);
      }
      if (best >= 0)
        return best;
      return std::clamp(mark, first, last);
    }
  } // namespace

  bool isSilent(const Frame& frame) {
    return frame[kAv] <= 0 && frame[kAh] <= 0 && frame[kAf] <= 0;
  }

  std::string engineText(const std::string& utf8, bool annotations) {
    std::string out;
    size_t at = 0;
    while (at < utf8.size()) {
      const uint32_t code = nextCodePoint(utf8, at);
      int byte = windows1252(code);
      // Curly quotes the engine reads as their plain forms.
      if (code == 0x2018 || code == 0x2019)
        byte = '\'';
      else if (code == 0x201c || code == 0x201d)
        byte = '"';
      if (byte < 0x20 || byte == 0x7f || (byte == '`' && !annotations))
        byte = ' ';
      out.push_back(static_cast<char>(byte));
    }
    return out;
  }

  void segment(Utterance& utterance) {
    for (auto& units : utterance.units)
      units.clear();
    const int num_frames = static_cast<int>(utterance.frames.size());
    if (num_frames == 0)
      return;

    const int vowel_ah = vowelAspiration(utterance);
    std::vector<float> raw(static_cast<size_t>(num_frames));
    for (int i = 0; i < num_frames; ++i)
      raw[static_cast<size_t>(i)] = sonority(utterance.frames[static_cast<size_t>(i)], vowel_ah);
    // How long a syllable lasts against the usual speed of 50: measured, the
    // engine's frames shrink by about 18% for every 10 steps faster (a
    // quarter as many at 120) and grow as much for every 10 slower.
    const float pace = std::exp(-0.02f * static_cast<float>(utterance.request.voice.speed - 50));
    // Smoothed over three frames, unless the voice is so fast that a
    // syllable is hardly longer than that.
    constexpr float kSmoothPace = 0.3f;
    std::vector<float> level(raw);
    for (int i = 0; i < num_frames && pace >= kSmoothPace; ++i) {
      const float before = raw[static_cast<size_t>(std::max(0, i - 1))];
      const float after = raw[static_cast<size_t>(std::min(num_frames - 1, i + 1))];
      level[static_cast<size_t>(i)] = (before + raw[static_cast<size_t>(i)] + after) / 3.0f;
    }

    std::vector<int> starts = utterance.word_starts;
    if (starts.empty() || starts.front() != 0)
      starts.insert(starts.begin(), 0);
    const int num_words = static_cast<int>(starts.size());
    // The engine reports each word's index mark where the sound starts to
    // move into the word: on its first frame, or as the word before is still
    // running into its consonant, or (after a short word like "a") up to a
    // short vowel early. Cut each word in the deepest dip just after its mark,
    // never at or past the marks either side, so no word is lost.
    const int before = std::max(1, static_cast<int>(std::lround(3.0f * pace)));
    const int after = std::max(4, static_cast<int>(std::lround(16.0f * pace)));
    const float distance = 1.5f / pace;
    for (int word = 1; word < num_words; ++word) {
      const int mark = std::clamp(starts[static_cast<size_t>(word)], 0, num_frames - 1);
      const int next = word + 1 < num_words ? starts[static_cast<size_t>(word + 1)] : num_frames;
      const int low = std::max(starts[static_cast<size_t>(word - 1)] + 1, mark - before);
      const int high = std::min({ next - 1, mark + after, num_frames - 1 });
      if (low <= high)
        starts[static_cast<size_t>(word)] = wordCut(utterance, level, low, high, mark, distance);
      starts[static_cast<size_t>(word)] =
          std::max(starts[static_cast<size_t>(word)], starts[static_cast<size_t>(word - 1)]);
    }

    auto& syllables = utterance.units[kSyllables];
    auto& words = utterance.units[kWords];
    for (int word = 0; word < num_words; ++word) {
      const int start = std::clamp(starts[static_cast<size_t>(word)], 0, num_frames);
      const int end = word + 1 < num_words ? std::clamp(starts[static_cast<size_t>(word + 1)], start, num_frames)
                                           : num_frames;
      if (end <= start)
        continue;
      const int display_word = std::min(word, std::max(0, static_cast<int>(utterance.words.size()) - 1));

      std::vector<int> peaks = nuclei(level, start, end, pace);
      if (peaks.empty())
        peaks.push_back(loudest(utterance, start, end));

      std::vector<Unit> pieces;
      int piece_start = start;
      for (size_t p = 0; p < peaks.size(); ++p) {
        Unit unit;
        unit.start = piece_start;
        unit.end = p + 1 < peaks.size() ? syllableCut(utterance, level, peaks[p], peaks[p + 1]) : end;
        unit.nucleus = steadyNucleus(utterance, level, peaks[p], unit.start, unit.end);
        unit.word = display_word;
        piece_start = unit.end;
        if (trim(utterance, unit))
          pieces.push_back(unit);
      }
      for (size_t p = 0; p < pieces.size(); ++p) {
        pieces[p].part = static_cast<int>(p);
        pieces[p].parts = static_cast<int>(pieces.size());
        pieces[p].f0 = unitF0(utterance, pieces[p]);
        syllables.push_back(pieces[p]);
      }
      if (pieces.empty())
        continue;

      Unit whole_word;
      whole_word.start = start;
      whole_word.end = end;
      whole_word.nucleus = pieces.back().nucleus;
      whole_word.word = display_word;
      if (trim(utterance, whole_word)) {
        whole_word.f0 = unitF0(utterance, whole_word);
        words.push_back(whole_word);
      }
    }

    if (!syllables.empty()) {
      Unit whole;
      whole.start = syllables.front().start;
      whole.end = syllables.back().end;
      whole.nucleus = syllables.back().nucleus;
      whole.word = 0;
      whole.f0 = unitF0(utterance, whole);
      utterance.units[kWholeText].push_back(whole);
    }
  }

#if ATLAS_HAS_OPENEVV
  namespace {
    std::mutex& renderMutex() {
      static std::mutex mutex;
      return mutex;
    }

    struct Capture {
      Utterance* utterance = nullptr;
      std::vector<long> frame_sample;
      long frame_samples = 0;
      long wave_samples = 0;
      std::vector<std::pair<int, long>> marks;
      short buffer[4096] {};
      bool truncated = false;
    };

    void tapFrame(void* user, const int32_t* parms, const AtlasKlattVoice* voice, int32_t volume) {
      auto* capture = static_cast<Capture*>(user);
      Utterance& utterance = *capture->utterance;
      if (static_cast<int>(utterance.frames.size()) >= kMaxFrames) {
        capture->truncated = true;
        return;
      }
      Frame frame {};
      std::copy(parms, parms + kFrameWords - 1, frame.begin());
      frame[kFrameWords - 1] = 0;
      KlattVoice klatt_voice;
      klatt_voice.add_20 = voice->add_20;
      klatt_voice.add_24 = voice->add_24;
      klatt_voice.add_28 = voice->add_28;
      klatt_voice.add_2c = voice->add_2c;
      klatt_voice.n_formants = voice->n_formants;
      if (utterance.voices.empty() || !(utterance.voices.back() == klatt_voice))
        utterance.voices.push_back(klatt_voice);
      utterance.frames.push_back(frame);
      utterance.volumes.push_back(volume);
      utterance.voice_of_frame.push_back(static_cast<uint16_t>(utterance.voices.size() - 1));
      capture->frame_sample.push_back(capture->frame_samples);
      // KlattSynth's own count at 11,025 hertz.
      capture->frame_samples += static_cast<long>(parms[kStep]) * kEngineRate / 1000;
    }

    int ECICALL onMessage(ECIHand, ECIMessage message, int param, void* data) {
      auto* capture = static_cast<Capture*>(data);
      if (message == eciWaveformBuffer && param > 0) {
        std::vector<int16_t>& samples = capture->utterance->samples;
        if (static_cast<long>(samples.size()) < static_cast<long>(kMaxFrames) * 64)
          samples.insert(samples.end(), capture->buffer, capture->buffer + param);
        capture->wave_samples += param;
      }
      else if (message == eciIndexReply)
        capture->marks.emplace_back(param, capture->wave_samples);
      return eciDataProcessed;
    }

    std::vector<unsigned int> availableDialects() {
      atlas_openevv_start();
      unsigned int dialects[32] {};
      int count = 32;
      // Nought when it filled in `count` of them.
      if (eciGetAvailableLanguages(dialects, &count) != 0)
        count = 0;
      return std::vector<unsigned int>(dialects, dialects + std::clamp(count, 0, 32));
    }
  } // namespace

  bool available() { return true; }

  bool languageAvailable(int language) {
    if (language < 0 || language >= kNumLanguages)
      return false;
    static const std::vector<unsigned int> dialects = availableDialects();
    return std::find(dialects.begin(), dialects.end(), kLanguages[language].dialect) != dialects.end();
  }

  std::shared_ptr<const Utterance> render(const RenderRequest& request, std::string& error) {
    auto utterance = std::make_shared<Utterance>();
    utterance->request = request;
    const std::vector<std::string> words = splitWords(request.text);
    if (words.empty())
      return utterance;

    atlas_openevv_start();
    int language = request.language;
    if (!languageAvailable(language)) {
      language = 0;
      while (language < kNumLanguages && !languageAvailable(language))
        ++language;
      if (language >= kNumLanguages) {
        error = "This build has no speech language";
        return nullptr;
      }
    }

    std::lock_guard<std::mutex> lock(renderMutex());
    ECIHand handle = eciNewEx(static_cast<int>(kLanguages[language].dialect));
    if (handle == NULL_ECI_HAND) {
      error = "The speech engine would not start";
      return nullptr;
    }

    // As OpenEVV's evv command does it: the voice, then the callback, then
    // the buffer, then the text.
    const VoiceSettings& voice = request.voice;
    const int values[eciNumVoiceParams] = {
      std::clamp(voice.gender, 0, 1), std::clamp(voice.head_size, 0, 100), std::clamp(voice.pitch_baseline, 0, 100),
      std::clamp(voice.pitch_fluctuation, 0, 100), std::clamp(voice.roughness, 0, 100),
      std::clamp(voice.breathiness, 0, 100), std::clamp(voice.speed, 0, 250), std::clamp(voice.volume, 0, 100)
    };
    for (int parameter = 0; parameter < eciNumVoiceParams; ++parameter)
      eciSetVoiceParam(handle, 0, parameter, values[parameter]);
    eciSetParam(handle, eciInputType, request.annotations ? 1 : 0);

    Capture capture;
    capture.utterance = utterance.get();
    eciRegisterCallback(handle, onMessage, &capture);
    if (!eciSetOutputBuffer(handle, 4096, capture.buffer)) {
      eciDelete(handle);
      error = "The speech engine refused its output buffer";
      return nullptr;
    }

    // Each word is followed by an index mark, which the engine reports when
    // the audio reaches it: that is where the word ends.
    bool added = true;
    for (size_t i = 0; i < words.size() && added; ++i) {
      const std::string text = engineText(words[i], request.annotations) + " ";
      added = eciAddText(handle, text.c_str()) != 0 && eciInsertIndex(handle, static_cast<int>(i) + 1) != 0;
    }
    atlas_openevv_set_tap(tapFrame, &capture);
    const bool started = added && eciSynthesize(handle) != 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    bool timed_out = false;
    // Asking whether the engine is speaking is what delivers its messages.
    while (started && eciSpeaking(handle)) {
      if (std::chrono::steady_clock::now() > deadline) {
        eciStop(handle);
        timed_out = true;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    eciSynchronize(handle);
    atlas_openevv_set_tap(nullptr, nullptr);
    eciDelete(handle);

    if (!started) {
      error = "The speech engine refused the text";
      return nullptr;
    }
    if (timed_out) {
      error = "The speech engine took too long";
      return nullptr;
    }

    // Each word starts where the previous one's mark fell.
    utterance->words = words;
    utterance->word_starts.assign(words.size(), static_cast<int>(utterance->frames.size()));
    if (!utterance->word_starts.empty())
      utterance->word_starts[0] = 0;
    for (const auto& mark : capture.marks) {
      const int word = mark.first;  // mark k ends word k - 1, so word k starts there
      if (word < 1 || word >= static_cast<int>(words.size()))
        continue;
      const auto found = std::lower_bound(capture.frame_sample.begin(), capture.frame_sample.end(), mark.second);
      utterance->word_starts[static_cast<size_t>(word)] =
          static_cast<int>(std::distance(capture.frame_sample.begin(), found));
    }
    for (size_t i = 1; i < utterance->word_starts.size(); ++i)
      utterance->word_starts[i] = std::max(utterance->word_starts[i], utterance->word_starts[i - 1]);

    segment(*utterance);
    return utterance;
  }
#else
  bool available() { return false; }
  bool languageAvailable(int) { return false; }

  std::shared_ptr<const Utterance> render(const RenderRequest&, std::string& error) {
    error = "This build of Atlas has no speech engine";
    return nullptr;
  }
#endif
} // namespace atlas::openevv
