// SPDX-License-Identifier: GPL-3.0-or-later
#include "trance_gate_editor.h"
#include "operators.h"
#include "synth_strings.h"

namespace {
String stepValueText(double value) {
  // Parameter conversion can leave tiny negative values around zero.
  return String(std::abs(value) < 0.005 ? 0.0 : value, 2);
}
String curveText(double curve) { return String(roundToInt(curve * 100.0)) + "%"; }
}

TranceGateEditor::TranceGateEditor(State& state) : state_(state) {
  setTitle("Trance gate Editor");
  setDescription("Keyboard step sequencer with position announcements and concise edit feedback.");
  setHelpText(helpText());
  getStatusText = [this] { return statusText(); };
  onKeyPressed = [this](const KeyPress& key) { return handleShortcut(key); };
}

String TranceGateEditor::helpText() {
  return "Left/Right move between steps. Up/Down or brackets change the value by 0.01; "
         "Shift changes 0.05. Comma/Period decrease/increase the curve by 1%, or 5% with Shift; I/K also adjust it. "
         "G/H choose a step shape, A applies it, and M/Shift M cycle the current step shape forward/backward. "
         "Minus/Equals make the speed coarser/finer. Semicolon/Apostrophe shorten/lengthen the pattern range. "
         "Delete clears the current step to silence. Backspace clears all steps. "
         "Navigation announces position first. Edits announce only the changed value.";
}

TranceGateEditor::Step TranceGateEditor::step(int index) const {
  const auto prefix = "step_" + String(index + 1) + "_";
  return { read(prefix + "value"), jlimit(0, vital::trance_gate::kModes - 1, roundToInt(read(prefix + "mode"))), read(prefix + "curve") };
}

void TranceGateEditor::appendStep(Changes& changes, int index, Step value) const {
  const auto prefix = "step_" + String(index + 1) + "_";
  changes.emplace_back(prefix + "value", static_cast<float>(jlimit(-1.0, 1.0, value.value)));
  changes.emplace_back(prefix + "mode", static_cast<float>(jlimit(0, vital::trance_gate::kModes - 1, value.mode)));
  changes.emplace_back(prefix + "curve", static_cast<float>(jlimit(-1.0, 1.0, value.curve)));
}

String TranceGateEditor::statusText() const {
  if (!read) return "Trance gate editor unavailable";
  const auto value = step(state_.cursor);
  const int grid = jlimit(0, 7, roundToInt(read("grid")));
  const int time = roundToInt(state_.cursor * vital::trance_gate::beatsPerStep(grid) * 100.0);
  return "Bar " + String(time / 400 + 1) + " beat " + String((time % 400) / 100 + 1) + " " +
      String(time % 100) + "%, Step " + String(state_.cursor + 1) + ", value " + stepValueText(value.value) +
      ", " + String(strings::kTranceGateModeNames[value.mode]) + ", curve " + curveText(value.curve);
}

void TranceGateEditor::refresh() {
  // The explicit announcement supplies edit feedback. A valueChanged event on
  // this group would also make screen readers repeat its full description.
  repaint();
}
void TranceGateEditor::say(const String& message) {
  refresh();
  if (announce) announce(message);
}
void TranceGateEditor::apply(Changes changes, const String& message) {
  if (write && !changes.empty()) write(changes);
  say(message);
}
void TranceGateEditor::move(int direction) {
  state_.cursor = jlimit(0, 31, state_.cursor + direction);
  say(statusText());
}
void TranceGateEditor::changeValue(double delta) {
  const double value = jlimit(-1.0, 1.0, std::round((step(state_.cursor).value + delta) * 100.0) / 100.0);
  apply({ { "step_" + String(state_.cursor + 1) + "_value", static_cast<float>(value) } }, stepValueText(value));
}
void TranceGateEditor::changeCurve(double delta) {
  const double curve = jlimit(-1.0, 1.0, std::round((step(state_.cursor).curve + delta) * 100.0) / 100.0);
  apply({ { "step_" + String(state_.cursor + 1) + "_curve", static_cast<float>(curve) } }, curveText(curve));
}
void TranceGateEditor::changeMode(int direction) {
  const int mode = (step(state_.cursor).mode + direction + vital::trance_gate::kModes) % vital::trance_gate::kModes;
  apply({ { "step_" + String(state_.cursor + 1) + "_mode", static_cast<float>(mode) } }, strings::kTranceGateModeNames[mode]);
}
void TranceGateEditor::clear(bool all) {
  Changes changes;
  if (all) {
    for (int i = 0; i < 32; ++i) appendStep(changes, i, { -1.0, 0, 0.0 });
    state_.cursor = 0;
  }
  else appendStep(changes, state_.cursor, { -1.0, 0, 0.0 });
  apply(std::move(changes), all ? "Cleared Trance gate" : "Cleared step");
}

bool TranceGateEditor::handleShortcut(const KeyPress& key) {
  if (!read || !write) return false;
  const auto modifiers = key.getModifiers();
  // Leave host and assistive-technology modifier chords untouched.
  if (modifiers.isCommandDown() || modifiers.isCtrlDown() || modifiers.isAltDown()) return false;
  const auto character = CharacterFunctions::toLowerCase(key.getTextCharacter());
  const bool shift = modifiers.isShiftDown();
  const int code = key.getKeyCode();
  if (!shift && code == KeyPress::leftKey) { move(-1); return true; }
  if (!shift && code == KeyPress::rightKey) { move(1); return true; }
  if (code == KeyPress::upKey || character == ']' || character == '}') { changeValue(shift ? 0.05 : 0.01); return true; }
  if (code == KeyPress::downKey || character == '[' || character == '{') { changeValue(shift ? -0.05 : -0.01); return true; }
  if (code == KeyPress::backspaceKey) { clear(true); return true; }
  if (code == KeyPress::deleteKey) { clear(false); return true; }
  if (character == ',' || character == '<' || character == 'k') {
    changeCurve(shift ? -0.05 : -0.01); return true;
  }
  if (character == '.' || character == '>' || character == 'i') {
    changeCurve(shift ? 0.05 : 0.01); return true;
  }
  if (character == 'g' || character == 'h') {
    state_.shape = (state_.shape + (character == 'h' ? 1 : -1) + vital::trance_gate::kModes) % vital::trance_gate::kModes;
    say(strings::kTranceGateModeNames[state_.shape]); return true;
  }
  if (character == 'a') {
    apply({ { "step_" + String(state_.cursor + 1) + "_mode", static_cast<float>(state_.shape) } },
          strings::kTranceGateModeNames[state_.shape]); return true;
  }
  if (character == 'm') { changeMode(shift ? -1 : 1); return true; }
  if (character == '-' || character == '_' || character == '=' || character == '+') {
    const int delta = character == '-' || character == '_' ? -1 : 1;
    const int grid = jlimit(0, 7, roundToInt(read("grid")) + delta);
    apply({ { "grid", static_cast<float>(grid) } }, strings::kTranceGateGridNames[grid]); return true;
  }
  if (character == ';' || character == '\'') {
    const int start = jlimit(1, 32, roundToInt(read("start")));
    const int stop = jlimit(1, 32, roundToInt(read("stop")));
    const int direction = start <= stop ? 1 : -1;
    const int length = jmax(1, std::abs(stop - start) + 1 + (character == ';' ? -1 : 1));
    const int next_stop = jlimit(1, 32, start + direction * (length - 1));
    apply({ { "stop", static_cast<float>(next_stop) } }, String(start) + " to " + String(next_stop)); return true;
  }
  return false;
}
