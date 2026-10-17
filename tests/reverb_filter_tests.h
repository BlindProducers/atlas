// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "filter_module.h"
#include "reverb_filter.h"
#include "filters_module.h"
#include "prepared_effect_module.h"
#include <complex>
#include "../src/synthesis/utilities/value_switch.h"

namespace reverb_filter_tests {
struct Fixture {
  vital::FilterModule module;
  vital::Output audio, reset;
  Fixture(int rate, int factor, bool mono = false) : module(mono ? "filter_fx" : "filter_1") {
    module.setMono(mono);
    module.plug(&audio, vital::FilterModule::kAudio);
    module.plug(&reset, vital::FilterModule::kReset);
    module.init(); module.setSampleRate(rate); module.setOversampleAmount(factor);
    audio.ensureBufferSize(vital::kMaxBufferSize * factor);
    const std::string p = mono ? "filter_fx_" : "filter_1_";
    auto c = module.getControls();
    c.at(p + "on")->set(1); c.at(p + "model")->set(vital::constants::kReverbFilter);
    c.at(p + "reverb_cutoff")->set(std::log2(100.f));
    c.at(p + "resonance")->set(1); c.at(p + "reverb_damp")->set(.3f);
  }
};
void testAudioAndRealtime() {
  for (int rate : {44100, 48000, 96000}) for (int factor : {1, 2, 4, 8}) for (bool mono : {false, true}) {
    Fixture f(rate, factor, mono);
    const std::string prefix = mono ? "filter_fx_" : "filter_1_";
    auto controls = f.module.getControls();
    double energy = 0;
    for (int block = 0; block < 60; ++block) {
      const int n = 128 * factor;
      if (block == 20) controls.at(prefix + "reverb_cutoff")->set(3);
      if (block == 10) controls.at(prefix + "reverb_bass_preserve")->set(1);
      if (block == 30) controls.at(prefix + "reverb_bass_preserve")->set(.35f);
      if (block == 40) controls.at(prefix + "reverb_cutoff")->set(std::log2(20000.f));
      for (int i = 0; i < n; ++i) {
        const float input = .1f * std::sin((block * n + i) * .17f);
        f.audio.buffer[i] = vital::poly_float(input, input, 0, 0);
      }
      tracked_allocations.store(0); track_allocations.store(true);
      f.module.process(n);
      track_allocations.store(false);
      expect(tracked_allocations.load() == 0, "Reverb filter processing and model selection allocate no memory");
      for (int i = 0; i < n; ++i) {
        const auto value = f.module.output()->buffer[i];
        expect(std::isfinite(value[0]) && std::abs(value[0]) < 20, "Reverb filter stays finite under cutoff sweeps");
        expect(value[0] == value[1] && value[2] == 0 && value[3] == 0,
               "Reverb filter preserves mono and isolates voice lanes");
        energy += value[0] * value[0];
      }
    }
    expect(energy > .01, "Reverb model produces processed audio at every rate and quality");
    // Output gain follows the mix, so even the fully dry signal has JSFX output gain.
    controls.at(prefix + "mix")->set(0); controls.at(prefix + "reverb_output")->set(-6);
    for (int b = 0; b < 16; ++b) f.module.process(128 * factor);
    const auto actual = f.module.output()->buffer[127 * factor][0];
    const auto expected = f.audio.buffer[127 * factor][0] * std::pow(10.f, -.3f);
    expect(std::abs(actual - expected) < 1e-5, "Reverb output gain applies after dry/wet mix");
    tracked_allocations.store(0); track_allocations.store(true);
    f.module.hardReset(); f.module.setOversampleAmount(1); f.module.process(128);
    track_allocations.store(false);
    expect(tracked_allocations.load() == 0, "Reverb reset and prepared quality switches allocate no memory");
  }
}
void testModulation() {
  for (bool mono : {false, true}) {
    Fixture base(48000, 1, mono), modulated(48000, 1, mono);
    const std::string prefix = mono ? "filter_fx_" : "filter_1_";
    vital::Value signal;
    auto* destination = modulated.module.getModulationDestination(prefix + "reverb_cutoff", !mono);
    destination->plugNext(&signal);
    modulated.module.getModulationSwitch(prefix + "reverb_cutoff", !mono)->set(1);
    double difference = 0;
    for (int b = 0; b < 32; ++b) {
      for (int i = 0; i < 128; ++i) {
        base.audio.buffer[i] = modulated.audio.buffer[i] = .1f * std::sin((b * 128 + i) * .17f);
        signal.output()->buffer[i] = i % 2 ? 2.f : -1.f;
      }
      base.module.process(128); modulated.module.process(128);
      for (int i = 0; i < 128; ++i)
        difference += std::abs(base.module.output()->buffer[i][0] - modulated.module.output()->buffer[i][0]);
    }
    expect(difference > 1, "Audio-rate Reverb cutoff modulation changes the rendered voice/FX audio");
    destination->unplug(&signal);
    // With a dry mix, modulating Output gives an exact observable gain change.
    auto controls = modulated.module.getControls();
    controls.at(prefix + "mix")->set(0);
    vital::cr::Value output_mod(-6.f);
    auto* output_destination = modulated.module.getModulationDestination(prefix + "reverb_output", !mono);
    output_destination->plugNext(&output_mod);
    modulated.module.getModulationSwitch(prefix + "reverb_output", !mono)->set(1);
    for (int b = 0; b < 32; ++b) modulated.module.process(128);
    expect(std::abs(modulated.module.output()->buffer[127][0] -
                   modulated.audio.buffer[127][0] * std::pow(10.f, -.3f)) < 1e-5,
           "Reverb Output modulation changes post-mix gain");
    output_destination->unplug(&output_mod);
  }
}
void testVoiceReset() {
  vital::ReverbFilter filter;
  vital::Output audio, reset, cutoff, keytrack;
  vital::cr::Value resonance(1), damp(0), bass(1);
  filter.plug(&audio, vital::ReverbFilter::kAudio); filter.plug(&reset, vital::ReverbFilter::kReset);
  filter.plug(&cutoff, vital::ReverbFilter::kCutoffLog); filter.plug(&keytrack, vital::ReverbFilter::kKeytrack);
  filter.plug(&bass, vital::ReverbFilter::kBassPreserve);
  filter.plug(&resonance, vital::ReverbFilter::kResonance); filter.plug(&damp, vital::ReverbFilter::kDamp);
  filter.setSampleRate(48000);
  for (int i = 0; i < 128; ++i) cutoff.buffer[i] = std::log2(800.f);
  for (int b = 0; b < 16; ++b) {
    for (int i = 0; i < 128; ++i) audio.buffer[i] = .1f * std::sin((b * 128 + i) * .17f);
    filter.process(128);
  }
  audio.clearBuffer();
  reset.trigger(vital::constants::kFirstMask, vital::kVoiceOn, 37);
  filter.process(128);
  double other_energy = 0, before_energy = 0;
  for (int i = 0; i < 128; ++i) {
    if (i < 37) before_energy += std::abs(filter.output()->buffer[i][0]);
    else expect(filter.output()->buffer[i][0] == 0 && filter.output()->buffer[i][1] == 0,
                "New voice discards all old delay/FIR history at its exact note offset");
    other_energy += std::abs(filter.output()->buffer[i][2]);
  }
  expect(before_energy > .001 && other_energy > .001, "Voice reset preserves pre-note audio and other voices' tails");
  reset.clearTrigger();
  std::unique_ptr<vital::Processor> copy(filter.clone());
  copy->process(128);
  for (int i = 0; i < 128; ++i)
    expect(copy->output()->buffer[i][2] == 0, "Cloned reverb filters start with independent empty history");
}
// The voice filters release their Reverb memory while another model is
// selected. Offline, a filter without it reserves it within the block, so the
// sound is unchanged; in realtime it passes audio through and asks for it.
void testMemoryOnDemand() {
  struct Rig {
    vital::ReverbFilter filter;
    vital::Output audio, reset, cutoff, keytrack;
    vital::cr::Value resonance { .8f }, damp { .2f }, bass { .5f };
    Rig() {
      filter.plug(&audio, vital::ReverbFilter::kAudio); filter.plug(&reset, vital::ReverbFilter::kReset);
      filter.plug(&cutoff, vital::ReverbFilter::kCutoffLog); filter.plug(&keytrack, vital::ReverbFilter::kKeytrack);
      filter.plug(&resonance, vital::ReverbFilter::kResonance); filter.plug(&damp, vital::ReverbFilter::kDamp);
      filter.plug(&bass, vital::ReverbFilter::kBassPreserve);
      filter.setSampleRate(48000);
      for (int i = 0; i < 128; ++i) cutoff.buffer[i] = std::log2(900.f);
    }
    void render(vital::Processor& processor, int block) {
      for (int i = 0; i < 128; ++i) audio.buffer[i] = .1f * std::sin((block * 128 + i) * .13f);
      processor.process(128);
    }
  };
  auto same = [](vital::Processor& a, vital::Processor& b) {
    for (int i = 0; i < 128; ++i)
      for (int lane = 0; lane < vital::poly_float::kSize; ++lane)
        if (a.output()->buffer[i][lane] != b.output()->buffer[i][lane]) return false;
    return true;
  };
  Rig always, on_demand;
  on_demand.filter.releaseMemory();
  // A voice copy made while released has no memory either.
  std::unique_ptr<vital::Processor> copy(on_demand.filter.clone());
  bool identical = true;
  for (int b = 0; b < 40; ++b) {
    always.render(always.filter, b);
    on_demand.render(on_demand.filter, b);
    identical = identical && same(always.filter, on_demand.filter);
  }
  expect(identical, "Offline, a released Reverb filter reserves its memory and sounds as before");
  expect(!on_demand.filter.takeMemoryRequest(), "Offline processing needs no request for Reverb memory");
  {
    vital::RealtimeEffectScope realtime(true);
    copy->process(128);
  }
  expect(!on_demand.filter.takeMemoryRequest(), "Reserving Reverb memory also gives it to the filter's voice copies");

  on_demand.filter.releaseMemory();
  tracked_allocations.store(0); track_allocations.store(true);
  {
    vital::RealtimeEffectScope realtime(true);
    on_demand.render(on_demand.filter, 40);
  }
  track_allocations.store(false);
  expect(tracked_allocations.load() == 0, "A realtime Reverb filter without memory allocates nothing");
  bool dry = true;
  for (int i = 0; i < 128; ++i)
    for (int lane = 0; lane < vital::poly_float::kSize; ++lane)
      dry = dry && on_demand.filter.output()->buffer[i][lane] == on_demand.audio.buffer[i][lane];
  expect(dry, "A realtime Reverb filter without memory passes its input through");
  expect(on_demand.filter.takeMemoryRequest(), "A realtime Reverb filter without memory asks for it");
  expect(!on_demand.filter.takeMemoryRequest(), "A Reverb memory request is taken once");

  on_demand.filter.reserveMemory();
  always.filter.hardReset();
  on_demand.filter.hardReset();
  identical = true;
  for (int b = 0; b < 20; ++b) {
    vital::RealtimeEffectScope realtime(true);
    always.render(always.filter, b);
    on_demand.render(on_demand.filter, b);
    identical = identical && same(always.filter, on_demand.filter);
  }
  expect(identical, "A Reverb filter given its memory again sounds as one that always had it");
}
// Selecting the voice Reverb model from the message thread reserves its
// memory before the audio thread runs it: realtime blocks neither pass the
// voices through dry nor allocate to reserve it.
void testVoiceMemoryOnSelection() {
  SynthPlugin synth;
  synth.setPlayConfigDetails(0, 2, 48000, 128);
  synth.prepareToPlay(48000, 128);
  auto c = synth.getControls();
  c.at("osc_1_on")->set(1);
  c.at("osc_1_destination")->set(vital::constants::kFilter1);
  c.at("filter_1_on")->set(1);
  AudioSampleBuffer audio(2, 128);
  MidiBuffer midi;
  midi.addEvent(MidiMessage::noteOn(1, 60, .7f), 0);
  for (int b = 0; b < 20; ++b) { audio.clear(); synth.processBlock(audio, midi); midi.clear(); }
  synth.parameterChanged("filter_1_model", vital::constants::kReverbFilter);
  tracked_allocations.store(0); track_allocations.store(true);
  for (int b = 0; b < 20; ++b) { audio.clear(); synth.processBlock(audio, midi); }
  track_allocations.store(false);
  expect(tracked_allocations.load() == 0, "Selecting the voice Reverb model reserves its memory before realtime blocks run it");
  synth.parameterChanged("filter_1_model", vital::constants::kAnalog);
  for (int b = 0; b < 4; ++b) { audio.clear(); synth.processBlock(audio, midi); }
  synth.parameterChanged("filter_1_model", vital::constants::kReverbFilter);
  tracked_allocations.store(0); track_allocations.store(true);
  for (int b = 0; b < 4; ++b) { audio.clear(); synth.processBlock(audio, midi); }
  track_allocations.store(false);
  expect(tracked_allocations.load() == 0, "Selecting Reverb again after leaving it reserves its memory before realtime blocks");
  // How long leaving and selecting the model hold the callback lock, freeing
  // and reserving every voice's memory once the delay lines have been used.
  for (int b = 0; b < 800; ++b) { audio.clear(); synth.processBlock(audio, midi); }
  auto start = std::chrono::steady_clock::now();
  synth.parameterChanged("filter_1_model", vital::constants::kAnalog);
  const double release_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  start = std::chrono::steady_clock::now();
  synth.parameterChanged("filter_1_model", vital::constants::kReverbFilter);
  const double reserve_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  std::cout << "Voice Reverb memory: release ms=" << release_ms << " reserve ms=" << reserve_ms << '\n';
}
std::vector<float> renderPluginFilter(bool fx, int quality, float resonance, bool dry = false) {
  SynthPlugin synth;
  synth.setPlayConfigDetails(0, 2, 48000, 128);
  synth.prepareToPlay(48000, 128);
  const auto controls = synth.getControls();
  controls.at("oversampling")->set(quality); synth.checkOversampling();
  controls.at("osc_1_on")->set(1);
  controls.at("osc_1_unison_voices")->set(1);
  controls.at("osc_1_random_phase")->set(0);
  controls.at("osc_1_destination")->set(fx ? vital::constants::kEffects : vital::constants::kFilter1);
  controls.at("filter_1_on")->set(!fx);
  controls.at("filter_2_on")->set(0);
  const std::string prefix = fx ? "fx_slot_1_filter_fx_" : "filter_1_";
  if (fx) {
    controls.at("fx_slot_1_type")->set(vital::constants::kFilterFx + 1);
    controls.at("fx_slot_1_enabled")->set(1);
  }
  synth.parameterChanged(prefix + "model", vital::constants::kReverbFilter);
  synth.parameterChanged(prefix + "reverb_cutoff", std::log2(1000.f));
  synth.parameterChanged(prefix + "resonance", resonance);
  synth.parameterChanged(prefix + "reverb_damp", 0);
  synth.parameterChanged(prefix + "mix", dry ? 0 : 1);
  AudioSampleBuffer audio(2, 128);
  MidiBuffer midi;
  for (int b = 0; b < 100; ++b) { audio.clear(); synth.processBlock(audio, midi); }
  midi.addEvent(MidiMessage::noteOn(1, 60, .7f), 0);
  std::vector<float> result;
  for (int b = 0; b < 100; ++b) {
    audio.clear(); synth.processBlock(audio, midi); midi.clear();
    for (int i = 0; i < 128; ++i) result.push_back(audio.getSample(0, i));
  }
  return result;
}
void testPluginResonance() {
  auto relativeDifference = [](const std::vector<float>& a, const std::vector<float>& b) {
    double error = 0, energy = 0;
    for (int i = 0; i < a.size(); ++i) { error += (a[i]-b[i])*(a[i]-b[i]); energy += b[i]*b[i]; }
    expect(energy > 1e-5, "Full-plugin comparison must render audible audio");
    return std::sqrt(error / std::max(1e-20, energy));
  };
  for (bool fx : {false, true}) for (int quality : {0, 1, 3}) {
    const auto dry = renderPluginFilter(fx, quality, 0, true);
    const auto low = renderPluginFilter(fx, quality, 0);
    const auto high = renderPluginFilter(fx, quality, 1);
    const double wet_difference = relativeDifference(low, dry);
    const double resonance_difference = relativeDifference(high, low);
    expect(wet_difference > .1, "1000 Hz Reverb must audibly differ from dry in the full plugin");
    expect(resonance_difference > .1, "Resonance must audibly change 1000 Hz Reverb in the full plugin");
    std::cout << "1000 Hz plugin " << (fx ? "FX" : "voice") << " quality=" << quality
              << " wet/dry difference=" << wet_difference << " resonance difference=" << resonance_difference << '\n';
  }
}
void testBassResponse() {
  // Measure the complete module with a low sine, including stereo lanes,
  // oversampling, mix and the new control. Warmup is excluded from the RMS.
  double worst_db = 0;
  for (bool mono : {false, true}) for (int factor : {1, 8})
    for (float cutoff : {100.f, 200.f, 800.f, 4000.f}) {
    Fixture f(48000, factor, mono);
    const std::string prefix = mono ? "filter_fx_" : "filter_1_";
    auto controls = f.module.getControls();
    controls.at(prefix + "reverb_cutoff")->set(std::log2(cutoff));
    controls.at(prefix + "resonance")->set(.9f);
    controls.at(prefix + "reverb_damp")->set(0);
    controls.at(prefix + "reverb_bass_preserve")->set(1);
    double energy = 0, input_energy = 0;
    for (int b = 0; b < 220; ++b) {
      for (int i = 0; i < 128; ++i) {
        const float x = .1f * std::sin(2 * vital::kPi * 50 * (b * 128 + i) / 48000.);
        for (int phase = 0; phase < factor; ++phase)
          f.audio.buffer[i * factor + phase] = vital::poly_float(x, -x, 0, 0);
      }
      f.module.process(128 * factor);
      if (b < 160) continue;
      for (int i = 0; i < 128 * factor; ++i) {
        const auto x = f.audio.buffer[i][0], y = f.module.output()->buffer[i][0];
        energy += y*y; input_energy += x*x;
      }
    }
    const double db = 10 * std::log10(energy / input_energy);
    worst_db = std::max(worst_db, std::abs(db));
    expect(std::abs(db) < .15, "Bass Preserve keeps 50 Hz near unity across cutoff, voice/FX and quality");
  }
  std::cout << "Bass Preserve maximum 50 Hz level deviation: " << worst_db << " dB\n";
}

void testReleaseTails() {
  // Include both voices, output buses, both serial directions and Filter FX.
  for (int scenario = 0; scenario < 7; ++scenario) {
    SynthPlugin synth;
    synth.setPlayConfigDetails(0, 2, 48000, 128); synth.prepareToPlay(48000, 128);
    auto c = synth.getControls();
    const bool fx = scenario == 6;
    const int filter = scenario == 1 || scenario == 3 || scenario == 5 ? 2 : 1;
    c.at("osc_1_on")->set(1); c.at("osc_1_unison_voices")->set(1);
    c.at("osc_1_random_phase")->set(0);
    c.at("osc_1_destination")->set(fx ? vital::constants::kEffects :
        filter == 1 ? vital::constants::kFilter1 : vital::constants::kFilter2);
    c.at("filter_1_on")->set(!fx && (filter == 1 || scenario >= 4));
    c.at("filter_2_on")->set(!fx && (filter == 2 || scenario >= 4));
    c.at("env_1_release")->set(std::pow(.030f, .25f));
    c.at("env_1_sustain")->set(1);
    if (scenario == 2) c.at("filter_1_destination")->set(vital::constants::kBus1);
    if (scenario == 3) c.at("filter_2_destination")->set(vital::constants::kDirectOut);
    if (scenario == 4) c.at("filter_2_filter_input")->set(1);
    if (scenario == 5) c.at("filter_1_filter_input")->set(1);
    if (fx) { c.at("fx_slot_1_type")->set(vital::constants::kFilterFx + 1); c.at("fx_slot_1_enabled")->set(1); }
    const std::string prefix = fx ? "fx_slot_1_filter_fx_" : "filter_" + std::to_string(filter) + "_";
    synth.parameterChanged(prefix + "model", vital::constants::kReverbFilter);
    synth.parameterChanged(prefix + "reverb_cutoff", std::log2(1000.f));
    synth.parameterChanged(prefix + "resonance", 1);
    synth.parameterChanged(prefix + "reverb_bass_preserve", scenario % 2);
    synth.parameterChanged(prefix + "mix", 1);
    AudioSampleBuffer audio(2, 128); MidiBuffer midi;
    for (int b = 0; b < 80; ++b) { audio.clear(); synth.processBlock(audio, midi); }
    midi.addEvent(MidiMessage::noteOn(1, 60, .7f), 0);
    for (int b = 0; b < 100; ++b) { audio.clear(); synth.processBlock(audio, midi); midi.clear(); }
    midi.addEvent(MidiMessage::noteOff(1, 60), 19);
    double tail = 0, final_energy = 0;
    for (int b = 0; b < 1100; ++b) {
      audio.clear(); synth.processBlock(audio, midi); midi.clear();
      for (int i = 0; i < 128; ++i) {
        const double x = audio.getSample(0, i);
        expect(std::isfinite(x), "Released Reverb audio remains finite");
        if (b >= 40 && b < 100) tail += x*x;
        if (b >= 1050) final_energy += x*x;
      }
      if (b == 40 && !fx)
        expect(synth.getEngine()->getNumActiveVoices() > 0, "Reverb voice survives the 30 ms envelope release");
    }
    expect(tail > 1e-6, "Reverb rings audibly 100-266 ms after note-off on every route");
    expect(final_energy < 1e-8, "Released Reverb eventually decays to silence");
    expect(synth.getEngine()->getNumActiveVoices() == 0, "Silent Reverb voices are retired");
    std::cout << "Reverb release scenario " << scenario << " tail energy=" << tail
              << " final energy=" << final_energy << '\n';
  }
}

void testForcedTailKill() {
  vital::FiltersModule filters;
  vital::Output audio, events;
  vital::cr::Value amplitude(1);
  filters.plug(&audio, vital::FiltersModule::kFilter1Input);
  filters.plug(&amplitude, vital::FiltersModule::kAmplitude);
  filters.plug(&events, vital::FiltersModule::kVoiceEvent);
  filters.init(); filters.setSampleRate(48000);
  auto c = filters.getControls();
  c.at("filter_1_on")->set(1); c.at("filter_2_on")->set(0);
  c.at("filter_1_model")->set(vital::constants::kReverbFilter);
  c.at("filter_1_reverb_cutoff")->set(std::log2(1000.f)); c.at("filter_1_resonance")->set(1);
  for (int b = 0; b < 60; ++b) {
    for (int i = 0; i < 128; ++i) audio.buffer[i] = .1f * std::sin((b * 128 + i) * .17f);
    filters.process(128);
  }
  amplitude.set(0);
  events.trigger(vital::poly_mask(-1), vital::kVoiceKill, 17);
  filters.process(128); events.clearTrigger();
  for (int b = 0; b < 10; ++b) filters.process(128);
  for (int i = 0; i < 128; ++i) {
    expect(filters.output()->buffer[i][0] == 0, "Voice stealing fades and stops a ringing Reverb tail");
    expect(filters.output(vital::FiltersModule::kVoiceAlive)->buffer[i][0] == 0,
           "Forced voice steal does not wait for natural Reverb decay");
  }
}
void testPresetRoundtrip() {
  MemoryBlock state;
  std::vector<std::string> prefixes {"filter_1_", "filter_2_", "filter_fx_"};
  for (int chain = 0; chain <= vital::kNumBuses; ++chain)
    prefixes.push_back((chain ? "bus_" + std::to_string(chain) + "_" : "") + "fx_slot_20_filter_fx_");
  {
    SynthPlugin synth;
    for (auto& p : prefixes) {
      synth.parameterChanged(p + "model", vital::constants::kReverbFilter);
      synth.parameterChanged(p + "reverb_cutoff", std::log2(8.f));
      synth.parameterChanged(p + "reverb_damp", .73f);
      synth.parameterChanged(p + "reverb_bass_preserve", .64f);
      synth.parameterChanged(p + "reverb_output", -7.5f);
      synth.parameterChanged(p + "mix", .67f);
      synth.parameterChanged(p + "resonance", .81f);
    }
    synth.getStateInformation(state);
  }
  SynthPlugin restored;
  restored.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
  for (auto& p : prefixes) {
    const auto controls = restored.getControls();
    expect(controls.at(p + "model")->value() == vital::constants::kReverbFilter, "Reverb model survives preset save/load");
    expect(std::abs(controls.at(p + "reverb_cutoff")->value() - 3) < 1e-5 &&
           std::abs(controls.at(p + "reverb_damp")->value() - .73f) < 1e-5 &&
           std::abs(controls.at(p + "reverb_bass_preserve")->value() - .64f) < 1e-5 &&
           std::abs(controls.at(p + "reverb_output")->value() + 7.5f) < 1e-5 &&
           std::abs(controls.at(p + "mix")->value() - .67f) < 1e-5 &&
           std::abs(controls.at(p + "resonance")->value() - .81f) < 1e-5,
           "All Reverb controls survive save/load in voice, legacy FX and every bus slot");
  }
}
int run() {
  testAudioAndRealtime(); testModulation(); testVoiceReset(); testMemoryOnDemand(); testVoiceMemoryOnSelection();
  testPluginResonance(); testBassResponse(); testReleaseTails(); testForcedTailKill(); testPresetRoundtrip();
  if (!failures) std::cout << "Atlas Reverb filter tests passed\n";
  return failures ? 1 : 0;
}
}
