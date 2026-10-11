#pragma once
#include "release_velocity_glide.h"

namespace release_velocity_glide_tests {
  using namespace vital;

  struct Fixture {
    ReleaseVelocityGlide glide;
    cr::Output lift, retrigger;
    cr::Value mode {1}, time {1}, curve {0}, target {0};
    Fixture(int rate = 48000, int oversample = 1) {
      glide.plug(&lift, ReleaseVelocityGlide::kLift);
      glide.plug(&retrigger, ReleaseVelocityGlide::kRetrigger);
      glide.plug(&mode, ReleaseVelocityGlide::kMode);
      glide.plug(&time, ReleaseVelocityGlide::kTime);
      glide.plug(&curve, ReleaseVelocityGlide::kCurve);
      glide.plug(&target, ReleaseVelocityGlide::kTarget);
      glide.setSampleRate(rate);
      glide.setOversampleAmount(oversample);
    }
    poly_float process(int count) {
      tracked_allocations = 0;
      track_allocations = true;
      glide.process(count);
      track_allocations = false;
      expect(tracked_allocations == 0, "release glide must not allocate during audio processing");
      lift.trigger_mask = 0;
      retrigger.trigger_mask = 0;
      return glide.output()->buffer[0];
    }
  };

  void testTimingAndCurve() {
    for (int target : {0, 1}) for (int rate : {44100, 48000, 96000}) for (int oversample : {1, 4})
      for (int block : {1, 17, 128}) for (float curve : {-4.0f, 0.0f, 4.0f}) {
        Fixture f(rate, oversample);
        f.target.set(target);
        f.curve.set(curve);
        f.lift.trigger(constants::kFullMask, target ? 0 : 1, 0);
        const int total = rate * oversample;
        int elapsed = 0;
        for (int checkpoint : {total / 4, total / 2, total, total + 128}) {
          poly_float value = 0;
          while (elapsed < checkpoint) {
            const int count = std::min(block * oversample, checkpoint - elapsed);
            value = f.process(count);
            elapsed += count;
          }
          const double phase = std::min(1.0, double(elapsed) / total);
          const double expected = curve == 0 ? phase : std::expm1(curve * phase) / std::expm1(curve);
          expectClose(value[0], float(expected), 2e-5f, "glide curve and completion time must not depend on block size, rate or oversampling");
        }
      }
  }

  void testPolyphonyOffsetsAndInstant() {
    Fixture f;
    f.lift.trigger(poly_mask(-1, -1, 0, 0), 0.8f, 13);
    f.lift.trigger(poly_mask(0, 0, -1, -1), 0.4f, 61);
    auto value = f.process(128);
    expectClose(value[0], .8f * 115 / 48000, 1e-7f, "first voice glide starts at its note-off offset");
    expectClose(value[2], .4f * 67 / 48000, 1e-7f, "second voice glide starts independently");
    f.retrigger.trigger(poly_mask(-1, -1, 0, 0), kVoiceOn, 0);
    f.lift.trigger_value = poly_float(0, 0, .4f, .4f);
    value = f.process(128);
    expectClose(value[0], 0, 0, "retriggered voice does not inherit its old release velocity");
    expectClose(value[2], .4f * 195 / 48000, 1e-7f, "retriggering one voice leaves the other glide running");
    f.mode.set(0);
    f.lift.trigger_value = poly_float(.3f, .3f, .7f, .7f);
    value = f.process(17);
    expectClose(value[0], .3f, 0, "Instant preserves raw release velocity");
    expectClose(value[2], .7f, 0, "Instant preserves each voice's velocity");
    f.mode.set(1); f.time.set(0);
    f.lift.trigger_value = .9f;
    expectClose(f.process(128)[0], .9f, 0, "zero glide time is instant");
    f.glide.hardReset();
    f.time.set(1); f.lift.trigger_value = 0;
    expectClose(f.process(128)[0], 0, 0, "hard reset clears release glide state");
  }

  void testFullAmountReleaseState() {
    const poly_mask first_voice(-1, -1, 0, 0);
    for (int mode : {0, 1}) {
      Fixture f;
      f.mode.set(mode);
      f.target.set(1);
      f.time.set(.01f);
      f.retrigger.trigger(constants::kFullMask, kVoiceOn, 0);
      expectClose(f.process(128)[0], 0, 0, "Full amount stays zero while a note is held");
      f.lift.trigger(first_voice, 0, 13);
      auto value = f.process(128);
      expectClose(value[0], mode ? 115.0f / 480 : 1, 1e-6f,
                  "zero-velocity release reaches Full amount with the correct timing");
      expectClose(value[2], 0, 0, "releasing one voice does not modulate another held voice");
      for (int i = 0; i < 4; ++i) value = f.process(128);
      expectClose(value[0], 1, 0, "Full amount settles exactly at one and remains there");
      f.retrigger.trigger(first_voice, kVoiceOn, 37);
      value = f.process(128);
      expectClose(value[0], 0, 0, "a new note clears the previous Full amount release");
      f.lift.trigger(first_voice, .25f, 0);
      for (int i = 0; i < 4; ++i) value = f.process(128);
      f.target.set(0);
      for (int i = 0; i < 4; ++i) value = f.process(128);
      expectClose(value[0], .25f, 0, "switching back restores MIDI release velocity depth");
      f.target.set(1);
      for (int i = 0; i < 4; ++i) value = f.process(128);
      expectClose(value[0], 1, 0, "switching target on a released note preserves its release state");
      f.glide.hardReset();
      expectClose(f.process(128)[0], 0, 0, "hard reset clears the Full amount release latch");
    }
  }

  void testPluginPitchAndState() {
    SynthPlugin plugin;
    plugin.setPlayConfigDetails(0, 2, 48000, 128);
    plugin.prepareToPlay(48000, 128);
    const auto controls = plugin.getControls();
    controls.at("oversampling")->set(0); plugin.checkOversampling();
    controls.at("release_velocity_mode")->set(1);
    controls.at("release_velocity_glide_time")->set(1);
    controls.at("env_1_release")->set(2);
    expect(plugin.connectModulation("lift", "osc_1_transpose"), "release velocity connects to oscillator pitch");
    const int index = plugin.getConnectionIndex("lift", "osc_1_transpose");
    if (index < 0) return;
    auto* route = plugin.getEngine()->getModulationBank().atIndex(index)->modulation_processor.get();
    controls.at("modulation_" + std::to_string(index + 1) + "_amount")->set(
        12.0f / Parameters::getParameterRange("osc_1_transpose"));
    AudioSampleBuffer audio(2, 128);
    MidiBuffer midi;
    const auto render = [&](int count) {
      while (count > 0) {
        const int block = std::min(count, 128);
        audio.setSize(2, block, false, false, true);
        audio.clear(); plugin.processBlock(audio, midi); midi.clear();
        count -= block;
      }
    };
    midi.addEvent(MidiMessage::noteOn(1, 60, 1.0f), 0);
    render(4800);
    expectClose(route->output()->buffer[0][0], 0, 1e-5f, "held note has no release pitch offset");
    midi.addEvent(MidiMessage::noteOff(1, 60, 1.0f), 0);
    render(24000);
    expectClose(route->output()->buffer[0][0], 6, .02f, "one-second linear release glide reaches six semitones halfway");
    render(24000);
    expectClose(route->output()->buffer[0][0], 12, .02f, "release glide reaches twelve semitones at one second");

    controls.at("release_velocity_glide_curve")->set(-3.5f);
    controls.at("release_velocity_target")->set(1);
    MemoryBlock state;
    plugin.getStateInformation(state);
    SynthPlugin restored;
    restored.setStateInformation(state.getData(), int(state.getSize()));
    for (const char* id : {"release_velocity_mode", "release_velocity_glide_time", "release_velocity_glide_curve", "release_velocity_target"})
      expectClose(restored.getControls().at(id)->value(), controls.at(id)->value(), 0, "release glide settings survive state reload");
    SynthPlugin fresh;
    expectClose(fresh.getControls().at("release_velocity_mode")->value(), 0, 0, "new presets default to Instant");
    expectClose(fresh.getControls().at("release_velocity_target")->value(), 0, 0, "new presets default to MIDI release velocity");
    MemoryInputStream stream(state.getData(), state.getSize(), false);
    auto legacy = json::parse(stream.readEntireStreamAsString().toStdString());
    for (const char* id : {"release_velocity_mode", "release_velocity_glide_time", "release_velocity_glide_curve", "release_velocity_target"})
      legacy["settings"].erase(id);
    const auto legacy_state = legacy.dump();
    restored.setStateInformation(legacy_state.data(), int(legacy_state.size()));
    expectClose(restored.getControls().at("release_velocity_mode")->value(), 0, 0, "legacy presets restore Instant even after a Glide preset");
    expectClose(restored.getControls().at("release_velocity_glide_time")->value(), .25f, 0, "legacy presets restore the default glide time");
    expectClose(restored.getControls().at("release_velocity_glide_curve")->value(), 0, 0, "legacy presets restore a linear curve");
    expectClose(restored.getControls().at("release_velocity_target")->value(), 0, 0, "legacy presets preserve MIDI release velocity depth after a Full amount preset");

    int old_index = 0;
    for (int i = 0; i < Parameters::getNumParameters(); ++i) {
      const auto* details = Parameters::getDetails(i);
      // Include every parameter predating the release target, including dual warps.
      if (details->version_added >= 0x02010e ||
          (!controls.count(details->name) && !controls.count(SynthPlugin::legacyAliasTarget(details->name))))
        continue; // Legacy source aliases keep their number.
      for (auto* parameter : plugin.getParameters()) {
        auto* bridge = dynamic_cast<ValueBridge*>(parameter);
        if (bridge && bridge->getParameterId().toStdString() == details->name)
          expect(bridge->getParameterID() == String(old_index), "existing host parameter IDs must remain stable");
      }
      ++old_index;
    }
  }

  void testEditor() {
    SynthPlugin plugin;
    SynthEditor editor(plugin);
    const String titles[] {"Release velocity target", "Release velocity mode", "Release velocity glide time", "Release velocity glide curve",
                            "Voice Transpose", "Voice Tune", "Voice Amplitude"};
    const auto& order = editor.getKeyboardFocusOrder();
    size_t previous = 0;
    for (const auto& title : titles) {
      auto* control = filter_routing_tests::withTitle(editor, title);
      expect(control != nullptr, "editor exposes " + title.toStdString());
      if (!control) continue;
      const auto found = std::find(order.begin(), order.end(), control);
      const auto position = size_t(std::distance(order.begin(), found));
      expect(found != order.end() && position > previous, "release glide controls precede voice pitch and amplitude in focus order");
      previous = position;
    }
    auto* target = dynamic_cast<Slider*>(filter_routing_tests::withTitle(editor, titles[0]));
    auto* mode = dynamic_cast<Slider*>(filter_routing_tests::withTitle(editor, titles[1]));
    auto* time = dynamic_cast<Slider*>(filter_routing_tests::withTitle(editor, titles[2]));
    auto* curve = dynamic_cast<Slider*>(filter_routing_tests::withTitle(editor, titles[3]));
    if (!target || !mode || !time || !curve) return;
    expect(target->getTextFromValue(target->getValue()) == "MIDI release velocity", "target announces the velocity-sensitive choice");
    target->keyPressed(KeyPress(KeyPress::rightKey));
    expect(target->getTextFromValue(target->getValue()) == "Full amount", "target announces Full amount rather than On");
    expectClose(plugin.getControls().at("release_velocity_target")->value(), 1, 0, "keyboard target selection reaches the engine");
    target->createAccessibilityHandler()->getValueInterface()->setValueAsString("MIDI release velocity");
    expectClose(plugin.getControls().at("release_velocity_target")->value(), 0, 0, "target supports accessible text selection");
    mode->keyPressed(KeyPress(KeyPress::rightKey));
    expect(mode->getTextFromValue(mode->getValue()) == "Glide", "mode announces Glide rather than On");
    auto handler = time->createAccessibilityHandler();
    auto* value = handler->getValueInterface();
    value->setValueAsString("500 ms");
    time->keyPressed(KeyPress(KeyPress::rightKey));
    expectClose(plugin.getControls().at("release_velocity_glide_time")->value(), .51f, 1e-5f, "time supports typed milliseconds and ten-ms keyboard steps");
    curve->keyPressed(KeyPress(KeyPress::rightKey));
    expectClose(plugin.getControls().at("release_velocity_glide_curve")->value(), .1f, 1e-5f, "curve keyboard steps are one tenth");
  }

  void testRenderedTune() {
    for (float curve : {-4.0f, 0.0f, 4.0f}) {
      SynthPlugin plugin;
      plugin.setPlayConfigDetails(0, 2, 48000, 128);
      plugin.prepareToPlay(48000, 128);
      const auto controls = plugin.getControls();
      controls.at("osc_1_on")->set(1);
      controls.at("osc_1_unison_voices")->set(1);
      controls.at("osc_1_random_phase")->set(0);
      controls.at("osc_1_destination")->set(constants::kDirectOut);
      controls.at("env_1_attack")->set(0);
      controls.at("env_1_release")->set(2);
      plugin.getWavetable(0)->loadWaveFrame(PredefinedWaveFrames::getWaveFrame(PredefinedWaveFrames::kSin), 0);
      SynthEditor editor(plugin);
      const auto setUI = [&](const char* title, const String& value) {
        auto* slider = dynamic_cast<Slider*>(filter_routing_tests::withTitle(editor, title));
        expect(slider != nullptr, std::string("missing control: ") + title);
        if (slider) slider->createAccessibilityHandler()->getValueInterface()->setValueAsString(value);
      };
      setUI("Release velocity mode", "Glide");
      setUI("Release velocity glide time", "1 secs");
      setUI("Release velocity glide curve", String(curve));
      expectClose(controls.at("release_velocity_mode")->value(), 1, 0, "UI selects Glide in the engine");
      plugin.connectModulation("lift", "osc_1_tune");
      const int index = plugin.getConnectionIndex("lift", "osc_1_tune");
      if (index < 0) continue;
      controls.at("modulation_" + std::to_string(index + 1) + "_amount")->set(.5f); // +100 cents.
      AudioSampleBuffer audio(2, 128);
      MidiBuffer midi;
      std::vector<float> output;
      const auto render = [&](int count) {
        for (int offset = 0; offset < count; offset += 128) {
          const int block = std::min(128, count - offset);
          audio.setSize(2, block, false, false, true);
          audio.clear(); plugin.processBlock(audio, midi); midi.clear();
          output.insert(output.end(), audio.getReadPointer(0), audio.getReadPointer(0) + block);
        }
      };
      midi.addEvent(MidiMessage::noteOn(1, 69, 1.0f), 0);
      render(4800);
      output.clear();
      midi.addEvent(MidiMessage::noteOff(1, 69, 1.0f), 0);
      render(57600);
      for (double seconds : {.25, .5, 1.1}) {
        std::vector<double> crossings;
        const int center = int(seconds * 48000);
        for (int i = center - 960; i < center + 960; ++i)
          if (output[i] < 0 && output[i + 1] >= 0)
            crossings.push_back(i - output[i] / double(output[i + 1] - output[i]));
        expect(crossings.size() > 8, "rendered oscillator has enough audible cycles to measure pitch");
        if (crossings.size() < 2) continue;
        const double hz = (crossings.size() - 1) * 48000 / (crossings.back() - crossings.front());
        const double cents = 1200 * std::log2(hz / 440);
        const double phase = std::min(1.0, seconds);
        const double expected = 100 * (curve == 0 ? phase : std::expm1(curve * phase) / std::expm1(curve));
        std::cout << "Tune audio curve=" << curve << " seconds=" << seconds << " cents=" << cents << " expected=" << expected << '\n';
        expectClose(cents, expected, 1.0f, "rendered oscillator tune follows the UI glide time and curve");
      }
    }
  }

  void testGlobalPitchReleaseEndpoints() {
    for (int target : {0, 1}) for (int mode : {0, 1}) for (int velocity : {-1, 0, 32, 64, 96, 127}) {
      SynthPlugin plugin;
      plugin.setPlayConfigDetails(0, 2, 48000, 128);
      plugin.prepareToPlay(48000, 128);
      const auto controls = plugin.getControls();
      controls.at("oversampling")->set(0); plugin.checkOversampling();
      controls.at("osc_1_on")->set(1);
      controls.at("osc_1_unison_voices")->set(1);
      controls.at("osc_1_random_phase")->set(0);
      controls.at("osc_1_destination")->set(constants::kDirectOut);
      controls.at("env_1_attack")->set(0);
      controls.at("env_1_release")->set(2);
      controls.at("release_velocity_mode")->set(mode);
      controls.at("release_velocity_target")->set(target);
      controls.at("release_velocity_glide_time")->set(.1f);
      plugin.getWavetable(0)->loadWaveFrame(PredefinedWaveFrames::getWaveFrame(PredefinedWaveFrames::kSin), 0);
      expect(plugin.connectModulation("lift", "voice_transpose"), "release velocity connects to global pitch");
      const int index = plugin.getConnectionIndex("lift", "voice_transpose");
      if (index < 0) continue;
      auto* route = plugin.getEngine()->getModulationBank().atIndex(index)->modulation_processor.get();
      controls.at("modulation_" + std::to_string(index + 1) + "_amount")->set(
          12.0f / Parameters::getParameterRange("voice_transpose"));
      AudioSampleBuffer audio(2, 128);
      MidiBuffer midi;
      std::vector<float> output;
      const auto render = [&](int samples) {
        while (samples > 0) {
          const int block = std::min(128, samples);
          audio.setSize(2, block, false, false, true);
          audio.clear(); plugin.processBlock(audio, midi); midi.clear();
          output.insert(output.end(), audio.getReadPointer(0), audio.getReadPointer(0) + block);
          samples -= block;
        }
      };
      midi.addEvent(MidiMessage::noteOn(1, 69, 1.0f), 0);
      render(4800);
      expectClose(route->output()->buffer[0][0], 0, 1e-5f, "held note has no global release pitch offset");
      output.clear();
      // Exercise the zero-velocity note-on form of note-off too, and defer
      // explicit zero-velocity note-offs with sustain in both target modes.
      if (velocity == 0) {
        midi.addEvent(MidiMessage::controllerEvent(1, 64, 127), 0);
        render(128);
      }
      midi.addEvent(velocity < 0 ? MidiMessage::noteOn(1, 69, static_cast<uint8>(0))
                                : MidiMessage::noteOff(1, 69, static_cast<uint8>(velocity)), 0);
      if (velocity == 0) {
        render(2400);
        expectClose(route->output()->buffer[0][0], 0, 1e-5f, "sustain postpones release modulation until pedal-up");
        midi.addEvent(MidiMessage::controllerEvent(1, 64, 0), 0);
      }
      render(14400);
      const float expected = target ? 12.0f : 12.0f * std::max(0, velocity) / 127.0f;
      expectClose(route->output()->buffer[0][0], expected, 1e-4f,
                  "global pitch route reaches the selected endpoint in Instant and Glide");
      std::vector<double> crossings;
      for (int i = int(output.size()) - 2048; i + 1 < int(output.size()); ++i)
        if (output[i] < 0 && output[i + 1] >= 0)
          crossings.push_back(i - output[i] / double(output[i + 1] - output[i]));
      expect(crossings.size() > 8, "global pitch endpoint has enough audible cycles to measure");
      if (crossings.size() < 2) continue;
      const double hz = (crossings.size() - 1) * 48000 / (crossings.back() - crossings.front());
      const double semitones = 12 * std::log2(hz / 440);
      std::cout << "Global pitch target=" << target << " mode=" << mode << " release velocity=" << velocity
                << " endpoint=" << semitones << " expected=" << expected << '\n';
      expectClose(semitones, expected, .02f, "rendered global pitch reaches its release velocity endpoint");
    }
  }

  int run() {
    ScopedJuceInitialiser_GUI gui;
    testTimingAndCurve();
    testPolyphonyOffsetsAndInstant();
    testFullAmountReleaseState();
    testPluginPitchAndState();
    testEditor();
    testRenderedTune();
    testGlobalPitchReleaseEndpoints();
    std::cout << "Release velocity glide: " << failures << " failures\n";
    return failures ? 1 : 0;
  }
}
