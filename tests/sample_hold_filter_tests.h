// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "filter_module.h"
#include "sample_hold_filter.h"

namespace sample_hold_filter_tests {
using vital::SampleHoldFilter;

struct Fixture {
  SampleHoldFilter filter;
  vital::Output audio, reset, cutoff;
  vital::cr::Value style, crush;
  explicit Fixture(int style_index, float midi, float blend = 0) : style(style_index), crush(blend) {
    filter.plug(&audio, SampleHoldFilter::kAudio); filter.plug(&reset, SampleHoldFilter::kReset);
    filter.plug(&cutoff, SampleHoldFilter::kMidiCutoff); filter.plug(&style, SampleHoldFilter::kStyle);
    filter.plug(&crush, SampleHoldFilter::kCrush);
    filter.setSampleRate(48000);
    for (int i = 0; i < 128; ++i) cutoff.buffer[i] = midi;
  }
  void process(int block) {
    for (int i = 0; i < 128; ++i) {
      const float t = float(block * 128 + i);
      audio.buffer[i] = vital::poly_float(.7f * std::sin(t * .05f), .5f * std::cos(t * .013f), 0, 0);
    }
    filter.process(128);
  }
};

// MIDI note whose frequency is the given number of hertz.
float midiFor(float hertz) { return 69.f + 12.f * std::log2(hertz / 440.f); }

void testHoldRate() {
  // A 4.8 kHz hold rate at 48 kHz captures a new value every 10 samples.
  Fixture f(SampleHoldFilter::kHold, midiFor(4800));
  int changes = 0; float last = 0;
  for (int b = 0; b < 10; ++b) {
    f.process(b);
    for (int i = 0; i < 128; ++i) {
      const float y = f.filter.output()->buffer[i][0];
      if (b + i > 0 && y != last) ++changes;
      // Each held value is an input sample from within the last hold period.
      bool found = false;
      for (int back = 0; back < 11 && !found; ++back) {
        const float t = float(b * 128 + i - back);
        found = t >= 0 && std::abs(y - .7f * std::sin(t * .05f)) < 1e-5f;
      }
      expect(found, "Sample & Hold outputs a recent input sample");
      expect(f.filter.output()->buffer[i][2] == 0, "Sample & Hold voice lanes do not leak into one another");
      last = y;
    }
  }
  expect(std::abs(changes - 128) <= 2, "Sample & Hold at 4.8 kHz holds each value for 10 samples (changes=" +
         std::to_string(changes) + ")");

  // At or above the processing rate the input passes through unchanged.
  Fixture open(SampleHoldFilter::kHold, 150);  // About 47.4 kHz, above a 44.1 kHz processing rate.
  open.filter.setSampleRate(44100);
  for (int b = 0; b < 3; ++b) {
    open.process(b);
    for (int i = 0; i < 128; ++i)
      expect(open.filter.output()->buffer[i][0] == open.audio.buffer[i][0], "Maximum hold rate is transparent");
  }
}

void testStyles() {
  Fixture hold(SampleHoldFilter::kHold, midiFor(900)), inverted(SampleHoldFilter::kInverted, midiFor(900)),
          smooth(SampleHoldFilter::kSmooth, midiFor(900));
  float hold_jump = 0, smooth_jump = 0, last_hold = 0, last_smooth = 0;
  for (int b = 0; b < 20; ++b) {
    hold.process(b); inverted.process(b); smooth.process(b);
    for (int i = 0; i < 128; ++i) {
      const float h = hold.filter.output()->buffer[i][0], s = smooth.filter.output()->buffer[i][0];
      expect(inverted.filter.output()->buffer[i][0] == -h, "Inverted Sample & Hold negates the held steps");
      expect(std::abs(s) <= .7f + 1e-5f, "Smooth Sample & Hold stays within the input range");
      if (b > 0) { hold_jump = std::max(hold_jump, std::abs(h - last_hold)); smooth_jump = std::max(smooth_jump, std::abs(s - last_smooth)); }
      last_hold = h; last_smooth = s;
    }
  }
  expect(smooth_jump < .25f * hold_jump, "Smooth Sample & Hold ramps instead of stepping (hold jump=" +
         std::to_string(hold_jump) + ", smooth jump=" + std::to_string(smooth_jump) + ")");
}

void testCrushAndReset() {
  expect(SampleHoldFilter::crushBits(0) == 0, "Blend 0 disables bit crush");
  expectClose(SampleHoldFilter::crushBits(2), 1, 1e-6f, "Full Blend crushes to 1 bit");
  expectClose(SampleHoldFilter::crushBits(1), 8.5f, 1e-5f, "Half Blend crushes to 8.5 bits");
  Fixture crushed(SampleHoldFilter::kHold, 150, 2);
  for (int b = 0; b < 4; ++b) {
    crushed.process(b);
    for (int i = 0; i < 128; ++i) {
      const float y = crushed.filter.output()->buffer[i][0];
      expect(y == -1 || y == 0 || y == 1, "1-bit crush leaves only -1, 0 and 1");
    }
  }
  Fixture eight(SampleHoldFilter::kHold, 150, 7.f / 7.5f);  // 9 bits: steps of 1/256.
  eight.process(0);
  for (int i = 0; i < 128; ++i) {
    const float scaled = eight.filter.output()->buffer[i][0] * 256.f;
    expect(std::abs(scaled - std::round(scaled)) < 1e-4f, "Bit crush quantizes to the chosen depth");
  }

  // A new voice captures its input exactly on the note offset.
  Fixture f(SampleHoldFilter::kHold, midiFor(50));
  for (int b = 0; b < 4; ++b) f.process(b);
  f.reset.trigger(vital::constants::kFirstMask, vital::kVoiceOn, 37);
  const float before = f.filter.output()->buffer[127][2];
  f.process(4);
  expect(f.filter.output()->buffer[37][0] == f.audio.buffer[37][0], "Sample & Hold restarts at the new-note offset");
  expect(f.filter.output()->buffer[37][1] == f.audio.buffer[37][1], "Both stereo lanes of the new voice restart");
  expect(f.filter.output()->buffer[0][2] == before, "Resetting one voice leaves others untouched");
}

void testModuleRatesAndPreset() {
  for (int rate : {44100, 48000, 96000}) for (int factor : {1, 2, 4}) for (bool fx : {false, true}) {
    vital::FilterModule filter(fx ? "filter_fx" : "filter_1");
    vital::Output audio;
    filter.setMono(fx); filter.plug(&audio, vital::FilterModule::kAudio); filter.init();
    filter.setSampleRate(rate); filter.setOversampleAmount(factor);
    audio.ensureBufferSize(vital::kMaxBufferSize * factor);
    auto c = filter.getControls(); const std::string p = fx ? "filter_fx_" : "filter_1_";
    c.at(p + "on")->set(1); c.at(p + "model")->set(vital::constants::kSampleHoldFilter);
    double energy = 0;
    for (int b = 0; b < 40; ++b) {
      c.at(p + "cutoff")->set(8 + (b % 12) * 11);
      c.at(p + "style")->set(b % SampleHoldFilter::kNumStyles);
      c.at(p + "blend")->set((b % 5) * .5f);
      for (int i = 0; i < 128 * factor; ++i)
        audio.buffer[i] = vital::poly_float(.8f * std::sin((b * 128 * factor + i) * .09f),
                                            .3f * std::cos((b * 128 * factor + i) * .023f), 0, 0);
      tracked_allocations = 0; track_allocations = true;
      filter.process(128 * factor);
      track_allocations = false;
      expect(tracked_allocations == 0, "Sample & Hold selection/modulation must allocate no audio-thread memory");
      for (int i = 0; i < 128 * factor; ++i) {
        const auto y = filter.output()->buffer[i];
        expect(std::isfinite(y[0]) && std::abs(y[0]) <= 1.f, "Sample & Hold output stays bounded");
        energy += y[0] * y[0];
      }
    }
    expect(energy > .01, "Sample & Hold produces audio at all rates/qualities");
  }

  MemoryBlock state;
  const std::vector<std::string> prefixes {"filter_1_", "filter_2_", "filter_fx_"};
  {
    SynthPlugin synth;
    for (const auto& p : prefixes) {
      synth.parameterChanged(p + "model", vital::constants::kSampleHoldFilter);
      synth.parameterChanged(p + "style", SampleHoldFilter::kInverted);
      synth.parameterChanged(p + "blend", 1.25f);
    }
    synth.getStateInformation(state);
  }
  SynthPlugin restored; restored.setStateInformation(state.getData(), int(state.getSize()));
  auto c = restored.getControls();
  for (const auto& p : prefixes)
    expect(c.at(p + "model")->value() == vital::constants::kSampleHoldFilter &&
           c.at(p + "style")->value() == SampleHoldFilter::kInverted &&
           std::abs(c.at(p + "blend")->value() - 1.25f) < 1e-6f, "Sample & Hold settings survive preset recall");
}

int run() {
  testHoldRate(); testStyles(); testCrushAndReset(); testModuleRatesAndPreset();
  if (!failures) std::cout << "Atlas Sample & Hold filter tests passed\n";
  return failures ? 1 : 0;
}
}
