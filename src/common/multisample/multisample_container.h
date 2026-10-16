// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "JuceHeader.h"
#include "multisample_loader.h"

#include <functional>
#include <set>
#include <string>
#include <vector>

namespace vital {
namespace multisample {

  // Self-contained presets. A preset whose oscillators play SFZ/SF2
  // instruments is a zip: "preset.json" first, then each instrument's files
  // under "multisample/<hash>/" in their original layout (audio and SF2
  // files stored as they are, SFZ text compressed). Every other preset stays
  // plain JSON. Instruments are played from an extracted copy in the cache.
  namespace container {
    constexpr const char* kPresetEntry = "preset.json";

    bool isContainer(const juce::File& file);
    // The preset JSON of a plain or container preset; at most `max_bytes`
    // bytes when positive (metadata reads).
    bool readPresetText(const juce::File& file, std::string& text, int64_t max_bytes = -1);

    struct Instrument {
      std::string hash;
      std::vector<FileInfo> files;
    };

    // Writes a container preset (to a temporary file, then into place).
    bool write(const juce::File& target, const std::string& preset_json,
               const std::vector<Instrument>& instruments, std::string& error);
    // Replaces the preset JSON of an existing preset, plain or container.
    bool replacePresetText(const juce::File& file, const std::string& preset_json, std::string& error);

    // ---- The instrument cache ----
    juce::File cacheRoot();
    // Tests point the cache somewhere temporary.
    void setCacheRootForTesting(const juce::File& folder);
    juce::File cacheFolder(const std::string& hash);
    // Editable copies of cached SFZ text, by hash.
    juce::File workingFolder(const std::string& hash);
    // True for the loader's own 32-digit hex hashes, the only form that may name a cache folder.
    bool isValidHash(const std::string& hash);
    bool isCached(const std::string& hash);
    // Marks a cached instrument as just used (eviction is least recently used first).
    void touch(const std::string& hash);

    // Extracts an instrument from a container preset into the cache, checking
    // each file's CRC. `progress` returns false to cancel.
    bool extractToCache(const juce::File& container, const std::string& hash, const std::vector<FileInfo>& files,
                        const std::function<bool(float)>& progress, std::string& error);
    // Places an instrument's files in the cache by hard link, or by copy when
    // linking fails and the files total less than `max_copy_bytes`.
    bool populateCache(const std::string& hash, const std::vector<FileInfo>& files, int64_t max_copy_bytes,
                       const std::function<bool()>& cancelled = {});
    // Removes least recently used instruments until the cache fits the budget.
    void pruneCache(int64_t budget_bytes, const std::set<std::string>& in_use);
  } // namespace container
} // namespace multisample
} // namespace vital
