// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "band_reject_core.h"
#include "band_reject_filter.h"
#include "filter_module.h"

#include <cmath>
#include <string>

namespace band_reject_filter_tests {
  using vital::BandRejectCore;
  using vital::BandRejectFilter;

  constexpr int kBlock = 128, kRate = 48000;

  // Lanes 0-1 get a stereo signal; a Filter FX slot's lanes 2-3 repeat it, a
  // voice filter's second voice stays silent.
  vital::poly_float signal(int n, bool mono) {
    const float t = float(n);
    const float left = .6f * std::sin(t * .05f) + .2f * std::sin(t * .37f);
    const float right = .5f * std::cos(t * .013f) + .2f * std::sin(t * .61f);
    return mono ? vital::poly_float(left, right, left, right) : vital::poly_float(left, right, 0, 0);
  }

  // The processor on its own, with exact inputs.
  struct Standalone {
    BandRejectFilter filter;
    vital::Output audio, reset, cutoff;
    vital::cr::Value resonance, drive_db, width;
    Standalone(bool mono, float midi, float resonance_value, float drive, float blend) :
        resonance(resonance_value), drive_db(drive), width(blend) {
      filter.setFxMode(mono);
      filter.plug(&audio, BandRejectFilter::kAudio);
      filter.plug(&reset, BandRejectFilter::kReset);
      filter.plug(&cutoff, BandRejectFilter::kMidiCutoff);
      filter.plug(&resonance, BandRejectFilter::kResonance);
      filter.plug(&drive_db, BandRejectFilter::kDriveGain);
      filter.plug(&width, BandRejectFilter::kWidth);
      filter.setSampleRate(kRate);
      for (int i = 0; i < kBlock; ++i)
        cutoff.buffer[i] = midi;
    }
    const vital::poly_float* process(int block, bool mono) {
      for (int i = 0; i < kBlock; ++i)
        audio.buffer[i] = signal(block * kBlock + i, mono);
      filter.process(kBlock);
      return filter.output()->buffer;
    }
  };

  // A filter module on the model: a voice filter, or Filter FX.
  struct Module {
    vital::FilterModule module;
    vital::Output audio, reset;
    std::string prefix;
    Module(bool mono, float midi, float resonance, float drive_db, float blend) :
        module(mono ? "filter_fx" : "filter_1"), prefix(mono ? "filter_fx_" : "filter_1_") {
      module.setMono(mono);
      module.plug(&audio, vital::FilterModule::kAudio);
      module.plug(&reset, vital::FilterModule::kReset);
      module.init();
      module.setSampleRate(kRate);
      auto controls = module.getControls();
      controls.at(prefix + "on")->set(1);
      controls.at(prefix + "model")->set(vital::constants::kBandRejectFilter);
      controls.at(prefix + "cutoff")->set(midi);
      controls.at(prefix + "resonance")->set(resonance);
      controls.at(prefix + "drive")->set(drive_db);
      controls.at(prefix + "blend")->set(blend);
      controls.at(prefix + "mix")->set(1);
    }
    const vital::poly_float* process(int block, bool mono) {
      for (int i = 0; i < kBlock; ++i)
        audio.buffer[i] = signal(block * kBlock + i, mono);
      module.process(kBlock);
      return module.output()->buffer;
    }
  };

  // The series the coefficients come from agree with the C library to a few
  // units of the last place over the ranges the model uses.
  void testSeries() {
    double worst_exp2 = 0, worst_sin = 0, worst_cos = 0;
    for (double x = -12; x <= 12; x += .001)
      worst_exp2 = std::max(worst_exp2, std::abs(BandRejectCore::exp2(x) / std::exp2(x) - 1));
    for (double x = 1e-4; x <= .8; x += .0001) {
      worst_sin = std::max(worst_sin, std::abs(BandRejectCore::sin(x) / std::sin(x) - 1));
      worst_cos = std::max(worst_cos, std::abs(BandRejectCore::cos(x) / std::cos(x) - 1));
    }
    std::cout << "Band Reject series: 2^x within " << worst_exp2 << ", sin within " << worst_sin << ", cos within "
              << worst_cos << " of the C library\n";
    expect(worst_exp2 < 1e-15 && worst_sin < 1e-15 && worst_cos < 1e-15,
           "the Band Reject coefficient series match the C library to a few units of the last place");
    expect(BandRejectCore::damping(0) == 2.0 && BandRejectCore::damping(1) == 1.0 / 16.0,
           "the Band Reject damping runs from 2 at no resonance to 1/16 at full resonance");
  }

  // The processor plays the core on every lane from its inputs, bit for bit:
  // the cutoff as a MIDI note, the resonance and Blend as the width (0.5 is
  // the script's Var 25 exactly; 0 dB of drive is a gain of 1 exactly).
  // Filter FX repeats lanes 0-1 in lanes 2-3, and nothing allocates.
  void testProcessorPlaysCore() {
    for (bool mono : { false, true }) {
      Standalone f(mono, 64, .5f, 0, .5f);
      BandRejectCore cores[4];
      for (auto& core : cores)
        core.setCutoff(64, 25, kRate);
      const double damping = BandRejectCore::damping(.5), drive = 1.0;
      bool same = true, mirrored = true;
      for (int block = 0; block < 20; ++block) {
        tracked_allocations.store(0);
        track_allocations.store(true);
        const auto* out = f.process(block, mono);
        track_allocations.store(false);
        expect(tracked_allocations.load() == 0, "Band Reject processing allocates no memory");
        for (int i = 0; i < kBlock; ++i) {
          for (int lane = 0; lane < 4; ++lane) {
            const float expected = float(cores[lane].tick(f.audio.buffer[i][lane], drive, damping));
            same = same && out[i][lane] == expected;
          }
          mirrored = mirrored && out[i][2] == out[i][0] && out[i][3] == out[i][1];
        }
      }
      expect(same, std::string("the Band Reject processor plays the core on every lane, ") + (mono ? "Filter FX" : "voice"));
      if (mono)
        expect(mirrored, "Filter FX Band Reject repeats its pair in lanes 2-3");
    }
  }

  // A voice restarts from silence at its note offset, and the other voice
  // plays on as if nothing happened.
  void testRestart() {
    Standalone restarted(false, 60, .7f, 0, 1), fresh(false, 60, .7f, 0, 1), untouched(false, 60, .7f, 0, 1);
    for (int block = 0; block < 6; ++block) {
      restarted.process(block, false);
      untouched.process(block, false);
    }
    restarted.reset.trigger(vital::constants::kFirstMask, vital::kVoiceOn, 37);
    const auto* out = restarted.process(6, false);
    restarted.reset.clearTrigger();
    const auto* before = untouched.process(6, false);
    for (int i = 0; i < kBlock; ++i)
      fresh.audio.buffer[i] = i < 37 ? vital::poly_float(0) : restarted.audio.buffer[i];
    fresh.filter.process(kBlock);
    const auto* expected = fresh.filter.output()->buffer;
    bool same = true, other_voice_same = true;
    for (int i = 0; i < kBlock; ++i) {
      same = same && (i < 37 || (out[i][0] == expected[i][0] && out[i][1] == expected[i][1]));
      other_voice_same = other_voice_same && out[i][2] == before[i][2] && out[i][3] == before[i][3];
    }
    expect(same, "Band Reject restarts a voice from silence at its note offset");
    expect(other_voice_same, "restarting one voice leaves the other voice's Band Reject alone");
  }

  // The module hands the processor its controls. Its cutoff control glides
  // with a 5 Hz corner and settles within a few thousandths of a semitone,
  // which the bound allows for; a crossed control would differ by far more.
  void testModuleWiring() {
    Module fx(true, 64, .5f, 6, .4f), voice(false, 64, .5f, 6, .4f);
    BandRejectCore cores[2];
    for (auto& core : cores)
      core.setCutoff(64, 20, kRate);
    const double damping = BandRejectCore::damping(.5), drive = std::pow(10.0, 6.0 / 20.0);
    double square_error = 0, energy = 0;
    for (int block = 0; block < 450; ++block) {
      const auto* out = fx.process(block, true);
      const auto* voice_out = voice.process(block, false);
      for (int i = 0; i < kBlock; ++i) {
        expect(voice_out[i][2] == 0 && voice_out[i][3] == 0, "a silent voice stays silent through Band Reject");
        for (int lane = 0; lane < 2; ++lane) {
          const double expected = cores[lane].tick(fx.audio.buffer[i][lane], drive, damping);
          if (block < 400)
            continue;
          square_error += (out[i][lane] - expected) * (out[i][lane] - expected);
          energy += expected * expected;
        }
      }
    }
    const double relative = std::sqrt(square_error / energy);
    std::cout << "Band Reject module vs core: relative RMS difference " << relative << "\n";
    expect(relative < 1e-2, "the Band Reject module hands the processor its controls (" + std::to_string(relative) + ")");
  }

  // Full resonance stays bounded at every cutoff and rate, in the two half
  // step regime and the once-per-sample regime near Nyquist.
  void testFullResonanceStaysBounded() {
    for (int rate : { 44100, 48000, 96000 })
      for (float cutoff : { 8.f, 40.f, 80.f, 120.f, 134.f, 136.f }) {
        Module f(false, cutoff, 1, 12, 0);
        f.module.setSampleRate(rate);
        float peak = 0;
        for (int block = 0; block < 40; ++block) {
          const auto* out = f.process(block, false);
          for (int i = 0; i < kBlock; ++i) {
            expect(std::isfinite(out[i][0]), "Band Reject at full resonance stays finite");
            peak = std::max(peak, std::abs(out[i][0]));
          }
        }
        expect(peak < 40, "Band Reject at full resonance stays bounded at " + std::to_string(rate) + " Hz, cutoff " +
                              std::to_string(int(cutoff)) + " (peak " + std::to_string(peak) + ")");
      }
  }

  int run() {
    testSeries();
    testProcessorPlaysCore();
    testRestart();
    testModuleWiring();
    testFullResonanceStaysBounded();
    std::cout << "Band Reject filter: " << failures << " failures\n";
    return failures ? 1 : 0;
  }
} // namespace band_reject_filter_tests
