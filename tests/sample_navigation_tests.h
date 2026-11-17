// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "../src/plugin/synth_editor.h"
#include "slot_test_ids.h"

namespace sample_navigation {
  int failures = 0;
  void check(bool value, const std::string& message) {
    if (!value) { ++failures; std::cerr << "Sample navigation: " << message << '\n'; }
  }

  Component* find(Component& parent, const String& value, bool section = false) {
    if ((section ? parent.getProperties()["SectionName"].toString() : parent.getTitle()) == value)
      return &parent;
    for (auto* child : parent.getChildren())
      if (auto* found = find(*child, value, section)) return found;
    return nullptr;
  }

  ParameterSlider* findSlider(Component& parent, const String& title) {
    if (auto* slider = dynamic_cast<ParameterSlider*>(&parent); slider != nullptr && parent.getTitle() == title)
      return slider;
    for (auto* child : parent.getChildren())
      if (auto* found = findSlider(*child, title)) return found;
    return nullptr;
  }

  vital::Sample* source(SynthPlugin& synth, int target) {
    return synth.getSample(target == 2 ? slot_ids::kSpectralSlot : target == 1 ? slot_ids::kGranularSlot : slot_ids::kSampleSlot);
  }

  bool writeWave(const File& file, int length, int channels) {
    auto stream = file.createOutputStream();
    if (!stream) return false;
    WavAudioFormat format;
    std::unique_ptr<AudioFormatWriter> writer(format.createWriterFor(stream.get(), 48000, channels, 16, {}, 0));
    if (!writer) return false;
    stream.release(); // The writer owns the stream after successful construction.
    AudioSampleBuffer audio(channels, length);
    for (int ch = 0; ch < channels; ++ch)
      for (int i = 0; i < length; ++i)
        audio.setSample(ch, i, .2f * std::sin(i * .09f + ch));
    return writer->writeFromAudioSampleBuffer(audio, 0, length);
  }

  void testRestoredNavigation(const File& root) {
    // The Sample, Granular and Spectral sources are oscillator slots 5, 6 and 7 now.
    const std::array<String, 3> sections { "Oscillator 5", "Oscillator 6", "Oscillator 7" };
    std::array<std::array<File, 3>, 3> files;
    MemoryBlock state;
    {
      SynthPlugin original;
      slot_ids::useLegacySourceTypes(original);
      original.getControls().at("portamento_glide_zones")->set(1);
      std::array<float, 512> audio {};
      for (int i = 0; i < audio.size(); ++i) audio[i] = .2f * std::sin(i * .09f);
      for (int target = 0; target < 3; ++target) {
        auto folder = root.getChildFile(sections[target]);
        check(folder.createDirectory().wasOk(), "fixture folder must be created");
        for (int i = 0; i < 3; ++i) {
          files[target][i] = folder.getChildFile("0" + String(i + 1) + " Kick.wav");
          check(writeWave(files[target][i], 512 + i * 128, target == 0 ? 1 : 2), "fixture WAV must be written");
        }
        folder.getChildFile("Ignore.txt").create();
        folder.getChildFile("Nested").createDirectory();
        check(writeWave(folder.getChildFile("Nested/00 Other.wav"), 512, 1), "nested fixture must be written");
        // Seed the same sample and file context that Load sample supplies, then
        // exercise the real preset restoration and editor Next/Previous actions.
        auto* sample = source(original, target);
        sample->loadSample(audio.data(), audio.size(), 48000);
        sample->setName(files[target][1].getFileNameWithoutExtension().toStdString());
        sample->setLastBrowsedFile(files[target][1].getFullPathName().toStdString());
      }
      original.getStateInformation(state);
    }
    SynthPlugin restored;
    restored.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    SynthEditor editor(restored);
    std::array<File, 3> expected { files[0][1], files[1][1], files[2][1] };
    for (int target = 0; target < 3; ++target) {
      const auto section = sections[target];
      auto* group = find(editor, section + " sample controls", true);
      check(group && group->getTitle() == "Sample controls", section.toStdString() + " needs a sample controls group");
      if (!group) continue;
      int previous_bottom = -1;
      int previous_focus = -1;
      for (const char* title : { "Load sample", "Browse sample folders", "Previous sample", "Next sample" }) {
        auto* button = dynamic_cast<Button*>(find(*group, title));
        check(button && button->getY() >= previous_bottom && button->getExplicitFocusOrder() > previous_focus,
              section.toStdString() + " sample actions must follow visual and keyboard order");
        if (button) { previous_bottom = button->getBottom(); previous_focus = button->getExplicitFocusOrder(); }
      }
      // Every type reads output rows first, then its controls. Rows within the
      // oscillator's section carry its number, not its name.
      auto* oscillator_section = find(editor, section, true);
      auto* on = oscillator_section ? find(*oscillator_section, String(section.getTrailingIntValue()) + " Switch") : nullptr;
      check(on && editor.getLocalArea(on, on->getLocalBounds()).getBottom() <=
                      editor.getLocalArea(group, group->getLocalBounds()).getY(),
            section.toStdString() + " sample controls must follow the output rows");
      check(File(source(restored, target)->getLastBrowsedFile()) == expected[target],
            section.toStdString() + " must restore its own sample folder");
      for (int step = 0; step < 3; ++step) {
        auto* button = dynamic_cast<Button*>(find(*group, step == 2 ? "Previous sample" : "Next sample"));
        if (!button || !button->onClick) { check(false, "sample navigation action missing"); break; }
        button->onClick();
        const int index = step == 1 ? 0 : 2;
        expected[target] = files[target][index];
        for (int other = 0; other < 3; ++other)
          check(File(source(restored, other)->getLastBrowsedFile()) == expected[other],
                section.toStdString() + " must cycle only its own folder and wrap in both directions");
        check(source(restored, target)->originalLength() == 512 + index * 128 &&
              source(restored, target)->getName() == expected[target].getFileNameWithoutExtension().toStdString(),
              section.toStdString() + " must decode the selected WAV");
      }
    }
    int previous_y = -1;
    int previous_focus = -1;
    for (const char* title : { "Polyphony", "Legato", "Portamento Time", "Portamento Slope", "Portamento Force",
                             "Portamento Scale", "Glide Zone Crossing", "Zone Crossfade", "Voice Priority",
                             "Voice Override", "Voice Transpose", "Voice Tune", "Voice Amplitude" }) {
      auto* control = find(editor, title);
      const int y = control ? editor.getLocalArea(control, control->getLocalBounds()).getY() : -1;
      check(control && y > previous_y && control->getExplicitFocusOrder() > previous_focus,
            std::string("voice control order: ") + title);
      if (control) { previous_y = y; previous_focus = control->getExplicitFocusOrder(); }
    }
  }

  void testLegacyAndPortableStates() {
    vital::Sample sample;
    for (bool stereo : {false, true}) {
      std::array<float, 512> audio {};
      if (stereo) sample.loadSample(audio.data(), audio.data(), audio.size(), 48000);
      else sample.loadSample(audio.data(), audio.size(), 48000);
      sample.setLastBrowsedFile("/missing/sample/folder/Kick.wav");
      auto portable = sample.stateToJson();
      sample.init();
      check(sample.getLastBrowsedFile().empty(), "Init must clear the previous sample's folder");
      sample.jsonToState(portable);
      check(sample.originalLength() == audio.size() && sample.sampleRate() == 48000,
            "embedded audio must restore without the source file");
      auto legacy = portable;
      legacy.erase("last_browsed_file");
      sample.jsonToState(legacy);
      check(sample.getLastBrowsedFile().empty(), "old presets must not inherit a stale folder");
    }
  }

  // Show as switches Start, End, Loop start and Loop end between percent,
  // milliseconds and samples of the loaded file, for reading, typing and keys.
  void testPositionUnits(const File& root) {
    check(root.createDirectory().wasOk(), "config folder must be created");
    LoadSave::setConfigFileForTesting(root.getChildFile("Atlas.config"));
    SynthPlugin plugin;
    slot_ids::useLegacySourceTypes(plugin);
    std::array<float, 4800> audio {};
    source(plugin, 0)->loadSample(audio.data(), audio.size(), 48000);
    plugin.getControls().at("osc_5_sample_loop_start")->set(0.25f);
    SynthEditor editor(plugin);
    auto* slider = findSlider(editor, "5 Sample Loop Start");
    check(slider != nullptr, "the Sample Loop Start row must be a slider");
    if (slider == nullptr) return;
    const auto reads = [&](const char* text, double adjustment) {
      const String shown = slider->getTextFromValue(0.25);
      check(shown == text, "Sample Loop Start must read " + std::string(text) + ", not " + shown.toStdString());
      check(std::abs(slider->adjustmentValue(0.25) - adjustment) < 1e-6 &&
            std::abs(slider->sliderValue(adjustment) - 0.25) < 1e-6,
            std::string("keys must step Sample Loop Start in the units it reads: ") + text);
    };
    reads("25%", 25.0);
    LoadSave::saveSamplePositionUnits(1);
    reads("25 ms", 25.0);
    LoadSave::saveSamplePositionUnits(2);
    reads("1200", 1200.0);
    check(slider->sliderValue(1200.4) == 0.25, "sample positions stay whole samples");
    LoadSave::saveSamplePositionUnits(0);
    reads("25%", 25.0);
    LoadSave::setConfigFileForTesting(File());

    // The root key reads the note of its MIDI value, not of the row's normalized value.
    auto* root_key = findSlider(editor, "5 Sample root key");
    check(root_key != nullptr, "the Sample root key row must be a slider");
    if (root_key != nullptr) {
      const String text = root_key->getTextFromValue(61.0 / 127.0);
      check(text == "C sharp 4, MIDI 61", "root key must read its MIDI note, not " + text.toStdString());
    }

    // Stretch mode decides which stretch rows show: Sample BPM in Tempo mode,
    // Sample Stretch Length in Fixed mode, neither when it is off.
    check(findSlider(editor, "5 Sample BPM") == nullptr && findSlider(editor, "5 Sample Stretch Length") == nullptr,
          "Stretch mode Off hides the BPM and stretch length rows");
    plugin.getControls().at("osc_5_sample_stretch_mode")->set(1.0f);
    SynthEditor tempo_editor(plugin);
    auto* bpm = findSlider(tempo_editor, "5 Sample BPM");
    check(bpm != nullptr && findSlider(tempo_editor, "5 Sample Stretch Length") == nullptr,
          "Tempo mode shows Sample BPM and not Sample Stretch Length");
    if (bpm != nullptr) {
      const String text = bpm->getTextFromValue((120.0 - 20.0) / (400.0 - 20.0));
      check(text == "120 BPM", "Sample BPM must read in BPM, not " + text.toStdString());
    }
    plugin.getControls().at("osc_5_sample_stretch_mode")->set(2.0f);
    SynthEditor fixed_editor(plugin);
    auto* length = findSlider(fixed_editor, "5 Sample Stretch Length");
    check(length != nullptr && findSlider(fixed_editor, "5 Sample BPM") == nullptr,
          "Fixed mode shows Sample Stretch Length and not Sample BPM");
    if (length != nullptr) {
      const String text = length->getTextFromValue((2.0 - 0.25) / (4.0 - 0.25));
      check(text == "200%", "Sample Stretch Length must read in percent, not " + text.toStdString());
    }
    plugin.getControls().at("osc_5_sample_stretch_mode")->set(0.0f);
  }

  // Mode is a group of Classic and Slicer buttons, Classic selected by
  // default with every Classic row; Slicer shows its own rows and the Slices
  // editor in their place.
  void testSlicerRows() {
    SynthPlugin plugin;
    slot_ids::useLegacySourceTypes(plugin);
    std::array<float, 48000> audio {};
    source(plugin, 0)->loadSample(audio.data(), audio.size(), 48000);
    SynthEditor editor(plugin);
    auto* mode = find(editor, "Oscillator 5 sample mode", true);
    check(mode != nullptr && mode->getTitle() == "Mode", "the Sample type has a Mode group");
    if (mode == nullptr) return;
    auto* classic = dynamic_cast<Button*>(find(*mode, "Classic"));
    auto* slicer = dynamic_cast<Button*>(find(*mode, "Slicer"));
    check(classic != nullptr && slicer != nullptr, "the Mode group holds a Classic and a Slicer button");
    if (classic == nullptr || slicer == nullptr) return;
    check(classic->getToggleState() && !slicer->getToggleState(), "Classic is selected by default");
    check(classic->getRadioGroupId() != 0 && classic->getRadioGroupId() == slicer->getRadioGroupId(),
          "the mode buttons form one radio group");
    check(findSlider(editor, "5 Sample Loop Start") != nullptr && findSlider(editor, "5 Sample Playback Mode") != nullptr,
          "Classic shows its Start, End, loop and playback rows");
    check(find(editor, "Slices editor") == nullptr && findSlider(editor, "5 Sample Number of Slices") == nullptr &&
          findSlider(editor, "5 Sample Stutter") == nullptr,
          "Classic shows no Slicer rows");

    plugin.parameterChanged("osc_5_sample_mode", 1.0f);
    SynthEditor slicer_editor(plugin);
    // The message loop is not running here: each edit is committed by hand, as it would be between keys.
    auto& history = plugin.undoHistory();
    history.ensureBaseline();
    auto* slicer_mode = find(slicer_editor, "Oscillator 5 sample mode", true);
    auto* selected = slicer_mode != nullptr ? dynamic_cast<Button*>(find(*slicer_mode, "Slicer")) : nullptr;
    check(selected != nullptr && selected->getToggleState(), "the Slicer button reads as selected in Slicer mode");
    check(findSlider(slicer_editor, "5 Sample Loop Start") == nullptr &&
          findSlider(slicer_editor, "5 Sample Start") == nullptr &&
          findSlider(slicer_editor, "5 Sample Playback Mode") == nullptr &&
          findSlider(slicer_editor, "5 Sample Keytrack") == nullptr &&
          find(slicer_editor, "5 Sample Random Phase") == nullptr,
          "Slicer hides the Start, End, loop, playback, Keytrack and Random Phase rows");
    check(findSlider(slicer_editor, "5 Sample Number of Slices") != nullptr &&
          findSlider(slicer_editor, "5 Sample Sensitivity") == nullptr,
          "Slice by Grid shows Number of Slices and not Sensitivity");
    check(findSlider(slicer_editor, "5 Sample BPM") != nullptr, "the Slicer's stretch mode defaults to Tempo");
    check(findSlider(slicer_editor, "5 Sample Stutter") != nullptr &&
          findSlider(slicer_editor, "5 Sample Stutter Chance") != nullptr &&
          findSlider(slicer_editor, "5 Sample Slice Decay") != nullptr,
          "Slicer shows Stutter, Stutter Chance and Slice Decay");
    auto* slices = dynamic_cast<SampleSliceEditor*>(find(slicer_editor, "Slices editor"));
    check(slices != nullptr, "Slicer shows the Slices editor");
    if (slices != nullptr) {
      check(slices->statusText().startsWith("Slice 1 of 16, start 0%, end 6."),
            "the editor reads the slice's start, end and length: " + slices->statusText().toStdString());
      slices->handleShortcut(KeyPress(KeyPress::downKey));
      check(slices->statusText().startsWith("Slice 2 of 16"), "Down moves to the next slice");
      slices->handleShortcut(KeyPress(KeyPress::rightKey));
      history.commitPendingEdits();
      check(source(plugin, 0)->slices()[1] == 3000 + 480, "Right moves the slice's start by 10 ms");
      slices->handleShortcut(KeyPress(KeyPress::leftKey, ModifierKeys::shiftModifier, 0));
      history.commitPendingEdits();
      check(source(plugin, 0)->slices()[1] == 3000 + 480 - 48, "Shift Left moves it back by 1 ms");
      slices->handleShortcut(KeyPress(KeyPress::returnKey));
      history.commitPendingEdits();
      check(source(plugin, 0)->slices().size() == 17, "Return splits the slice");
      slices->handleShortcut(KeyPress(KeyPress::backspaceKey));
      history.commitPendingEdits();
      check(source(plugin, 0)->slices().size() == 16, "Backspace removes it again");
      const auto before = source(plugin, 0)->slices();
      slices->handleShortcut(KeyPress('R', ModifierKeys(), 'r'));
      history.commitPendingEdits();
      const auto randomized = source(plugin, 0)->slices();
      check(randomized.size() == 16 && randomized != before && randomized[0] == 0 &&
            std::is_sorted(randomized.begin(), randomized.end()),
            "R moves the starts to random places, in order");
      // Each edit is a step undo takes back.
      check(history.canUndo() && history.undoDescription().contains("randomize"),
            "randomizing is an undo step: " + history.undoDescription().toStdString());
      history.undo();
      check(source(plugin, 0)->slices() == before, "undo puts the slices back");
      history.undo();
      check(source(plugin, 0)->slices().size() == 17, "a second undo puts the removed slice back");
      history.redo();
      check(source(plugin, 0)->slices() == before, "redo removes it again");
    }
    plugin.getControls().at("osc_5_sample_slice_by")->set(1.0f);
    SynthEditor transient_editor(plugin);
    check(findSlider(transient_editor, "5 Sample Sensitivity") != nullptr &&
          findSlider(transient_editor, "5 Sample Divisions") == nullptr,
          "Slice by Transients shows Sensitivity and not Divisions");
    plugin.getControls().at("osc_5_sample_slice_by")->set(0.0f);
    plugin.getControls().at("osc_5_sample_mode")->set(0.0f);
  }

  int run() {
    ScopedJuceInitialiser_GUI gui;
    const File root = File::getSpecialLocation(File::tempDirectory).getNonexistentChildFile("atlas_sample_ui", {}, false);
    testPositionUnits(root.getChildFile("Config"));
    testSlicerRows();
    testRestoredNavigation(root);
    testLegacyAndPortableStates();
    check(root.deleteRecursively(), "sample fixtures must be removed");
    std::cout << "Sample navigation and control layout: " << failures << " failures\n";
    return failures ? 1 : 0;
  }
}
