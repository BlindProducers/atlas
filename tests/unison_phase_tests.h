// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "random_amplitude_seed_tests.h"

namespace unison_phase_tests {
  using O = vital::SynthOscillator;
  using oscillator_warp_tests::Fixture;
  using oscillator_warp_tests::difference;
  using random_amplitude_seed_tests::findControl;

  // Mean power of the left and right lanes over many notes. With no detune every
  // unison oscillator plays the same pitch, so the power shows how far their
  // starting phases agree: all in phase gives the most, uniformly random phases
  // only the sum of each oscillator's own power. Frozen, each note takes the
  // next seed, so the notes still sample many patterns.
  double notePower(float random_phase, int mode = O::kRandomPhaseEachNote) {
    Fixture<O> f(48000, 16, 1);
    f.set(O::kUnisonDetune, 0.0f);
    f.set(O::kRandomPhase, random_phase);
    f.set(O::kRandomPhaseMode, mode);
    auto* reset = f.controls[O::kReset].output();
    double power = 0.0;
    int count = 0;
    for (int note = 0; note < 256; ++note) {
      f.set(O::kRandomPhaseSeed, note + 1);
      reset->trigger(vital::poly_mask(-1, -1, 0, 0), vital::kVoiceOn, 0);
      f.process(128);
      reset->trigger_mask = 0;
      f.process(128);
      for (int i = 0; i < 128; ++i) {
        const auto sample = f.oscillator->output(O::kRaw)->buffer[i];
        power += double(sample[0]) * sample[0] + double(sample[1]) * sample[1];
        count += 2;
      }
    }
    return power / count;
  }

  void testRandomPhaseCoversTheCycle() {
    // 16 oscillators at their default blend leave about 0.125 of the in-phase
    // power when their phases are uniform over the whole cycle. Converting a
    // negative phase to unsigned saturates to 0 on arm64, which left half of them
    // in phase and the rest in the first half of the cycle: about 0.35 or more.
    const double in_phase = notePower(0.0f);
    const double ratio = notePower(1.0f) / in_phase;
    std::cout << "Unison power at 100% phase randomization, relative to 0%: " << ratio << '\n';
    expect(ratio > 0.07 && ratio < 0.2, "100% phase randomization spreads unison phases over the whole cycle");
    const double frozen_ratio = notePower(1.0f, O::kRandomPhaseFrozen) / in_phase;
    std::cout << "Frozen unison power over 256 seeds, relative to 0%: " << frozen_ratio << '\n';
    expect(frozen_ratio > 0.07 && frozen_ratio < 0.2, "Frozen seeds spread unison phases over the whole cycle");
  }

  // Two notes in a row from one oscillator, each rendered from its note-on,
  // after a first note: a new oscillator's first note settles smoothing that a
  // note-on doesn't reset, a difference of about 1e-4 for a few blocks.
  std::pair<std::vector<float>, std::vector<float>> twoNotes(int mode, int seed, int first_seed = -1,
                                                             int type = O::kWavetableType) {
    Fixture<O> f(48000, 7, 1);
    f.set(O::kOscillatorType, type);
    f.set(O::kUnisonDetune, 3.0f);
    f.set(O::kRandomPhase, 1.0f);
    f.set(O::kRandomPhaseMode, mode);
    auto* reset = f.controls[O::kReset].output();
    auto note = [&](int note_seed) {
      f.set(O::kRandomPhaseSeed, note_seed);
      reset->trigger(vital::poly_mask(-1, -1, 0, 0), vital::kVoiceOn, 0);
      f.process(128);
      reset->trigger_mask = 0;
      return f.render(16);
    };
    note(seed);
    auto first = note(first_seed < 0 ? seed : first_seed);
    return { first, note(seed) };
  }

  void testFrozenNotesRepeat() {
    for (int type : { O::kWavetableType, O::kAnalogType }) {
      const std::string name = type == O::kAnalogType ? "Analog" : "Wavetable";
      const auto frozen = twoNotes(O::kRandomPhaseFrozen, 42, -1, type);
      expect(frozen.first == frozen.second, name + ": a frozen seed starts every note the same way");
      const auto each_note = twoNotes(O::kRandomPhaseEachNote, 42, -1, type);
      expect(difference(each_note.first, each_note.second) > 1e-3, name + ": New Each Note still varies every note");
      const auto seeds = twoNotes(O::kRandomPhaseFrozen, 43, 42, type);
      expect(difference(seeds.first, seeds.second) > 1e-3, name + ": another seed freezes another pattern");
    }
  }

  int run() {
    testRandomPhaseCoversTheCycle();
    testFrozenNotesRepeat();
    return failures ? 1 : 0;
  }

  void pumpEditor() {
    for (int i = 0; i < 4; ++i) {
      Thread::sleep(40);
      Timer::callPendingTimersSynchronously();
    }
  }

  int runControls() {
    juce::ScopedJuceInitialiser_GUI gui;
    SynthPlugin plugin;
    const auto& controls = plugin.getControls();
    for (int osc = 1; osc <= vital::kNumOscillators; ++osc) {
      const auto prefix = "osc_" + std::to_string(osc) + "_";
      expectClose(controls.at(prefix + "random_phase_mode")->value(), O::kRandomPhaseEachNote, 0,
                  "Phase randomization defaults to new phases each note");
      expectClose(controls.at(prefix + "random_phase_seed")->value(), 0, 0, "The phase seed defaults to 0");
    }
    for (auto* parameter : plugin.getParameters()) {
      auto* bridge = dynamic_cast<ValueBridge*>(parameter);
      if (!bridge || !bridge->getParameterId().endsWith("_random_phase_seed")) continue;
      expect(bridge->isAutomatable() && bridge->isDiscrete() && bridge->getNumSteps() == 65536,
             "Host exposes the full integer phase seed range");
    }

    plugin.parameterChanged("osc_1_random_phase_mode", O::kRandomPhaseFrozen);
    plugin.parameterChanged("osc_1_random_phase_seed", 1234);
    MemoryBlock state;
    plugin.getStateInformation(state);
    SynthPlugin restored;
    restored.setStateInformation(state.getData(), int(state.getSize()));
    for (const char* id : { "osc_1_random_phase_mode", "osc_1_random_phase_seed" })
      expectClose(restored.getControls().at(id)->value(), controls.at(id)->value(), 0,
                  "Frozen phase mode and seed survive state reload");
    MemoryInputStream stream(state.getData(), state.getSize(), false);
    auto preset = json::parse(stream.readEntireStreamAsString().toStdString());
    preset["settings"].erase("osc_1_random_phase_mode");
    preset["settings"].erase("osc_1_random_phase_seed");
    const auto old_preset = preset.dump();
    restored.setStateInformation(old_preset.data(), int(old_preset.size()));
    expectClose(restored.getControls().at("osc_1_random_phase_mode")->value(), O::kRandomPhaseEachNote, 0,
                "An older preset loads with new phases each note");

    plugin.parameterChanged("osc_1_random_phase_mode", O::kRandomPhaseEachNote);
    SynthEditor editor(plugin);
    expect(findControl(editor, "1 Phase Randomization Seed") == nullptr,
           "The phase seed is hidden while phases are new each note");
    expect(findControl(editor, "Randomize phase seed") == nullptr,
           "Its Randomize button is hidden with it");
    plugin.parameterChanged("osc_1_random_phase_mode", O::kRandomPhaseFrozen);
    pumpEditor();
    auto* slider = dynamic_cast<Slider*>(findControl(editor, "1 Phase Randomization Seed"));
    expect(slider != nullptr, "Choosing Frozen reveals the phase seed without reopening");
    auto* button = dynamic_cast<Button*>(findControl(editor, "Randomize phase seed"));
    expect(button != nullptr, "The phase seed has a Randomize button");
    if (slider != nullptr && button != nullptr) {
      const auto& order = editor.getKeyboardFocusOrder();
      expect(std::find(order.begin(), order.end(), slider) != order.end(), "The phase seed is in keyboard focus order");
      expect(std::find(order.begin(), order.end(), button) != order.end(), "Randomize phase seed is keyboard reachable");
      const float before = controls.at("osc_1_random_phase_seed")->value();
      slider->keyPressed(KeyPress(KeyPress::rightKey));
      expectClose(controls.at("osc_1_random_phase_seed")->value(), before + 1, 0, "Arrow key steps the phase seed by one");
      for (int i = 0; i < 5; ++i) {
        const float previous = controls.at("osc_1_random_phase_seed")->value();
        button->onClick();
        expect(controls.at("osc_1_random_phase_seed")->value() > 0 &&
               controls.at("osc_1_random_phase_seed")->value() != previous,
               "Randomize phase seed always chooses a different nonzero seed");
      }
    }
    plugin.parameterChanged("osc_1_random_phase_mode", O::kRandomPhaseEachNote);
    pumpEditor();
    expect(findControl(editor, "1 Phase Randomization Seed") == nullptr,
           "Going back to New Each Note hides the phase seed");
    std::cout << "Unison phase control tests: " << failures << " failures\n";
    return failures ? 1 : 0;
  }
}
