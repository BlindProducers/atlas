// SPDX-License-Identifier: GPL-3.0-or-later
#include "JuceHeader.h"
#include "../slot_test_ids.h"
#include "synth_plugin.h"
#include "sound_engine.h"
#include "defuse_module.h"
#include "sample_source.h"
#include <array>
#include <cmath>
#include <iostream>

namespace {
  int failures = 0;
  void expect(bool ok, const char* message) {
    if (!ok) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
  }
  void set(SynthPlugin& synth, const std::string& name, float value) {
    synth.getControls().at(name)->set(value);
  }
  void prepare(SynthPlugin& synth) {
    synth.setPlayConfigDetails(0, 2, 48000, 128);
    synth.prepareToPlay(48000, 128);
    set(synth, "oversampling", 0); synth.checkOversampling();
    set(synth, "volume", 6400); // sqrt(6400) - 80 = 0 dB
    for (int osc = 1; osc <= 4; ++osc) {
      const auto prefix = "osc_" + std::to_string(osc) + "_";
      set(synth, prefix + "on", 0);
      set(synth, prefix + "random_phase", 0);
      set(synth, prefix + "unison_voices", 1);
    }
    // Standalone test processors have no factory wavetable installed. Load
    // explicit deterministic PCM so this exercises real, nonzero MIDI audio.
    std::vector<float> pcm(48000);
    for (int i = 0; i < int(pcm.size()); ++i)
      pcm[i] = .4f * std::sin(float(i) * (2.0f * 3.14159265358979323846f * 130.8128f / 48000.0f));
    synth.getEngine()->getSample(slot_ids::kSampleSlot)->loadSample(pcm.data(), int(pcm.size()), 48000);
    set(synth, slot_ids::id("sample_on"), 1); set(synth, slot_ids::id("sample_level"), .3f);
    set(synth, slot_ids::id("sample_random_phase"), 0); set(synth, slot_ids::id("sample_loop"), 0);
    set(synth, "env_1_attack", .001f); set(synth, "env_1_decay", .001f);
    set(synth, "env_1_sustain", 1); set(synth, "env_1_release", .005f);
    for (int bus = 0; bus <= vital::kNumBuses; ++bus) {
      const auto prefix = bus == 0 ? "" : "bus_" + std::to_string(bus) + "_";
      for (int slot = 1; slot <= vital::constants::kMaxEffectSlots; ++slot)
        set(synth, prefix + "fx_slot_" + std::to_string(slot) + "_type", 0);
    }
  }
}

int main() {
  juce::ScopedJuceInitialiser_GUI juce;
  SynthPlugin dry, wet;
  // This test renders offline and must wait for selected effect preparation.
  dry.setNonRealtime(true); wet.setNonRealtime(true);
  prepare(dry); prepare(wet);
  constexpr int length = 192000;
  // The final release is followed by another note while only the FX tail is
  // audible. This also exercises overlapping voices and note-on offsets.
  const int positions[] = { 11, 3200, 4803, 7200, 11013, 15000, 16007, 19500, 22019, 25000, 28003, 31500 };
  const int pitches[] = { 48, 48, 55, 55, 60, 60, 43, 43, 48, 48, 67, 67 };
  const char* names[] = { "stages", "frequency", "resonance", "spread", "spread_style", "automation_precision" };
  const std::array<std::array<float, 6>, 4> settings {{
    {{40, 200, .5f, 0, 0, 1}}, {{40, 200, 5, -2, 0, 1}},
    {{40, 20, 30, 0, 0, 1}}, {{128, 200, .5f, 5, 1, 1}}
  }};
  for (int bus = 0; bus <= vital::kNumBuses; ++bus) {
    const auto chain = bus == 0 ? "" : "bus_" + std::to_string(bus) + "_";
    const auto prefix = chain + "fx_slot_1_defuse_";
    for (int scenario = 0; scenario < int(settings.size()); ++scenario) {
      for (auto* synth : { &dry, &wet }) {
        for (int other = 0; other <= vital::kNumBuses; ++other)
          set(*synth, (other == 0 ? "" : "bus_" + std::to_string(other) + "_") + "fx_slot_1_type", 0);
        set(*synth, slot_ids::id("sample_destination"), std::array<int, 4> { vital::constants::kEffects, vital::constants::kBus1, vital::constants::kBus2, vital::constants::kBus3 }[bus]);
      }
      set(wet, chain + "fx_slot_1_type", vital::constants::kDefuse + 1);
      set(wet, chain + "fx_slot_1_enabled", 1);
      vital::DefuseModule reference;
      reference.init(); reference.setSampleRate(48000);
      for (int i = 0; i < 6; ++i) {
        set(wet, prefix + names[i], settings[scenario][i]);
        reference.getControls().at(std::string("defuse_") + names[i])->set(settings[scenario][i]);
      }
      dry.getEngine()->allSoundsOff(); wet.getEngine()->allSoundsOff();
      // Settle the synth's master gain while no voices or tails are active.
      for (int block = 0; block < 32; ++block) for (auto* synth : { &dry, &wet }) {
        juce::AudioBuffer<float> audio(2, 128); audio.clear(); juce::MidiBuffer midi;
        synth->processBlock(audio, midi);
      }
      double error_energy = 0, input_energy = 0, output_energy = 0;
      double dry_peak = 0, wet_peak = 0, max_error = 0, tail_at_new_note = 0;
      int next_event = 0;
      for (int offset = 0; offset < length; offset += 128) {
        const int count = std::min(128, length - offset);
        juce::AudioBuffer<float> dry_audio(2, count), wet_audio(2, count);
        dry_audio.clear(); wet_audio.clear();
        juce::MidiBuffer midi;
        while (next_event < 12 && positions[next_event] < offset + count) {
          midi.addEvent(next_event % 2 == 0 ? juce::MidiMessage::noteOn(1, pitches[next_event], .7f)
                                           : juce::MidiMessage::noteOff(1, pitches[next_event]),
                        positions[next_event] - offset);
          ++next_event;
        }
        auto wet_midi = midi;
        dry.processBlock(dry_audio, midi); wet.processBlock(wet_audio, wet_midi);
        std::array<vital::poly_float, 128> input {};
        for (int i = 0; i < count; ++i)
          for (int lane = 0; lane < 4; ++lane) input[i].set(lane, dry_audio.getSample(lane % 2, i));
        reference.processWithInput(input.data(), count);
        for (int i = 0; i < count; ++i) for (int ch = 0; ch < 2; ++ch) {
          const double x = dry_audio.getSample(ch, i), y = wet_audio.getSample(ch, i);
          const double error = y - reference.output()->buffer[i][ch];
          error_energy += error * error; input_energy += x * x; output_energy += y * y;
          dry_peak = std::max(dry_peak, std::abs(x)); wet_peak = std::max(wet_peak, std::abs(y));
          max_error = std::max(max_error, std::abs(error));
          if (offset + i == 11012) tail_at_new_note = std::max(tail_at_new_note, std::abs(y));
        }
      }
      const double relative_error = std::sqrt(error_energy / std::max(1e-30, output_energy));
      std::cout << "Notes: bus=" << bus << " case=" << scenario << " reference_error=" << relative_error
                << " max_error=" << max_error << " energy_ratio=" << output_energy / input_energy
                << " peaks=" << dry_peak << '/' << wet_peak << " existing_tail=" << tail_at_new_note << '\n';
      expect(input_energy > .001 && tail_at_new_note > 1e-7, "test must play actual notes into an audible retained tail");
      expect(relative_error < .001, "new MIDI notes must not reset, duplicate or change the gain of the continuous FX tail");
      expect(std::isfinite(output_energy) && output_energy <= input_energy * 1.005,
             "static Defuse must not add energy when new notes overlap its tail");
    }
  }
  std::cout << "Defuse note tails: " << failures << " failures\n";
  return failures ? 1 : 0;
}
