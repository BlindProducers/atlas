// SPDX-License-Identifier: GPL-3.0-or-later
#include "multisample_manager.h"
#include "multisample_container.h"

#include <chrono>
#include <thread>

namespace vital {
namespace multisample {

  namespace {
    // A direct load is linked into the cache; copying is limited to this size.
    constexpr int64_t kMaxCacheCopyBytes = 2ll << 30;
    constexpr int64_t kCacheBudgetBytes = 20ll << 30;

    std::string stringField(const nlohmann::json& data, const char* key) {
      auto found = data.find(key);
      return found != data.end() && found->is_string() ? found->get<std::string>() : std::string();
    }

    int intField(const nlohmann::json& data, const char* key, int fallback) {
      auto found = data.find(key);
      return found != data.end() && found->is_number() ? found->get<int>() : fallback;
    }

    void describePreset(InstrumentSpec& spec, const Instrument& instrument) {
      spec.sf2_index = instrument.preset_index;
      for (const PresetName& preset : instrument.preset_names) {
        if (preset.index == instrument.preset_index) {
          spec.sf2_bank = preset.bank;
          spec.sf2_program = preset.program;
          spec.sf2_name = preset.name;
        }
      }
    }
  } // namespace

  nlohmann::json InstrumentSpec::toJson(bool with_files) const {
    nlohmann::json data;
    data["format"] = format;
    data["name"] = name;
    data["path"] = path;
    data["hash"] = hash;
    data["entry"] = entry;
    if (format == "sf2") {
      data["sf2_preset"] = { { "index", sf2_index }, { "bank", sf2_bank }, { "program", sf2_program },
                             { "name", sf2_name } };
    }
    if (with_files) {
      nlohmann::json list = nlohmann::json::array();
      for (const FileInfo& file : files)
        list.push_back({ { "path", file.relative }, { "size", file.size }, { "crc", file.crc } });
      data["files"] = list;
      nlohmann::json overrides = nlohmann::json::object();
      for (const auto& entry_override : path_overrides)
        overrides[entry_override.first] = entry_override.second;
      data["path_overrides"] = overrides;
    }
    return data;
  }

  InstrumentSpec InstrumentSpec::fromJson(const nlohmann::json& data) {
    InstrumentSpec spec;
    if (!data.is_object())
      return spec;
    spec.format = stringField(data, "format");
    spec.name = stringField(data, "name");
    spec.path = stringField(data, "path");
    spec.hash = stringField(data, "hash");
    spec.entry = stringField(data, "entry");
    auto preset = data.find("sf2_preset");
    if (preset != data.end() && preset->is_object()) {
      spec.sf2_index = intField(*preset, "index", -1);
      spec.sf2_bank = intField(*preset, "bank", -1);
      spec.sf2_program = intField(*preset, "program", -1);
      spec.sf2_name = stringField(*preset, "name");
    }
    auto files = data.find("files");
    if (files != data.end() && files->is_array()) {
      for (const auto& item : *files) {
        if (!item.is_object())
          continue;
        FileInfo info;
        info.relative = stringField(item, "path");
        auto size = item.find("size");
        info.size = size != item.end() && size->is_number() ? size->get<int64_t>() : 0;
        auto crc = item.find("crc");
        info.crc = crc != item.end() && crc->is_number() ? crc->get<uint32_t>() : 0;
        info.text = juce::String(info.relative).endsWithIgnoreCase(".sfz") ||
                    juce::String(info.relative).endsWithIgnoreCase(".dspreset");
        spec.files.push_back(info);
      }
    }
    auto overrides = data.find("path_overrides");
    if (overrides != data.end() && overrides->is_object()) {
      for (auto it = overrides->begin(); it != overrides->end(); ++it) {
        if (it.value().is_string())
          spec.path_overrides[it.key()] = it.value().get<std::string>();
      }
    }
    return spec;
  }

  Manager::Manager(ApplyCallback apply) : apply_(std::move(apply)) { }

  Manager::~Manager() {
    shutting_down_ = true;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      for (Slot& slot : slots_)
        slot.request = next_request_++;
    }
    pool_.removeAllJobs(true, 30000);
    cancelPendingUpdate();
  }

  void Manager::setStatus(int slot, SlotStatus::State state, const std::string& phase, float progress,
                          const std::string& error) {
    SlotStatus& status = slots_[static_cast<size_t>(slot)].status;
    status.state = state;
    status.phase = phase;
    status.progress = progress;
    status.error = error;
    if (state != SlotStatus::kLoaded)
      status.notice.clear();
    status.revision++;
  }

  void Manager::start(Job job) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      job.request = next_request_++;
      Slot& slot = slots_[static_cast<size_t>(job.slot)];
      slot.request = job.request;
      slot.has_pending = false;
      slot.pending = nullptr;
      setStatus(job.slot, SlotStatus::kLoading, "Loading", 0.0f);
    }
    running_++;
    pool_.addJob([this, job] {
      run(job);
      running_--;
    });
  }

  SfzParser::Options Manager::cacheOptions(const InstrumentSpec& spec, const juce::File& entry) const {
    SfzParser::Options options;
    const juce::File cache = container::cacheFolder(spec.hash);
    const juce::File working = container::workingFolder(spec.hash);
    const juce::String entry_path(spec.entry);
    options.search_roots = { working.getChildFile(entry_path).getParentDirectory(),
                             cache.getChildFile(entry_path).getParentDirectory() };
    for (const auto& path_override : spec.path_overrides)
      options.path_overrides[path_override.first] = cache.getChildFile(juce::String(path_override.second));
    (void)entry;
    return options;
  }

  void Manager::run(Job job) {
    const int slot = job.slot;
    const uint64_t request = job.request;
    auto progress = [this, slot, request](float amount, const std::string& phase) {
      std::lock_guard<std::mutex> lock(mutex_);
      Slot& state = slots_[static_cast<size_t>(slot)];
      if (state.request != request)
        return false;
      if (state.status.phase != phase || amount - state.status.progress >= 0.01f || amount < state.status.progress) {
        state.status.phase = phase;
        state.status.progress = amount;
        state.status.revision++;
      }
      return true;
    };

    auto fail = [&](const std::string& error) {
      auto result = std::make_shared<LoadResult>();
      result->error = error;
      finish(slot, request, result, {}, {}, {}, job.from_cache);
    };

    if (job.container != juce::File()) {
      std::string error;
      const bool extracted = container::extractToCache(job.container, job.spec.hash, job.spec.files,
                                                       [&progress](float amount) {
                                                         return progress(amount, "Unpacking samples");
                                                       }, error);
      if (!extracted) {
        if (!error.empty())
          fail(error);
        return;
      }
    }

    std::string notice;
    if (job.from_cache) {
      container::touch(job.spec.hash);
      const juce::File working = container::workingFolder(job.spec.hash).getChildFile(juce::String(job.spec.entry));
      const juce::File cached = container::cacheFolder(job.spec.hash).getChildFile(juce::String(job.spec.entry));
      job.load.file = working.existsAsFile() ? working : cached;
      if (isTextInstrument(job.load.file))
        job.load.sfz_options = cacheOptions(job.spec, job.load.file);
    }

    std::shared_ptr<LoadResult> result = loadInstrument(job.load, progress);
    if (result == nullptr)
      return;

    if (job.check_hash && result->error.empty() && !job.spec.hash.empty() && result->hash != job.spec.hash) {
      if (container::isCached(job.spec.hash)) {
        // The file changed on disk: play the copy saved with the project.
        Job cached = job;
        cached.check_hash = false;
        cached.populate_cache = false;
        cached.from_cache = true;
        cached.load.previous = nullptr;
        container::touch(job.spec.hash);
        const juce::File entry = container::cacheFolder(job.spec.hash).getChildFile(juce::String(job.spec.entry));
        cached.load.file = entry;
        if (isTextInstrument(entry))
          cached.load.sfz_options = cacheOptions(job.spec, entry);
        std::shared_ptr<LoadResult> cached_result = loadInstrument(cached.load, progress);
        if (cached_result == nullptr)
          return;
        finish(slot, request, cached_result,
               "The instrument file changed since this was saved; playing the saved copy.",
               cached.load.file, cached.load.sfz_options, true);
        return;
      }
      notice = "The instrument file changed since this was saved.";
    }

    finish(slot, request, result, notice, job.load.file, job.load.sfz_options, job.from_cache);

    if (result->error.empty() && job.populate_cache && !job.from_cache) {
      if (shutting_down_)
        return;
      container::populateCache(result->hash, result->files, kMaxCacheCopyBytes,
                               [this] { return shutting_down_.load(); });
      if (!shutting_down_)
        container::pruneCache(kCacheBudgetBytes, hashesInUse());
    }
  }

  void Manager::finish(int slot, uint64_t request, std::shared_ptr<const LoadResult> result, const std::string& notice,
                       const juce::File& loaded_file, const SfzParser::Options& options, bool from_cache) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      Slot& state = slots_[static_cast<size_t>(slot)];
      if (state.request != request)
        return;
      state.pending = result;
      state.has_pending = true;
      state.status.notice = notice;
      if (result && result->error.empty()) {
        state.loaded_file = loaded_file;
        state.sfz_options = options;
        state.from_cache = from_cache;
      }
    }
    triggerAsyncUpdate();
  }

  void Manager::handleAsyncUpdate() {
    processPendingResults();
  }

  void Manager::applyNow(int slot, std::shared_ptr<const LoadResult> result) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      slots_[static_cast<size_t>(slot)].current = result;
    }
    if (apply_)
      apply_(slot, result ? result->instrument : nullptr);
  }

  void Manager::processPendingResults(bool blocking) {
    std::unique_lock<std::mutex> apply_lock(apply_mutex_, std::defer_lock);
    if (blocking)
      apply_lock.lock();
    else if (!apply_lock.try_lock())
      return;
    for (int slot = 0; slot < kNumSlots; ++slot) {
      std::shared_ptr<const LoadResult> result;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        Slot& state = slots_[static_cast<size_t>(slot)];
        if (!state.has_pending)
          continue;
        result = state.pending;
        state.pending = nullptr;
        state.has_pending = false;

        if (result == nullptr || !result->error.empty() || result->instrument == nullptr) {
          const std::string error = result && !result->error.empty() ? result->error : "The instrument did not load.";
          setStatus(slot, SlotStatus::kFailed, "Failed", 0.0f, error);
          // The previous instrument keeps playing when there was one.
          if (state.current) {
            state.spec = state.applied_spec;
            continue;
          }
          result = nullptr;
        }
        else {
          InstrumentSpec& spec = state.spec;
          spec.format = result->instrument->format;
          spec.name = result->instrument->name;
          spec.hash = result->hash;
          spec.entry = result->entry;
          if (!state.from_cache)
            spec.path = state.loaded_file.getFullPathName().toStdString();
          if (spec.format == "sf2")
            describePreset(spec, *result->instrument);
          spec.files.clear();
          spec.path_overrides.clear();
          state.applied_spec = spec;
          const std::string notice = state.status.notice;
          setStatus(slot, SlotStatus::kLoaded, "Loaded", 1.0f);
          state.status.notice = notice;
        }
      }
      applyNow(slot, result);
    }
  }

  void Manager::loadFromUser(int slot, const juce::File& file) {
    if (slot < 0 || slot >= kNumSlots)
      return;
    Job job;
    job.slot = slot;
    job.load.file = file;
    job.populate_cache = true;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      Slot& state = slots_[static_cast<size_t>(slot)];
      if (state.current && !state.from_cache && state.loaded_file == file)
        job.load.previous = state.current;
      state.spec = InstrumentSpec();
      state.spec.format = instrumentFormat(file);
      state.spec.name = file.getFileNameWithoutExtension().toStdString();
      state.spec.path = file.getFullPathName().toStdString();
    }
    start(job);
  }

  void Manager::selectSf2Preset(int slot, int preset_index) {
    if (slot < 0 || slot >= kNumSlots)
      return;
    std::shared_ptr<const LoadResult> current;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      current = slots_[static_cast<size_t>(slot)].current;
    }
    if (current == nullptr || current->sf2 == nullptr)
      return;
    std::shared_ptr<const LoadResult> result = withSf2Preset(*current, preset_index);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      Slot& state = slots_[static_cast<size_t>(slot)];
      // This cancels a load in progress (or finished but not yet applied),
      // whose spec names the other file.
      state.request = next_request_++;
      state.has_pending = false;
      state.pending = nullptr;
      state.spec = state.applied_spec;
      describePreset(state.spec, *result->instrument);
      state.applied_spec = state.spec;
      setStatus(slot, SlotStatus::kLoaded, "Loaded", 1.0f);
    }
    applyNow(slot, result);
  }

  void Manager::reload(int slot) {
    if (slot < 0 || slot >= kNumSlots)
      return;
    Job job;
    job.slot = slot;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      Slot& state = slots_[static_cast<size_t>(slot)];
      if (state.loaded_file == juce::File())
        return;
      job.load.file = state.loaded_file;
      job.load.sfz_options = state.sfz_options;
      job.load.previous = state.current;
      if (state.current && state.current->sf2) {
        describePreset(state.spec, *state.current->instrument);
        job.load.sf2_index = state.spec.sf2_index;
        job.load.sf2_bank = state.spec.sf2_bank;
        job.load.sf2_program = state.spec.sf2_program;
        job.load.sf2_name = state.spec.sf2_name;
        job.load.previous = nullptr;
      }
      job.populate_cache = true;
    }
    start(job);
  }

  void Manager::clear(int slot) {
    if (slot < 0 || slot >= kNumSlots)
      return;
    bool had_instrument = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      Slot& state = slots_[static_cast<size_t>(slot)];
      state.request = next_request_++;
      state.has_pending = false;
      state.pending = nullptr;
      had_instrument = state.current != nullptr || !state.spec.empty();
      state.spec = InstrumentSpec();
      state.applied_spec = InstrumentSpec();
      state.loaded_file = juce::File();
      state.from_cache = false;
      if (had_instrument || state.status.state != SlotStatus::kEmpty)
        setStatus(slot, SlotStatus::kEmpty, "", 0.0f);
    }
    if (had_instrument)
      applyNow(slot, nullptr);
  }

  void Manager::clearAll() {
    for (int slot = 0; slot < kNumSlots; ++slot)
      clear(slot);
  }

  bool Manager::shareLoaded(int slot, const InstrumentSpec& spec) {
    if (spec.hash.empty())
      return false;
    std::shared_ptr<const LoadResult> found;
    juce::File loaded_file;
    SfzParser::Options options;
    bool from_cache = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      for (int other = 0; other < kNumSlots; ++other) {
        const Slot& state = slots_[static_cast<size_t>(other)];
        if (state.current && state.current->hash == spec.hash && state.current->instrument) {
          found = state.current;
          loaded_file = state.loaded_file;
          options = state.sfz_options;
          from_cache = state.from_cache;
          if (other == slot)
            break;
        }
      }
    }
    if (found == nullptr)
      return false;

    if (found->sf2) {
      const int preset = found->sf2->findPreset(spec.sf2_bank, spec.sf2_program, spec.sf2_name, spec.sf2_index);
      if (preset != found->instrument->preset_index)
        found = withSf2Preset(*found, preset);
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      Slot& state = slots_[static_cast<size_t>(slot)];
      state.request = next_request_++;
      state.has_pending = false;
      state.spec = spec;
      state.spec.files.clear();
      state.spec.path_overrides.clear();
      if (found->sf2)
        describePreset(state.spec, *found->instrument);
      state.applied_spec = state.spec;
      state.loaded_file = loaded_file;
      state.sfz_options = options;
      state.from_cache = from_cache;
      setStatus(slot, SlotStatus::kLoaded, "Loaded", 1.0f);
    }
    applyNow(slot, found);
    return true;
  }

  void Manager::applySpec(int slot, const InstrumentSpec& spec, const juce::File& container_file) {
    if (slot < 0 || slot >= kNumSlots)
      return;
    if (spec.empty()) {
      clear(slot);
      return;
    }
    if (shareLoaded(slot, spec))
      return;

    Job job;
    job.slot = slot;
    job.spec = spec;
    job.load.sf2_index = spec.sf2_index;
    job.load.sf2_bank = spec.sf2_bank;
    job.load.sf2_program = spec.sf2_program;
    job.load.sf2_name = spec.sf2_name;

    const juce::File original(juce::String(spec.path));
    if (container_file != juce::File() && !spec.hash.empty() && !spec.files.empty() && !spec.entry.empty()) {
      job.container = container_file;
      job.from_cache = true;
    }
    else if (!spec.path.empty() && original.existsAsFile()) {
      job.load.file = original;
      job.check_hash = true;
      job.populate_cache = true;
    }
    else if (container::isCached(spec.hash) && !spec.entry.empty())
      job.from_cache = true;
    else {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        Slot& state = slots_[static_cast<size_t>(slot)];
        state.request = next_request_++;
        state.has_pending = false;
        state.spec = spec;
        state.spec.files.clear();
        state.spec.path_overrides.clear();
        state.loaded_file = juce::File();
        setStatus(slot, SlotStatus::kFailed, "Failed", 0.0f,
                  "Instrument not found: " + (spec.path.empty() ? spec.name : spec.path));
      }
      applyNow(slot, nullptr);
      return;
    }

    {
      std::lock_guard<std::mutex> lock(mutex_);
      Slot& state = slots_[static_cast<size_t>(slot)];
      state.spec = spec;
      state.spec.files.clear();
      state.spec.path_overrides.clear();
      // A different instrument's sound must not linger while this one loads.
      state.current = nullptr;
    }
    if (apply_)
      apply_(slot, nullptr);
    start(job);
  }

  InstrumentSpec Manager::spec(int slot) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return slot >= 0 && slot < kNumSlots ? slots_[static_cast<size_t>(slot)].spec : InstrumentSpec();
  }

  std::shared_ptr<const LoadResult> Manager::result(int slot) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return slot >= 0 && slot < kNumSlots ? slots_[static_cast<size_t>(slot)].current : nullptr;
  }

  SlotStatus Manager::status(int slot) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return slot >= 0 && slot < kNumSlots ? slots_[static_cast<size_t>(slot)].status : SlotStatus();
  }

  juce::File Manager::editableFile(int slot) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (slot < 0 || slot >= kNumSlots)
      return {};
    Slot& state = slots_[static_cast<size_t>(slot)];
    if (state.current == nullptr || state.spec.format == "sf2" || state.loaded_file == juce::File())
      return {};
    const juce::File working_root = container::cacheRoot().getChildFile("Working");
    if (!state.from_cache || state.loaded_file.isAChildOf(working_root))
      return state.loaded_file;

    // A cached instrument is never edited in place: copy its text out.
    const std::string& hash = state.current->hash;
    const juce::File working = container::workingFolder(hash);
    for (const FileInfo& info : state.current->files) {
      if (!info.text)
        continue;
      const juce::File target = working.getChildFile(juce::String(info.relative));
      if (!target.existsAsFile()) {
        target.getParentDirectory().createDirectory();
        info.file.copyFileTo(target);
      }
    }
    const juce::File entry = working.getChildFile(juce::String(state.current->entry));
    const juce::File cache_entry = container::cacheFolder(hash).getChildFile(juce::String(state.current->entry));
    SfzParser::Options options = state.sfz_options;
    options.search_roots = { entry.getParentDirectory(), cache_entry.getParentDirectory() };
    state.sfz_options = options;
    state.loaded_file = entry;
    return entry;
  }

  bool Manager::hasPendingLoads() const {
    if (running_ > 0)
      return true;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const Slot& slot : slots_) {
      if (slot.has_pending)
        return true;
    }
    return false;
  }

  bool Manager::waitForPendingLoads(int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (running_ > 0) {
      if (std::chrono::steady_clock::now() > deadline)
        return false;
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
  }

  std::set<std::string> Manager::hashesInUse() const {
    std::set<std::string> hashes;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const Slot& slot : slots_) {
      if (slot.current)
        hashes.insert(slot.current->hash);
      if (!slot.spec.hash.empty())
        hashes.insert(slot.spec.hash);
    }
    return hashes;
  }
} // namespace multisample
} // namespace vital
