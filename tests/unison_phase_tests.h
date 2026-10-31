// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "oscillator_warp_fixture.h"

namespace unison_phase_tests {
  using O = vital::SynthOscillator;

  // Mean power of the left and right lanes over many notes. With no detune every
  // unison oscillator plays the same pitch, so the power shows how far their
  // starting phases agree: all in phase gives the most, uniformly random phases
  // only the sum of each oscillator's own power.
  double notePower(float random_phase) {
    oscillator_warp_tests::Fixture<O> f(48000, 16, 1);
    f.set(O::kUnisonDetune, 0.0f);
    f.set(O::kRandomPhase, random_phase);
    auto* reset = f.controls[O::kReset].output();
    double power = 0.0;
    int count = 0;
    for (int note = 0; note < 256; ++note) {
      reset->trigger(vital::poly_mask(-1, -1, 0, 0), vital::kVoiceOn, 0);
      f.process(128);
      reset->trigger_mask = 0;
      f.process(128);
      for (int i = 0; i < 128; ++i) {
        const auto sample = f.oscillator->output(O::kRaw)->buffer[i];
        power += double(sample[0]) * sample[0] + double(sample[1]) * sample[1];
        count += 2;
      }
    }
    return power / count;
  }

  void testRandomPhaseCoversTheCycle() {
    // 16 oscillators at their default blend leave about 0.125 of the in-phase
    // power when their phases are uniform over the whole cycle. Converting a
    // negative phase to unsigned saturates to 0 on arm64, which left half of them
    // in phase and the rest in the first half of the cycle: about 0.35 or more.
    const double ratio = notePower(1.0f) / notePower(0.0f);
    std::cout << "Unison power at 100% phase randomization, relative to 0%: " << ratio << '\n';
    expect(ratio > 0.07 && ratio < 0.2, "100% phase randomization spreads unison phases over the whole cycle");
  }

  int run() {
    testRandomPhaseCoversTheCycle();
    return failures ? 1 : 0;
  }
}
