// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "JuceHeader.h"
#include "sfz_parser.h"

#include <string>

namespace vital {
namespace multisample {

  // Parses a DecentSampler .dspreset (XML) into the same instrument model the
  // SFZ parser builds: regions that name their sample files but hold no audio
  // yet. Paths resolve as for SFZ, from the preset's folder or the option's
  // search roots and overrides. Supported attributes, the load-time binding
  // values that are applied and what is ignored are listed in README.atlas.md.
  class DecentSamplerParser {
    public:
      static SfzParser::Result parse(const juce::File& file, const SfzParser::Options& options = {});
      // Parses text as if it were the file `main_file` (tests).
      static SfzParser::Result parseText(const std::string& text, const juce::File& main_file,
                                         const SfzParser::Options& options = {});
  };
} // namespace multisample
} // namespace vital
