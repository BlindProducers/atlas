// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "oscillator_warp_fixture.h"
#include <fstream>
#include "../src/plugin/synth_editor.h"
#include "producers_module.h"

namespace oscillator_warp_tests {
  using O = vital::SynthOscillator;
  double difference(const std::vector<float>& a, const std::vector<float>& b) {
    double sum = 0;
    for (size_t i = 0; i < a.size(); ++i) sum += std::abs(double(a[i]) - b[i]);
    return sum / a.size();
  }
  std::vector<float> render(int warp1, int warp2, int morph1 = 0, int morph2 = 0,
                            float amount1 = .37f, float amount2 = .72f, int voices = 3, int active = 3) {
    Fixture<O> f(48000, voices, active);
    f.set(O::kDistortionType, warp1); f.set(O::kDistortionAmount, amount1);
    f.set(O::kSecondaryDistortionType, warp2); f.set(O::kSecondaryDistortionAmount, amount2);
    f.set(O::kSpectralMorphType, morph1); f.set(O::kSpectralMorphAmount, amount1);
    f.set(O::kSecondarySpectralMorphType, morph2); f.set(O::kSecondarySpectralMorphAmount, amount2);
    return f.render();
  }
  void checkFinite(const std::vector<float>& samples, const std::string& label) {
    expect(std::all_of(samples.begin(), samples.end(), [](float x) { return std::isfinite(x); }), label + " stays finite");
  }
  void testExamplesAndIndependentAmounts() {
    const auto sync_bend = render(O::kSync, O::kBend);
    expect(difference(sync_bend, render(O::kSync, O::kNone)) > 1e-3, "Bend changes the synced waveform");
    expect(difference(sync_bend, render(O::kNone, O::kBend)) > 1e-3, "Sync remains active with Bend");
    expect(difference(sync_bend, render(O::kSync, O::kBend, 0, 0, .2f)) > 1e-3, "Primary warp amount remains independent");
    expect(difference(sync_bend, render(O::kSync, O::kBend, 0, 0, .37f, .2f)) > 1e-3, "Secondary warp amount remains independent");
    expect(difference(sync_bend, render(O::kBend, O::kSync, 0, 0, .72f, .37f)) > 1e-3, "Warp order changes the result");
    const auto vocode_random = render(0, 0, O::kVocode, O::kRandomAmplitudes);
    expect(difference(vocode_random, render(0, 0, O::kVocode, 0)) > 1e-3, "Random amplitudes changes Vocode");
    expect(difference(vocode_random, render(0, 0, 0, O::kRandomAmplitudes)) > 1e-3, "Vocode remains active with random amplitudes");
    expect(difference(vocode_random, render(0, 0, O::kVocode, O::kRandomAmplitudes, .2f)) > 1e-3, "Primary morph amount remains independent");
    expect(difference(vocode_random, render(0, 0, O::kVocode, O::kRandomAmplitudes, .37f, .2f)) > 1e-3, "Secondary morph amount remains independent");
    expect(difference(vocode_random, render(0, 0, O::kRandomAmplitudes, O::kVocode, .72f, .37f)) > 1e-3, "Morph order changes the result");
  }
  void testBypassAndStandaloneSlots() {
    for (int active : {1, 2, 3}) for (int voices : {1, 3, 16}) {
      for (int type = 0; type < O::kNumDistortionTypes; ++type) {
        const auto primary = render(type, 0, 0, 0, .37f, .2f, voices, active);
        const auto bypass = render(type, 0, 0, 0, .37f, .9f, voices, active);
        expect(primary == bypass, "Disabled secondary warp is exactly transparent");
        const auto secondary = render(0, type, 0, 0, .37f, .37f, voices, active);
        // Let the existing primary amount interpolation settle before comparing.
        expect(difference(std::vector<float>(primary.end()-4096, primary.end()),
                          std::vector<float>(secondary.end()-4096, secondary.end())) < 2e-5,
               "Secondary-only warp matches primary mode " + std::to_string(type));
      }
    }
    for (int type = 0; type < O::kNumSpectralMorphTypes; ++type) {
      const auto primary = render(0, 0, type, 0, .37f, .2f, 1);
      expect(primary == render(0, 0, type, 0, .37f, .9f, 1), "Disabled secondary morph is exactly transparent");
      const auto secondary = render(0, 0, 0, type, .37f, .37f, 1);
      expect(difference(primary, secondary) < 2e-5, "Secondary-only morph matches primary mode " + std::to_string(type));
    }
  }
  void testAllPairsAndAutomation() {
    for (int a = 0; a < O::kNumDistortionTypes; ++a)
      for (int b = 1; b < O::kNumDistortionTypes; ++b)
        checkFinite(render(a, b), "Warp pair " + std::to_string(a) + "/" + std::to_string(b));
    for (int a = 0; a < O::kNumSpectralMorphTypes; ++a)
      for (int b = 1; b < O::kNumSpectralMorphTypes; ++b)
        checkFinite(render(O::kSync, O::kBend, a, b), "Morph pair " + std::to_string(a) + "/" + std::to_string(b));
    for (int rate : {44100, 48000, 96000}) for (int block : {1, 17, 128}) {
      Fixture<O> f(rate, 16, 2);
      f.set(O::kDistortionType, O::kSync);
      f.set(O::kSecondaryDistortionType, O::kBend);
      f.set(O::kSpectralMorphType, O::kShepardTone);
      f.set(O::kSecondarySpectralMorphType, O::kShepardTone);
      for (int b = 0; b < 300; ++b) {
        const float amount = (b % 101) / 100.f;
        f.set(O::kDistortionAmount, amount);
        f.set(O::kSecondaryDistortionAmount, 1.f - amount);
        f.set(O::kSpectralMorphAmount, amount);
        f.set(O::kSecondarySpectralMorphAmount, amount);
        tracked_allocations.store(0);
        track_allocations.store(true);
        f.process(block);
        track_allocations.store(false);
        expect(tracked_allocations.load() == 0, "Dual warps do not allocate while processing");
        for (int i = 0; i < block; ++i)
          expect(vital::utils::isFinite(f.oscillator->output(O::kRaw)->buffer[i]), "Modulated dual Shepard and warps stay finite");
      }
    }
  }
  int compareLegacy(const char* path) {
    std::ifstream file(path, std::ios::binary);
    expect(bool(file), "Legacy audio reference exists");
    if (!file) return 1;
    double maximum_error = 0;
    int cases = 0;
    for (int active : {1, 2, 3}) for (int unison : {1, 3, 16})
      for (int kind = 0; kind < 2; ++kind) {
        int modes = kind ? O::kNumSpectralMorphTypes : O::kNumDistortionTypes;
        for (int mode = 0; mode < modes; ++mode) {
          Fixture<O> f(48000, unison, active);
          f.set(O::kDistortionType, kind ? O::kBend : mode);
          f.set(O::kDistortionAmount, .37f);
          f.set(O::kSpectralMorphType, kind ? mode : O::kRandomAmplitudes);
          f.set(O::kSpectralMorphAmount, .21f);
          const auto actual = f.render(16);
          std::vector<float> expected(actual.size());
          file.read(reinterpret_cast<char*>(expected.data()), expected.size() * sizeof(float));
          expect(bool(file), "Complete legacy audio reference");
          for (size_t i = 0; i < actual.size(); ++i)
            maximum_error = std::max(maximum_error, std::abs(double(actual[i])-expected[i]));
          expect(actual == expected, "Secondary None preserves original audio exactly, case " + std::to_string(cases++));
        }
      }
    std::cout << "Legacy comparison: " << cases << " cases, maximum error=" << maximum_error << '\n';
    return failures ? 1 : 0;
  }
  Component* sliderWithTitle(Component& parent, const String& title) {
    if (dynamic_cast<Slider*>(&parent) && parent.getTitle().equalsIgnoreCase(title)) return &parent;
    for (auto* child : parent.getChildren())
      if (auto* found = sliderWithTitle(*child, title)) return found;
    return nullptr;
  }
  int runControls() {
    juce::ScopedJuceInitialiser_GUI gui;
    SynthPlugin plugin;
    plugin.setPlayConfigDetails(0, 2, 48000, 128);
    plugin.prepareToPlay(48000, 128);
    const auto controls = plugin.getControls();
    const char* suffixes[] { "secondary_distortion_type", "secondary_distortion_amount",
                            "secondary_spectral_morph_type", "secondary_spectral_morph_amount" };
    for (int osc = 1; osc <= vital::kNumOscillators; ++osc) {
      const std::string prefix = "osc_" + std::to_string(osc) + "_";
      expectClose(controls.at(prefix + suffixes[0])->value(), 0, 0, "Secondary distortion defaults to None");
      expectClose(controls.at(prefix + suffixes[2])->value(), 0, 0, "Secondary morph defaults to None");
      for (int i = 0; i < 4; ++i)
        plugin.parameterChanged(prefix + suffixes[i], i == 0 ? O::kBend : i == 2 ? O::kRandomAmplitudes : .25f + osc * .05f);
    }
    for (const char* suffix : {"secondary_distortion_amount", "secondary_spectral_morph_amount"}) {
      const std::string id = std::string("osc_1_") + suffix;
      expect(plugin.connectModulation("macro_control_1", id), "Secondary amounts accept independent modulation routes");
      const int route = plugin.getConnectionIndex("macro_control_1", id);
      if (route >= 0) controls.at("modulation_" + std::to_string(route + 1) + "_amount")->set(.25f);
    }
    plugin.parameterChanged("macro_control_1", 1);
    AudioSampleBuffer audio(2, 128);
    MidiBuffer midi;
    midi.addEvent(MidiMessage::noteOn(1, 60, .8f), 0);
    for (int b = 0; b < 32; ++b) { audio.clear(); plugin.processBlock(audio, midi); midi.clear(); }
    for (const char* suffix : {"secondary_distortion_amount", "secondary_spectral_morph_amount"}) {
      const int route = plugin.getConnectionIndex("macro_control_1", std::string("osc_1_") + suffix);
      if (route >= 0) {
        auto* processor = plugin.getEngine()->getModulationBank().atIndex(route)->modulation_processor.get();
        expect(processor->output()->buffer[0][0] > .1f, "Modulation reaches the secondary amount at audio processing time");
      }
    }
    MemoryBlock state;
    plugin.getStateInformation(state);
    SynthPlugin restored;
    restored.setStateInformation(state.getData(), int(state.getSize()));
    for (int osc = 1; osc <= vital::kNumOscillators; ++osc) for (const char* suffix : suffixes) {
      const std::string id = "osc_" + std::to_string(osc) + "_" + suffix;
      expectClose(restored.getControls().at(id)->value(), controls.at(id)->value(), 0, "Secondary modes and amounts survive state reload");
    }
    expect(restored.getConnectionIndex("macro_control_1", "osc_1_secondary_distortion_amount") >= 0,
           "Secondary modulation route survives state reload");
    MemoryInputStream stream(state.getData(), state.getSize(), false);
    auto legacy = json::parse(stream.readEntireStreamAsString().toStdString());
    for (int osc = 1; osc <= vital::kNumOscillators; ++osc) for (const char* suffix : suffixes)
      legacy["settings"].erase("osc_" + std::to_string(osc) + "_" + suffix);
    legacy["settings"]["modulations"] = json::array();
    const auto text = legacy.dump();
    restored.setStateInformation(text.data(), int(text.size()));
    for (int osc = 1; osc <= vital::kNumOscillators; ++osc) {
      const auto prefix = "osc_" + std::to_string(osc) + "_";
      expectClose(restored.getControls().at(prefix + suffixes[0])->value(), 0, 0, "Old preset clears secondary distortion");
      expectClose(restored.getControls().at(prefix + suffixes[2])->value(), 0, 0, "Old preset clears secondary morph");
    }
    int legacy_index = 0;
    for (int i = 0; i < vital::Parameters::getNumParameters(); ++i) {
      const auto* details = vital::Parameters::getDetails(i);
      if (details->version_added >= 0x02010d ||
          (!controls.count(details->name) && !controls.count(SynthPlugin::legacyAliasTarget(details->name))))
        continue; // Legacy source aliases keep their number.
      for (auto* parameter : plugin.getParameters()) {
        auto* bridge = dynamic_cast<ValueBridge*>(parameter);
        if (bridge && bridge->getParameterId().toStdString() == details->name)
          expect(bridge->getParameterID() == String(legacy_index), "Original host automation ID remains stable");
      }
      ++legacy_index;
    }
    SynthEditor editor(plugin);
    for (const char* title : {"Secondary distortion mode", "Secondary Distortion Amount",
                              "Secondary Frequency Morph Mode", "Secondary Frequency Morph Amount"}) {
      auto* control = sliderWithTitle(editor, title);
      expect(control != nullptr, std::string("Accessible slider exists: ") + title);
      if (!control) continue;
      const auto& order = editor.getKeyboardFocusOrder();
      expect(std::find(order.begin(), order.end(), control) != order.end(), "Secondary slider is in keyboard focus order");
      expect(dynamic_cast<Slider*>(control)->createAccessibilityHandler() != nullptr, "Secondary slider exposes accessibility");
      expect(control->keyPressed(KeyPress(KeyPress::rightKey)), "Secondary slider handles arrow adjustment");
      expect(control->keyPressed(KeyPress(KeyPress::backspaceKey)), "Secondary slider handles reset");
    }
    expectClose(controls.at("osc_1_secondary_distortion_type")->value(), 0, 0, "Backspace disables the secondary distortion");
    expectClose(controls.at("osc_1_secondary_spectral_morph_type")->value(), 0, 0, "Backspace disables the secondary morph");
    std::cout << "Dual warp control tests: " << failures << " failures\n";
    return failures ? 1 : 0;
  }
  int benchmark() {
    for (int voices : {1, 16}) for (int config = 0; config < 5; ++config) {
      Fixture<O> f(48000, voices, 3);
      f.set(O::kDistortionType, config < 2 ? O::kSync : 0);
      f.set(O::kDistortionAmount, .4f);
      f.set(O::kSecondaryDistortionType, config == 1 ? O::kBend : 0);
      f.set(O::kSecondaryDistortionAmount, .6f);
      f.set(O::kSpectralMorphType, config >= 2 ? O::kVocode : 0);
      f.set(O::kSpectralMorphAmount, .4f);
      f.set(O::kSecondarySpectralMorphType, config == 3 ? O::kRandomAmplitudes : config == 4 ? O::kSkew : 0);
      f.set(O::kSecondarySpectralMorphAmount, .3f);
      for (int i = 0; i < 40; ++i) f.process(128);
      const auto begin = std::chrono::steady_clock::now();
      for (int i = 0; i < 1000; ++i) f.process(128);
      const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now()-begin).count()/1000;
      std::cout << "unison=" << voices << " config=" << config << " microseconds/128=" << us << '\n';
    }
    return 0;
  }

  void testIndependentModulationSources() {
    const int fm_types[] {O::kFmOscillatorA, O::kFmOscillatorB, O::kFmOscillatorC, O::kFmSample,
                          O::kFmGranular, O::kFmNoise, O::kFmSpectral};
    const int rm_types[] {O::kRmOscillatorA, O::kRmOscillatorB, O::kRmOscillatorC, O::kRmSample,
                          O::kRmGranular, O::kRmNoise, O::kRmSpectral};
    const auto withSources = [&](int primary, int secondary) {
      Fixture<O> f;
      std::array<vital::Output, 7> sources;
      for (int source = 0; source < 7; ++source)
        for (int i = 0; i < 128; ++i) sources[source].buffer[i] = .1f * (source + 1);
      for (int source = 0; source < 7; ++source)
        f.oscillator->setModulatorOutput(O::modulatorPosition(fm_types[source]), &sources[source]);
      f.set(O::kDistortionType, primary); f.set(O::kDistortionAmount, .5f);
      f.set(O::kSecondaryDistortionType, secondary); f.set(O::kSecondaryDistortionAmount, .5f);
      const auto audio = f.render();
      return std::vector<float>(audio.end() - 4096, audio.end());
    };
    const auto dry = withSources(0, 0);
    for (int i = 0; i < 7; ++i) {
      expect(difference(withSources(fm_types[i], 0), withSources(0, fm_types[i])) < 2e-5,
             "Secondary FM reads the selected source " + std::to_string(i));
      const auto ring = withSources(0, rm_types[i]);
      std::vector<float> expected = dry;
      for (auto& value : expected) value *= .5f + .05f * (i + 1);
      expect(difference(expected, ring) < 2e-5, "Secondary ring modulation uses the selected source");
      const auto both = withSources(rm_types[0], rm_types[i]);
      for (auto& value : expected) value *= .55f;
      expect(difference(expected, both) < 2e-5, "Each ring-modulation slot has its own source buffer");
    }
  }
  void testModeChangesAndNoteReset() {
    Fixture<O> f(48000, 16, 3);
    for (int mode = 0; mode < O::kNumDistortionTypes; ++mode) {
      f.set(O::kDistortionType, O::kSync);
      f.set(O::kSecondaryDistortionType, mode);
      f.set(O::kSpectralMorphType, O::kVocode);
      f.set(O::kSecondarySpectralMorphType, mode % O::kNumSpectralMorphTypes);
      for (float amount : {0.f, .5f, 1.f}) {
        f.set(O::kDistortionAmount, 1.f - amount);
        f.set(O::kSecondaryDistortionAmount, amount);
        f.set(O::kSpectralMorphAmount, amount);
        f.set(O::kSecondarySpectralMorphAmount, amount);
        auto* reset = f.controls[O::kReset].output();
        reset->trigger(vital::poly_mask(-1, -1, 0, 0), vital::kVoiceOn, 17);
        tracked_allocations.store(0); track_allocations.store(true);
        f.process(128);
        track_allocations.store(false);
        expect(tracked_allocations.load() == 0, "Mode changes and note resets allocate no audio memory");
        reset->trigger_mask = 0;
        for (int i = 0; i < 128; ++i) {
          const auto sample = f.oscillator->output(O::kRaw)->buffer[i];
          expect(vital::utils::isFinite(sample), "Mode changes and partial voice resets stay finite");
          if (i < 17) expect(sample[0] == 0 && sample[1] == 0, "Secondary modes respect note-on offsets");
        }
      }
    }
  }

  void testOscillatorSourceDependencies() {
    struct Producers : vital::ProducersModule {
      std::array<vital::Value, kNumInputs> inputs;
      Producers() {
        for (int i = 0; i < kNumInputs; ++i) plug(&inputs[i], i);
        init();
        setSampleRate(48000);
        inputs[kMidi].set(57); inputs[kZoneMidi].set(57); inputs[kZoneGlideMidi].set(57);
        inputs[kVelocity].set(1); inputs[kActiveVoices].set(1);
        inputs[kNotePressed].set(1); inputs[kNoteCount].set(1);
        const auto controls = getControls();
        for (int i = 1; i <= vital::kNumOscillators; ++i) {
          const std::string prefix = "osc_" + std::to_string(i) + "_";
          controls.at(prefix + "on")->set(1);
          controls.at(prefix + "type")->set(0); // Every slot renders as a wavetable oscillator here.
          controls.at(prefix + "random_phase")->set(0);
          controls.at(prefix + "unison_voices")->set(1);
          controls.at(prefix + "transpose")->set(i);
          vital::WaveFrame frame;
          for (int sample = 0; sample < vital::WaveFrame::kWaveformSize; ++sample)
            frame.time_domain[sample] = .7f * std::sin(2 * vital::kPi * sample / vital::WaveFrame::kWaveformSize);
          frame.toFrequencyDomain();
          getWavetable(i - 1)->loadWaveFrame(&frame);
        }
        prepareSelectedSources();
      }
      const vital::poly_float* raw(int index) { return slots_[index]->output(vital::SourceSlotModule::kRaw)->buffer; }
    };
    auto dry = std::make_unique<Producers>();
    auto chained = std::make_unique<Producers>();
    auto controls = chained->getControls();
    controls.at("osc_1_distortion_type")->set(O::kRmOscillatorA);
    controls.at("osc_1_distortion_amount")->set(1);
    controls.at("osc_1_secondary_distortion_type")->set(O::kRmOscillatorC);
    controls.at("osc_1_secondary_distortion_amount")->set(1);
    double energy = 0;
    double max_error = 0;
    for (int b = 0; b < 40; ++b) {
      dry->process(128); chained->process(128);
      if (b < 8) continue;
      for (int i = 0; i < 128; ++i) for (int lane = 0; lane < vital::poly_float::kSize; ++lane) {
        const float expected = dry->raw(0)[i][lane] * dry->raw(1)[i][lane] * dry->raw(3)[i][lane];
        energy += double(expected) * expected;
        max_error = std::max(max_error, std::abs(double(chained->raw(0)[i][lane]) - expected));
      }
    }
    expect(energy > .01, "Dual-source routing fixture produces audio");
    expect(max_error < 1e-4, "Both selected oscillators render before their modulation destination");
    controls.at("osc_2_secondary_distortion_type")->set(O::kFmOscillatorA);
    std::array<float, 128> previous {};
    double changing_audio = 0;
    for (int b = 0; b < 12; ++b) {
      chained->process(128);
      for (int i = 0; i < 128; ++i) {
        const float sample = chained->raw(0)[i][0];
        expect(std::isfinite(sample), "Cyclic modulation stays finite");
        changing_audio += std::abs(sample - previous[i]);
        previous[i] = sample;
      }
    }
    expect(changing_audio > .01, "A modulation cycle advances instead of repeating stale oscillator output");
  }

  void testShepardWrapKeepsDistortion() {
    Fixture<O> f(48000, 1, 3);
    f.set(O::kDistortionType, O::kPulseWidth);
    f.set(O::kDistortionAmount, .97f);
    f.set(O::kSecondarySpectralMorphType, O::kShepardTone);
    f.set(O::kSecondarySpectralMorphAmount, .99f);
    for (int i = 0; i < 20; ++i) f.process(128);
    f.set(O::kSecondarySpectralMorphAmount, .01f);
    int silent = 0;
    double energy = 0;
    for (int b = 0; b < 8; ++b) {
      f.process(128);
      for (int i = 0; i < 128; ++i) {
        const float sample = f.oscillator->output(O::kRaw)->buffer[i][0];
        silent += sample == 0.f;
        energy += double(sample) * sample;
      }
    }
    expect(energy > 1e-8, "Shepard/pulse transition fixture produces audio");
    expect(silent > 900, "Shepard octave transitions keep the narrow pulse window active");
  }

  int run() {
    testExamplesAndIndependentAmounts();
    testIndependentModulationSources();
    testOscillatorSourceDependencies();
    testModeChangesAndNoteReset();
    testShepardWrapKeepsDistortion();
    testBypassAndStandaloneSlots();
    testAllPairsAndAutomation();
    std::cout << "Dual oscillator warp tests: " << failures << " failures\n";
    return failures ? 1 : 0;
  }
}
