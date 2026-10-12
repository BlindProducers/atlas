// SPDX-License-Identifier: GPL-3.0-or-later
// OpenB3's tonewheel generator (setBfree's Beatrix tonegen.c): the wiring of
// the upper manual's key contacts to the 91 tonewheels, the wheels' gear-ratio
// frequencies and the key compression table. Everything here is fixed at
// OpenB3's defaults (tonegen.c initValues, no configuration file) and built
// once.
#pragma once

#include <array>
#include <vector>

namespace atlas::openb3 {
  // Wheels are numbered 1 to 91, as in tonegen.c; index 0 is unused.
  constexpr int kNumWheels = 91;
  // The upper manual: 61 keys, C2 (MIDI 36) to C7 (MIDI 96).
  constexpr int kNumKeys = 61;
  constexpr int kFirstKeyNote = 36;
  // The nine drawbars: 16', 5 1/3', 8', 4', 2 2/3', 2', 1 3/5', 1 1/3', 1'.
  constexpr int kNumDrawbars = 9;
  // The percussion taps (osc.perc.bus.a and .b) and the trigger bus muted
  // while percussion is on (osc.perc.bus.trig): 4', 2 2/3' and 1'.
  constexpr int kPercussionSecondBus = 3;
  constexpr int kPercussionThirdBus = 4;
  constexpr int kPercussionTriggerBus = 8;
  // tonegen.c's MAXKEYS: the key compression table's length.
  constexpr int kKeyCompressionSize = 128;

  // osc.temperament: gear ratios of a 60 Hz organ (the default), the
  // estimated ratios of a 50 Hz organ, or equal temperament. A = 440 Hz.
  enum Temperament { kGear60, kGear50, kEqual, kNumTemperaments };

  // One entry of a key's contribution list (tonegen.c's keyContrib): the
  // level at which `wheel` reaches drawbar bus `bus` (0-8) while the key is
  // down, through its tapering wire, the terminal crosstalk and the wiring
  // crosstalk.
  struct Contribution {
    short wheel;
    short bus;
    float level;
  };

  struct Tables {
    // Per key, sorted by wheel then bus, as compilePlayMatrix leaves it.
    std::array<std::vector<Contribution>, kNumKeys> contributions;
    // Per temperament, the frequency of wheels 1-91 (index 0 unused).
    std::array<std::array<double, kNumWheels + 1>, kNumTemperaments> frequencies {};
    std::array<float, kKeyCompressionSize> key_compression {};
  };

  // Built on first use; thread safe.
  const Tables& tables();
}
