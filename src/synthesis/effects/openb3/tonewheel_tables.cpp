// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from setBfree's tonegen.c as used by OpenB3 (GPL v2 or later):
// Copyright (C) 2003-2004 Fredrik Kilander, 2008-2018 Robin Gareus,
// 2012 Will Panther. See README.atlas.md.
#include "tonewheel_tables.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace atlas::openb3 {
  namespace {
    // tonegen.c's defaults (initValues).
    constexpr double kCompartmentCrosstalk = 0.01;
    constexpr double kTerminalStripCrosstalk = 0.01;
    constexpr double kWiringCrosstalk = 0.01;
    constexpr double kContributionFloor = 0.0000158;
    constexpr double kTuning = 440.0;

    // Taper levels in dB.
    constexpr double kTaperMinusThree = -10.0;
    constexpr double kTaperMinusTwo = -7.0;
    constexpr double kTaperMinusOne = -3.5;
    constexpr double kTaperReference = 0.0;
    constexpr double kTaperPlusOne = 3.5;
    constexpr double kTaperPlusTwo = 7.0;

    constexpr double kGears60[12][2] = {
      { 85, 104 }, { 71, 82 }, { 67, 73 }, { 35, 36 }, { 69, 67 }, { 12, 11 },
      { 37, 32 }, { 49, 40 }, { 48, 37 }, { 11, 8 }, { 67, 46 }, { 54, 35 }
    };
    constexpr double kGears50[12][2] = {
      { 17, 26 }, { 57, 82 }, { 11, 15 }, { 49, 63 }, { 33, 40 }, { 55, 63 },
      { 49, 53 }, { 49, 50 }, { 55, 53 }, { 11, 10 }, { 7, 6 }, { 90, 73 }
    };

    // The wheel sharing each wheel's compartment.
    constexpr short kWheelPairs[kNumWheels + 1] = {
      0,
      49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60,
      61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71, 72,
      73, 74, 75, 76, 77, 78, 79, 80, 81, 82, 83, 84,
      0, 0, 0, 0, 0, 85, 86, 87, 88, 89, 90, 91,
      1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12,
      13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24,
      25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36,
      42, 43, 44, 45, 46, 47, 48
    };

    // The wheels' order along the terminal soldering strip, as tonegen.c has
    // it (76 and 34 appear twice there; the first match wins).
    constexpr short kTerminalStrip[] = {
      85, 42, 30, 76, 66, 18, 6, 54, 90, 35, 83, 71, 23, 11, 59, 47, 40,
      28, 76, 64, 16, 4, 52, 88, 33, 81, 69, 21, 9, 57, 45, 34, 26, 74,
      62, 14, 2, 50, 86, 43, 31, 79, 67, 19, 7, 55, 91, 36, 84, 72, 24,
      12, 60, 48, 41, 29, 77, 65, 17, 5, 53, 89, 34, 82, 70, 22, 10, 58,
      46, 39, 27, 75, 63, 15, 3, 51, 87, 32, 80, 68, 20, 8, 56, 44, 37,
      25, 73, 61, 13, 1, 49,
      0
    };

    double dBToGain(double dB) { return std::pow(10.0, dB / 20.0); }

    // taperingModel(): the resistance wires' level per key and bus.
    double taper(int key, int bus) {
      double tapering = kTaperReference;
      switch (bus) {
        case 0:
          if (key < 12) tapering = kTaperMinusThree;
          else if (key < 17) tapering = kTaperMinusTwo;
          else if (key < 24) tapering = kTaperMinusOne;
          else if (key < 36) tapering = kTaperReference;
          else if (key < 48) tapering = kTaperPlusOne;
          else tapering = kTaperPlusTwo;
          break;
        case 1:
          if (key < 15) tapering = kTaperMinusOne;
          else if (key < 38) tapering = kTaperReference;
          else if (key < 50) tapering = kTaperPlusOne;
          else tapering = kTaperPlusTwo;
          break;
        case 2:
          if (key < 17) tapering = kTaperMinusTwo;
          else if (key < 22) tapering = kTaperMinusOne;
          else if (key < 37) tapering = kTaperReference;
          else if (key < 49) tapering = kTaperPlusOne;
          else tapering = kTaperPlusTwo;
          break;
        case 3:
          if (key < 17) tapering = kTaperMinusOne;
          else if (key < 39) tapering = kTaperReference;
          else tapering = kTaperMinusOne;
          break;
        case 4:
          if (key < 14) tapering = kTaperPlusTwo;
          else if (key < 20) tapering = kTaperPlusOne;
          else if (key < 40) tapering = kTaperReference;
          else if (key < 50) tapering = kTaperMinusOne;
          else tapering = kTaperMinusTwo;
          break;
        case 5:
          if (key < 12) tapering = kTaperPlusTwo;
          else if (key < 15) tapering = kTaperPlusOne;
          else if (key < 41) tapering = kTaperReference;
          else if (key < 54) tapering = kTaperMinusOne;
          else tapering = kTaperMinusTwo;
          break;
        case 6:
          if (key < 14) tapering = kTaperPlusOne;
          else if (key < 42) tapering = kTaperReference;
          else if (key < 50) tapering = kTaperMinusOne;
          else tapering = kTaperMinusTwo;
          break;
        case 7:
          if (key < 43) tapering = kTaperReference;
          else if (key < 48) tapering = kTaperMinusOne;
          else tapering = kTaperMinusTwo;
          break;
        case 8:
          if (key < 43) tapering = kTaperReference;
          else tapering = kTaperMinusTwo;
          break;
      }
      return dBToGain(tapering);
    }

    struct Link {  // a terminal (or wheel) heard on a bus at a level
      short terminal;
      short bus;
      float level;
    };
    struct Mix {
      short wheel;
      float level;
    };

    // findEastWestNeighbours().
    void neighbours(int wheel, int& east, int& west) {
      east = 0;
      west = 0;
      for (int i = 0; kTerminalStrip[i] > 0; ++i) {
        if (kTerminalStrip[i] == wheel) {
          if (i > 0) east = kTerminalStrip[i - 1];
          west = kTerminalStrip[i + 1];
          return;
        }
      }
    }

    Tables build() {
      Tables result;

      // applyDefaultConfiguration(): crosstalk at the generator's terminals.
      // Transformer crosstalk is 0 by default and skipped there too.
      std::array<std::vector<Mix>, kNumWheels + 1> terminal_mix;
      for (int i = 1; i <= kNumWheels; ++i) {
        terminal_mix[i].push_back({ static_cast<short>(i), static_cast<float>(1.0 - kCompartmentCrosstalk) });
        if (kWheelPairs[i] > 0)
          terminal_mix[i].push_back({ kWheelPairs[i], static_cast<float>(kCompartmentCrosstalk) });
      }
      for (int i = 1; i <= kNumWheels; ++i) {
        int east = 0, west = 0;
        neighbours(i, east, west);
        if (east > 0)
          terminal_mix[i].push_back({ static_cast<short>(east), static_cast<float>(kTerminalStripCrosstalk) });
        if (west > 0)
          terminal_mix[i].push_back({ static_cast<short>(west), static_cast<float>(kTerminalStripCrosstalk) });
      }

      // applyManualDefaults() for the upper manual of the TG_91FB12 generator
      // (91 wheels, the 16' foldback below terminal 13).
      constexpr int kOffsets[kNumDrawbars] = { -12, 7, 0, 12, 19, 24, 28, 31, 36 };
      constexpr int kLowerFoldback = 13, kUpperFoldback = 91, kLeastTerminal = 1;
      for (int k = 0; k < kNumKeys; ++k) {
        std::vector<Link> tapers;
        for (int b = 0; b < kNumDrawbars; ++b) {
          int terminal = (k + 13) + kOffsets[b];
          while (terminal < kLeastTerminal) terminal += 12;
          while (terminal < kLowerFoldback) terminal += 12;
          while (kUpperFoldback < terminal) terminal -= 12;
          tapers.push_back({ static_cast<short>(terminal), static_cast<short>(b), static_cast<float>(taper(k, b)) });
        }

        // applyDefaultCrosstalk(): each bus contact also hears the wires of
        // the contacts above and below it, weaker with distance.
        std::vector<Link> crosstalk;
        for (int b = 0; b < kNumDrawbars; ++b) {
          for (const Link& link : tapers) {
            if (link.bus == b)
              continue;
            crosstalk.push_back({ link.terminal, static_cast<short>(b),
                                  static_cast<float>((kWiringCrosstalk * link.level) / std::abs(b - link.bus)) });
          }
        }

        // compilePlayMatrix(): resolve terminals into wheels and sum per
        // wheel and bus, in the order cpmInsert adds them.
        float gain[kNumWheels + 1][kNumDrawbars] = {};
        bool seen[kNumWheels + 1][kNumDrawbars] = {};
        const auto insert = [&](const Link& link) {
          for (const Mix& mix : terminal_mix[link.terminal]) {
            const float level = mix.level * link.level;
            if (level == 0.0f)
              continue;
            if (seen[mix.wheel][link.bus])
              gain[mix.wheel][link.bus] += level;
            else {
              gain[mix.wheel][link.bus] = level;
              seen[mix.wheel][link.bus] = true;
            }
          }
        };
        for (const Link& link : tapers) insert(link);
        for (const Link& link : crosstalk) insert(link);

        auto& list = result.contributions[static_cast<size_t>(k)];
        for (int w = 1; w <= kNumWheels; ++w) {
          for (int b = 0; b < kNumDrawbars; ++b) {
            if (seen[w][b] && !(gain[w][b] < kContributionFloor))
              list.push_back({ static_cast<short>(w), static_cast<short>(b), gain[w][b] });
          }
        }
      }

      // initOscillators(): the 91-wheel generator, wheel 10 is A = 55 Hz.
      constexpr int kTuningWheel = 10;
      for (int i = 1; i <= kNumWheels; ++i) {
        const int frequency_number = i + 9 - kTuningWheel;
        const int note = frequency_number % 12;
        const int octave = frequency_number / 12;
        double teeth = std::pow(2.0, static_cast<double>(octave + 1));
        int select = note;
        if (84 <= frequency_number) {
          select += 5;
          teeth = 192.0;
        }
        result.frequencies[kGear60][i] = (20.0 * teeth * kGears60[select][0]) / kGears60[select][1] * (kTuning / 440.0);
        result.frequencies[kGear50][i] = (25.0 * teeth * kGears50[select][0]) / kGears50[select][1] * (kTuning / 440.0);
        result.frequencies[kEqual][i] = (kTuning / 8.0) * std::pow(2.0, static_cast<double>(i - kTuningWheel) / 12.0);
      }

      // initKeyCompTable(): output level against the number of keys down.
      result.key_compression[0] = result.key_compression[1] = 1.0f;
      result.key_compression[2] = static_cast<float>(dBToGain(-1.1598));
      result.key_compression[3] = static_cast<float>(dBToGain(-2.0291));
      result.key_compression[4] = static_cast<float>(dBToGain(-2.4987));
      result.key_compression[5] = static_cast<float>(dBToGain(-2.9952));
      result.key_compression[6] = static_cast<float>(dBToGain(-3.5218));
      result.key_compression[7] = static_cast<float>(dBToGain(-4.0823));
      result.key_compression[8] = static_cast<float>(dBToGain(-4.6815));
      result.key_compression[9] = static_cast<float>(dBToGain(-4.9975));
      result.key_compression[10] = static_cast<float>(dBToGain(-4.9998));
      const float u = -5.0f, v = -9.0f, m = static_cast<float>(1.0 / (kKeyCompressionSize - 12));
      for (int i = 11; i < kKeyCompressionSize; ++i) {
        const float a = static_cast<float>(i - 11);
        result.key_compression[static_cast<size_t>(i)] = static_cast<float>(dBToGain(u + ((v - u) * a * m)));
      }
      return result;
    }
  }

  const Tables& tables() {
    static const Tables instance = build();
    return instance;
  }
}
