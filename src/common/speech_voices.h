// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstring>
#include <string>

namespace vital {
namespace speech {
  // The espeak-ng languages and variants the Speech oscillator offers,
  // generated into speech_voices.inc. The names are in strings::
  // kSpeechLanguageNames and kSpeechVariantNames, in the same order.
  inline constexpr const char* kLanguageIds[] = {
#define SPEECH_LANGUAGE(name, identifier) identifier,
#define SPEECH_VARIANT(name, identifier)
#include "speech_voices.inc"
#undef SPEECH_LANGUAGE
#undef SPEECH_VARIANT
  };

  inline constexpr const char* kVariantIds[] = {
#define SPEECH_LANGUAGE(name, identifier)
#define SPEECH_VARIANT(name, identifier) identifier,
#include "speech_voices.inc"
#undef SPEECH_LANGUAGE
#undef SPEECH_VARIANT
  };

  inline constexpr int kNumLanguages = static_cast<int>(sizeof(kLanguageIds) / sizeof(kLanguageIds[0]));
  // Variant 0 is the language's own voice.
  inline constexpr int kNumVariants = 1 + static_cast<int>(sizeof(kVariantIds) / sizeof(kVariantIds[0]));

  inline constexpr bool sameId(const char* a, const char* b) {
    while (*a != '\0' && *a == *b) {
      ++a;
      ++b;
    }
    return *a == *b;
  }

  inline constexpr int languageIndex(const char* identifier) {
    for (int i = 0; i < kNumLanguages; ++i) {
      if (sameId(kLanguageIds[i], identifier))
        return i;
    }
    return 0;
  }

  inline constexpr int kDefaultLanguage = languageIndex("gmw/en-US");

  // The espeak-ng voice name: "gmw/en-US", or "gmw/en-US+klatt" with a variant.
  inline std::string voiceName(int language, int variant) {
    language = language < 0 || language >= kNumLanguages ? kDefaultLanguage : language;
    std::string name = kLanguageIds[language];
    if (variant > 0 && variant < kNumVariants)
      name += std::string("+") + kVariantIds[variant - 1];
    return name;
  }
} // namespace speech
} // namespace vital
