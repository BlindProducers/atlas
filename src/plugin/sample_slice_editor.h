// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "sequence_keyboard_component.h"
#include <vector>

// The Sample type's Slicer mode: the slices of a slot's file, reviewed and
// edited from the keyboard, each step spoken. Uses the LFO editor's focusable
// group, paint and accessibility handler. Positions are in samples of the
// file; the editor that owns it formats them in the Show as units.
class SampleSliceEditor : public LfoMsegKeyboardComponent {
  public:
    struct State { int cursor = 0; };
    explicit SampleSliceEditor(State& state);

    std::function<std::vector<int>()> readSlices;                 // where each slice starts
    // Replaces the slices; `description` names the step as undo says it and
    // `mergeKey` joins repeats of the same edit into one step.
    std::function<void(const std::vector<int>&, const String& description, const String& mergeKey)> writeSlices;
    std::function<int()> fileLength;                               // samples
    std::function<int()> sampleRate;
    std::function<String(int)> positionText;                       // a position or length, in the Show as units
    std::function<void(const String&)> announce;
    std::function<void(int, int, int)> audition;                   // plays slice `index`, from `start` to `end`
    std::function<void()> onShowMenu;                              // the context menu (Show as)

    bool handleShortcut(const KeyPress& key);
    std::unique_ptr<AccessibilityHandler> createAccessibilityHandler() override;
    String statusText() const;
    static String helpText();

  private:
    std::vector<int> slices() const;
    int cursor(const std::vector<int>& starts) const;
    int endOf(const std::vector<int>& starts, int index) const;
    String sliceText(const std::vector<int>& starts, int index) const;
    void say(const String& message);
    void move(int direction);
    void moveTo(int index);
    void nudge(float seconds);
    void split();
    void remove();
    void randomize();
    void play();
    void write(const std::vector<int>& starts, const String& description, const String& mergeKey = {});

    State& state_;
};
