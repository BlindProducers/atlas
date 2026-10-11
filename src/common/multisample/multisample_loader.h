// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "JuceHeader.h"
#include "multisample_instrument.h"
#include "sf2_parser.h"
#include "sfz_parser.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace vital {
namespace multisample {

  uint32_t crc32(const void* data, size_t size, uint32_t crc = 0);

  // One file an instrument reads. `relative` is its path inside a preset
  // container (and in the content hash).
  struct FileInfo {
    juce::File file;
    std::string relative;
    // The path as an SFZ file names it (samples outside the library are
    // stored under "_external/" and found again through this).
    std::string written;
    int64_t size = 0;
    uint32_t crc = 0;
    bool text = false;
  };

  // A content hash over the files' relative paths, sizes and CRCs: 32 hex
  // characters, the same for an instrument and its extracted copy.
  std::string manifestHash(const std::vector<FileInfo>& files);

  // Decodes a WAV/AIFF/FLAC/Ogg file to 16-bit audio (24-bit and float data
  // are rounded), with any loop and root key stored in the file.
  std::shared_ptr<SampleData> decodeSample(const void* data, size_t size, std::string& error);

  struct LoadResult;

  struct LoadRequest {
    juce::File file;
    SfzParser::Options sfz_options;
    // SF2: the preset to build, matched by bank and program, then name, then index.
    int sf2_bank = -1;
    int sf2_program = -1;
    std::string sf2_name;
    int sf2_index = -1;
    // Reuse decoded audio from this earlier load when files are unchanged.
    std::shared_ptr<const LoadResult> previous;
  };

  struct LoadResult {
    std::shared_ptr<const Instrument> instrument;
    std::vector<FileInfo> files;
    // The entry file's path relative to the container root.
    std::string entry;
    std::string hash;
    std::string error;
    // SF2 structure and samples, kept to switch presets without reloading.
    std::shared_ptr<const Sf2File> sf2;
    std::shared_ptr<const std::vector<int16_t>> sf2_pool;
    // SFZ: decoded audio by file, kept so a text reload decodes nothing.
    struct Decoded {
      juce::File file;
      juce::Time modified;
      int64_t size = 0;
      uint32_t crc = 0;
      std::shared_ptr<const SampleData> data;
    };
    std::vector<Decoded> decoded;
  };

  // `progress` gets 0..1 and a phase description; it returns false to cancel.
  using ProgressCallback = std::function<bool(float, const std::string&)>;

  std::shared_ptr<LoadResult> loadInstrument(const LoadRequest& request, const ProgressCallback& progress);
  // Rebuilds an SF2 load for another preset, sharing its sample pool.
  std::shared_ptr<LoadResult> withSf2Preset(const LoadResult& base, int preset_index);

  bool isSfz(const juce::File& file);
  bool isSf2(const juce::File& file);
} // namespace multisample
} // namespace vital
