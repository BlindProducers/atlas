// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "filter_module.h"
#include "scream_filter.h"
#include "scream_module.h"
#include "scream_tanh.h"
#include "../third_party/scream/Source/DSP/FastMath.h"
#include "../third_party/scream/Source/DSP/TanhADAA2.h"

namespace scream_filter_tests {
// The four-lane waveshaper against BP Scream's TanhADAA2, which it replaces.
void testWaveshaper() {
  using vital::scream::TanhAdaa2;
  double ad1_error = 0, ad2_error = 0, tanh_error = 0;
  for (int step = -64000; step <= 64000; ++step) {
    const double x = step * (32.0 / 64000.0) + (step % 7) * 1.0e-7;
    ad1_error = std::max(ad1_error, std::abs(TanhAdaa2::ad1(x) - TanhADAA2::AD1(x)));
    ad2_error = std::max(ad2_error, std::abs(TanhAdaa2::ad2(x) - TanhADAA2::AD2(x)) / (1.0 + x * x));
    tanh_error = std::max(tanh_error, std::abs(TanhAdaa2::tanh(x) - std::tanh(x)));
  }
  std::cout << "Scream waveshaper vs BP Scream: AD1 " << ad1_error << ", AD2 " << ad2_error
            << " (relative to 1 + x^2), tanh " << tanh_error << '\n';
  expect(ad1_error < 1e-14 && ad2_error < 1e-15 && tanh_error < 1e-15,
         "Scream's antiderivatives match BP Scream's to double precision");

  // Signals for each lane: tones from gentle to driven hard into the clamp,
  // a slow ramp and a held value (the ill-conditioned branches), and noise.
  auto signal = [](int kind, int n) {
    switch (kind) {
      case 0: return 0.7 * std::sin(n * 0.031);
      case 1: return 6.0 * std::sin(n * 0.0007) + 0.2 * std::sin(n * 0.9);
      case 2: return 31.0 * std::sin(n * 0.013);
      case 3: return n < 3000 ? n * 2.0e-6 : (n < 6000 ? 0.25 : 0.25 + (n - 6000) * 3.0e-6 * ((n / 50) % 2 ? 1 : -1));
      case 4: return (n / 9) % 3 == 0 ? 0.0 : 2.0 * std::sin(n * 1.7) * std::sin(n * 0.37);
      default: return 0.0;
    }
  };
  // ADAA2 divides a difference of AD2 values by x[n] - x[n-1] and again by
  // x[n] - x[n-2], so one rounding of AD2 (about eps (1 + x^2)) reaches the
  // output divided by those steps, in the original as here: up to about 1e-5
  // where they are just above the 1e-5 tolerance. Measure the difference in
  // units of that rounding noise.
  constexpr double kEpsilon = 2.220446049250313e-16;
  double worst = 0, worst_noise = 0;
  bool independent = true;
  for (int arrangement = 0; arrangement < 5; ++arrangement) {
    TanhAdaa2 lanes, alone;
    TanhADAA2 reference[4];
    double previous[4][3] {};
    for (int n = 0; n < 12000; ++n) {
      alignas(16) double in[4], out[4], solo_in[4] {}, solo_out[4];
      for (int lane = 0; lane < 4; ++lane) in[lane] = signal((arrangement + lane) % 5, n);
      lanes.process(in, out, 2);
      solo_in[0] = in[0];
      alone.process(solo_in, solo_out, 2);
      independent = independent && solo_out[0] == out[0];
      for (int lane = 0; lane < 4; ++lane) {
        const double difference = std::abs(out[lane] - reference[lane].process(in[lane]));
        const double* p = previous[lane];
        const double x = in[lane];
        auto step = [](double a, double b) { return std::max(std::abs(a - b), TanhAdaa2::kTolerance); };
        const double size = 1.0 + std::max({ x * x, p[0] * p[0], p[1] * p[1], p[2] * p[2] });
        const double noise = kEpsilon * size * (1.0 / step(x, p[0]) + 1.0 / step(p[0], p[1])) / step(x, p[1]);
        worst = std::max(worst, difference);
        worst_noise = std::max(worst_noise, difference / noise);
        previous[lane][2] = p[1]; previous[lane][1] = p[0]; previous[lane][0] = x;
      }
    }
  }
  std::cout << "Scream waveshaper output vs BP Scream: worst difference " << worst
            << ", at most " << worst_noise << " times ADAA2's rounding noise\n";
  expect(worst_noise < 16, "Scream's four-lane waveshaper matches BP Scream's TanhADAA2 within its rounding noise");
  expect(independent, "A Scream lane's waveshaper output does not depend on the other lanes");
}

void testReferenceTone() {
  vital::FilterModule filter("filter_1");
  vital::ScreamModule reference;
  vital::Output audio;
  filter.plug(&audio, vital::FilterModule::kAudio);
  filter.init(); reference.init(); filter.setSampleRate(48000); reference.setSampleRate(48000);
  auto c = filter.getControls(), r = reference.getControls();
  c.at("filter_1_on")->set(1); c.at("filter_1_model")->set(vital::constants::kScreamFilter);
  c.at("filter_1_scream_autogain")->set(0); r.at("scream_autogain")->set(0);
  const float normalized = .65f;
  c.at("filter_1_cutoff")->set(FastMath::lerpf(normalized, FastMath::MIDI_NOTE_NUM_20Hz, FastMath::MIDI_NOTE_NUM_20kHz));
  r.at("scream_cutoff")->set(normalized);
  c.at("filter_1_resonance")->set(.2f); r.at("scream_resonance")->set(.2f);
  double difference = 0, energy = 0;
  for (int b = 0; b < 700; ++b) {
    for (int i = 0; i < 128; ++i) audio.buffer[i] = .3f * std::sin((b * 128 + i) * .09f);
    filter.process(128); reference.processWithInput(audio.buffer, 128);
    if (b < 600) continue;
    for (int i = 0; i < 128; ++i) {
      double x = filter.output()->buffer[i][0], y = reference.output()->buffer[i][0];
      difference += (x-y)*(x-y); energy += y*y;
    }
  }
  const double error = std::sqrt(difference / energy);
  std::cout << "Scream filter steady tone vs BP Scream: relative error=" << error << '\n';
  expect(error < .002, "Scream preserves the original nonlinear filter tone with gates open");
}

void testRatesAndState() {
  for (int rate : {44100, 48000, 96000}) for (int factor : {1, 2, 4, 8}) for (bool fx : {false, true}) {
    vital::FilterModule filter(fx ? "filter_fx" : "filter_1");
    vital::Output audio;
    filter.setMono(fx); filter.plug(&audio, vital::FilterModule::kAudio); filter.init();
    filter.setSampleRate(rate); filter.setOversampleAmount(factor);
    audio.ensureBufferSize(vital::kMaxBufferSize * factor);
    auto c = filter.getControls(); const std::string p = fx ? "filter_fx_" : "filter_1_";
    c.at(p + "on")->set(1); c.at(p + "model")->set(vital::constants::kScreamFilter);
    c.at(p + "resonance")->set(1); c.at(p + "scream_input_gain")->set(24);
    double energy = 0;
    for (int b = 0; b < 50; ++b) {
      c.at(p + "cutoff")->set(8 + (b % 12) * 11);
      c.at(p + "scream_amount")->set((b % 5) * .25f);
      for (int i = 0; i < 128 * factor; ++i)
        audio.buffer[i] = vital::poly_float(.8f * std::sin((b * 128 * factor + i) * .09f),
                                          .3f * std::cos((b * 128 * factor + i) * .023f), 0, 0);
      tracked_allocations = 0; track_allocations = true;
      filter.process(128 * factor);
      track_allocations = false;
      expect(tracked_allocations == 0, "Scream model selection/modulation must allocate no audio-thread memory");
      for (int i = 0; i < 128 * factor; ++i) {
        auto y = filter.output()->buffer[i];
        expect(std::isfinite(y[0]) && std::abs(y[0]) < 50, "Scream remains finite at maximum resonance/drive");
        if (fx) expect(y[2] == y[0] && y[3] == y[1], "Filter FX Scream mirrors its stereo pair into lanes 2-3");
        else expect(y[2] == 0 && y[3] == 0, "Scream voice lanes do not leak into one another");
        energy += y[0] * y[0];
      }
    }
    expect(energy > .01, "Scream produces audio at all rates/qualities");
    audio.clearBuffer();
    for (int b = 0; b < 8; ++b) filter.process(128 * factor);
    if (fx) expect(filter.output()->buffer[127 * factor][0] == 0,
                   "Filter FX Scream clears feedback after input becomes silent");
    c.at(p + "on")->set(0); filter.process(128 * factor);
    c.at(p + "on")->set(1); filter.process(128 * factor);
    expect(filter.output()->buffer[127 * factor][0] == 0, "Re-enabling Scream must not revive old resonance");
    tracked_allocations = 0; track_allocations = true; filter.hardReset();
    filter.process(128 * factor); track_allocations = false;
    expect(tracked_allocations == 0 && filter.output()->buffer[0][0] == 0, "Scream reset is allocation-free and clears feedback");
  }
}

void testVoiceReset() {
  vital::ScreamFilter filter;
  vital::Output audio, reset, cutoff;
  vital::cr::Value resonance(1), scream(.5f), input_gain(0), mix(1), output_gain(0), autogain(0);
  filter.plug(&audio, vital::ScreamFilter::kAudio); filter.plug(&reset, vital::ScreamFilter::kReset);
  filter.plug(&cutoff, vital::ScreamFilter::kMidiCutoff);
  filter.plug(&resonance, vital::ScreamFilter::kResonance); filter.plug(&scream, vital::ScreamFilter::kScream);
  filter.plug(&input_gain, vital::ScreamFilter::kInputGain); filter.plug(&mix, vital::ScreamFilter::kMix);
  filter.plug(&output_gain, vital::ScreamFilter::kOutputGain); filter.plug(&autogain, vital::ScreamFilter::kAutogain);
  filter.setSampleRate(48000);
  for (int i = 0; i < 128; ++i) cutoff.buffer[i] = 72;
  for (int b = 0; b < 60; ++b) {
    for (int i = 0; i < 128; ++i) audio.buffer[i] = .4f * std::sin((b*128+i)*.1f);
    filter.process(128);
  }
  audio.clearBuffer(); reset.trigger(vital::constants::kFirstMask, vital::kVoiceOn, 37);
  filter.process(128); double other = 0;
  for (int i = 37; i < 128; ++i) {
    expect(filter.output()->buffer[i][0] == 0, "Scream resets old resonance at the exact new-note offset");
    other += std::abs(filter.output()->buffer[i][2]);
  }
  expect(other > .001, "Resetting one Scream voice preserves another voice's independent state");
  reset.clearTrigger(); std::unique_ptr<vital::Processor> clone(filter.clone()); clone->process(128);
  expect(clone->output()->buffer[127][2] == 0, "Cloned Scream voices start with empty feedback");
}

void testEnvelopeRelease() {
  for (int scenario = 0; scenario < 8; ++scenario) for (float release : {.030f, .250f}) {
    SynthPlugin synth; synth.setPlayConfigDetails(0, 2, 48000, 128); synth.prepareToPlay(48000, 128);
    auto c = synth.getControls();
    const int voice_filter = scenario % 2 + 1;
    c.at("oversampling")->set(scenario == 5 ? 3 : 1); synth.checkOversampling();
    c.at("osc_1_unison_voices")->set(1); c.at("osc_1_random_phase")->set(0);
    c.at("osc_1_destination")->set(voice_filter == 1 ? vital::constants::kFilter1 : vital::constants::kFilter2);
    c.at("filter_1_on")->set(voice_filter == 1 || scenario >= 4);
    c.at("filter_2_on")->set(voice_filter == 2 || scenario >= 4);
    const std::string p = "filter_" + std::to_string(voice_filter) + "_";
    c.at(p + "model")->set(vital::constants::kScreamFilter);
    c.at(p + "resonance")->set(1); c.at(p + "cutoff")->set(72);
    c.at("env_1_sustain")->set(1); c.at("env_1_release")->set(std::pow(release, .25f));
    if (scenario == 2) c.at(p + "destination")->set(vital::constants::kBus2);
    if (scenario == 3) c.at(p + "destination")->set(vital::constants::kDirectOut);
    if (scenario >= 4) {
      const std::string other = "filter_" + std::to_string(3 - voice_filter) + "_";
      c.at(other + "model")->set(vital::constants::kReverbFilter);
      c.at(other + "resonance")->set(1);
      if (scenario < 6) c.at(other + "filter_input")->set(1);
      else {
        c.at(p + "filter_input")->set(1);
        c.at("osc_1_destination")->set(voice_filter == 1 ? vital::constants::kFilter2 : vital::constants::kFilter1);
      }
    }
    AudioSampleBuffer audio(2, 128); MidiBuffer midi;
    midi.addEvent(MidiMessage::noteOn(1, 60, .8f), 17);
    double held = 0;
    for (int b = 0; b < 100; ++b) {
      audio.clear(); synth.processBlock(audio, midi); midi.clear();
      if (b > 50) for (int i = 0; i < 128; ++i) held += std::abs(audio.getSample(0, i));
    }
    expect(held > .001, "Scream at 100% resonance is audible while the note is held");
    midi.addEvent(MidiMessage::noteOff(1, 60), 23);
    double release_energy = 0, late_energy = 0;
    for (int b = 0; b < 230; ++b) {
      audio.clear(); synth.processBlock(audio, midi); midi.clear();
      for (int i = 0; i < 128; ++i) {
        const double x = audio.getSample(0, i);
        if (b < 5) release_energy += x*x;
        if (b * 128 > (release + .02f) * 48000) late_energy += x*x;
      }
    }
    expect(release_energy > 1e-8, "Scream retains the requested Env 1 release");
    expect(late_energy < 1e-12, "100% Scream resonance must not ring after Env 1 ends, including serial Reverb chains");
    expect(synth.getEngine()->getNumActiveVoices() == 0, "Scream does not leave stuck tail voices");
    std::cout << "Scream release scenario=" << scenario << " release=" << release
              << " late energy=" << late_energy << '\n';
  }
}

void testPreset() {
  MemoryBlock state;
  std::vector<std::string> prefixes {"filter_1_", "filter_2_", "filter_fx_", "fx_slot_20_filter_fx_",
                                    "bus_3_fx_slot_20_filter_fx_"};
  {
    SynthPlugin synth;
    for (const auto& p : prefixes) {
      synth.parameterChanged(p + "model", vital::constants::kScreamFilter);
      synth.parameterChanged(p + "scream_amount", .73f);
      synth.parameterChanged(p + "scream_input_gain", 12);
      synth.parameterChanged(p + "scream_output_gain", -9);
      synth.parameterChanged(p + "scream_autogain", 0);
    }
    synth.getStateInformation(state);
  }
  SynthPlugin restored; restored.setStateInformation(state.getData(), int(state.getSize()));
  for (const auto& p : prefixes) {
    auto c = restored.getControls();
    expect(c.at(p + "model")->value() == vital::constants::kScreamFilter &&
           std::abs(c.at(p + "scream_amount")->value() - .73f) < 1e-6 &&
           c.at(p + "scream_input_gain")->value() == 12 && c.at(p + "scream_output_gain")->value() == -9 &&
           c.at(p + "scream_autogain")->value() == 0, "Scream filter settings survive preset recall in all contexts");
  }
}
int run() {
  testWaveshaper(); testReferenceTone(); testRatesAndState(); testVoiceReset(); testEnvelopeRelease(); testPreset();
  if (!failures) std::cout << "Atlas Scream filter tests passed\n";
  return failures ? 1 : 0;
}
}
