// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "JuceHeader.h"
#include "multisample_loader.h"
#include "nlohmann/json.hpp"

#include <array>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace vital {
namespace multisample {

  // Which instrument an oscillator slot plays, as presets and host state store it.
  struct InstrumentSpec {
    std::string format;
    std::string name;
    // The entry file where the user loaded it from.
    std::string path;
    std::string hash;
    // The entry file relative to the instrument's root folder.
    std::string entry;
    int sf2_index = -1;
    int sf2_bank = -1;
    int sf2_program = -1;
    std::string sf2_name;
    // Present in container presets: every file, and SFZ paths outside the
    // library mapped to their place under "_external/".
    std::vector<FileInfo> files;
    std::map<std::string, std::string> path_overrides;
    // DecentSampler control values the user chose, by Control::index.
    std::map<int, double> control_values;

    bool empty() const { return path.empty() && hash.empty(); }
    nlohmann::json toJson(bool with_files) const;
    static InstrumentSpec fromJson(const nlohmann::json& data);
  };

  struct SlotStatus {
    enum State { kEmpty, kLoading, kLoaded, kFailed };
    State state = kEmpty;
    float progress = 0.0f;
    std::string phase;
    std::string error;
    // Notes for the user after a load (file changed on disk, played from the cache).
    std::string notice;
    // Increases on every change, for the editor to notice.
    uint32_t revision = 0;
  };

  // Loads SFZ/SF2 instruments for the oscillator slots on a background thread
  // and hands finished instruments to the engine on the message thread.
  class Manager : private juce::AsyncUpdater {
    public:
      static constexpr int kNumSlots = 8;
      // Message thread (or the audio thread offline): give a slot its instrument.
      // `keep_sounding`: the same instrument with other control values, so
      // notes already playing go on with the previous one.
      using ApplyCallback = std::function<void(int slot, std::shared_ptr<const Instrument> instrument,
                                               bool keep_sounding)>;

      explicit Manager(ApplyCallback apply);
      ~Manager() override;

      // The user chose a file.
      void loadFromUser(int slot, const juce::File& file);
      // Switches an SF2 slot to another preset of the same file (instant).
      void selectSf2Preset(int slot, int preset_index);
      // Parses the SFZ text again (after editing), reusing decoded audio.
      void reload(int slot);
      // Sets one of the instrument's controls and rebuilds its regions in the
      // background, reusing the decoded audio. The status stays as it is.
      void setControlValue(int slot, int index, double value);
      void clear(int slot);
      void clearAll();
      // A preset or host state names this slot's instrument. `container` is
      // the preset file when it holds the instrument's files.
      void applySpec(int slot, const InstrumentSpec& spec, const juce::File& container);

      InstrumentSpec spec(int slot) const;
      std::shared_ptr<const LoadResult> result(int slot) const;
      SlotStatus status(int slot) const;
      // The SFZ file to open in a text editor; a cached instrument gets an
      // editable working copy of its text first.
      juce::File editableFile(int slot);

      // Loads only: copying a loaded instrument into the cache and pruning the
      // cache run afterwards on their own thread, and nothing waits for them.
      bool hasPendingLoads() const;
      // Tests and offline rendering: wait for loads, then apply them here.
      // The audio thread passes `blocking` false: if the message thread is
      // applying a result at that moment it waits for the audio lock this
      // thread holds, so this thread must not wait for it in turn.
      bool waitForPendingLoads(int timeout_ms);
      void processPendingResults(bool blocking = true);
      // Tests: waits for the cache copy and pruning as well.
      bool waitForCacheWork(int timeout_ms);

      // Every cached instrument a slot uses (kept by cache pruning).
      std::set<std::string> hashesInUse() const;

    private:
      struct Slot {
        InstrumentSpec spec;
        // The spec of the instrument playing now (a failed load returns to it).
        InstrumentSpec applied_spec;
        std::shared_ptr<const LoadResult> current;
        std::shared_ptr<const LoadResult> pending;
        bool has_pending = false;
        // The pending result only changes control values.
        bool pending_rebuild = false;
        uint64_t request = 0;
        SlotStatus status;
        // Where the loaded copy lives, for reloads.
        juce::File loaded_file;
        SfzParser::Options sfz_options;
        bool from_cache = false;
      };

      struct Job {
        int slot = 0;
        uint64_t request = 0;
        LoadRequest load;
        juce::File container;
        InstrumentSpec spec;
        bool from_cache = false;
        bool check_hash = false;
        bool populate_cache = false;
        // A control changed: parse the same file again, quietly.
        bool rebuild = false;
      };

      void handleAsyncUpdate() override;
      void start(Job job);
      void run(Job job);
      void finish(int slot, uint64_t request, std::shared_ptr<const LoadResult> result, const std::string& notice,
                  const juce::File& loaded_file, const SfzParser::Options& options, bool from_cache,
                  bool rebuild = false);
      bool shareLoaded(int slot, const InstrumentSpec& spec);
      void applyNow(int slot, std::shared_ptr<const LoadResult> result, bool keep_sounding = false);
      // Rebuilds a slot whose control values differ from what it plays.
      void rebuildControls(int slot);
      void setStatus(int slot, SlotStatus::State state, const std::string& phase, float progress,
                     const std::string& error = {});
      SfzParser::Options cacheOptions(const InstrumentSpec& spec, const juce::File& entry) const;

      ApplyCallback apply_;
      mutable std::mutex mutex_;
      std::array<Slot, kNumSlots> slots_;
      std::atomic<uint64_t> next_request_ { 1 };
      std::atomic<int> loading_ { 0 };
      std::atomic<int> caching_ { 0 };
      std::atomic<bool> shutting_down_ { false };
      juce::ThreadPool pool_ { 1 };
      // The cache copy of a loaded instrument (up to 2 GB from another drive)
      // must not hold up the next load.
      juce::ThreadPool cache_pool_ { 1 };
      std::mutex apply_mutex_;

      JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Manager)
  };
} // namespace multisample
} // namespace vital
