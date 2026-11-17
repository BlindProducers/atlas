// SPDX-License-Identifier: GPL-3.0-or-later
#include "sample_slice_editor.h"
#include "sample_source.h"

namespace {
  constexpr float kNudgeSeconds = 0.01f;
  constexpr float kFineNudgeSeconds = 0.001f;
}

SampleSliceEditor::SampleSliceEditor(State& state) : state_(state) {
  setTitle("Slices");
  setDescription("Keyboard slice editor: moves between the slices, moves their starts, splits and removes them, and plays them.");
  setHelpText(helpText());
  getStatusText = [this] { return statusText(); };
  onKeyPressed = [this](const KeyPress& key) { return handleShortcut(key); };
}

String SampleSliceEditor::helpText() {
  return "Up and Down move between slices, Home and End to the first and last. Left and Right move the slice's start "
         "by 10 ms, with Shift by 1 ms. Return splits the slice in the middle. Backspace or Delete removes it. "
         "Space plays it.";
}

std::vector<int> SampleSliceEditor::slices() const {
  return readSlices ? readSlices() : std::vector<int>();
}

int SampleSliceEditor::cursor(const std::vector<int>& starts) const {
  return jlimit(0, jmax(0, static_cast<int>(starts.size()) - 1), state_.cursor);
}

int SampleSliceEditor::endOf(const std::vector<int>& starts, int index) const {
  if (index + 1 < static_cast<int>(starts.size()))
    return starts[static_cast<size_t>(index + 1)];
  return fileLength ? fileLength() : 0;
}

String SampleSliceEditor::sliceText(const std::vector<int>& starts, int index) const {
  const int start = starts[static_cast<size_t>(index)];
  const int end = endOf(starts, index);
  const auto text = [this](int samples) { return positionText ? positionText(samples) : String(samples); };
  return "Slice " + String(index + 1) + " of " + String(static_cast<int>(starts.size())) + ", start " + text(start) +
         ", end " + text(end) + ", length " + text(end - start);
}

String SampleSliceEditor::statusText() const {
  const std::vector<int> starts = slices();
  if (starts.empty())
    return "No slices";
  return sliceText(starts, cursor(starts));
}

void SampleSliceEditor::say(const String& message) {
  repaint();
  if (announce)
    announce(message);
}

bool SampleSliceEditor::handleShortcut(const KeyPress& key) {
  const int code = key.getKeyCode();
  const bool shift = key.getModifiers().isShiftDown();
  if (key.getModifiers().isCommandDown() || key.getModifiers().isCtrlDown() || key.getModifiers().isAltDown())
    return false;
  if (code == KeyPress::upKey) { move(-1); return true; }
  if (code == KeyPress::downKey) { move(1); return true; }
  if (code == KeyPress::homeKey) { moveTo(0); return true; }
  if (code == KeyPress::endKey) { moveTo(static_cast<int>(slices().size()) - 1); return true; }
  if (code == KeyPress::leftKey) { nudge(shift ? -kFineNudgeSeconds : -kNudgeSeconds); return true; }
  if (code == KeyPress::rightKey) { nudge(shift ? kFineNudgeSeconds : kNudgeSeconds); return true; }
  if (code == KeyPress::returnKey) { split(); return true; }
  if (code == KeyPress::backspaceKey || code == KeyPress::deleteKey) { remove(); return true; }
  if (code == KeyPress::spaceKey) { play(); return true; }
  return false;
}

void SampleSliceEditor::move(int direction) {
  const std::vector<int> starts = slices();
  moveTo(cursor(starts) + direction);
}

void SampleSliceEditor::moveTo(int index) {
  const std::vector<int> starts = slices();
  if (starts.empty()) {
    say("No slices");
    return;
  }
  state_.cursor = jlimit(0, static_cast<int>(starts.size()) - 1, index);
  say(sliceText(starts, state_.cursor));
}

// Moves the slice's start, keeping it at least a slice's shortest length from
// its neighbours. Slice 1 always starts at the beginning.
void SampleSliceEditor::nudge(float seconds) {
  std::vector<int> starts = slices();
  if (starts.empty())
    return;
  const int index = cursor(starts);
  if (index == 0) {
    say("Slice 1 starts at the beginning");
    return;
  }
  const int rate = sampleRate ? sampleRate() : 0;
  const int step = jmax(1, roundToInt(std::abs(seconds) * rate));
  const int lowest = starts[static_cast<size_t>(index - 1)] + vital::Sample::kMinSliceLength;
  const int highest = endOf(starts, index) - vital::Sample::kMinSliceLength;
  int& start = starts[static_cast<size_t>(index)];
  const int moved = jlimit(lowest, jmax(lowest, highest), start + (seconds < 0.0f ? -step : step));
  if (moved == start) {
    say(moved == lowest ? "At the previous slice" : "At the next slice");
    return;
  }
  start = moved;
  if (writeSlices)
    writeSlices(starts);
  say("Start " + (positionText ? positionText(start) : String(start)));
}

void SampleSliceEditor::split() {
  std::vector<int> starts = slices();
  if (starts.empty() || static_cast<int>(starts.size()) >= vital::Sample::kMaxSlices) {
    say(starts.empty() ? "No slices" : "No room for another slice");
    return;
  }
  const int index = cursor(starts);
  const int start = starts[static_cast<size_t>(index)];
  const int end = endOf(starts, index);
  if (end - start < 2 * vital::Sample::kMinSliceLength) {
    say("Slice too short to split");
    return;
  }
  starts.insert(starts.begin() + index + 1, start + (end - start) / 2);
  state_.cursor = index;
  if (writeSlices)
    writeSlices(starts);
  say("Split, " + String(static_cast<int>(starts.size())) + " slices");
}

// Removes the slice: the one before it takes its part, or slice 1 takes
// slice 2's when it is slice 1.
void SampleSliceEditor::remove() {
  std::vector<int> starts = slices();
  if (starts.size() < 2) {
    say("The last slice stays");
    return;
  }
  const int index = cursor(starts);
  starts.erase(starts.begin() + jmax(1, index));
  state_.cursor = jlimit(0, static_cast<int>(starts.size()) - 1, index == 0 ? 0 : index - 1);
  if (writeSlices)
    writeSlices(starts);
  say("Removed, " + String(static_cast<int>(starts.size())) + (starts.size() == 1 ? " slice" : " slices"));
}

void SampleSliceEditor::play() {
  const std::vector<int> starts = slices();
  if (starts.empty() || !audition)
    return;
  const int index = cursor(starts);
  audition(index, starts[static_cast<size_t>(index)], endOf(starts, index));
}
