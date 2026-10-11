// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "JuceHeader.h"
#include "multisample_instrument.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace vital {
namespace multisample {

  // A SoundFont 2 file: its preset structure, read without the audio, and the
  // sample pool on request. Every preset is built from the one shared pool, so
  // switching presets costs no decoding and no extra memory.
  class Sf2File {
    public:
      static std::unique_ptr<Sf2File> open(const juce::File& file, std::string& error);
      static std::unique_ptr<Sf2File> openStream(juce::InputStream& stream, const juce::File& file,
                                                 std::string& error);

      const std::string& name() const { return name_; }
      const juce::File& file() const { return file_; }
      // Every preset, sorted by bank then program.
      const std::vector<PresetName>& presets() const { return presets_; }
      // The preset header index matching bank and program, then name, then
      // the stored index; -1 when the file has no presets.
      int findPreset(int bank, int program, const std::string& name, int index) const;

      // Reads the 16-bit sample data, rounding 24-bit data (sm24) to 16 bits.
      // `progress` returns false to cancel.
      std::shared_ptr<std::vector<int16_t>> readSamplePool(const std::function<bool(float)>& progress,
                                                           std::string& error) const;
      std::shared_ptr<std::vector<int16_t>> readSamplePool(juce::InputStream& stream,
                                                           const std::function<bool(float)>& progress,
                                                           std::string& error) const;

      // Builds the playable regions of preset header `preset_index`.
      std::shared_ptr<Instrument> buildPreset(int preset_index,
                                              std::shared_ptr<const std::vector<int16_t>> pool) const;

      static constexpr int kNumGenerators = 61;

      struct Generator {
        uint16_t oper = 0;
        uint16_t amount = 0;
      };

      struct SampleHeader {
        std::string name;
        uint32_t start = 0;
        uint32_t end = 0;
        uint32_t loop_start = 0;
        uint32_t loop_end = 0;
        uint32_t sample_rate = 44100;
        uint8_t original_pitch = 60;
        int8_t pitch_correction = 0;
        uint16_t link = 0;
        uint16_t type = 1;
      };

    private:
      struct PresetHeader {
        std::string name;
        uint16_t program = 0;
        uint16_t bank = 0;
        uint16_t bag = 0;
      };

      struct InstrumentHeader {
        std::string name;
        uint16_t bag = 0;
      };

      struct Bag {
        uint16_t generator = 0;
      };

      juce::File file_;
      std::string name_;
      std::vector<PresetHeader> preset_headers_;
      std::vector<Bag> preset_bags_;
      std::vector<Generator> preset_generators_;
      std::vector<InstrumentHeader> instruments_;
      std::vector<Bag> instrument_bags_;
      std::vector<Generator> instrument_generators_;
      std::vector<SampleHeader> samples_;
      std::vector<PresetName> presets_;
      int64_t smpl_offset_ = -1;
      int64_t smpl_bytes_ = 0;
      int64_t sm24_offset_ = -1;
      int64_t sm24_bytes_ = 0;
  };
} // namespace multisample
} // namespace vital
