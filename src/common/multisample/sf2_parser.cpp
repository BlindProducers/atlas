// SPDX-License-Identifier: GPL-3.0-or-later
#include "sf2_parser.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace vital {
namespace multisample {

  namespace {
    // SoundFont 2.04 generator numbers.
    enum GeneratorId {
      kStartAddrsOffset = 0,
      kEndAddrsOffset = 1,
      kStartloopAddrsOffset = 2,
      kEndloopAddrsOffset = 3,
      kStartAddrsCoarseOffset = 4,
      kEndAddrsCoarseOffset = 12,
      kPan = 17,
      kDelayVolEnv = 33,
      kAttackVolEnv = 34,
      kHoldVolEnv = 35,
      kDecayVolEnv = 36,
      kSustainVolEnv = 37,
      kReleaseVolEnv = 38,
      kKeynumToVolEnvHold = 39,
      kKeynumToVolEnvDecay = 40,
      kInstrument = 41,
      kKeyRange = 43,
      kVelRange = 44,
      kStartloopAddrsCoarseOffset = 45,
      kKeynum = 46,
      kVelocity = 47,
      kInitialAttenuation = 48,
      kEndloopAddrsCoarseOffset = 50,
      kCoarseTune = 51,
      kFineTune = 52,
      kSampleId = 53,
      kSampleModes = 54,
      kScaleTuning = 56,
      kExclusiveClass = 57,
      kOverridingRootKey = 58,
    };

    // EMU hardware, FluidSynth and Polyphone scale initialAttenuation by 0.4.
    constexpr float kAttenuationScale = 0.4f;
    // A volume envelope stage spans 96 dB.
    constexpr float kEnvelopeRangeDb = 96.0f;
    // Exclusive classes become choke groups outside the range SFZ files use.
    constexpr int64_t kExclusiveClassGroupBase = int64_t(1) << 40;

    struct GeneratorSet {
      std::array<int, Sf2File::kNumGenerators> values {};
      std::array<bool, Sf2File::kNumGenerators> set {};

      void apply(const Sf2File::Generator& generator) {
        if (generator.oper >= Sf2File::kNumGenerators)
          return;
        set[generator.oper] = true;
        if (generator.oper == kKeyRange || generator.oper == kVelRange)
          values[generator.oper] = generator.amount;
        else
          values[generator.oper] = static_cast<int16_t>(generator.amount);
      }
    };

    GeneratorSet instrumentDefaults() {
      GeneratorSet defaults;
      defaults.values[kKeyRange] = 127 << 8;
      defaults.values[kVelRange] = 127 << 8;
      defaults.values[kDelayVolEnv] = -12000;
      defaults.values[kAttackVolEnv] = -12000;
      defaults.values[kHoldVolEnv] = -12000;
      defaults.values[kDecayVolEnv] = -12000;
      defaults.values[kReleaseVolEnv] = -12000;
      defaults.values[kScaleTuning] = 100;
      defaults.values[kOverridingRootKey] = -1;
      defaults.values[kKeynum] = -1;
      defaults.values[kVelocity] = -1;
      return defaults;
    }

    // Generators a preset zone adds to the instrument's value.
    bool isPresetAdditive(int oper) {
      switch (oper) {
        case kStartAddrsOffset:
        case kEndAddrsOffset:
        case kStartloopAddrsOffset:
        case kEndloopAddrsOffset:
        case kStartAddrsCoarseOffset:
        case kEndAddrsCoarseOffset:
        case kStartloopAddrsCoarseOffset:
        case kEndloopAddrsCoarseOffset:
        case kInstrument:
        case kKeyRange:
        case kVelRange:
        case kKeynum:
        case kVelocity:
        case kSampleId:
        case kSampleModes:
        case kExclusiveClass:
        case kOverridingRootKey:
          return false;
        default:
          return true;
      }
    }

    float timecentsToSeconds(int timecents) {
      return std::pow(2.0f, std::max(-12000, std::min(8000, timecents)) / 1200.0f);
    }

    std::string readName(juce::InputStream& stream, int length) {
      std::string name(static_cast<size_t>(length), '\0');
      stream.read(&name[0], length);
      name.resize(std::strlen(name.c_str()));
      while (!name.empty() && (name.back() == ' ' || name.back() == '\0'))
        name.pop_back();
      return name;
    }

    std::string readId(juce::InputStream& stream) {
      char id[5] = {};
      stream.read(id, 4);
      return std::string(id, 4);
    }

    bool intersect(int range_a, int range_b, int& low, int& high) {
      low = std::max(range_a & 0xff, range_b & 0xff);
      high = std::min(range_a >> 8, range_b >> 8);
      return low <= high;
    }
  } // namespace

  std::unique_ptr<Sf2File> Sf2File::open(const juce::File& file, std::string& error) {
    juce::FileInputStream stream(file);
    if (!stream.openedOk()) {
      error = "Cannot open " + file.getFullPathName().toStdString();
      return nullptr;
    }
    return openStream(stream, file, error);
  }

  std::unique_ptr<Sf2File> Sf2File::openStream(juce::InputStream& stream, const juce::File& file,
                                               std::string& error) {
    auto result = std::unique_ptr<Sf2File>(new Sf2File());
    result->file_ = file;
    result->name_ = file.getFileNameWithoutExtension().toStdString();

    const int64_t total = stream.getTotalLength();
    if (readId(stream) != "RIFF") {
      error = "Not a SoundFont file (no RIFF header).";
      return nullptr;
    }
    const int64_t riff_size = static_cast<uint32_t>(stream.readInt());
    if (readId(stream) != "sfbk") {
      error = "Not a SoundFont 2 file.";
      return nullptr;
    }
    const int64_t riff_end = std::min<int64_t>(total, 8 + riff_size);

    while (stream.getPosition() + 8 <= riff_end) {
      const std::string id = readId(stream);
      const int64_t size = static_cast<uint32_t>(stream.readInt());
      const int64_t start = stream.getPosition();
      const int64_t end = std::min(riff_end, start + size);

      if (id == "LIST" && size >= 4) {
        const std::string list_type = readId(stream);
        while (stream.getPosition() + 8 <= end) {
          const std::string sub_id = readId(stream);
          const int64_t sub_size = static_cast<uint32_t>(stream.readInt());
          const int64_t sub_start = stream.getPosition();
          if (sub_start + sub_size > end) {
            error = "The SoundFont is truncated (" + sub_id + " chunk).";
            return nullptr;
          }

          if (list_type == "INFO" && sub_id == "INAM") {
            const std::string name = readName(stream, static_cast<int>(std::min<int64_t>(sub_size, 256)));
            if (!name.empty())
              result->name_ = name;
          }
          else if (list_type == "sdta" && sub_id == "smpl") {
            result->smpl_offset_ = sub_start;
            result->smpl_bytes_ = sub_size;
          }
          else if (list_type == "sdta" && sub_id == "sm24") {
            result->sm24_offset_ = sub_start;
            result->sm24_bytes_ = sub_size;
          }
          else if (list_type == "pdta") {
            if (sub_id == "phdr") {
              for (int64_t i = 0; i + 38 <= sub_size; i += 38) {
                PresetHeader header;
                header.name = readName(stream, 20);
                header.program = static_cast<uint16_t>(stream.readShort());
                header.bank = static_cast<uint16_t>(stream.readShort());
                header.bag = static_cast<uint16_t>(stream.readShort());
                stream.skipNextBytes(12);
                result->preset_headers_.push_back(header);
              }
            }
            else if (sub_id == "pbag" || sub_id == "ibag") {
              auto& bags = sub_id == "pbag" ? result->preset_bags_ : result->instrument_bags_;
              for (int64_t i = 0; i + 4 <= sub_size; i += 4) {
                Bag bag;
                bag.generator = static_cast<uint16_t>(stream.readShort());
                stream.skipNextBytes(2);
                bags.push_back(bag);
              }
            }
            else if (sub_id == "pgen" || sub_id == "igen") {
              auto& generators = sub_id == "pgen" ? result->preset_generators_ : result->instrument_generators_;
              for (int64_t i = 0; i + 4 <= sub_size; i += 4) {
                Generator generator;
                generator.oper = static_cast<uint16_t>(stream.readShort());
                generator.amount = static_cast<uint16_t>(stream.readShort());
                generators.push_back(generator);
              }
            }
            else if (sub_id == "inst") {
              for (int64_t i = 0; i + 22 <= sub_size; i += 22) {
                InstrumentHeader header;
                header.name = readName(stream, 20);
                header.bag = static_cast<uint16_t>(stream.readShort());
                result->instruments_.push_back(header);
              }
            }
            else if (sub_id == "shdr") {
              for (int64_t i = 0; i + 46 <= sub_size; i += 46) {
                SampleHeader header;
                header.name = readName(stream, 20);
                header.start = static_cast<uint32_t>(stream.readInt());
                header.end = static_cast<uint32_t>(stream.readInt());
                header.loop_start = static_cast<uint32_t>(stream.readInt());
                header.loop_end = static_cast<uint32_t>(stream.readInt());
                header.sample_rate = static_cast<uint32_t>(stream.readInt());
                header.original_pitch = static_cast<uint8_t>(stream.readByte());
                header.pitch_correction = static_cast<int8_t>(stream.readByte());
                header.link = static_cast<uint16_t>(stream.readShort());
                header.type = static_cast<uint16_t>(stream.readShort());
                result->samples_.push_back(header);
              }
            }
          }
          stream.setPosition(sub_start + sub_size + (sub_size & 1));
        }
      }
      stream.setPosition(start + size + (size & 1));
    }

    if (result->smpl_offset_ < 0) {
      error = "The SoundFont has no sample data.";
      return nullptr;
    }
    if (result->preset_headers_.size() < 2 || result->preset_bags_.empty() || result->instruments_.size() < 2 ||
        result->instrument_bags_.empty() || result->samples_.empty()) {
      error = "The SoundFont has no presets.";
      return nullptr;
    }

    const int num_presets = static_cast<int>(result->preset_headers_.size()) - 1;
    for (int i = 0; i < num_presets; ++i) {
      const PresetHeader& header = result->preset_headers_[i];
      PresetName preset;
      preset.bank = header.bank;
      preset.program = header.program;
      preset.name = header.name;
      preset.index = i;
      result->presets_.push_back(preset);
    }
    std::stable_sort(result->presets_.begin(), result->presets_.end(), [](const PresetName& a, const PresetName& b) {
      return a.bank != b.bank ? a.bank < b.bank : a.program < b.program;
    });
    return result;
  }

  int Sf2File::findPreset(int bank, int program, const std::string& name, int index) const {
    if (presets_.empty())
      return -1;
    for (const PresetName& preset : presets_) {
      if (preset.bank == bank && preset.program == program && (name.empty() || preset.name == name))
        return preset.index;
    }
    for (const PresetName& preset : presets_) {
      if (!name.empty() && preset.name == name)
        return preset.index;
    }
    for (const PresetName& preset : presets_) {
      if (preset.bank == bank && preset.program == program)
        return preset.index;
    }
    for (const PresetName& preset : presets_) {
      if (preset.index == index)
        return preset.index;
    }
    return presets_.front().index;
  }

  std::shared_ptr<std::vector<int16_t>> Sf2File::readSamplePool(const std::function<bool(float)>& progress,
                                                                std::string& error) const {
    juce::FileInputStream stream(file_);
    if (!stream.openedOk()) {
      error = "Cannot open " + file_.getFullPathName().toStdString();
      return nullptr;
    }
    return readSamplePool(stream, progress, error);
  }

  std::shared_ptr<std::vector<int16_t>> Sf2File::readSamplePool(juce::InputStream& stream,
                                                                const std::function<bool(float)>& progress,
                                                                std::string& error) const {
    const int64_t num_samples = smpl_bytes_ / 2;
    const bool has_24_bit = sm24_offset_ >= 0 && sm24_bytes_ >= num_samples;
    auto pool = std::make_shared<std::vector<int16_t>>();
    try {
      pool->resize(static_cast<size_t>(num_samples));
    }
    catch (const std::bad_alloc&) {
      error = "Not enough memory for the SoundFont's samples.";
      return nullptr;
    }

    constexpr int64_t kChunk = 1 << 18;
    std::vector<char> bytes(static_cast<size_t>(kChunk * 2));
    std::vector<uint8_t> low_bytes(has_24_bit ? static_cast<size_t>(kChunk) : 0);
    for (int64_t position = 0; position < num_samples; position += kChunk) {
      const int64_t count = std::min(kChunk, num_samples - position);
      stream.setPosition(smpl_offset_ + position * 2);
      if (stream.read(bytes.data(), static_cast<int>(count * 2)) != count * 2) {
        error = "The SoundFont's sample data is truncated.";
        return nullptr;
      }
      if (has_24_bit) {
        stream.setPosition(sm24_offset_ + position);
        stream.read(low_bytes.data(), static_cast<int>(count));
      }
      int16_t* destination = pool->data() + position;
      for (int64_t i = 0; i < count; ++i) {
        const auto low = static_cast<uint8_t>(bytes[static_cast<size_t>(2 * i)]);
        const auto high = static_cast<uint8_t>(bytes[static_cast<size_t>(2 * i + 1)]);
        const int16_t value = static_cast<int16_t>(low | (high << 8));
        if (has_24_bit) {
          const int32_t wide = (static_cast<int32_t>(value) * 256) | low_bytes[static_cast<size_t>(i)];
          destination[i] = static_cast<int16_t>(std::min(32767, (wide + 128) >> 8));
        }
        else
          destination[i] = value;
      }
      if (progress && !progress(static_cast<float>(position + count) / std::max<int64_t>(1, num_samples)))
        return nullptr;
    }
    return pool;
  }

  std::shared_ptr<Instrument> Sf2File::buildPreset(int preset_index,
                                                   std::shared_ptr<const std::vector<int16_t>> pool) const {
    auto instrument = std::make_shared<Instrument>();
    instrument->format = "sf2";
    instrument->name = name_;
    instrument->source_path = file_.getFullPathName().toStdString();
    instrument->preset_names = presets_;
    instrument->preset_index = preset_index;
    if (pool)
      instrument->memory_bytes = pool->size() * sizeof(int16_t);

    const int num_presets = static_cast<int>(preset_headers_.size()) - 1;
    if (preset_index < 0 || preset_index >= num_presets)
      return instrument;

    const int64_t pool_frames = pool ? static_cast<int64_t>(pool->size()) : 0;
    auto generatorRange = [](const std::vector<Bag>& bags, const std::vector<Generator>& generators, int bag,
                             int& first, int& last) {
      if (bag < 0 || bag + 1 >= static_cast<int>(bags.size()))
        return false;
      first = bags[bag].generator;
      last = bags[bag + 1].generator;
      first = std::min<int>(first, static_cast<int>(generators.size()));
      last = std::min<int>(std::max(first, last), static_cast<int>(generators.size()));
      return true;
    };

    const int preset_bag_start = preset_headers_[preset_index].bag;
    const int preset_bag_end = preset_headers_[preset_index + 1].bag;
    GeneratorSet preset_global;
    for (int bag = preset_bag_start; bag < preset_bag_end; ++bag) {
      int first = 0;
      int last = 0;
      if (!generatorRange(preset_bags_, preset_generators_, bag, first, last) || first == last)
        continue;
      if (preset_generators_[last - 1].oper != kInstrument) {
        if (bag == preset_bag_start) {
          for (int g = first; g < last; ++g)
            preset_global.apply(preset_generators_[g]);
        }
        continue;
      }

      GeneratorSet preset_zone = preset_global;
      for (int g = first; g < last; ++g)
        preset_zone.apply(preset_generators_[g]);
      if (!preset_zone.set[kKeyRange])
        preset_zone.values[kKeyRange] = 127 << 8;
      if (!preset_zone.set[kVelRange])
        preset_zone.values[kVelRange] = 127 << 8;

      const int instrument_index = preset_generators_[last - 1].amount;
      if (instrument_index < 0 || instrument_index + 1 >= static_cast<int>(instruments_.size()))
        continue;

      const int instrument_bag_start = instruments_[instrument_index].bag;
      const int instrument_bag_end = instruments_[instrument_index + 1].bag;
      GeneratorSet instrument_global = instrumentDefaults();
      for (int ibag = instrument_bag_start; ibag < instrument_bag_end; ++ibag) {
        int ifirst = 0;
        int ilast = 0;
        if (!generatorRange(instrument_bags_, instrument_generators_, ibag, ifirst, ilast) || ifirst == ilast)
          continue;
        if (instrument_generators_[ilast - 1].oper != kSampleId) {
          if (ibag == instrument_bag_start) {
            for (int g = ifirst; g < ilast; ++g)
              instrument_global.apply(instrument_generators_[g]);
          }
          continue;
        }

        GeneratorSet zone = instrument_global;
        for (int g = ifirst; g < ilast; ++g)
          zone.apply(instrument_generators_[g]);

        int lokey = 0;
        int hikey = 127;
        int lovel = 0;
        int hivel = 127;
        if (!intersect(zone.values[kKeyRange], preset_zone.values[kKeyRange], lokey, hikey) ||
            !intersect(zone.values[kVelRange], preset_zone.values[kVelRange], lovel, hivel)) {
          continue;
        }
        for (int oper = 0; oper < kNumGenerators; ++oper) {
          if (preset_zone.set[oper] && isPresetAdditive(oper))
            zone.values[oper] += preset_zone.values[oper];
        }

        const int sample_index = static_cast<uint16_t>(zone.values[kSampleId]);
        if (sample_index >= static_cast<int>(samples_.size()))
          continue;
        const SampleHeader& sample = samples_[sample_index];
        if (sample.type & 0x8000)
          continue;

        Region region;
        region.sample_path = sample.name;
        region.pool = pool;
        region.left = pool ? pool->data() : nullptr;
        region.right = region.left;
        region.data_frames = pool_frames;
        region.sample_rate = std::max<uint32_t>(1, sample.sample_rate);

        auto clampFrame = [pool_frames](int64_t frame) {
          return std::max<int64_t>(0, std::min(pool_frames, frame));
        };
        region.offset = clampFrame(static_cast<int64_t>(sample.start) + zone.values[kStartAddrsOffset] +
                                   32768 * zone.values[kStartAddrsCoarseOffset]);
        region.end = clampFrame(static_cast<int64_t>(sample.end) + zone.values[kEndAddrsOffset] +
                                32768 * zone.values[kEndAddrsCoarseOffset]);
        region.loop_start = clampFrame(static_cast<int64_t>(sample.loop_start) + zone.values[kStartloopAddrsOffset] +
                                       32768 * zone.values[kStartloopAddrsCoarseOffset]);
        region.loop_end = clampFrame(static_cast<int64_t>(sample.loop_end) + zone.values[kEndloopAddrsOffset] +
                                     32768 * zone.values[kEndloopAddrsCoarseOffset]);
        if (region.end <= region.offset)
          continue;
        region.loop_mode_set = true;
        region.loop_start_set = true;
        region.loop_end_set = true;
        const int modes = zone.values[kSampleModes] & 3;
        region.loop_mode = modes == 1 ? LoopMode::kContinuous : modes == 3 ? LoopMode::kSustain : LoopMode::kNoLoop;
        if (region.loop_end <= region.loop_start + 1)
          region.loop_mode = LoopMode::kNoLoop;

        region.lokey = lokey;
        region.hikey = hikey;
        region.lovel = lovel;
        region.hivel = hivel;
        const int root = zone.values[kOverridingRootKey] >= 0 ? zone.values[kOverridingRootKey]
                                                              : sample.original_pitch <= 127 ? sample.original_pitch
                                                                                             : 60;
        region.pitch_keycenter = static_cast<float>(root);
        region.pitch_keytrack = static_cast<float>(zone.values[kScaleTuning]);
        region.tune_cents = zone.values[kCoarseTune] * 100.0f + zone.values[kFineTune] + sample.pitch_correction;
        region.pan = std::max(-1.0f, std::min(1.0f, zone.values[kPan] / 500.0f));
        const int attenuation = std::max(0, std::min(1440, zone.values[kInitialAttenuation]));
        region.volume_db = -kAttenuationScale * attenuation / 10.0f;

        Envelope& envelope = region.ampeg;
        envelope.delay = zone.values[kDelayVolEnv] <= -12000 ? 0.0f : timecentsToSeconds(zone.values[kDelayVolEnv]);
        envelope.attack = timecentsToSeconds(zone.values[kAttackVolEnv]);
        envelope.hold = zone.values[kHoldVolEnv] <= -12000 ? 0.0f : timecentsToSeconds(zone.values[kHoldVolEnv]);
        const float sustain_db = std::max(0, std::min(1440, zone.values[kSustainVolEnv])) / 10.0f;
        envelope.sustain = std::pow(10.0f, -sustain_db / 20.0f);
        // SF2 decay is the time a full 96 dB fall would take; stop at sustain.
        envelope.decay = timecentsToSeconds(zone.values[kDecayVolEnv]) *
                         std::min(1.0f, sustain_db / kEnvelopeRangeDb);
        envelope.release = timecentsToSeconds(zone.values[kReleaseVolEnv]);
        region.hold_keytrack = static_cast<float>(zone.values[kKeynumToVolEnvHold]);
        region.decay_keytrack = static_cast<float>(zone.values[kKeynumToVolEnvDecay]);

        if (zone.values[kExclusiveClass] > 0) {
          region.group = kExclusiveClassGroupBase + zone.values[kExclusiveClass];
          region.off_by = region.group;
          region.off_mode = OffMode::kFast;
        }

        region.buildVelocityCurve();
        instrument->regions.push_back(std::move(region));
      }
    }

    instrument->build();
    return instrument;
  }
} // namespace multisample
} // namespace vital
