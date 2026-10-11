// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "oscillator_warp_tests.h"

namespace random_amplitude_seed_tests {
  using O = vital::SynthOscillator;
  using oscillator_warp_tests::Fixture;
  using oscillator_warp_tests::difference;

  std::vector<float> render(int seed, int secondary_seed, int primary = O::kRandomAmplitudes,
                            int secondary = O::kNoSpectralMorph, float amount = .4f) {
    Fixture<O> f;
    f.set(O::kSpectralMorphType, primary);
    f.set(O::kSecondarySpectralMorphType, secondary);
    f.set(O::kSpectralMorphAmount, amount);
    f.set(O::kSecondarySpectralMorphAmount, amount);
    f.set(O::kSpectralMorphSeed, seed);
    f.set(O::kSecondarySpectralMorphSeed, secondary_seed);
    return f.render();
  }

  int runDsp() {
    // Independent reconstruction of the table used before seed support.
    vital::utils::RandomGenerator legacy(-1.0f, 1.0f);
    legacy.seed(4);
    vital::RandomValues values;
    const auto* original = values.buffer(0);
    for (int i = 0; i < vital::RandomValues::kBufferSize; ++i) {
      const auto expected = legacy.polyNext();
      expect(!vital::poly_float::notEqual(original[i], expected).anyMask(),
             "Seed zero preserves the complete legacy random table exactly");
    }

    const auto initial = render(0, 0);
    const auto seeded = render(1234, 0);
    expect(difference(initial, seeded) > 1e-3, "Changing the primary seed changes rendered audio");
    expect(seeded == render(1234, 0), "A saved seed reproduces the same rendered audio");
    expect(seeded == render(1234, 9876), "An unused secondary seed cannot affect the primary morph");
    expect(seeded == render(0, 1234, O::kNoSpectralMorph, O::kRandomAmplitudes),
           "Secondary-only random amplitudes uses its own seed");
    expect(initial == render(0, 0), "Rendering another seed cannot mutate the legacy pattern");
    expect(render(0, 0, O::kSmear, O::kSkew) == render(1234, 5678, O::kSmear, O::kSkew),
           "Seeds do not change other morph modes");
    expect(render(0, 0, O::kRandomAmplitudes, 0, 0) == render(65535, 0, O::kRandomAmplitudes, 0, 0),
           "Zero random amplitude amount is unchanged for every seed");
    for (const auto modes : { std::pair<int, int>{O::kRandomAmplitudes, O::kRandomAmplitudes},
                             {O::kRandomAmplitudes, O::kSkew},
                             {O::kVocode, O::kRandomAmplitudes} }) {
      const auto baseline = render(123, 456, modes.first, modes.second);
      expect(baseline == render(123, 456, modes.first, modes.second), "Chained seeded morphs are repeatable");
      if (modes.first == O::kRandomAmplitudes)
        expect(difference(baseline, render(789, 456, modes.first, modes.second)) > 1e-4,
               "Primary seed affects the intermediate spectral cache");
      if (modes.second == O::kRandomAmplitudes)
        expect(difference(baseline, render(123, 789, modes.first, modes.second)) > 1e-4,
               "Secondary seed independently affects the chained output");
    }

    // Interleaved oscillators must not overwrite each other's random patterns.
    Fixture<O> a, b, reference;
    for (auto* f : { &a, &b, &reference }) {
      f->set(O::kSpectralMorphType, O::kRandomAmplitudes);
      f->set(O::kSpectralMorphAmount, .4f);
      f->set(O::kSpectralMorphSeed, 1234);
    }
    for (int i = 0; i < 20; ++i) {
      b.set(O::kSpectralMorphSeed, 33 + i);
      b.process(128);
      expect(a.render(1) == reference.render(1), "Oscillator instances retain independent seed caches");
    }
    for (int rate : {44100, 96000}) for (int block : {17, 128}) {
      Fixture<O> f(rate, 16);
      f.set(O::kSpectralMorphType, O::kRandomAmplitudes);
      f.set(O::kSecondarySpectralMorphType, O::kRandomAmplitudes);
      f.set(O::kSpectralMorphAmount, .4f);
      f.set(O::kSecondarySpectralMorphAmount, .2f);
      for (int seed : {0, 1, 1234, 65535, 2, 0}) {
        f.set(O::kSpectralMorphSeed, seed);
        f.set(O::kSecondarySpectralMorphSeed, 65535 - seed);
        for (int i = 0; i < 8; ++i) {
          tracked_allocations.store(0);
          track_allocations.store(true);
          f.process(block);
          track_allocations.store(false);
          expect(tracked_allocations.load() == 0, "Changing either seed during playback does not allocate");
          for (int sample = 0; sample < block; ++sample)
            expect(vital::utils::isFinite(f.oscillator->output(O::kRaw)->buffer[sample]),
                   "Seed automation stays finite with 16-voice unison");
        }
      }
    }
    std::cout << "Random amplitude seed DSP tests: " << failures << " failures\n";
    return failures ? 1 : 0;
  }

  Component* findControl(Component& parent, const String& title) {
    if (parent.getTitle() == title && (dynamic_cast<Slider*>(&parent) || dynamic_cast<Button*>(&parent)))
      return &parent;
    for (auto* child : parent.getChildren())
      if (auto* found = findControl(*child, title)) return found;
    return nullptr;
  }

  int runControls() {
    juce::ScopedJuceInitialiser_GUI gui;
    SynthPlugin plugin;
    const auto& controls = plugin.getControls();
    const char* suffixes[] { "spectral_morph_seed", "secondary_spectral_morph_seed" };
    for (int osc = 1; osc <= vital::kNumOscillators; ++osc) for (const auto* suffix : suffixes) {
      const auto id = "osc_" + std::to_string(osc) + "_" + suffix;
      expectClose(controls.at(id)->value(), 0, 0, "New oscillator seeds default to the legacy pattern");
      plugin.parameterChanged(id, osc * 100 + (suffix == suffixes[0] ? 1 : 2));
    }
    int old_index = 0;
    for (int i = 0; i < vital::Parameters::getNumParameters(); ++i) {
      const auto* details = vital::Parameters::getDetails(i);
      if (details->version_added >= 0x020110 ||
          (!controls.count(details->name) && !controls.count(SynthPlugin::legacyAliasTarget(details->name))))
        continue; // Legacy source aliases keep their number.
      for (auto* parameter : plugin.getParameters()) {
        auto* bridge = dynamic_cast<ValueBridge*>(parameter);
        if (bridge && bridge->getParameterId().toStdString() == details->name)
          expect(bridge->getParameterID() == String(old_index), "Existing host automation IDs remain stable");
      }
      ++old_index;
    }
    for (auto* parameter : plugin.getParameters()) {
      auto* bridge = dynamic_cast<ValueBridge*>(parameter);
      if (!bridge || !bridge->getParameterId().endsWith("_spectral_morph_seed")) continue;
      expect(bridge->isAutomatable() && bridge->isDiscrete() && bridge->getNumSteps() == 65536,
             "Host exposes the full integer seed range for automation");
      const auto value = bridge->getValueForText("65535");
      expectClose(bridge->convertToEngineValue(value), 65535, 0, "Seed text entry accepts the maximum seed");
    }
    plugin.parameterChanged("osc_1_random_phase", 0);
    plugin.parameterChanged("osc_1_spectral_morph_type", O::kRandomAmplitudes);
    plugin.parameterChanged("osc_1_spectral_morph_amount", .35f);
    plugin.parameterChanged("osc_1_secondary_spectral_morph_type", O::kRandomAmplitudes);
    plugin.parameterChanged("osc_1_secondary_spectral_morph_amount", .15f);
    MemoryBlock state;
    plugin.getStateInformation(state);
    SynthPlugin restored;
    restored.setStateInformation(state.getData(), int(state.getSize()));
    for (int osc = 1; osc <= vital::kNumOscillators; ++osc) for (const auto* suffix : suffixes) {
      const auto id = "osc_" + std::to_string(osc) + "_" + suffix;
      expectClose(restored.getControls().at(id)->value(), controls.at(id)->value(), 0,
                  "Every oscillator and slot seed survives state reload");
    }
    const auto audio = [](SynthPlugin& synth, bool start_note) {
      if (start_note) {
        synth.setPlayConfigDetails(0, 2, 48000, 128);
        synth.prepareToPlay(48000, 128);
      }
      AudioSampleBuffer block(2, 128);
      MidiBuffer midi;
      if (start_note) midi.addEvent(MidiMessage::noteOn(1, 60, .8f), 0);
      std::vector<float> result;
      for (int b = 0; b < 32; ++b) {
        block.clear();
        synth.processBlock(block, midi);
        midi.clear();
        for (int channel = 0; channel < 2; ++channel)
          result.insert(result.end(), block.getReadPointer(channel), block.getReadPointer(channel) + 128);
      }
      return result;
    };
    const auto before_reload = audio(plugin, true);
    const auto after_reload = audio(restored, true);
    expect(difference(before_reload, after_reload) < 1e-6,
           "Restoring a saved seeded patch reproduces actual plugin audio");
    restored.parameterChanged("osc_1_spectral_morph_seed", 4321);
    expect(difference(audio(plugin, false), audio(restored, false)) > 1e-4,
           "Host seed changes reach the oscillator while a plugin note is sustained");
    MemoryInputStream stream(state.getData(), state.getSize(), false);
    auto preset = json::parse(stream.readEntireStreamAsString().toStdString());
    for (int osc = 1; osc <= vital::kNumOscillators; ++osc) for (const auto* suffix : suffixes)
      preset["settings"].erase("osc_" + std::to_string(osc) + "_" + suffix);
    const auto old_preset = preset.dump();
    restored.setStateInformation(old_preset.data(), int(old_preset.size()));
    for (int osc = 1; osc <= vital::kNumOscillators; ++osc) for (const auto* suffix : suffixes)
      expectClose(restored.getControls().at("osc_" + std::to_string(osc) + "_" + suffix)->value(), 0, 0,
                  "Loading an old preset resets a previously changed seed to zero");

    plugin.parameterChanged("osc_1_spectral_morph_type", O::kRandomAmplitudes);
    plugin.parameterChanged("osc_1_secondary_spectral_morph_type", O::kRandomAmplitudes);
    SynthEditor editor(plugin);
    for (int slot = 0; slot < 2; ++slot) {
      const String title = slot ? "Secondary Random Amplitudes Seed" : "Random Amplitudes Seed";
      auto* slider = dynamic_cast<Slider*>(findControl(editor, title));
      expect(slider != nullptr, "Seed slider is visible for each random amplitudes slot");
      if (!slider) continue;
      const auto& order = editor.getKeyboardFocusOrder();
      expect(std::find(order.begin(), order.end(), slider) != order.end(), "Seed slider is in keyboard focus order");
      expect(slider->createAccessibilityHandler() != nullptr, "Seed slider exposes accessibility");
      const auto id = std::string("osc_1_") + suffixes[slot];
      const float before = controls.at(id)->value();
      slider->keyPressed(KeyPress(KeyPress::rightKey));
      expectClose(controls.at(id)->value(), before + 1, 0, "Arrow key advances seed by exactly one");
      slider->keyPressed(KeyPress(KeyPress::backspaceKey));
      expectClose(controls.at(id)->value(), 0, 0, "Backspace restores the original seed");
      auto* button = dynamic_cast<Button*>(findControl(editor, slot ? "Randomize secondary amplitudes seed"
                                                                  : "Randomize amplitudes seed"));
      expect(button != nullptr, "Each seed has a randomize button");
      if (!button) continue;
      expect(std::find(order.begin(), order.end(), button) != order.end(), "Randomize button is keyboard reachable");
      expect(static_cast<Component*>(button)->createAccessibilityHandler() != nullptr,
             "Randomize button exposes accessibility even in an unattached test editor");
      const auto other_id = std::string("osc_1_") + suffixes[1 - slot];
      const float other_seed = controls.at(other_id)->value();
      for (int i = 0; i < 5; ++i) {
        const float previous = controls.at(id)->value();
        button->onClick();
        expect(controls.at(id)->value() > 0 && controls.at(id)->value() != previous,
               "Randomize always chooses a different nonzero seed");
        expectClose(controls.at(other_id)->value(), other_seed, 0, "Randomize leaves the other slot seed unchanged");
      }
    }
    plugin.parameterChanged("osc_1_spectral_morph_type", O::kSmear);
    for (int i = 0; i < 4; ++i) {
      Thread::sleep(40);
      Timer::callPendingTimersSynchronously();
    }
    expect(findControl(editor, "Random Amplitudes Seed") == nullptr,
           "Changing away from Random Amplitudes hides its seed control");
    expect(findControl(editor, "Secondary Random Amplitudes Seed") != nullptr,
           "Changing the primary mode leaves the secondary seed visible");
    plugin.parameterChanged("osc_1_spectral_morph_type", O::kRandomAmplitudes);
    for (int i = 0; i < 4; ++i) {
      Thread::sleep(40);
      Timer::callPendingTimersSynchronously();
    }
    expect(findControl(editor, "Random Amplitudes Seed") != nullptr,
           "Selecting Random Amplitudes reveals the seed control without reopening");
    std::cout << "Random amplitude seed control tests: " << failures << " failures\n";
    return failures ? 1 : 0;
  }
}
