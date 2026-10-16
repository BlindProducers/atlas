// SPDX-License-Identifier: GPL-3.0-or-later
#include "multisample_loader.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace vital {
namespace multisample {

  namespace {
    // Refuse instruments whose audio would take more than this share of RAM.
    constexpr double kMaxMemoryShare = 0.7;
    constexpr int kReadChunk = 1 << 16;

    const std::array<uint32_t, 256>& crcTable() {
      static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> result {};
        for (uint32_t i = 0; i < 256; ++i) {
          uint32_t value = i;
          for (int bit = 0; bit < 8; ++bit)
            value = (value & 1) ? (0xEDB88320u ^ (value >> 1)) : (value >> 1);
          result[i] = value;
        }
        return result;
      }();
      return table;
    }

    int16_t roundTo16(int32_t left_justified) {
      const int64_t rounded = (static_cast<int64_t>(left_justified) + 0x8000) >> 16;
      return static_cast<int16_t>(std::max<int64_t>(-32768, std::min<int64_t>(32767, rounded)));
    }

    int16_t floatTo16(float value) {
      const float scaled = std::round(value * 32767.0f);
      return static_cast<int16_t>(std::max(-32768.0f, std::min(32767.0f, scaled)));
    }

    juce::AudioFormatManager& formatManager() {
      thread_local std::unique_ptr<juce::AudioFormatManager> manager;
      if (manager == nullptr) {
        manager = std::make_unique<juce::AudioFormatManager>();
        manager->registerBasicFormats();
      }
      return *manager;
    }

    size_t memoryBudget() {
      const double megabytes = juce::SystemStats::getMemorySizeInMegabytes();
      return static_cast<size_t>(megabytes * 1024.0 * 1024.0 * kMaxMemoryShare);
    }

    std::string formatBytes(size_t bytes) {
      return juce::File::descriptionOfSizeInBytes(static_cast<int64_t>(bytes)).toStdString();
    }

    // The deepest folder containing every file.
    juce::File commonRoot(const std::vector<juce::File>& files) {
      if (files.empty())
        return {};
      juce::File root = files.front().getParentDirectory();
      for (const juce::File& file : files) {
        while (!file.isAChildOf(root) && root.getParentDirectory() != root)
          root = root.getParentDirectory();
      }
      return root;
    }

    std::string relativeTo(const juce::File& file, const juce::File& root) {
      return file.getRelativePathFrom(root).replaceCharacter('\\', '/').toStdString();
    }

    // The size and CRC of a file that is stored but not decoded, read in
    // chunks so that a large sample never sits in memory whole.
    bool checksumFile(const juce::File& file, std::vector<char>& buffer, int64_t& size, uint32_t& crc) {
      juce::FileInputStream stream(file);
      if (!stream.openedOk())
        return false;
      if (buffer.size() < static_cast<size_t>(1 << 20))
        buffer.resize(1 << 20);
      size = 0;
      crc = 0;
      while (!stream.isExhausted()) {
        const int read = stream.read(buffer.data(), static_cast<int>(buffer.size()));
        if (read < 0)
          return false;
        if (read == 0)
          break;
        crc = crc32(buffer.data(), static_cast<size_t>(read), crc);
        size += read;
      }
      return true;
    }

    // Applies the decoded audio to SFZ regions: data views, root keys from
    // the file, default loops and range clamping.
    void attachSample(Region& region, std::shared_ptr<const SampleData> data) {
      region.sample = data;
      region.left = data->channel(0);
      region.right = data->channel(1);
      region.data_frames = data->frames;
      region.sample_rate = data->sample_rate;
      if (region.keycenter_from_sample)
        region.pitch_keycenter = static_cast<float>(data->root_key >= 0 ? data->root_key : 60);

      const int64_t frames = data->frames;
      region.offset = std::max<int64_t>(0, std::min(frames, region.offset));
      if (region.end < 0 || region.end > frames)
        region.end = frames;
      region.end = std::max(region.end, region.offset);

      if (!region.loop_mode_set)
        region.loop_mode = data->has_loop ? LoopMode::kContinuous : LoopMode::kNoLoop;
      if (region.loop_mode == LoopMode::kContinuous || region.loop_mode == LoopMode::kSustain) {
        if (!region.loop_start_set)
          region.loop_start = data->has_loop ? data->loop_start : 0;
        if (!region.loop_end_set)
          region.loop_end = data->has_loop ? data->loop_end : region.end;
        region.loop_start = std::max<int64_t>(0, std::min(region.end, region.loop_start));
        region.loop_end = std::max<int64_t>(0, std::min(region.end, region.loop_end));
        if (region.loop_end <= region.loop_start + 1)
          region.loop_mode = LoopMode::kNoLoop;
      }
    }
  } // namespace

  uint32_t crc32(const void* data, size_t size, uint32_t crc) {
    const auto& table = crcTable();
    const auto* bytes = static_cast<const uint8_t*>(data);
    crc = ~crc;
    for (size_t i = 0; i < size; ++i)
      crc = table[(crc ^ bytes[i]) & 0xff] ^ (crc >> 8);
    return ~crc;
  }

  std::string manifestHash(const std::vector<FileInfo>& files) {
    std::vector<std::string> lines;
    for (const FileInfo& file : files)
      lines.push_back(file.relative + "\t" + std::to_string(file.size) + "\t" + std::to_string(file.crc));
    std::sort(lines.begin(), lines.end());

    // Two FNV-1a 64-bit hashes with different offset bases.
    uint64_t first = 0xcbf29ce484222325ull;
    uint64_t second = 0x84222325cbf29ce4ull;
    constexpr uint64_t kPrime = 0x100000001b3ull;
    for (const std::string& line : lines) {
      for (char c : line + "\n") {
        first = (first ^ static_cast<uint8_t>(c)) * kPrime;
        second = (second ^ static_cast<uint8_t>(c)) * kPrime;
        second ^= second >> 29;
      }
    }
    char text[33];
    std::snprintf(text, sizeof(text), "%016llx%016llx", static_cast<unsigned long long>(first),
                  static_cast<unsigned long long>(second));
    return text;
  }

  std::shared_ptr<SampleData> decodeSample(const void* data, size_t size, std::string& error) {
    auto stream = std::make_unique<juce::MemoryInputStream>(data, size, false);
    std::unique_ptr<juce::AudioFormatReader> reader(formatManager().createReaderFor(std::move(stream)));
    if (reader == nullptr) {
      error = "unsupported audio format";
      return nullptr;
    }

    auto sample = std::make_shared<SampleData>();
    sample->num_channels = std::max(1, std::min(2, static_cast<int>(reader->numChannels)));
    sample->frames = reader->lengthInSamples;
    sample->sample_rate = reader->sampleRate > 0.0 ? reader->sampleRate : 44100.0;
    try {
      for (int channel = 0; channel < sample->num_channels; ++channel)
        sample->channels[channel].resize(static_cast<size_t>(sample->frames));
    }
    catch (const std::bad_alloc&) {
      error = "not enough memory";
      return nullptr;
    }

    std::vector<int> buffers[2];
    buffers[0].resize(kReadChunk);
    buffers[1].resize(kReadChunk);
    int* destinations[2] = { buffers[0].data(), buffers[1].data() };
    const bool floating = reader->usesFloatingPointData;
    for (int64_t position = 0; position < sample->frames; position += kReadChunk) {
      const int count = static_cast<int>(std::min<int64_t>(kReadChunk, sample->frames - position));
      if (!reader->read(destinations, sample->num_channels, position, count, false)) {
        error = "the audio data is damaged";
        return nullptr;
      }
      for (int channel = 0; channel < sample->num_channels; ++channel) {
        int16_t* output = sample->channels[channel].data() + position;
        const int* input = destinations[channel];
        if (floating) {
          const auto* floats = reinterpret_cast<const float*>(input);
          for (int i = 0; i < count; ++i)
            output[i] = floatTo16(floats[i]);
        }
        else {
          for (int i = 0; i < count; ++i)
            output[i] = roundTo16(input[i]);
        }
      }
    }

    const juce::StringPairArray& metadata = reader->metadataValues;
    if (metadata.getValue("NumSampleLoops", "0").getIntValue() > 0 && metadata.containsKey("Loop0Start")) {
      const int64_t loop_start = metadata.getValue("Loop0Start", "0").getLargeIntValue();
      // The smpl chunk stores the last frame of the loop.
      const int64_t loop_end = metadata.getValue("Loop0End", "0").getLargeIntValue() + 1;
      if (loop_end > loop_start + 1 && loop_end <= sample->frames) {
        sample->has_loop = true;
        sample->loop_start = loop_start;
        sample->loop_end = loop_end;
      }
    }
    if (metadata.containsKey("MidiUnityNote")) {
      const int root = metadata.getValue("MidiUnityNote", "-1").getIntValue();
      if (root >= 0 && root < 128)
        sample->root_key = root;
    }
    return sample;
  }

  bool isSfz(const juce::File& file) {
    return file.hasFileExtension("sfz");
  }

  bool isDecentSampler(const juce::File& file) {
    return file.hasFileExtension("dspreset");
  }

  bool isTextInstrument(const juce::File& file) {
    return isSfz(file) || isDecentSampler(file);
  }

  std::string instrumentFormat(const juce::File& file) {
    return isSf2(file) ? "sf2" : isDecentSampler(file) ? "dspreset" : "sfz";
  }

  bool isSf2(const juce::File& file) {
    return file.hasFileExtension("sf2");
  }

  namespace {
    std::shared_ptr<LoadResult> loadSf2(const LoadRequest& request, const ProgressCallback& progress) {
      auto result = std::make_shared<LoadResult>();
      std::string error;

      // A different preset of the same unchanged file shares the pool.
      if (request.previous && request.previous->sf2 && request.previous->sf2_pool &&
          request.previous->sf2->file() == request.file && !request.previous->files.empty() &&
          request.previous->files.front().size == request.file.getSize()) {
        const int preset = request.previous->sf2->findPreset(request.sf2_bank, request.sf2_program,
                                                             request.sf2_name, request.sf2_index);
        return withSf2Preset(*request.previous, preset);
      }

      if (progress && !progress(0.0f, "Reading SoundFont"))
        return nullptr;
      std::shared_ptr<Sf2File> sf2 = Sf2File::open(request.file, error);
      if (sf2 == nullptr) {
        result->error = error;
        return result;
      }

      const size_t needed = static_cast<size_t>(std::max<int64_t>(0, request.file.getSize()));
      if (needed > memoryBudget()) {
        result->error = "The SoundFont needs about " + formatBytes(needed) + " of memory, more than this computer can spare.";
        return result;
      }

      std::shared_ptr<std::vector<int16_t>> pool = sf2->readSamplePool([&progress](float amount) {
        return !progress || progress(amount * 0.9f, "Reading samples");
      }, error);
      if (pool == nullptr) {
        if (error.empty())
          return nullptr;
        result->error = error;
        return result;
      }

      // The file's CRC identifies it in containers and the cache.
      FileInfo info;
      info.file = request.file;
      info.relative = request.file.getFileName().toStdString();
      info.size = request.file.getSize();
      {
        juce::FileInputStream stream(request.file);
        std::vector<char> buffer(1 << 20);
        uint32_t crc = 0;
        int64_t done = 0;
        while (!stream.isExhausted()) {
          const int read = stream.read(buffer.data(), static_cast<int>(buffer.size()));
          if (read <= 0)
            break;
          crc = crc32(buffer.data(), static_cast<size_t>(read), crc);
          done += read;
          if (progress && !progress(0.9f + 0.1f * done / std::max<int64_t>(1, info.size), "Checking file"))
            return nullptr;
        }
        info.crc = crc;
      }

      result->sf2 = sf2;
      result->sf2_pool = pool;
      result->files.push_back(info);
      result->entry = info.relative;
      result->hash = manifestHash(result->files);
      const int preset = sf2->findPreset(request.sf2_bank, request.sf2_program, request.sf2_name, request.sf2_index);
      return withSf2Preset(*result, preset);
    }

    // SFZ and DecentSampler presets: parse the text, then decode the samples it names.
    std::shared_ptr<LoadResult> loadText(const LoadRequest& request, const ProgressCallback& progress) {
      auto result = std::make_shared<LoadResult>();
      const bool decent_sampler = isDecentSampler(request.file);
      if (progress && !progress(0.0f, decent_sampler ? "Reading DecentSampler preset" : "Reading SFZ"))
        return nullptr;

      SfzParser::Result parsed = decent_sampler ? DecentSamplerParser::parse(request.file, request.sfz_options)
                                                : SfzParser::parse(request.file, request.sfz_options);
      if (!parsed.error.empty()) {
        result->error = parsed.error;
        return result;
      }
      std::shared_ptr<Instrument> instrument = parsed.instrument;

      struct Job {
        juce::File file;
        // A region plays it. Otherwise (a DecentSampler sample in a disabled
        // group or tag) the file is stored and hashed but not decoded.
        bool decode = true;
        // The size and CRC are known: the file was read, or an earlier load knew them.
        bool read = false;
        int64_t size = 0;
        uint32_t crc = 0;
        juce::Time modified;
        std::shared_ptr<const SampleData> data;
        std::string error;
      };
      std::vector<Job> jobs;
      int playable = 0;
      for (const auto& reference : parsed.samples) {
        Job job;
        job.file = reference.file;
        job.decode = reference.played;
        playable += reference.played ? 1 : 0;
        jobs.push_back(job);
      }

      // Unchanged files reuse the previous load's audio, and the size and CRC
      // of the files it only stored. Audio decoded for a group that is now
      // disabled is kept, so turning it on again decodes nothing.
      if (request.previous) {
        for (Job& job : jobs) {
          for (const auto& decoded : request.previous->decoded) {
            if (decoded.file == job.file && job.file.existsAsFile() &&
                decoded.modified == job.file.getLastModificationTime() && decoded.size == job.file.getSize()) {
              job.data = decoded.data;
              job.size = decoded.size;
              job.crc = decoded.crc;
              job.modified = decoded.modified;
              job.read = true;
              break;
            }
          }
        }
      }

      std::atomic<int> next { 0 };
      std::atomic<int> completed { 0 };
      std::atomic<bool> cancelled { false };
      std::atomic<size_t> memory { 0 };
      const size_t budget = memoryBudget();
      std::atomic<bool> over_budget { false };
      for (const Job& job : jobs) {
        if (job.data)
          memory += job.data->memoryBytes();
      }

      auto work = [&] {
        std::vector<char> scratch;
        while (!cancelled && !over_budget) {
          const int index = next++;
          if (index >= static_cast<int>(jobs.size()))
            break;
          Job& job = jobs[static_cast<size_t>(index)];
          if (job.decode && !job.data) {
            juce::MemoryBlock bytes;
            if (!job.file.existsAsFile() || !job.file.loadFileAsData(bytes)) {
              job.error = "missing";
            }
            else {
              job.size = static_cast<int64_t>(bytes.getSize());
              job.modified = job.file.getLastModificationTime();
              job.crc = crc32(bytes.getData(), bytes.getSize());
              job.read = true;
              std::string error;
              std::shared_ptr<SampleData> data = decodeSample(bytes.getData(), bytes.getSize(), error);
              if (data == nullptr)
                job.error = error;
              else {
                memory += data->memoryBytes();
                if (memory > budget)
                  over_budget = true;
                job.data = data;
              }
            }
          }
          else if (!job.decode && !job.read) {
            if (!job.file.existsAsFile() || !checksumFile(job.file, scratch, job.size, job.crc))
              job.error = "missing";
            else {
              job.modified = job.file.getLastModificationTime();
              job.read = true;
            }
          }
          completed++;
        }
      };

      const int num_threads = std::max(1, std::min(4, juce::SystemStats::getNumCpus() - 1));
      std::vector<std::unique_ptr<std::thread>> threads;
      for (int i = 1; i < num_threads; ++i)
        threads.push_back(std::make_unique<std::thread>(work));

      // The calling thread decodes too, and reports progress between files.
      std::thread reporter;
      bool keep_reporting = true;
      std::mutex report_mutex;
      std::condition_variable report_condition;
      if (progress) {
        reporter = std::thread([&] {
          std::unique_lock<std::mutex> lock(report_mutex);
          while (keep_reporting) {
            report_condition.wait_for(lock, std::chrono::milliseconds(100));
            if (!keep_reporting)
              break;
            const float amount = jobs.empty() ? 1.0f : static_cast<float>(completed.load()) / jobs.size();
            if (!progress(amount * 0.98f, "Loading samples"))
              cancelled = true;
          }
        });
      }
      work();
      for (auto& thread : threads)
        thread->join();
      if (reporter.joinable()) {
        {
          std::lock_guard<std::mutex> lock(report_mutex);
          keep_reporting = false;
        }
        report_condition.notify_all();
        reporter.join();
      }

      if (cancelled)
        return nullptr;
      if (over_budget) {
        result->error = "The instrument needs more than " + formatBytes(budget) +
                        " of memory, more than this computer can spare.";
        return result;
      }

      // Attach audio to the regions; drop regions whose sample is missing.
      std::map<juce::String, const Job*> by_path;
      for (const Job& job : jobs)
        by_path[job.file.getFullPathName()] = &job;

      std::vector<Region> regions;
      regions.reserve(instrument->regions.size());
      std::set<std::string> missing;
      for (Region& region : instrument->regions) {
        if (region.silent) {
          regions.push_back(std::move(region));
          continue;
        }
        auto found = by_path.find(juce::String(region.sample_path));
        if (found == by_path.end() || !found->second->data) {
          std::string reason = found == by_path.end() ? "missing" : found->second->error;
          missing.insert(region.sample_path + (reason == "missing" ? "" : " (" + reason + ")"));
          continue;
        }
        attachSample(region, found->second->data);
        regions.push_back(std::move(region));
      }
      instrument->regions = std::move(regions);
      for (const std::string& file : missing)
        instrument->missing_files.push_back(file);

      if (instrument->regions.empty()) {
        if (!instrument->missing_files.empty()) {
          result->error = "None of the instrument's " + std::to_string(playable) +
                          " samples could be loaded. First problem: " + instrument->missing_files.front();
        }
        else
          result->error = decent_sampler ? "The DecentSampler preset has no playable samples."
                                         : "The SFZ file has no playable regions.";
        return result;
      }

      // The files a container must hold: the text and every sample file that
      // exists, whether a region plays it now or a disabled group names it,
      // so that the hash and the stored library do not depend on the
      // controls. Only files that were read: a missing one may resolve
      // anywhere and must not move the root (or the hash) of the files that
      // are stored.
      std::vector<juce::File> relative_files;
      for (const juce::File& file : parsed.text_files)
        relative_files.push_back(file);
      for (size_t i = 0; i < parsed.samples.size(); ++i) {
        if (!parsed.samples[i].absolute && jobs[i].read)
          relative_files.push_back(parsed.samples[i].file);
      }
      const juce::File root = commonRoot(relative_files);

      for (const juce::File& file : parsed.text_files) {
        if (!file.existsAsFile())
          continue;
        juce::MemoryBlock bytes;
        file.loadFileAsData(bytes);
        FileInfo info;
        info.file = file;
        info.relative = relativeTo(file, root);
        info.size = static_cast<int64_t>(bytes.getSize());
        info.crc = crc32(bytes.getData(), bytes.getSize());
        info.text = true;
        result->files.push_back(info);
      }
      int external = 0;
      size_t memory_bytes = 0;
      for (size_t i = 0; i < jobs.size(); ++i) {
        const Job& job = jobs[i];
        if (!job.read)
          continue;
        const auto& reference = parsed.samples[i];
        FileInfo info;
        info.file = job.file;
        info.relative = reference.absolute ? "_external/" + std::to_string(external++) + "/" +
                                                 job.file.getFileName().toStdString()
                                           : relativeTo(job.file, root);
        info.written = reference.written;
        info.size = job.size;
        info.crc = job.crc;
        result->files.push_back(info);
        if (job.data)
          memory_bytes += job.data->memoryBytes();

        // A file that was only stored has no audio; its size and CRC are kept.
        LoadResult::Decoded decoded;
        decoded.file = job.file;
        decoded.modified = job.modified;
        decoded.size = job.size;
        decoded.crc = job.crc;
        decoded.data = job.data;
        result->decoded.push_back(decoded);
      }

      instrument->memory_bytes = memory_bytes;
      instrument->build();
      result->entry = relativeTo(request.file, root);
      result->hash = manifestHash(result->files);
      result->instrument = instrument;
      if (progress)
        progress(1.0f, "Loaded");
      return result;
    }
  } // namespace

  std::shared_ptr<LoadResult> loadInstrument(const LoadRequest& request, const ProgressCallback& progress) {
    if (!request.file.existsAsFile()) {
      auto result = std::make_shared<LoadResult>();
      result->error = "Instrument not found: " + request.file.getFullPathName().toStdString();
      return result;
    }
    if (isSf2(request.file))
      return loadSf2(request, progress);
    if (isTextInstrument(request.file))
      return loadText(request, progress);
    auto result = std::make_shared<LoadResult>();
    result->error = "Choose an SFZ, DecentSampler or SF2 file.";
    return result;
  }

  std::shared_ptr<LoadResult> withSf2Preset(const LoadResult& base, int preset_index) {
    auto result = std::make_shared<LoadResult>();
    result->files = base.files;
    result->entry = base.entry;
    result->hash = base.hash;
    result->sf2 = base.sf2;
    result->sf2_pool = base.sf2_pool;
    if (!base.sf2) {
      result->error = "Not a SoundFont.";
      return result;
    }
    std::shared_ptr<Instrument> instrument = base.sf2->buildPreset(preset_index, base.sf2_pool);
    if (instrument->regions.empty() && instrument->warnings.empty())
      instrument->warnings.push_back("This preset has no playable regions.");
    result->instrument = instrument;
    return result;
  }
} // namespace multisample
} // namespace vital
