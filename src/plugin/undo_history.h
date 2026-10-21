// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "JuceHeader.h"
#include "nlohmann/json.hpp"

#include <atomic>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

class SynthPlugin;

namespace vital {
  class Value;
}

// The patch's undo history, behind Command Z (Ctrl Z on Windows) in the
// editor: one step for each thing the user does there, as Serum keeps it. A
// value moved, a preset loaded, the patch initialized, an effect added, a
// route made, an LFO or a wavetable edited, the patch saved as the default:
// each is undone and redone as a whole.
//
// A step keeps only what changed. Controls are kept as their values before
// and after. The rest of the saved state is kept in parts (each LFO, each
// wavetable, each route, each sample, the preset's name...) as the text they
// are saved as. Undoing a step that changed only controls sets them back, as
// the editor would; a step that changed one of those parts loads the patch
// again with them put back, as a preset load does. Saving the patch as the
// default keeps the file it replaced, so that is undone too.
//
// Only the user's changes in the editor are steps: a control changed inside
// one of its gestures, and the changes the editor marks with
// SynthPlugin::markStateChanged. The host's automation and MIDI learn move
// the starting point instead, and a state the host restores clears the
// history. Quick changes to the same control join one step, as one drag of a
// knob does, and so do presets loaded one after another while the preset
// browser autoloads them.
//
// Message thread only, except absorbExternalChange and clear.
class UndoHistory : private AsyncUpdater, private Timer {
  public:
    using json = nlohmann::json;

    // Changes to the same control join one step while no more than this
    // passes between them: a held key, or the same key pressed again.
    static constexpr double kMergeWindowMs = 2000.0;
    // Changes in one part of the state (an LFO point moved, a harmonic
    // changed) become a step once they have stopped for this long, so a held
    // key makes one step and one snapshot. Other changes are a step at once.
    static constexpr int kStructureSettleMs = 600;
    static constexpr size_t kMaxSteps = 200;
    // Roughly what the history may hold, counting each part it keeps.
    static constexpr size_t kMaxBytes = 64 * 1024 * 1024;

    // What an undo or redo did, for the editor to say and to show.
    struct Outcome {
      bool applied = false;
      // What the step did: "load preset Bass", "add Reverb", or a control's
      // name for a step that only moved values.
      String description;
      // A step that only moved values: the controls the user changed, in the
      // order they were changed. Empty for one that loaded the patch again.
      std::vector<std::string> controls;
      bool reloaded = false;          // the patch was loaded again
      bool effects_changed = false;   // an effect was added, removed or moved
      bool names_changed = false;     // the preset's name, file or details, or a macro's name
      bool preset_name_changed = false;
      bool file_changed = false;      // a file was written back (the default patch)
      // Why nothing was done, when there was a step.
      String failure;
    };

    explicit UndoHistory(SynthPlugin& synth);
    ~UndoHistory() override;

    // ---- Recording
    // An editor gesture on a control starts or ends. Changes inside one are the user's.
    void beginUserEdit();
    void endUserEdit();
    // True on the message thread inside an editor gesture, while no undo is applied.
    bool isRecordingUserEdit() const;
    // The user is about to change control `name`; its value has not moved yet.
    void noteValueEdit(const std::string& name);
    // State outside the controls has changed. `block` names the part,
    // "settings/lfos/2" or "settings/modulations" (see captureBlocks), so
    // recording it costs little; empty means any part may have changed.
    void noteStructureEdit(const std::string& block = {});
    // Names the step the user's next action makes, as undo says it ("load
    // preset Bass"). Call it before the action changes anything. Actions with
    // the same non-empty `mergeKey` in a row make one step that goes back to
    // where the first began (presets autoloaded while browsing).
    void describeNextStep(const String& description, const String& mergeKey = {});
    // The user's action wrote `file` (Save patch as default): its bytes before,
    // none when it did not exist, and after.
    void recordFileChange(const File& file, const String& description, std::optional<MemoryBlock> before,
                          MemoryBlock after);
    // The host, its automation or MIDI learn changed control `name`: the
    // history starts from its new value. Any thread.
    void absorbExternalChange(const std::string& name);
    // Records the state the next step starts from, once (the editor opened).
    void ensureBaseline();
    // Starts from the current state without making a step: after a save,
    // whose new name and file are not the user's to undo.
    void rebaseline();
    // Forgets every step (the host restored a state of its own). Any thread.
    void clear();
    // Makes whatever is waiting its step now.
    void commitPendingEdits();

    // ---- Undo and redo
    bool canUndo();
    bool canRedo();
    String undoDescription();
    String redoDescription();
    Outcome undo();
    Outcome redo();
    bool isApplying() const { return applying_; }
    size_t numUndoSteps() const { return undo_.size(); }
    // A change is waiting to become a step (message thread). The descriptions
    // above make it one at once, which would split a held key's changes into
    // several steps, so the editor's toolbar reads them only when this is false.
    bool hasPendingEdits() const { return pending_; }
    size_t numRedoSteps() const { return redo_.size(); }

  private:
    using Text = std::shared_ptr<const std::string>;
    using Blocks = std::map<std::string, Text>;

    struct ControlChange {
      int index = 0;
      float before = 0.0f;
      float after = 0.0f;
    };

    // A part of the saved state; null where it was absent.
    struct BlockChange {
      std::string key;
      Text before;
      Text after;
    };

    struct FileChange {
      File file;
      std::optional<MemoryBlock> before;
      MemoryBlock after;
    };

    struct Step {
      String description;
      String merge_key;
      std::vector<std::string> touched;
      std::vector<ControlChange> controls;
      std::vector<BlockChange> blocks;
      std::shared_ptr<const FileChange> file;
      double time_ms = 0.0;
      size_t bytes = 0;
    };

    void handleAsyncUpdate() override;
    void timerCallback() override;

    int indexOf(const std::string& name) const;
    void markPending();
    bool hasPendingStructure() const { return pending_all_ || !pending_scopes_.empty(); }
    void resetPending();
    void processResetRequest();
    void clearNow();
    void commit();
    void snapshotControls();
    // The saved state's parts outside the controls, keyed "preset_name",
    // "macro3", "settings/lfos/2", "settings/osc_1_sample", ..., and
    // "atlas/..." for what the plugin keeps beside the patch (its preset
    // file, host automation slots, tuning).
    Blocks captureBlocks();
    // Only the parts in `scopes`. Empty, or naming a part that cannot be read
    // alone, reads all of them and leaves `scopes` empty.
    Blocks captureBlocks(std::set<std::string>& scopes);
    bool capturePart(const std::string& scope, Blocks& blocks);
    void captureEverything(Blocks& blocks);
    void diffBlocks(const std::set<std::string>& scopes, Blocks current, std::vector<BlockChange>& changes);
    void pushStep(Step step);
    static bool canMerge(const Step& top, const Step& step);
    static void mergeInto(Step& top, Step step);
    static size_t stepBytes(const Step& step);
    void trim();
    String describe(const Step& step) const;
    Outcome apply(const Step& step, bool undoing);
    bool applyFile(const FileChange& change, bool undoing, String& failure);
    void applyLightBlock(const std::string& key, const Text& text);

    SynthPlugin& synth_;
    // Every control, sorted by name as the control map is.
    std::vector<std::string> names_;
    std::vector<vital::Value*> values_;
    std::vector<bool> volatile_;
    // The controls' values where the next step starts.
    std::unique_ptr<std::atomic<float>[]> baseline_;
    Blocks baseline_blocks_;
    std::atomic<bool> baseline_valid_ { false };
    std::atomic<bool> reset_requested_ { false };

    std::deque<Step> undo_;
    std::deque<Step> redo_;
    // The newest step may take in the next change (no undo, redo or save since).
    bool top_mergeable_ = false;

    int user_edit_depth_ = 0;
    bool applying_ = false;

    // The step being recorded.
    bool pending_ = false;
    // Its changes came in the message being handled now: one action.
    bool awaiting_async_ = false;
    bool pending_all_ = false;
    std::set<std::string> pending_scopes_;
    std::vector<std::string> pending_touched_;
    String pending_description_;
    String pending_merge_key_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(UndoHistory)
};
