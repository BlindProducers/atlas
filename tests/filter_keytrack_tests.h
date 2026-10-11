// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "filter_module.h"

namespace filter_keytrack {
  int failures = 0;
  void check(bool value, const std::string& message) {
    if (!value) { ++failures; std::cerr << "Filter keytrack: " << message << '\n'; }
  }

  struct Fixture {
    vital::FilterModule filter;
    vital::cr::Value keytrack;
    vital::Output audio, reset;
    std::string prefix;
    vital::Output* readout;
    int position = 0;
    Fixture(bool mono, int rate, int model, int oversampling = 1) :
        filter(mono ? "filter_fx" : "filter_1"), prefix(mono ? "filter_fx" : "filter_1") {
      filter.setMono(mono);
      filter.plug(&keytrack, vital::FilterModule::kKeytrack);
      filter.plug(&audio, vital::FilterModule::kAudio);
      filter.plug(&reset, vital::FilterModule::kReset);
      filter.init();
      filter.setSampleRate(rate);
      filter.setOversampleAmount(oversampling);
      audio.ensureBufferSize(vital::kMaxBufferSize * oversampling);
      auto controls = filter.getControls();
      controls.at(prefix + "_on")->set(1);
      controls.at(prefix + "_model")->set(model);
      controls.at(prefix + "_style")->set(0);
      controls.at(prefix + "_cutoff")->set(60);
      controls.at(prefix + "_keytrack")->set(1);
      controls.at(prefix + "_resonance")->set(.85f);
      controls.at(prefix + "_mix")->set(1);
      readout = mono ? filter.getMonoModulations().at(prefix + "_cutoff") :
                       filter.getPolyModulations().at(prefix + "_cutoff");
      for (int i = 0; i < 100; ++i) process(128 * oversampling);
    }
    void process(int count) {
      for (int i = 0; i < count; ++i, ++position)
        audio.buffer[i] = .15f * std::sin(position * .031f) + .1f * std::sin(position * .17f);
      filter.process(count);
      reset.clearTrigger();
    }
  };

  std::vector<vital::poly_float> trajectory(bool mono, int rate, int model, int block, int oversampling = 1) {
    Fixture f(mono, rate, model, oversampling);
    const int effective_rate = rate * oversampling;
    const int event_spacing = 223 * oversampling;
    const int total = event_spacing * 6 + effective_rate / 25;
    const vital::poly_float base = f.readout->buffer[0];
    std::vector<vital::poly_float> result;
    result.reserve(total);
    for (int offset = 0; offset < total;) {
      if (offset % event_spacing == 0 && offset < event_spacing * 6) {
        const float wheel[] { 1, 1.5f, -.5f, -1, .25f, 0 };
        f.keytrack.set(vital::poly_float(12, -12, 24, -24) * wheel[offset / event_spacing]);
      }
      const int to_event = offset < event_spacing * 6 ? event_spacing - offset % event_spacing : total - offset;
      const int count = std::min({block * oversampling, to_event, total - offset});
      f.process(count);
      for (int i = 0; i < count; ++i) result.push_back(f.readout->buffer[i] - base);
      offset += count;
    }
    return result;
  }

  void testEventPartitioning() {
    float worst_difference = 0, worst_jump = 0;
    for (bool mono : {false, true}) for (int rate : {44100, 48000, 96000})
      for (int oversampling : {1, 2, 4}) {
        const auto single = trajectory(mono, rate, vital::constants::kComb, 1, oversampling);
        for (int block : {17, 128}) {
          const auto chunked = trajectory(mono, rate, vital::constants::kComb, block, oversampling);
          float difference = 0, jump = 0;
          for (int i = 0; i < single.size(); ++i) for (int lane = 0; lane < vital::poly_float::kSize; ++lane) {
            difference = std::max(difference, std::abs(single[i][lane] - chunked[i][lane]));
            jump = std::max(jump, std::abs(chunked[i][lane] - (i ? chunked[i-1][lane] : 0)));
          }
          worst_difference = std::max(worst_difference, difference);
          worst_jump = std::max(worst_jump, jump);
          const std::string context = std::string(mono ? "FX" : "voice") + " rate=" + std::to_string(rate) +
                                      " oversampling=" + std::to_string(oversampling) + " block=" + std::to_string(block);
          check(difference < .0001f, "MIDI event partitions must not alter smoothing: " + context);
          check(jump < .4f / oversampling, "bend changes must not produce cutoff jumps: " + context);
        }
        check(std::abs(single.back()[0]) < .01f, "returning the wheel to centre must settle promptly");
      }
    std::cout << "Keytrack maximum partition difference=" << worst_difference << " semitones; maximum sample step="
              << worst_jump << " semitones\n";
  }

  void testModelsAndFreshVoices() {
    for (int model = 0; model < vital::constants::kNumFilterModels; ++model) {
      Fixture f(false, 48000, model);
      f.keytrack.set(vital::poly_float(12, 12, -12, -12));
      tracked_allocations.store(0);
      track_allocations.store(true);
      f.process(1);
      track_allocations.store(false);
      check(tracked_allocations.load() == 0, "moving keytrack must not allocate on the audio thread");
      check(f.readout->buffer[0][0] > 0 && f.readout->buffer[0][0] < .2f,
            "every filter model must start a bend with a small immediate movement");
      f.keytrack.set(vital::poly_float(24, 24, -12, -12));
      f.reset.trigger(vital::constants::kFirstMask, vital::kVoiceOn, 17);
      f.process(64);
      check(std::abs(f.readout->buffer[17][0] - 24) < .0001f,
            "fresh voices must start at their own keytracked pitch at the note offset");
      check(f.readout->buffer[17][2] > -12 && f.readout->buffer[17][2] < 0,
            "a fresh voice must not reset another voice's pitch movement");

      // An envelope/LFO cutoff route must retain its exact sample-level shape.
      vital::Value modulation;
      auto* sum = f.filter.getPolyModulationDestination(f.prefix + "_cutoff");
      sum->plugNext(&modulation);
      for (int i = 0; i < 64; ++i) modulation.output()->buffer[i] = i % 2 ? 7.f : -7.f;
      f.process(64);
      for (int i = 0; i < 64; ++i)
        check(std::abs(f.readout->buffer[i][0] - (24 + (i % 2 ? 7.f : -7.f))) < .0001f,
              "keytrack smoothing must preserve audio-rate cutoff modulation");
      sum->unplug(&modulation);
    }
  }

  void testMidiThroughPlugin() {
    for (bool legato : {false, true}) {
      SynthPlugin plugin;
      plugin.setPlayConfigDetails(0, 2, 48000, 128);
      plugin.prepareToPlay(48000, 128);
      auto set = [&](const std::string& id, float value) { plugin.getControls().at(id)->set(value); };
      set("oversampling", 0); plugin.checkOversampling();
      set("polyphony", legato ? 1 : 8); set("legato", legato ? 1 : 0);
      set("pitch_bend_range", 24); set("env_1_release", 1);
      set("env_1_attack", .1f); set("env_1_sustain", 1);
      std::vector<std::string> prefixes { "filter_1", "filter_2" };
      for (int bus = 0; bus <= vital::kNumBuses; ++bus) {
        const std::string chain = bus ? "bus_" + std::to_string(bus) + "_" : "";
        set(chain + "fx_slot_1_type", vital::constants::kFilterFx + 1);
        set(chain + "fx_slot_1_enabled", 1);
        prefixes.push_back(chain + "fx_slot_1_filter_fx");
      }
      for (const auto& prefix : prefixes) {
        if (prefix == "filter_1" || prefix == "filter_2") set(prefix + "_on", 1);
        set(prefix + "_model", vital::constants::kComb);
        set(prefix + "_style", 0); set(prefix + "_cutoff", 60);
        set(prefix + "_resonance", .85f); set(prefix + "_keytrack", 1);
      }
      AudioSampleBuffer audio(2, 128);
      MidiBuffer midi;
      bool finite = true;
      auto process = [&] {
        audio.clear(); plugin.processBlock(audio, midi); midi.clear();
        for (int i = 0; i < 128; ++i)
          finite = finite && std::isfinite(audio.getSample(0, i)) && std::isfinite(audio.getSample(1, i));
      };
      midi.addEvent(MidiMessage::noteOn(1, 60, .8f), 0);
      if (!legato) midi.addEvent(MidiMessage::noteOn(1, 67, .8f), 19);
      for (int i = 0; i < 100; ++i) process();
      auto readouts = plugin.getEngine()->getPolyModulations();
      const auto mono = plugin.getEngine()->getMonoModulations();
      std::vector<vital::Output*> values;
      for (int i = 0; i < prefixes.size(); ++i)
        values.push_back((i < 2 ? readouts : mono).at(prefixes[i] + "_cutoff"));
      std::vector<float> previous;
      for (auto* value : values) previous.push_back(value->buffer[0][0]);
      // Placing the wheel event last leaves precisely one sample to render:
      // this was the worst case for the former block-length ramp.
      midi.addEvent(MidiMessage::pitchWheel(1, 12288), 127);
      process();
      for (int i = 0; i < values.size(); ++i) {
        const float movement = values[i]->buffer[0][0] - previous[i];
        check(movement >= 0 && movement < .2f, "a final-sample MIDI bend must be smooth in " + prefixes[i]);
      }
      for (int i = 0; i < 20; ++i) process();
      for (int i = 0; i < values.size(); ++i)
        check(std::abs(values[i]->buffer[0][0] - previous[i] - 12) < .02f,
              "the actual MIDI bend must reach the correct pitch in " + prefixes[i]);
      midi.addEvent(MidiMessage::noteOff(1, 60), 0);
      if (!legato) midi.addEvent(MidiMessage::noteOff(1, 67), 0);
      process();
      previous.clear();
      for (auto* value : values) previous.push_back(value->buffer[0][0]);
      midi.addEvent(MidiMessage::pitchWheel(1, 8192), 127);
      process();
      for (int i = 0; i < values.size(); ++i)
        check(std::abs(values[i]->buffer[0][0] - previous[i]) < .2f,
              "pitch bend during release must remain smooth in " + prefixes[i]);
      check(finite, "the MIDI regression must render finite audio in mono and poly modes");
    }
  }

  int run() {
    ScopedJuceInitialiser_GUI gui;
    testEventPartitioning();
    testModelsAndFreshVoices();
    testMidiThroughPlugin();
    std::cout << "Filter keytracking: " << failures << " failures\n";
    return failures ? 1 : 0;
  }
}
