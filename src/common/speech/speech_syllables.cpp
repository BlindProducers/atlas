// SPDX-License-Identifier: GPL-3.0-or-later
#include "speech_syllables.h"

#include <algorithm>
#include <cmath>

namespace vital {
namespace speech {
  namespace {
    // The code points of a UTF-8 string; malformed bytes read as themselves.
    std::vector<uint32_t> codePoints(const std::string& text) {
      std::vector<uint32_t> points;
      for (size_t i = 0; i < text.size();) {
        const unsigned char lead = static_cast<unsigned char>(text[i]);
        int extra = lead >= 0xf0 ? 3 : lead >= 0xe0 ? 2 : lead >= 0xc0 ? 1 : 0;
        if (extra > 0 && i + extra >= text.size())
          extra = 0;
        uint32_t point = extra == 0 ? lead : lead & (0x3f >> extra);
        for (int k = 1; k <= extra; ++k)
          point = (point << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3f);
        points.push_back(point);
        i += static_cast<size_t>(extra) + 1;
      }
      return points;
    }

    bool isVowelLetter(uint32_t point) {
      switch (point) {
        case 'a': case 'e': case 'i': case 'o': case 'u': case 'y':
        case 0x00e6:  // æ
        case 0x00f8:  // ø
        case 0x0153:  // œ
        case 0x0250: case 0x0251: case 0x0252:  // ɐ ɑ ɒ
        case 0x0254:  // ɔ
        case 0x0258: case 0x0259: case 0x025a: case 0x025b:  // ɘ ə ɚ ɛ
        case 0x025c: case 0x025d: case 0x025e:  // ɜ ɝ ɞ
        case 0x0264:  // ɤ
        case 0x0268: case 0x0269: case 0x026a:  // ɨ ɩ ɪ
        case 0x026f:  // ɯ
        case 0x0275: case 0x0276:  // ɵ ɶ
        case 0x0289: case 0x028a:  // ʉ ʊ
        case 0x028c:  // ʌ
        case 0x028f:  // ʏ
        case 0x1d4a:  // ᵊ, espeak-ng's reduced schwa
        case 0x1d7b: case 0x1d7f:  // ᵻ ᵿ
          return true;
        default:
          return false;
      }
    }

    bool isStressMark(uint32_t point) {
      // ˈ ˌ, and the tie and linking marks.
      return point == 0x02c8 || point == 0x02cc || point == 0x203f || point == 0x0361;
    }

    struct Phone {
      std::string ipa;
      int64_t start = 0;
      int64_t end = 0;
      bool nucleus = false;
    };
  } // namespace

  bool isNucleus(const std::string& ipa) {
    const std::vector<uint32_t> points = codePoints(ipa);
    for (uint32_t point : points) {
      // Combining vertical line above or below: a syllabic consonant.
      if (point == 0x0329 || point == 0x030d)
        return true;
    }
    for (uint32_t point : points) {
      if (isStressMark(point))
        continue;
      return isVowelLetter(point);
    }
    return false;
  }

  std::vector<SyllableSpan> syllabify(const std::vector<PhonemeEvent>& phonemes, int64_t total_samples) {
    // Each phoneme ends where the next event (a phoneme or a pause) starts.
    std::vector<std::vector<Phone>> words;
    std::vector<int> word_ids;
    for (size_t i = 0; i < phonemes.size(); ++i) {
      const PhonemeEvent& event = phonemes[i];
      // A pause, or espeak-ng switching language for a word ("(en)").
      if (event.ipa.empty() || event.ipa.front() == '(')
        continue;
      Phone phone;
      phone.ipa = event.ipa;
      phone.start = event.sample;
      phone.end = i + 1 < phonemes.size() ? phonemes[i + 1].sample : total_samples;
      phone.end = std::min(std::max(phone.end, phone.start), total_samples);
      phone.nucleus = isNucleus(event.ipa);
      if (words.empty() || word_ids.back() != event.word) {
        words.emplace_back();
        word_ids.push_back(event.word);
      }
      words.back().push_back(std::move(phone));
    }

    std::vector<SyllableSpan> spans;
    for (size_t w = 0; w < words.size(); ++w) {
      const std::vector<Phone>& phones = words[w];
      std::vector<int> nuclei;
      for (int i = 0; i < static_cast<int>(phones.size()); ++i) {
        if (phones[static_cast<size_t>(i)].nucleus)
          nuclei.push_back(i);
      }

      // Where each syllable's phonemes begin.
      std::vector<int> starts { 0 };
      for (size_t k = 1; k < nuclei.size(); ++k) {
        const int previous = nuclei[k - 1];
        const int next = nuclei[k];
        const int consonants = next - previous - 1;
        starts.push_back(consonants == 0 ? next : consonants == 1 ? next - 1 : previous + 2);
      }
      starts.push_back(static_cast<int>(phones.size()));

      const size_t count = std::max<size_t>(1, nuclei.size());
      for (size_t k = 0; k < count; ++k) {
        const int first = starts[k];
        const int last = starts[k + 1] - 1;
        if (last < first)
          continue;
        SyllableSpan span;
        span.start = phones[static_cast<size_t>(first)].start;
        span.end = phones[static_cast<size_t>(last)].end;
        span.word = static_cast<int>(w);
        span.index_in_word = static_cast<int>(k);
        for (int i = first; i <= last; ++i)
          span.phonemes += phones[static_cast<size_t>(i)].ipa;
        if (k < nuclei.size()) {
          const Phone& nucleus = phones[static_cast<size_t>(nuclei[k])];
          span.nucleus_start = nucleus.start;
          span.nucleus_end = nucleus.end;
        }
        if (span.end > span.start)
          spans.push_back(std::move(span));
      }
    }
    return spans;
  }

  namespace {
    Layer buildLayer(const Recording& recording, bool spoken) {
      Layer layer;
      const int64_t total = static_cast<int64_t>(recording.audio.size());
      float peak = 0.0f;
      for (float sample : recording.audio)
        peak = std::max(peak, std::abs(sample));
      const float gain = peak > 0.0f ? 0.9f / peak : 1.0f;

      // Short fades where a syllable is cut from the words around it.
      const int fade_in = static_cast<int>(recording.sample_rate * 0.0015);
      const int fade_out = static_cast<int>(recording.sample_rate * 0.003);
      std::vector<int32_t> nucleus_start;
      std::vector<int32_t> nucleus_end;
      for (const SyllableSpan& span : syllabify(recording.phonemes, total)) {
        const int64_t start = std::clamp<int64_t>(span.start, 0, total);
        const int64_t end = std::clamp<int64_t>(span.end, start, total);
        if (end - start < fade_in + fade_out)
          continue;
        Syllable syllable;
        syllable.audio.assign(recording.audio.begin() + start, recording.audio.begin() + end);
        const int length = syllable.length();
        for (int i = 0; i < length; ++i) {
          float value = syllable.audio[static_cast<size_t>(i)] * gain;
          if (i < fade_in)
            value *= static_cast<float>(i) / fade_in;
          if (length - 1 - i < fade_out)
            value *= static_cast<float>(length - 1 - i) / fade_out;
          syllable.audio[static_cast<size_t>(i)] = value;
        }
        syllable.phonemes = span.phonemes;
        syllable.word = span.word;
        syllable.index_in_word = span.index_in_word;
        layer.syllables.push_back(std::move(syllable));
        if (span.nucleus_start >= 0) {
          nucleus_start.push_back(static_cast<int32_t>(std::clamp<int64_t>(span.nucleus_start - start, 0, length)));
          nucleus_end.push_back(static_cast<int32_t>(std::clamp<int64_t>(span.nucleus_end - start, 0, length)));
        }
        else {
          nucleus_start.push_back(-1);
          nucleus_end.push_back(-1);
        }
      }
      analyseLayer(layer, recording.sample_rate, nucleus_start, nucleus_end, spoken);
      return layer;
    }

    bool sameSyllables(const Layer& a, const Layer& b) {
      if (a.syllables.size() != b.syllables.size())
        return false;
      for (size_t i = 0; i < a.syllables.size(); ++i) {
        if (a.syllables[i].phonemes != b.syllables[i].phonemes)
          return false;
      }
      return true;
    }
  } // namespace

  std::shared_ptr<Phrase> buildPhrase(const std::vector<Recording>& recordings, int reference, bool spoken) {
    auto phrase = std::make_shared<Phrase>();
    phrase->spoken = spoken;
    if (recordings.empty())
      return phrase;
    reference = std::clamp(reference, 0, static_cast<int>(recordings.size()) - 1);
    phrase->sample_rate = recordings[static_cast<size_t>(reference)].sample_rate;
    std::vector<Layer> layers(recordings.size());
    std::vector<bool> usable(recordings.size(), false);
    layers[static_cast<size_t>(reference)] = buildLayer(recordings[static_cast<size_t>(reference)], spoken);
    usable[static_cast<size_t>(reference)] = true;
    for (int i = 0; i < static_cast<int>(recordings.size()); ++i) {
      if (i == reference || recordings[static_cast<size_t>(i)].sample_rate != phrase->sample_rate)
        continue;
      layers[static_cast<size_t>(i)] = buildLayer(recordings[static_cast<size_t>(i)], spoken);
      // A layer is only usable when it split into the same syllables.
      usable[static_cast<size_t>(i)] = sameSyllables(layers[static_cast<size_t>(i)],
                                                     layers[static_cast<size_t>(reference)]);
    }
    // The recordings rise in pitch: a layer whose measured pitch does not
    // (a period mistaken for two) is left out.
    float below = layers[static_cast<size_t>(reference)].frequency;
    for (int i = reference - 1; i >= 0; --i) {
      if (usable[static_cast<size_t>(i)] && layers[static_cast<size_t>(i)].frequency < below)
        below = layers[static_cast<size_t>(i)].frequency;
      else
        usable[static_cast<size_t>(i)] = false;
    }
    float above = layers[static_cast<size_t>(reference)].frequency;
    for (int i = reference + 1; i < static_cast<int>(layers.size()); ++i) {
      if (usable[static_cast<size_t>(i)] && layers[static_cast<size_t>(i)].frequency > above)
        above = layers[static_cast<size_t>(i)].frequency;
      else
        usable[static_cast<size_t>(i)] = false;
    }
    for (int i = 0; i < static_cast<int>(layers.size()); ++i) {
      if (!usable[static_cast<size_t>(i)])
        continue;
      if (i == reference)
        phrase->reference_layer = static_cast<int>(phrase->layers.size());
      phrase->layers.push_back(std::move(layers[static_cast<size_t>(i)]));
    }
    phrase->findWords();
    return phrase;
  }

  std::shared_ptr<Phrase> buildPhrase(const Recording& recording, bool spoken) {
    return buildPhrase(std::vector<Recording> { recording }, 0, spoken);
  }
} // namespace speech
} // namespace vital
