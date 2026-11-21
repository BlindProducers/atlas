// SPDX-License-Identifier: GPL-3.0-or-later
#include "sample_slice_editor.h"

namespace {
  constexpr float kNudgeSeconds = 0.01f;
  constexpr float kFineNudgeSeconds = 0.001f;
  constexpr float kFinePitchStep = 0.1f;   // a tenth of a semitone
  constexpr float kGainStep = 1.0f;        // dB
  constexpr float kFineGainStep = 0.1f;

  bool isZero(float value) { return std::abs(value) < 0.005f; }

  // "+2", "-1", "+0.3", "0".
  String signedText(float value) {
    if (isZero(value))
      return "0";
    const bool whole = std::abs(value - std::round(value)) < 0.005f;
    const String number = whole ? String(roundToInt(std::abs(value))) : String(std::abs(value), 1);
    return (value < 0.0f ? "-" : "+") + number;
  }

  // "+2 semitones", "-1 semitone", "+0.3 semitones", "0".
  String pitchText(float semitones) {
    if (isZero(semitones))
      return "0";
    const bool one = std::abs(std::abs(semitones) - 1.0f) < 0.005f;
    return signedText(semitones) + (one ? " semitone" : " semitones");
  }

  String gainText(float decibels) { return signedText(decibels) + " dB"; }
}

SampleSliceEditor::SampleSliceEditor(State& state) : state_(state) {
  setTitle("Slices editor");
  setDescription("Keyboard slice editor: moves between the slices, moves their starts, sets their pitch, gain and direction, splits and removes them, and plays them.");
  setHelpText(helpText());
  getStatusText = [this] { return statusText(); };
  onKeyPressed = [this](const KeyPress& key) { return handleShortcut(key); };
}

String SampleSliceEditor::helpText() {
  return "Up and Down move between slices, Home and End to the first and last. Left and Right move the slice's start "
         "by 10 ms, with Shift by 1 ms. Comma and Period lower and raise the slice's pitch by a semitone, with Shift "
         "by a tenth. Page Down and Page Up lower and raise its gain by 1 dB, with Shift by a tenth. V turns it "
         "around. 0 puts its pitch, gain and direction back. Return splits the slice in the middle. Backspace or "
         "Delete removes it. R moves every start to a random place. Space plays it.";
}

std::unique_ptr<AccessibilityHandler> SampleSliceEditor::createAccessibilityHandler() {
  class Handler final : public AccessibilityHandler {
    public:
      explicit Handler(SampleSliceEditor& component) :
          AccessibilityHandler(component, AccessibilityRole::group,
                               AccessibilityActions().addAction(AccessibilityActionType::showMenu, [&component] {
                                 if (component.onShowMenu)
                                   component.onShowMenu();
                               })),
          component_(component) { }

      AccessibleState getCurrentState() const override {
        return AccessibilityHandler::getCurrentState().withAccessibleOffscreen();
      }

      String getDescription() const override { return component_.statusText(); }

    private:
      SampleSliceEditor& component_;
  };
  return std::make_unique<Handler>(*this);
}

void SampleSliceEditor::write(const std::vector<Slice>& slices, const String& description, const String& mergeKey) {
  if (writeSlices)
    writeSlices(slices, description, mergeKey);
}

std::vector<SampleSliceEditor::Slice> SampleSliceEditor::slices() const {
  return readSlices ? readSlices() : std::vector<Slice>();
}

int SampleSliceEditor::cursor(const std::vector<Slice>& slices) const {
  return jlimit(0, jmax(0, static_cast<int>(slices.size()) - 1), state_.cursor);
}

int SampleSliceEditor::endOf(const std::vector<Slice>& slices, int index) const {
  if (index + 1 < static_cast<int>(slices.size()))
    return slices[static_cast<size_t>(index + 1)].start;
  return fileLength ? fileLength() : 0;
}

String SampleSliceEditor::sliceText(const std::vector<Slice>& slices, int index) const {
  const Slice& slice = slices[static_cast<size_t>(index)];
  const int end = endOf(slices, index);
  const auto text = [this](int samples) { return positionText ? positionText(samples) : String(samples); };
  String result = "Slice " + String(index + 1) + " of " + String(static_cast<int>(slices.size())) + ", start " +
                  text(slice.start) + ", end " + text(end) + ", length " + text(end - slice.start);
  if (!isZero(slice.pitch))
    result += ", pitch " + pitchText(slice.pitch);
  if (!isZero(slice.gain))
    result += ", gain " + gainText(slice.gain);
  if (slice.reverse)
    result += ", reversed";
  return result;
}

String SampleSliceEditor::statusText() const {
  const std::vector<Slice> all = slices();
  if (all.empty())
    return "No slices";
  return sliceText(all, cursor(all));
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
  if (code == KeyPress::pageDownKey) { changeGain(shift ? -kFineGainStep : -kGainStep); return true; }
  if (code == KeyPress::pageUpKey) { changeGain(shift ? kFineGainStep : kGainStep); return true; }
  if (code == KeyPress::returnKey) { split(); return true; }
  if (code == KeyPress::backspaceKey || code == KeyPress::deleteKey) { remove(); return true; }
  if (code == KeyPress::spaceKey) { play(); return true; }
  const juce_wchar character = key.getTextCharacter();
  if (!shift && (code == 'R' || code == 'r' || character == 'r' || character == 'R')) { randomize(); return true; }
  if (!shift && (code == 'V' || code == 'v' || character == 'v' || character == 'V')) { toggleReverse(); return true; }
  if (character == ',' || character == '<') { changePitch(shift ? -kFinePitchStep : -1.0f); return true; }
  if (character == '.' || character == '>') { changePitch(shift ? kFinePitchStep : 1.0f); return true; }
  if (character == '0' || code == '0') { resetSlice(); return true; }
  return false;
}

void SampleSliceEditor::move(int direction) {
  moveTo(cursor(slices()) + direction);
}

void SampleSliceEditor::moveTo(int index) {
  const std::vector<Slice> all = slices();
  if (all.empty()) {
    say("No slices");
    return;
  }
  state_.cursor = jlimit(0, static_cast<int>(all.size()) - 1, index);
  say(sliceText(all, state_.cursor));
}

// Moves the slice's start, keeping it at least a slice's shortest length from
// its neighbours. Slice 1 always starts at the beginning.
void SampleSliceEditor::nudge(float seconds) {
  std::vector<Slice> all = slices();
  if (all.empty())
    return;
  const int index = cursor(all);
  if (index == 0) {
    say("Slice 1 starts at the beginning");
    return;
  }
  const int rate = sampleRate ? sampleRate() : 0;
  const int step = jmax(1, roundToInt(std::abs(seconds) * rate));
  const int lowest = all[static_cast<size_t>(index - 1)].start + vital::Sample::kMinSliceLength;
  const int highest = endOf(all, index) - vital::Sample::kMinSliceLength;
  int& start = all[static_cast<size_t>(index)].start;
  const int moved = jlimit(lowest, jmax(lowest, highest), start + (seconds < 0.0f ? -step : step));
  if (moved == start) {
    say(moved == lowest ? "At the previous slice" : "At the next slice");
    return;
  }
  start = moved;
  write(all, "move slice " + String(index + 1), "slice-start:" + String(index));
  say("Start " + (positionText ? positionText(start) : String(start)));
}

// Splits the slice in the middle; both halves keep its pitch, gain and direction.
void SampleSliceEditor::split() {
  std::vector<Slice> all = slices();
  if (all.empty() || static_cast<int>(all.size()) >= vital::Sample::kMaxSlices) {
    say(all.empty() ? "No slices" : "No room for another slice");
    return;
  }
  const int index = cursor(all);
  const int start = all[static_cast<size_t>(index)].start;
  const int end = endOf(all, index);
  if (end - start < 2 * vital::Sample::kMinSliceLength) {
    say("Slice too short to split");
    return;
  }
  Slice second = all[static_cast<size_t>(index)];
  second.start = start + (end - start) / 2;
  all.insert(all.begin() + index + 1, second);
  state_.cursor = index;
  write(all, "split slice " + String(index + 1));
  say("Split, " + String(static_cast<int>(all.size())) + " slices");
}

// Removes the slice: the one before it takes its part, or slice 1 takes
// slice 2's when it is slice 1.
void SampleSliceEditor::remove() {
  std::vector<Slice> all = slices();
  if (all.size() < 2) {
    say("The last slice stays");
    return;
  }
  const int index = cursor(all);
  all.erase(all.begin() + jmax(1, index));
  state_.cursor = jlimit(0, static_cast<int>(all.size()) - 1, index == 0 ? 0 : index - 1);
  write(all, "remove slice " + String(index + 1));
  say("Removed, " + String(static_cast<int>(all.size())) + (all.size() == 1 ? " slice" : " slices"));
}

// Moves every start but the first to a random place between the slice before
// it (as moved) and the one after it (as it was), so the slices keep their
// order and their number.
void SampleSliceEditor::randomize() {
  std::vector<Slice> all = slices();
  if (all.size() < 2) {
    say("Nothing to randomize");
    return;
  }
  const int length = fileLength ? fileLength() : 0;
  Random& random = Random::getSystemRandom();
  for (size_t i = 1; i < all.size(); ++i) {
    const int lowest = all[i - 1].start + vital::Sample::kMinSliceLength;
    const int highest = (i + 1 < all.size() ? all[i + 1].start : length) - vital::Sample::kMinSliceLength;
    if (highest > lowest)
      all[i].start = lowest + random.nextInt(highest - lowest + 1);
  }
  write(all, "randomize slices");
  say("Randomized, " + String(static_cast<int>(all.size())) + " slices");
}

void SampleSliceEditor::changePitch(float semitones) {
  std::vector<Slice> all = slices();
  if (all.empty())
    return;
  const int index = cursor(all);
  float& pitch = all[static_cast<size_t>(index)].pitch;
  const float moved = jlimit(-vital::Sample::kMaxSlicePitch, vital::Sample::kMaxSlicePitch,
                             std::round((pitch + semitones) * 100.0f) / 100.0f);
  if (moved == pitch) {
    say("Pitch " + pitchText(pitch));
    return;
  }
  pitch = moved;
  write(all, "pitch slice " + String(index + 1), "slice-pitch:" + String(index));
  say("Pitch " + pitchText(pitch));
}

void SampleSliceEditor::changeGain(float decibels) {
  std::vector<Slice> all = slices();
  if (all.empty())
    return;
  const int index = cursor(all);
  float& gain = all[static_cast<size_t>(index)].gain;
  const float moved = jlimit(vital::Sample::kMinSliceGain, vital::Sample::kMaxSliceGain,
                             std::round((gain + decibels) * 100.0f) / 100.0f);
  if (moved == gain) {
    say("Gain " + gainText(gain));
    return;
  }
  gain = moved;
  write(all, "gain of slice " + String(index + 1), "slice-gain:" + String(index));
  say("Gain " + gainText(gain));
}

void SampleSliceEditor::toggleReverse() {
  std::vector<Slice> all = slices();
  if (all.empty())
    return;
  const int index = cursor(all);
  bool& reverse = all[static_cast<size_t>(index)].reverse;
  reverse = !reverse;
  write(all, (reverse ? "reverse slice " : "turn slice ") + String(index + 1));
  say(reverse ? "Reversed" : "Forward");
}

// Puts the slice's pitch, gain and direction back.
void SampleSliceEditor::resetSlice() {
  std::vector<Slice> all = slices();
  if (all.empty())
    return;
  const int index = cursor(all);
  Slice& slice = all[static_cast<size_t>(index)];
  if (isZero(slice.pitch) && isZero(slice.gain) && !slice.reverse) {
    say("Slice as it is");
    return;
  }
  slice.pitch = 0.0f;
  slice.gain = 0.0f;
  slice.reverse = false;
  write(all, "reset slice " + String(index + 1));
  say("Pitch 0, gain 0 dB, forward");
}

void SampleSliceEditor::play() {
  const std::vector<Slice> all = slices();
  if (all.empty() || !audition)
    return;
  const int index = cursor(all);
  audition(index, all[static_cast<size_t>(index)].start, endOf(all, index));
}
