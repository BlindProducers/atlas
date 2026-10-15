// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "JuceHeader.h"

#include <string>

namespace vital {
namespace text {
  // Well-formed UTF-8 as RFC 3629 defines it, and as the JSON writer demands:
  // no overlong forms, no surrogates, nothing above U+10FFFF.
  inline bool isValidUtf8(const std::string& bytes) {
    const auto* data = reinterpret_cast<const unsigned char*>(bytes.data());
    const size_t size = bytes.size();
    for (size_t i = 0; i < size;) {
      const unsigned char lead = data[i];
      if (lead < 0x80) {
        ++i;
        continue;
      }
      int length = 0;
      unsigned char low = 0x80, high = 0xbf;
      if (lead >= 0xc2 && lead <= 0xdf) length = 2;
      else if (lead == 0xe0) { length = 3; low = 0xa0; }
      else if (lead >= 0xe1 && lead <= 0xec) length = 3;
      else if (lead == 0xed) { length = 3; high = 0x9f; }
      else if (lead >= 0xee && lead <= 0xef) length = 3;
      else if (lead == 0xf0) { length = 4; low = 0x90; }
      else if (lead >= 0xf1 && lead <= 0xf3) length = 4;
      else if (lead == 0xf4) { length = 4; high = 0x8f; }
      else return false;
      if (i + static_cast<size_t>(length) > size || data[i + 1] < low || data[i + 1] > high)
        return false;
      for (int k = 2; k < length; ++k) {
        if (data[i + k] < 0x80 || data[i + k] > 0xbf)
          return false;
      }
      i += static_cast<size_t>(length);
    }
    return true;
  }

  // Text from a file format that predates UTF-8 (SoundFont names made with
  // Windows tools carry their code page, such as 0xE9 for é): valid UTF-8 is
  // kept, anything else is read as Windows-1252, so the result is always UTF-8.
  inline std::string utf8FromLegacyText(const std::string& bytes) {
    if (isValidUtf8(bytes))
      return bytes;
    static const juce::juce_wchar kWindows1252[32] = {
      0x20ac, 0x0081, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021,
      0x02c6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008d, 0x017d, 0x008f,
      0x0090, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014,
      0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0x009d, 0x017e, 0x0178 };
    juce::String text;
    text.preallocateBytes(bytes.size() * 3);
    for (const char character : bytes) {
      const auto byte = static_cast<unsigned char>(character);
      text += juce::String::charToString(byte >= 0x80 && byte < 0xa0 ? kWindows1252[byte - 0x80]
                                                                      : static_cast<juce::juce_wchar>(byte));
    }
    return text.toStdString();
  }
} // namespace text
} // namespace vital
