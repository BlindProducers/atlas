// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "JuceHeader.h"
#include "multisample_instrument.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace vital {
namespace multisample {

  // Parses SFZ text into an Instrument whose regions name their sample files
  // but hold no audio yet (the loader decodes them). Supported opcodes and the
  // deliberate differences from other players are listed in README.atlas.md.
  class SfzParser {
    public:
      struct Options {
        // Folders that stand in for the main file's folder when resolving
        // #include and sample paths, tried in order (an edited working copy
        // first, then the extracted preset). Empty: the main file's folder.
        std::vector<juce::File> search_roots;
        // Paths as written in the file (after default_path, with forward
        // slashes) mapped to the file that replaces them.
        std::map<std::string, juce::File> path_overrides;
      };

      struct SampleReference {
        // Path as written (after default_path, forward slashes).
        std::string written;
        juce::File file;
        bool absolute = false;
      };

      struct Result {
        std::shared_ptr<Instrument> instrument;
        // The main file first, then every included file, each once.
        std::vector<juce::File> text_files;
        // Every sample file the regions name, each once.
        std::vector<SampleReference> samples;
        std::string error;
      };

      static Result parse(const juce::File& file, const Options& options = {});
      // Parses text as if it were the file `main_file` (tests).
      static Result parseText(const std::string& text, const juce::File& main_file, const Options& options = {});
  };
} // namespace multisample
} // namespace vital
