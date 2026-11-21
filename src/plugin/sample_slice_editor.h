// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "sequence_keyboard_component.h"
#include "sample_source.h"
#include <vector>

// The Sample type's Slicer mode: the slices of a slot's file, reviewed and
// edited from the keyboard, each step spoken. Uses the LFO editor's focusable
// group, paint and accessibility handler. Positions are in samples of the
// file; the editor that owns it formats them in the Show as units.
class SampleSliceEditor : public LfoMsegKeyboardComponent {
  public:
    using Slice = vital::Sample::Slice;
    struct State { int cursor = 0; };
    explicit SampleSliceEditor(State& state);

    std::function<std::vector<Slice>()> readSlices;
    // Replaces the slices; `description` names the step as undo says it and
    // `mergeKey` joins repeats of the same edit into one step.
    std::function<void(const std::vector<Slice>&, const String& description, const String& mergeKey)> writeSlices;
    std::function<int()> fileLength;                               // samples
    std::function<int()> sampleRate;
    std::function<String(int)> positionText;                       // a position or length, in the Show as units
    std::function<void(const String&)> announce;
    std::function<void(int, int, int)> audition;                   // plays slice `index`, from `start` to `end`
    std::function<void()> onShowMenu;                              // the context menu (Show as)

    bool handleShortcut(const KeyPress& key);
    String statusText() const;
    static String helpText();
    std::unique_ptr<AccessibilityHandler> createAccessibilityHandler() override;

  private:
    std::vector<Slice> slices() const;
    int cursor(const std::vector<Slice>& slices) const;
    int endOf(const std::vector<Slice>& slices, int index) const;
    String sliceText(const std::vector<Slice>& slices, int index) const;
    void say(const String& message);
    void move(int direction);
    void moveTo(int index);
    void nudge(float seconds);
    void split();
    void remove();
    void randomize();
    void changePitch(float semitones);
    void changeGain(float decibels);
    void toggleReverse();
    void resetSlice();
    void play();
    void write(const std::vector<Slice>& slices, const String& description, const String& mergeKey = {});

    State& state_;
};
