// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "filter_module.h"
#include "ring_mod_filter.h"

namespace ring_mod_filter_tests {
using vital::RingModFilter;

void testCarrierShapes() {
  const float dt = 1e-4f;
  for (int w = 0; w < RingModFilter::kNumWaveforms; ++w)
    expect(std::abs(RingModFilter::carrier(w, .5f * dt, dt)) < .01f || w == RingModFilter::kSquare,
           "Ring Mod carriers other than square start at zero");
  expectClose(RingModFilter::carrier(RingModFilter::kSine, .25f, dt), 1, 1e-5f, "Sine peaks at a quarter cycle");
  expectClose(RingModFilter::carrier(RingModFilter::kSquare, .25f, dt), 1, 1e-6f, "Square is high on the first half");
  expectClose(RingModFilter::carrier(RingModFilter::kSquare, .75f, dt), -1, 1e-6f, "Square is low on the second half");
  expectClose(RingModFilter::carrier(RingModFilter::kSaw, .25f, dt), .5f, 1e-5f, "Saw rises through zero");
  expectClose(RingModFilter::carrier(RingModFilter::kSaw, .75f, dt), -.5f, 1e-5f, "Saw wraps at half a cycle");
  expectClose(RingModFilter::carrier(RingModFilter::kTriangle, .25f, dt), 1, 1e-6f, "Triangle peaks at a quarter cycle");
  expectClose(RingModFilter::carrier(RingModFilter::kTriangle, .75f, dt), -1, 1e-6f, "Triangle troughs at three quarters");

  // Each carrier is bipolar with no DC, so ring modulation removes the dry tone.
  for (int w = 0; w < RingModFilter::kNumWaveforms; ++w) {
    const int n = 4800; double sum = 0, peak = 0;
    for (int i = 0; i < n; ++i) {
      const float value = RingModFilter::carrier(w, (i + .5f) / n, 1.f / n);
      sum += value; peak = std::max(peak, double(std::abs(value)));
    }
    expect(std::abs(sum / n) < .001 && peak <= 1.001, "Ring Mod carrier " + std::to_string(w) + " is bipolar and bounded");
  }
}

void testRingModulation() {
  for (int style = 0; style < RingModFilter::kNumWaveforms; ++style) {
    RingModFilter filter;
    vital::Output audio, reset, cutoff;
    vital::cr::Value style_value(style);
    filter.plug(&audio, RingModFilter::kAudio); filter.plug(&reset, RingModFilter::kReset);
    filter.plug(&cutoff, RingModFilter::kMidiCutoff); filter.plug(&style_value, RingModFilter::kStyle);
    filter.setSampleRate(48000);
    for (int i = 0; i < 128; ++i) cutoff.buffer[i] = 69;  // 440 Hz carrier.

    // A DC input reproduces the carrier itself; a sine input loses its own tone.
    double dry_correlation = 0, energy = 0, carrier_error = 0;
    for (int b = 0; b < 300; ++b) {
      for (int i = 0; i < 128; ++i) {
        const int n = b * 128 + i;
        audio.buffer[i] = vital::poly_float(std::sin(2 * vital::kPi * 1000.f * n / 48000.f), 1, 0, 0);
      }
      filter.process(128);
      for (int i = 0; i < 128; ++i) {
        const int n = b * 128 + i;
        const auto y = filter.output()->buffer[i];
        dry_correlation += y[0] * audio.buffer[i][0]; energy += y[0] * y[0];
        if (style == RingModFilter::kSine)
          carrier_error = std::max(carrier_error, std::abs(y[1] - std::sin(2 * 3.14159265358979 * 440.0 * n / 48000.0)));
        expect(y[2] == 0 && y[3] == 0, "Ring Mod voice lanes do not leak into one another");
      }
    }
    expect(std::abs(dry_correlation) < .01 * energy, "Ring Mod style " + std::to_string(style) + " suppresses the dry tone");
    expect(energy > 1000, "Ring Mod style " + std::to_string(style) + " produces audio");
    if (style == RingModFilter::kSine) {
      std::cout << "Sine Ring Mod carrier maximum error=" << carrier_error << '\n';
      expect(carrier_error < 5e-3, "Sine Ring Mod multiplies by a 440 Hz sine at Cutoff 69");
    }

    // A new voice restarts its carrier at the note offset without touching others.
    for (int i = 0; i < 128; ++i) audio.buffer[i] = vital::poly_float(1, 1, 1, 1);
    reset.trigger(vital::constants::kFirstMask, vital::kVoiceOn, 37);
    filter.process(128);
    expectClose(filter.output()->buffer[37][0], RingModFilter::carrier(style, 0, 440.f / 48000.f), 1e-6f,
                "Ring Mod carrier restarts at the exact new-note offset");
    reset.clearTrigger();
  }
}

void testModuleRatesAndPreset() {
  for (int rate : {44100, 48000, 96000}) for (int factor : {1, 2, 4}) for (bool fx : {false, true}) {
    vital::FilterModule filter(fx ? "filter_fx" : "filter_1");
    vital::Output audio;
    filter.setMono(fx); filter.plug(&audio, vital::FilterModule::kAudio); filter.init();
    filter.setSampleRate(rate); filter.setOversampleAmount(factor);
    audio.ensureBufferSize(vital::kMaxBufferSize * factor);
    auto c = filter.getControls(); const std::string p = fx ? "filter_fx_" : "filter_1_";
    c.at(p + "on")->set(1); c.at(p + "model")->set(vital::constants::kRingModFilter);
    double energy = 0;
    for (int b = 0; b < 40; ++b) {
      c.at(p + "cutoff")->set(8 + (b % 12) * 11);
      c.at(p + "style")->set(b % RingModFilter::kNumWaveforms);
      for (int i = 0; i < 128 * factor; ++i)
        audio.buffer[i] = vital::poly_float(.8f * std::sin((b * 128 * factor + i) * .09f),
                                            .3f * std::cos((b * 128 * factor + i) * .023f), 0, 0);
      tracked_allocations = 0; track_allocations = true;
      filter.process(128 * factor);
      track_allocations = false;
      expect(tracked_allocations == 0, "Ring Mod selection/modulation must allocate no audio-thread memory");
      for (int i = 0; i < 128 * factor; ++i) {
        const auto y = filter.output()->buffer[i];
        expect(std::isfinite(y[0]) && std::abs(y[0]) <= .8f * 1.1f, "Ring Mod output stays within the input level");
        energy += y[0] * y[0];
      }
    }
    expect(energy > .01, "Ring Mod produces audio at all rates/qualities");
  }

  MemoryBlock state;
  const std::vector<std::string> prefixes {"filter_1_", "filter_2_", "filter_fx_"};
  {
    SynthPlugin synth;
    for (const auto& p : prefixes) {
      synth.parameterChanged(p + "model", vital::constants::kRingModFilter);
      synth.parameterChanged(p + "style", RingModFilter::kTriangle);
    }
    synth.getStateInformation(state);
  }
  SynthPlugin restored; restored.setStateInformation(state.getData(), int(state.getSize()));
  auto c = restored.getControls();
  for (const auto& p : prefixes)
    expect(c.at(p + "model")->value() == vital::constants::kRingModFilter &&
           c.at(p + "style")->value() == RingModFilter::kTriangle, "Ring Mod model and waveform survive preset recall");
}

int run() {
  testCarrierShapes(); testRingModulation(); testModuleRatesAndPreset();
  if (!failures) std::cout << "Atlas Ring Mod filter tests passed\n";
  return failures ? 1 : 0;
}
}
