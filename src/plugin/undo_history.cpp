// SPDX-License-Identifier: GPL-3.0-or-later
#include "undo_history.h"

#include "line_generator.h"
#include "load_save.h"
#include "modulation_connection_processor.h"
#include "synth_parameters.h"
#include "synth_plugin.h"
#include "tuning.h"
#include "wavetable_creator.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

namespace {
  using PartText = std::shared_ptr<const std::string>;

  bool onMessageThread() {
    return MessageManager::existsAndIsCurrentThread();
  }

  bool startsWith(const std::string& text, const std::string& prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
  }

  bool endsWith(const std::string& text, const std::string& suffix) {
    return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
  }

  // `key` is the part `scope` names, or inside it ("settings/lfos/2" is in "settings/lfos").
  bool inScope(const std::string& key, const std::string& scope) {
    return key == scope || (key.size() > scope.size() && startsWith(key, scope) && key[scope.size()] == '/');
  }

  bool sameValue(float a, float b) {
    return a == b || (std::isnan(a) && std::isnan(b));
  }

  bool sameText(const PartText& a, const PartText& b) {
    if (a == nullptr || b == nullptr)
      return a == b;
    return a == b || *a == *b;
  }

  // A part as it is saved. Text from an old preset that is not UTF-8 is
  // converted as saving converts it, so the snapshot never fails.
  PartText textOf(const nlohmann::json& value) {
    try {
      return std::make_shared<const std::string>(value.dump());
    }
    catch (const nlohmann::json::exception&) {
      nlohmann::json copy = value;
      return std::make_shared<const std::string>(LoadSave::dumpState(copy));
    }
  }

  // The host's tempo and the wheels move on their own; no step records them.
  bool isVolatileControl(const std::string& name) {
    return name == "beats_per_minute" || name == "mod_wheel" || name == "pitch_wheel";
  }

  // An effect slot's effect or a chain's order: the editor rebuilds that chain's rows.
  bool isEffectStructureControl(const std::string& name) {
    return SynthBase::isEffectSlotTypeControl(name) || name.find("effect_chain_") != std::string::npos ||
           endsWith(name, "post_effect_order");
  }

  // Controls that rebuild part of the engine when they change. The editor
  // pauses processing around them, and so does undo; a step that changes one
  // also records every part of the state, since building the new oscillator
  // type can drop routes into the old one's controls.
  bool isEngineStructureControl(const std::string& name) {
    return SynthBase::isSourceTypeControl(name) || isEffectStructureControl(name);
  }

  // "settings/lfos/3" after "settings/lfos/" is 3.
  bool indexAfter(const std::string& key, const std::string& prefix, int& index) {
    if (!startsWith(key, prefix) || key.size() == prefix.size())
      return false;
    for (size_t i = prefix.size(); i < key.size(); ++i) {
      if (!std::isdigit(static_cast<unsigned char>(key[i])))
        return false;
    }
    index = std::atoi(key.c_str() + prefix.size());
    return true;
  }

  // "macro3" is macro 2; -1 for any other key.
  int macroIndexForKey(const std::string& key) {
    int index = -1;
    if (!indexAfter(key, "macro", index) || index < 1 || index > vital::kNumMacros)
      return -1;
    return index - 1;
  }

  bool isMetadataKey(const std::string& key) {
    return key == "preset_name" || key == "author" || key == "comments" || key == "preset_style" ||
           key == "preset_tags" || macroIndexForKey(key) >= 0;
  }

  // Parts the patch takes back without loading again: its name and details,
  // the macros' names, and what the plugin keeps beside the patch.
  bool isLightBlock(const std::string& key) {
    return isMetadataKey(key) || startsWith(key, "atlas/");
  }

  String stringIn(const PartText& text) {
    if (text == nullptr)
      return {};
    const nlohmann::json value = nlohmann::json::parse(*text, nullptr, false);
    return value.is_string() ? String::fromUTF8(value.get_ref<const std::string&>().c_str()) : String();
  }

  // Puts a part of the saved state back into `state`, or takes it out where
  // it was absent.
  void setBlock(nlohmann::json& state, const std::string& key, const PartText& text) {
    static const std::string kSettings = "settings/";
    if (!startsWith(key, kSettings)) {
      if (text != nullptr)
        state[key] = nlohmann::json::parse(*text);
      else
        state.erase(key);
      return;
    }

    nlohmann::json& settings = state["settings"];
    const std::string rest = key.substr(kSettings.size());
    const size_t slash = rest.find('/');
    if (slash == std::string::npos) {
      if (text != nullptr)
        settings[rest] = nlohmann::json::parse(*text);
      else
        settings.erase(rest);
      return;
    }

    const std::string container = rest.substr(0, slash);
    const std::string part = rest.substr(slash + 1);
    if (container == "lfos" || container == "wavetables" || container == "modulations") {
      nlohmann::json& array = settings[container];
      int index = -1;
      if (array.is_array() && indexAfter(part, "", index) && index < static_cast<int>(array.size()) && text != nullptr)
        array[static_cast<size_t>(index)] = nlohmann::json::parse(*text);
      return;
    }

    nlohmann::json& object = settings[container];
    if (!object.is_object())
      object = nlohmann::json::object();
    if (text != nullptr)
      object[part] = nlohmann::json::parse(*text);
    else {
      object.erase(part);
      if (object.empty())
        settings.erase(container);
    }
  }
}

UndoHistory::UndoHistory(SynthPlugin& synth) : synth_(synth) {
  const vital::control_map& controls = synth_.getControls();
  names_.reserve(controls.size());
  values_.reserve(controls.size());
  volatile_.reserve(controls.size());
  for (const auto& control : controls) {
    if (control.second == nullptr)
      continue;
    names_.push_back(control.first);
    values_.push_back(control.second);
    volatile_.push_back(isVolatileControl(control.first));
  }
  baseline_ = std::make_unique<std::atomic<float>[]>(names_.size());
}

UndoHistory::~UndoHistory() {
  stopTimer();
  cancelPendingUpdate();
}

int UndoHistory::indexOf(const std::string& name) const {
  const auto found = std::lower_bound(names_.begin(), names_.end(), name);
  if (found == names_.end() || *found != name)
    return -1;
  return static_cast<int>(found - names_.begin());
}

void UndoHistory::beginUserEdit() {
  if (onMessageThread())
    ++user_edit_depth_;
}

void UndoHistory::endUserEdit() {
  if (onMessageThread() && user_edit_depth_ > 0)
    --user_edit_depth_;
}

bool UndoHistory::isRecordingUserEdit() const {
  // The thread first: the depth belongs to the message thread.
  return onMessageThread() && user_edit_depth_ > 0 && !applying_;
}

void UndoHistory::markPending() {
  pending_ = true;
  awaiting_async_ = true;
  triggerAsyncUpdate();
}

void UndoHistory::resetPending() {
  pending_ = false;
  awaiting_async_ = false;
  pending_all_ = false;
  pending_scopes_.clear();
  pending_touched_.clear();
  pending_description_.clear();
  pending_merge_key_.clear();
}

void UndoHistory::noteValueEdit(const std::string& name) {
  if (!onMessageThread() || applying_)
    return;
  processResetRequest();
  ensureBaseline();
  // A change outside the controls that an earlier action made is still
  // settling: it is a step of its own, recorded before this value moves.
  if (pending_ && !awaiting_async_ && hasPendingStructure())
    commit();
  if (std::find(pending_touched_.begin(), pending_touched_.end(), name) == pending_touched_.end())
    pending_touched_.push_back(name);
  markPending();
}

void UndoHistory::noteStructureEdit(const std::string& block) {
  if (!onMessageThread() || applying_)
    return;
  processResetRequest();
  if (!baseline_valid_.load(std::memory_order_acquire)) {
    // Too late for this change, which is already made: it is where the history starts.
    ensureBaseline();
    return;
  }
  // An earlier action's change in another part settles first, so each is its
  // own step. A change in the same part continues it (a held key).
  if (pending_ && !awaiting_async_ && !pending_all_ && !pending_scopes_.empty() && !block.empty()) {
    bool continues = false;
    for (const auto& scope : pending_scopes_)
      continues = continues || inScope(block, scope) || inScope(scope, block);
    if (!continues)
      commit();
  }
  if (block.empty())
    pending_all_ = true;
  else
    pending_scopes_.insert(block);
  markPending();
}

void UndoHistory::describeNextStep(const String& description, const String& mergeKey) {
  if (!onMessageThread() || applying_)
    return;
  processResetRequest();
  ensureBaseline();
  // Nothing of this action has changed yet: what an earlier one left waiting is its own step.
  if (pending_ && !awaiting_async_)
    commit();
  if (pending_description_.isEmpty() || mergeKey.isNotEmpty()) {
    pending_description_ = description;
    pending_merge_key_ = mergeKey;
  }
  // Recorded at the end of this message even if the action changes nothing,
  // so the name never passes to a later step.
  markPending();
}

void UndoHistory::recordFileChange(const File& file, const String& description, std::optional<MemoryBlock> before,
                                   MemoryBlock after) {
  if (!onMessageThread() || applying_)
    return;
  processResetRequest();
  commit();
  Step step;
  step.description = description;
  step.file = std::make_shared<const FileChange>(FileChange { file, std::move(before), std::move(after) });
  step.time_ms = Time::getMillisecondCounterHiRes();
  step.bytes = stepBytes(step);
  redo_.clear();
  undo_.push_back(std::move(step));
  top_mergeable_ = false;
  trim();
}

void UndoHistory::absorbExternalChange(const std::string& name) {
  if (!baseline_valid_.load(std::memory_order_acquire))
    return;
  const int index = indexOf(name);
  if (index >= 0)
    baseline_[static_cast<size_t>(index)].store(values_[static_cast<size_t>(index)]->value(),
                                                std::memory_order_relaxed);
}

void UndoHistory::snapshotControls() {
  for (size_t i = 0; i < values_.size(); ++i)
    baseline_[i].store(values_[i]->value(), std::memory_order_relaxed);
}

void UndoHistory::ensureBaseline() {
  if (!onMessageThread())
    return;
  processResetRequest();
  if (baseline_valid_.load(std::memory_order_acquire))
    return;
  snapshotControls();
  baseline_blocks_ = captureBlocks();
  baseline_valid_.store(true, std::memory_order_release);
}

void UndoHistory::rebaseline() {
  if (!onMessageThread() || applying_)
    return;
  processResetRequest();
  if (!baseline_valid_.load(std::memory_order_acquire)) {
    ensureBaseline();
    return;
  }
  commit();
  snapshotControls();
  baseline_blocks_ = captureBlocks();
  top_mergeable_ = false;
}

void UndoHistory::clear() {
  // Absorbed changes stop at once; the rest waits for the message thread.
  baseline_valid_.store(false, std::memory_order_release);
  if (!onMessageThread()) {
    reset_requested_.store(true);
    triggerAsyncUpdate();
    return;
  }
  reset_requested_.store(false);
  clearNow();
}

void UndoHistory::processResetRequest() {
  if (reset_requested_.exchange(false))
    clearNow();
}

void UndoHistory::clearNow() {
  stopTimer();
  cancelPendingUpdate();
  undo_.clear();
  redo_.clear();
  resetPending();
  baseline_blocks_.clear();
  baseline_valid_.store(false, std::memory_order_release);
  top_mergeable_ = false;
}

void UndoHistory::commitPendingEdits() {
  if (!onMessageThread())
    return;
  processResetRequest();
  commit();
}

void UndoHistory::handleAsyncUpdate() {
  processResetRequest();
  awaiting_async_ = false;
  if (!pending_)
    return;
  // Changes in one part of the state wait to settle (a held key); the rest
  // are a step at once, so the next action never joins them.
  if (!pending_all_ && !pending_scopes_.empty())
    startTimer(kStructureSettleMs);
  else
    commit();
}

void UndoHistory::timerCallback() {
  commit();
}

void UndoHistory::commit() {
  stopTimer();
  cancelPendingUpdate();
  awaiting_async_ = false;
  if (!pending_)
    return;
  if (!baseline_valid_.load(std::memory_order_acquire)) {
    resetPending();
    ensureBaseline();
    return;
  }

  Step step;
  bool engine_structure = false;
  for (size_t i = 0; i < values_.size(); ++i) {
    if (volatile_[i])
      continue;
    const float current = values_[i]->value();
    const float before = baseline_[i].load(std::memory_order_relaxed);
    if (sameValue(current, before))
      continue;
    step.controls.push_back({ static_cast<int>(i), before, current });
    baseline_[i].store(current, std::memory_order_relaxed);
    engine_structure = engine_structure || isEngineStructureControl(names_[i]);
  }

  if (pending_all_ || engine_structure || !pending_scopes_.empty()) {
    std::set<std::string> scopes = pending_all_ || engine_structure ? std::set<std::string>() : pending_scopes_;
    Blocks current = captureBlocks(scopes);
    diffBlocks(scopes, std::move(current), step.blocks);
  }

  if (step.controls.empty() && step.blocks.empty()) {
    resetPending();
    return;
  }

  step.description = pending_description_;
  step.merge_key = pending_merge_key_;
  step.touched = pending_touched_;
  // Quick changes to the same controls join one step, as one drag of a knob.
  // An action the editor named (adding an effect) stays a step of its own.
  if (step.merge_key.isEmpty() && step.description.isEmpty() && step.blocks.empty() && !step.touched.empty()) {
    StringArray touched;
    for (const auto& name : step.touched)
      touched.add(String(name));
    step.merge_key = "value:" + touched.joinIntoString(",");
  }
  step.time_ms = Time::getMillisecondCounterHiRes();
  resetPending();
  pushStep(std::move(step));
}

UndoHistory::Blocks UndoHistory::captureBlocks() {
  Blocks blocks;
  captureEverything(blocks);
  return blocks;
}

UndoHistory::Blocks UndoHistory::captureBlocks(std::set<std::string>& scopes) {
  Blocks blocks;
  for (const auto& scope : scopes) {
    if (!capturePart(scope, blocks)) {
      // A part it cannot read alone: everything, and so compare everything.
      scopes.clear();
      break;
    }
  }
  if (scopes.empty()) {
    blocks.clear();
    captureEverything(blocks);
  }
  return blocks;
}

bool UndoHistory::capturePart(const std::string& scope, Blocks& blocks) {
  // Each part as LoadSave::stateToJson writes it, so a part read alone
  // matches the same part read with everything else.
  int index = -1;
  if (indexAfter(scope, "settings/lfos/", index)) {
    if (index < vital::kNumLfos) {
      if (LineGenerator* lfo = synth_.getLfoSource(index))
        blocks[scope] = textOf(lfo->stateToJson());
    }
    return true;
  }
  if (indexAfter(scope, "settings/wavetables/", index)) {
    if (index < vital::kNumOscillators && synth_.getWavetableCreator(0) != nullptr)
      blocks[scope] = textOf(synth_.getWavetableCreator(index)->stateToJson());
    return true;
  }
  if (scope == "settings/modulations") {
    vital::ModulationConnectionBank& bank = synth_.getModulationBank();
    for (int i = 0; i < vital::kMaxModulationConnections; ++i) {
      vital::ModulationConnection* connection = bank.atIndex(i);
      json modulation;
      modulation["source"] = connection->source_name;
      modulation["destination"] = connection->destination_name;
      LineGenerator* line_mapping = connection->modulation_processor->lineMapGenerator();
      if (!line_mapping->linear())
        modulation["line_mapping"] = line_mapping->stateToJson();
      blocks[scope + "/" + std::to_string(i)] = textOf(modulation);
    }
    return true;
  }
  if (scope == "settings/filter_tables") {
    for (const std::string& prefix : synth_.filterTablePrefixes()) {
      if (WavetableCreator* creator = synth_.getFilterTableCreator(prefix, false))
        blocks[scope + "/" + prefix] = textOf(creator->stateToJson());
    }
    return true;
  }
  if (scope == "preset_name") {
    blocks[scope] = textOf(synth_.getPresetName().toStdString());
    return true;
  }
  if (scope == "author") {
    blocks[scope] = textOf(synth_.getAuthor().toStdString());
    return true;
  }
  if (scope == "comments") {
    blocks[scope] = textOf(synth_.getComments().toStdString());
    return true;
  }
  if (scope == "preset_style") {
    blocks[scope] = textOf(synth_.getStyle().toStdString());
    return true;
  }
  if (scope == "preset_tags") {
    blocks[scope] = textOf(synth_.getTags().toStdString());
    return true;
  }
  if (const int macro = macroIndexForKey(scope); macro >= 0) {
    blocks[scope] = textOf(synth_.getMacroName(macro).toStdString());
    return true;
  }
  // Beside the patch: the preset file Save writes over, the host automation
  // slots and the tuning.
  if (scope == "atlas/active_file") {
    const File file = synth_.getActiveFile();
    if (file != File())
      blocks[scope] = textOf(file.getFullPathName().toStdString());
    return true;
  }
  if (scope == "atlas/fx_automation") {
    blocks[scope] = textOf(synth_.fxAutomationAssignmentsToJson());
    return true;
  }
  if (scope == "atlas/tuning") {
    if (const Tuning* tuning = synth_.getTuning())
      blocks[scope] = textOf(tuning->stateToJson());
    return true;
  }
  return false;
}

void UndoHistory::captureEverything(Blocks& blocks) {
  json state = synth_.saveToJson();
  for (auto it = state.begin(); it != state.end(); ++it) {
    const std::string& key = it.key();
    if (key == "settings" || key == "synth_version" || key == "engine_behavior_version")
      continue;
    blocks[key] = textOf(it.value());
  }

  const auto settings = state.find("settings");
  if (settings != state.end() && settings->is_object()) {
    for (auto it = settings->begin(); it != settings->end(); ++it) {
      const std::string& key = it.key();
      const json& value = it.value();
      // The controls are kept as values.
      if (value.is_number() || value.is_boolean())
        continue;
      if ((key == "lfos" || key == "wavetables" || key == "modulations") && value.is_array()) {
        for (size_t i = 0; i < value.size(); ++i)
          blocks["settings/" + key + "/" + std::to_string(i)] = textOf(value[i]);
      }
      else if ((key == "filter_tables" || key == "convolution_irs") && value.is_object()) {
        for (auto part = value.begin(); part != value.end(); ++part)
          blocks["settings/" + key + "/" + part.key()] = textOf(part.value());
      }
      else {
        blocks["settings/" + key] = textOf(value);
      }
    }
  }

  capturePart("atlas/active_file", blocks);
  capturePart("atlas/fx_automation", blocks);
  capturePart("atlas/tuning", blocks);
}

void UndoHistory::diffBlocks(const std::set<std::string>& scopes, Blocks current, std::vector<BlockChange>& changes) {
  const auto in_scope = [&scopes](const std::string& key) {
    if (scopes.empty())
      return true;
    for (const auto& scope : scopes) {
      if (inScope(key, scope))
        return true;
    }
    return false;
  };

  for (auto it = baseline_blocks_.begin(); it != baseline_blocks_.end();) {
    if (!in_scope(it->first)) {
      ++it;
      continue;
    }
    const auto found = current.find(it->first);
    if (found == current.end()) {
      changes.push_back({ it->first, it->second, nullptr });
      it = baseline_blocks_.erase(it);
      continue;
    }
    if (!sameText(found->second, it->second)) {
      changes.push_back({ it->first, it->second, found->second });
      it->second = found->second;
    }
    current.erase(found);
    ++it;
  }

  for (auto& added : current) {
    if (!in_scope(added.first))
      continue;
    changes.push_back({ added.first, nullptr, added.second });
    baseline_blocks_[added.first] = added.second;
  }
}

bool UndoHistory::canMerge(const Step& top, const Step& step) {
  if (top.file != nullptr || step.file != nullptr || top.merge_key.isEmpty() || top.merge_key != step.merge_key)
    return false;
  return !top.merge_key.startsWith("value:") || step.time_ms - top.time_ms <= kMergeWindowMs;
}

void UndoHistory::mergeInto(Step& top, Step step) {
  for (const auto& change : step.controls) {
    const auto found = std::find_if(top.controls.begin(), top.controls.end(),
                                    [&change](const ControlChange& existing) { return existing.index == change.index; });
    if (found == top.controls.end())
      top.controls.push_back(change);
    else
      found->after = change.after;
  }
  top.controls.erase(std::remove_if(top.controls.begin(), top.controls.end(),
                                    [](const ControlChange& change) { return sameValue(change.before, change.after); }),
                     top.controls.end());

  for (auto& block : step.blocks) {
    const auto found = std::find_if(top.blocks.begin(), top.blocks.end(),
                                    [&block](const BlockChange& existing) { return existing.key == block.key; });
    if (found == top.blocks.end())
      top.blocks.push_back(std::move(block));
    else
      found->after = std::move(block.after);
  }
  top.blocks.erase(std::remove_if(top.blocks.begin(), top.blocks.end(),
                                  [](const BlockChange& block) { return sameText(block.before, block.after); }),
                   top.blocks.end());

  for (auto& name : step.touched) {
    if (std::find(top.touched.begin(), top.touched.end(), name) == top.touched.end())
      top.touched.push_back(std::move(name));
  }
  if (step.description.isNotEmpty())
    top.description = step.description;
  top.time_ms = step.time_ms;
  top.bytes = stepBytes(top);
}

size_t UndoHistory::stepBytes(const Step& step) {
  size_t bytes = sizeof(Step) + step.controls.size() * sizeof(ControlChange);
  for (const auto& name : step.touched)
    bytes += name.size();
  for (const auto& block : step.blocks) {
    bytes += block.key.size();
    if (block.before != nullptr)
      bytes += block.before->size();
    if (block.after != nullptr)
      bytes += block.after->size();
  }
  if (step.file != nullptr) {
    bytes += step.file->after.getSize();
    if (step.file->before.has_value())
      bytes += step.file->before->getSize();
  }
  return bytes;
}

void UndoHistory::pushStep(Step step) {
  redo_.clear();
  if (top_mergeable_ && !undo_.empty() && canMerge(undo_.back(), step)) {
    mergeInto(undo_.back(), std::move(step));
    const Step& top = undo_.back();
    // Changed and changed back: nothing is left to undo.
    if (top.controls.empty() && top.blocks.empty() && top.file == nullptr) {
      undo_.pop_back();
      top_mergeable_ = false;
      return;
    }
  }
  else {
    step.bytes = stepBytes(step);
    undo_.push_back(std::move(step));
  }
  top_mergeable_ = true;
  trim();
}

void UndoHistory::trim() {
  size_t total = 0;
  for (const auto& step : undo_)
    total += step.bytes;
  for (const auto& step : redo_)
    total += step.bytes;
  // The oldest steps go first; the newest stays whatever its size.
  while (undo_.size() > 1 && (undo_.size() > kMaxSteps || total > kMaxBytes)) {
    total -= undo_.front().bytes;
    undo_.pop_front();
  }
}

String UndoHistory::describe(const Step& step) const {
  if (step.description.isNotEmpty())
    return step.description;

  if (step.file != nullptr)
    return "file " + step.file->file.getFileNameWithoutExtension();

  if (!step.blocks.empty()) {
    const std::string& key = step.blocks.front().key;
    int index = -1;
    if (indexAfter(key, "settings/lfos/", index))
      return "LFO " + String(index + 1) + " shape";
    if (indexAfter(key, "settings/wavetables/", index))
      return "Oscillator " + String(index + 1) + " wavetable";
    if (startsWith(key, "settings/modulations"))
      return "modulation routing";
    if (startsWith(key, "settings/filter_tables"))
      return "filter table";
    if (startsWith(key, "settings/convolution_irs"))
      return "impulse response";
    if (startsWith(key, "settings/osc_")) {
      const String slot(std::atoi(key.c_str() + std::string("settings/osc_").size()));
      if (endsWith(key, "_multisample"))
        return "Oscillator " + slot + " instrument";
      if (endsWith(key, "_speech_text"))
        return "Oscillator " + slot + " speech text";
      if (endsWith(key, "_sample"))
        return "Oscillator " + slot + " sample";
    }
    if (key == "preset_name")
      return "preset name";
    if (const int macro = macroIndexForKey(key); macro >= 0)
      return "Macro " + String(macro + 1) + " name";
    if (isMetadataKey(key))
      return "preset details";
    if (key == "atlas/tuning")
      return "tuning";
    if (key == "atlas/fx_automation")
      return "automation slots";
    return "patch change";
  }

  const std::string name = !step.touched.empty() ? step.touched.front()
                         : step.controls.empty() ? std::string()
                         : names_[static_cast<size_t>(step.controls.front().index)];
  String text = vital::Parameters::isParameter(name) ? String(vital::Parameters::getDisplayName(name)) : String(name);
  const size_t count = step.touched.empty() ? step.controls.size() : step.touched.size();
  if (count > 1)
    text << " and " << String(static_cast<int>(count - 1)) << (count == 2 ? " other control" : " other controls");
  return text;
}

bool UndoHistory::canUndo() {
  commitPendingEdits();
  return !undo_.empty();
}

bool UndoHistory::canRedo() {
  commitPendingEdits();
  return !redo_.empty();
}

String UndoHistory::undoDescription() {
  commitPendingEdits();
  return undo_.empty() ? String() : describe(undo_.back());
}

String UndoHistory::redoDescription() {
  commitPendingEdits();
  return redo_.empty() ? String() : describe(redo_.back());
}

UndoHistory::Outcome UndoHistory::undo() {
  Outcome outcome;
  if (!onMessageThread())
    return outcome;
  commitPendingEdits();
  if (undo_.empty())
    return outcome;
  Step step = std::move(undo_.back());
  undo_.pop_back();
  top_mergeable_ = false;
  outcome = apply(step, true);
  // A step that could not be applied is dropped: it no longer fits the patch.
  if (outcome.applied)
    redo_.push_back(std::move(step));
  return outcome;
}

UndoHistory::Outcome UndoHistory::redo() {
  Outcome outcome;
  if (!onMessageThread())
    return outcome;
  commitPendingEdits();
  if (redo_.empty())
    return outcome;
  Step step = std::move(redo_.back());
  redo_.pop_back();
  top_mergeable_ = false;
  outcome = apply(step, false);
  if (outcome.applied)
    undo_.push_back(std::move(step));
  return outcome;
}

bool UndoHistory::applyFile(const FileChange& change, bool undoing, String& failure) {
  // The file must still be what the step left (or found), or something else
  // has written it since (another Atlas saving its own default): leave it.
  std::optional<MemoryBlock> current;
  if (change.file.existsAsFile()) {
    MemoryBlock data;
    if (!change.file.loadFileAsData(data)) {
      failure = "Could not read " + change.file.getFileName();
      return false;
    }
    current = std::move(data);
  }
  const MemoryBlock* expected = undoing ? &change.after : (change.before.has_value() ? &*change.before : nullptr);
  const bool unchanged = current.has_value() == (expected != nullptr) && (!current.has_value() || *current == *expected);
  if (!unchanged) {
    failure = change.file.getFileNameWithoutExtension() + " has changed since, so it was left as it is";
    return false;
  }

  const MemoryBlock* target = undoing ? (change.before.has_value() ? &*change.before : nullptr) : &change.after;
  // The file did not exist before the step: undoing takes away the one it made.
  const bool written = target != nullptr ? change.file.replaceWithData(target->getData(), target->getSize())
                                         : change.file.deleteFile();
  if (!written)
    failure = "Could not write " + change.file.getFileName();
  return written;
}

void UndoHistory::applyLightBlock(const std::string& key, const Text& text) {
  if (key == "preset_name")
    synth_.setPresetName(stringIn(text));
  else if (key == "author")
    synth_.setAuthor(stringIn(text));
  else if (key == "comments")
    synth_.setComments(stringIn(text));
  else if (key == "preset_style")
    synth_.setStyle(stringIn(text));
  else if (key == "preset_tags")
    synth_.setTags(stringIn(text));
  else if (const int macro = macroIndexForKey(key); macro >= 0)
    synth_.setMacroName(macro, stringIn(text));
  else if (key == "atlas/active_file") {
    const String path = stringIn(text);
    if (path.isEmpty())
      synth_.clearActiveFile();
    else
      synth_.setActiveFile(File(path));
  }
  else if (key == "atlas/fx_automation") {
    synth_.loadFxAutomationAssignments(text != nullptr ? json::parse(*text, nullptr, false) : json());
    synth_.updateHostDisplay();
  }
  else if (key == "atlas/tuning" && text != nullptr) {
    const json tuning = json::parse(*text, nullptr, false);
    try {
      if (tuning.is_object())
        synth_.getTuning()->jsonToState(tuning);
    }
    catch (const json::exception&) {
      synth_.getTuning()->setDefaultTuning();
    }
  }
}

UndoHistory::Outcome UndoHistory::apply(const Step& step, bool undoing) {
  Outcome outcome;
  outcome.description = describe(step);
  const ScopedValueSetter<bool> applying(applying_, true);

  if (step.file != nullptr) {
    if (!applyFile(*step.file, undoing, outcome.failure))
      return outcome;
    outcome.file_changed = true;
  }

  bool reload = false;
  for (const auto& block : step.blocks) {
    reload = reload || !isLightBlock(block.key);
    outcome.preset_name_changed = outcome.preset_name_changed || block.key == "preset_name";
    outcome.names_changed = outcome.names_changed || isMetadataKey(block.key) || block.key == "atlas/active_file";
  }

  if (reload) {
    // The patch as it is, with the step's parts and values put back, loaded
    // as a preset is.
    json state = synth_.saveToJson();
    json& settings = state["settings"];
    for (const auto& change : step.controls)
      settings[names_[static_cast<size_t>(change.index)]] = undoing ? change.before : change.after;
    std::vector<const BlockChange*> beside;
    for (const auto& block : step.blocks) {
      if (startsWith(block.key, "atlas/"))
        beside.push_back(&block);
      else
        setBlock(state, block.key, undoing ? block.before : block.after);
    }
    bool loaded = false;
    try {
      loaded = synth_.loadFromJson(std::move(state));
    }
    catch (const json::exception&) {
      loaded = false;
    }
    if (loaded) {
      for (const BlockChange* block : beside)
        applyLightBlock(block->key, undoing ? block->before : block->after);
    }
    synth_.updateHostDisplay();
    synth_.notifyHostStateChanged();
    // Start from what the engine made of it.
    snapshotControls();
    baseline_blocks_ = captureBlocks();
    if (!loaded) {
      outcome.failure = "Could not load the patch back";
      return outcome;
    }
    outcome.reloaded = true;
    outcome.applied = true;
    return outcome;
  }

  for (const auto& block : step.blocks) {
    const Text& text = undoing ? block.before : block.after;
    applyLightBlock(block.key, text);
    if (text != nullptr)
      baseline_blocks_[block.key] = text;
    else
      baseline_blocks_.erase(block.key);
  }

  if (!step.controls.empty()) {
    bool engine_structure = false;
    for (const auto& change : step.controls) {
      const std::string& name = names_[static_cast<size_t>(change.index)];
      engine_structure = engine_structure || isEngineStructureControl(name);
      outcome.effects_changed = outcome.effects_changed || isEffectStructureControl(name);
    }
    if (engine_structure)
      synth_.pauseProcessing(true);
    // Twice: a control that resets others as it changes (a Speech slot's
    // Variant resets its Formant and Breathiness) would undo theirs.
    for (int pass = 0; pass < 2; ++pass) {
      for (const auto& change : step.controls) {
        const size_t index = static_cast<size_t>(change.index);
        const float value = undoing ? change.before : change.after;
        if (pass == 1 && sameValue(values_[index]->value(), value))
          continue;
        synth_.applyUndoValue(names_[index], value);
      }
    }
    if (engine_structure)
      synth_.pauseProcessing(false);
    synth_.notifyHostStateChanged();
    snapshotControls();

    if (!step.touched.empty())
      outcome.controls = step.touched;
    else {
      for (const auto& change : step.controls)
        outcome.controls.push_back(names_[static_cast<size_t>(change.index)]);
    }
  }

  outcome.applied = true;
  return outcome;
}
