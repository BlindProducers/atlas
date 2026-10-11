// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "sequence_keyboard_component.h"
#include "trance_gate_sequence.h"
#include <vector>

// Uses the LFO's same focusable group, paint and accessibility handler. Each FX
// instance owns its current step and shape preset.
class TranceGateEditor : public LfoMsegKeyboardComponent {
  public:
    using Step = vital::trance_gate::Step;
    using Changes = std::vector<std::pair<String, float>>;
    struct State { int cursor = 0, shape = 0; };
    explicit TranceGateEditor(State& state);
    std::function<float(const String&)> read;
    std::function<void(const Changes&)> write;
    std::function<void(const String&)> announce;
    bool handleShortcut(const KeyPress& key);
    String statusText() const;
    void refresh();
    static String helpText();
  private:
    Step step(int index) const;
    void appendStep(Changes& changes, int index, Step value) const;
    void apply(Changes changes, const String& message);
    void say(const String& message);
    void move(int direction);
    void changeValue(double delta);
    void changeCurve(double delta);
    void changeMode(int direction);
    void clear(bool all);
    State& state_;
};
