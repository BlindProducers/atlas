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
      // Every type reads output rows first, then its controls.
      auto* on = find(editor, section + " Switch");
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

  int run() {
    ScopedJuceInitialiser_GUI gui;
    const File root = File::getSpecialLocation(File::tempDirectory).getNonexistentChildFile("atlas_sample_ui", {}, false);
    testRestoredNavigation(root);
    testLegacyAndPortableStates();
    check(root.deleteRecursively(), "sample fixtures must be removed");
    std::cout << "Sample navigation and control layout: " << failures << " failures\n";
    return failures ? 1 : 0;
  }
}
