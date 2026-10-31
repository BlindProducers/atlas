// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "unison_phase_tests.h"

namespace unison_fundamental_tests {
  using O = vital::SynthOscillator;
  using oscillator_warp_tests::Fixture;
  using random_amplitude_seed_tests::findControl;

  constexpr int kSampleRate = 48000;
  constexpr double kNoteFrequency = 220.0;  // MIDI note 57, which the fixture plays.

  // One second of the left lane, after the buffers settle.
  std::vector<float> render(int voices, bool remove, float frame_spread = 0.0f) {
    Fixture<O> f(kSampleRate, voices, 1);
    f.set(O::kUnisonDetune, 0.0f);
    f.set(O::kUnisonFrameSpread, frame_spread);
    f.set(O::kUnisonRemoveFundamental, remove ? 1.0f : 0.0f);
    f.render(40);
    const auto audio = f.render(kSampleRate / 128);
    std::vector<float> left;
    for (size_t i = 0; i < audio.size(); i += vital::poly_float::kSize)
      left.push_back(audio[i]);
    return left;
  }

  // Magnitude of one harmonic of the note over the whole second (a whole number of cycles).
  double harmonic(const std::vector<float>& audio, int number) {
    double real = 0.0, imaginary = 0.0;
    for (size_t i = 0; i < audio.size(); ++i) {
      const double phase = 2.0 * vital::kPi * kNoteFrequency * number * i / kSampleRate;
      real += audio[i] * std::cos(phase);
      imaginary += audio[i] * std::sin(phase);
    }
    return std::sqrt(real * real + imaginary * imaginary) / audio.size();
  }

  struct Ratios { double fundamental, second; };
  Ratios ratios(int voices, float frame_spread = 0.0f) {
    const auto off = render(voices, false, frame_spread);
    const auto on = render(voices, true, frame_spread);
    return { harmonic(on, 1) / harmonic(off, 1), harmonic(on, 2) / harmonic(off, 2) };
  }

  void testDsp() {
    // No detune and no random phase: the voices add in phase, so the fundamental
    // left is the middle voice's share. Each side has the middle voice at the
    // blend's centre amplitude and three detuned voices: 0.52 / (0.52 + 3 * 0.576).
    const auto odd = ratios(7);
    std::cout << "7 voices, fundamental on/off " << odd.fundamental << ", second harmonic " << odd.second << '\n';
    expectClose(odd.fundamental, 0.52 / (0.52 + 3 * 0.576), 0.01, "With 7 voices only the middle one keeps its fundamental");
    expectClose(odd.second, 1.0, 1e-3, "Remove Fundamental leaves the second harmonic alone");

    const auto even = ratios(6);
    std::cout << "6 voices, fundamental on/off " << even.fundamental << ", second harmonic " << even.second << '\n';
    expect(even.fundamental < 1e-3, "With an even count no voice keeps its fundamental");
    expectClose(even.second, 1.0, 1e-3, "An even count keeps its second harmonic");

    expect(render(1, true) == render(1, false), "A single voice is on pitch and keeps everything");

    // A frame spread gives every voice a buffer of its own. The test frames
    // differ above the fundamental only, so the middle voice keeps the same share.
    expect(oscillator_warp_tests::difference(render(7, true, 3.0f), render(7, true)) > 1e-3,
           "A frame spread gives the voices frames of their own");
    const auto spread_odd = ratios(7, 3.0f);
    const auto spread_even = ratios(6, 3.0f);
    std::cout << "With frame spread: 7 voices " << spread_odd.fundamental << ", 6 voices " << spread_even.fundamental << '\n';
    expect(spread_odd.fundamental > 0.05 && spread_odd.fundamental < 0.5,
           "With per-voice buffers the middle voice still keeps its fundamental");
    expect(spread_even.fundamental < 1e-3, "With per-voice buffers an even count keeps none");
    expect(spread_even.second > 0.5, "With per-voice buffers the second harmonic stays");
  }

  int runControls() {
    juce::ScopedJuceInitialiser_GUI gui;
    SynthPlugin plugin;
    const auto& controls = plugin.getControls();
    for (int osc = 1; osc <= vital::kNumOscillators; ++osc)
      expectClose(controls.at("osc_" + std::to_string(osc) + "_unison_remove_fundamental")->value(), 0, 0,
                  "Remove Fundamental defaults to Off");

    plugin.parameterChanged("osc_1_unison_remove_fundamental", 1);
    MemoryBlock state;
    plugin.getStateInformation(state);
    SynthPlugin restored;
    restored.setStateInformation(state.getData(), int(state.getSize()));
    expectClose(restored.getControls().at("osc_1_unison_remove_fundamental")->value(), 1, 0,
                "Remove Fundamental survives state reload");
    MemoryInputStream stream(state.getData(), state.getSize(), false);
    auto preset = json::parse(stream.readEntireStreamAsString().toStdString());
    preset["settings"].erase("osc_1_unison_remove_fundamental");
    const auto old_preset = preset.dump();
    restored.setStateInformation(old_preset.data(), int(old_preset.size()));
    expectClose(restored.getControls().at("osc_1_unison_remove_fundamental")->value(), 0, 0,
                "An older preset loads with Remove Fundamental off");

    SynthEditor editor(plugin);
    auto* control = findControl(editor, "1 Unison Remove Fundamental");
    expect(control != nullptr, "A Wavetable oscillator shows Remove Fundamental");
    if (control != nullptr) {
      const auto& order = editor.getKeyboardFocusOrder();
      expect(std::find(order.begin(), order.end(), control) != order.end(), "Remove Fundamental is keyboard reachable");
      expect(control->createAccessibilityHandler() != nullptr, "Remove Fundamental exposes accessibility");
    }
    plugin.parameterChanged("osc_1_type", O::kAnalogType);
    unison_phase_tests::pumpEditor();
    expect(findControl(editor, "1 Unison Remove Fundamental") == nullptr,
           "An Analog oscillator, which has no wave buffers, hides Remove Fundamental");
    std::cout << "Unison remove fundamental control tests: " << failures << " failures\n";
    return failures ? 1 : 0;
  }

  int run() {
    testDsp();
    return failures ? 1 : 0;
  }
}
