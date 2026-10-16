// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <algorithm>
#include <cmath>
#include <string>

namespace vital::envelope_time {
  struct Division {
    const char* name;
    float beats;
  };

  // Quarter-note beats, sorted from shortest to longest for keyboard navigation.
  constexpr auto makeDivisions() {
    std::array<Division, 36> values {{
      {"1/64t", 1.0f / 24}, {"1/64", 1.0f / 16}, {"1/64d", 3.0f / 32},
      {"1/32t", 1.0f / 12}, {"1/32", 1.0f / 8}, {"1/32d", 3.0f / 16},
      {"1/16t", 1.0f / 6}, {"1/16", 1.0f / 4}, {"1/16d", 3.0f / 8},
      {"1/8t", 1.0f / 3}, {"1/8", 1.0f / 2}, {"1/8d", 3.0f / 4},
      {"1/4t", 2.0f / 3}, {"1/4", 1}, {"1/4d", 1.5f},
      {"1/2t", 4.0f / 3}, {"1/2", 2}, {"1/2d", 3},
      {"1/1t", 8.0f / 3}, {"1/1", 4}, {"1/1d", 6},
      {"2/1t", 16.0f / 3}, {"2/1", 8}, {"2/1d", 12},
      {"4/1t", 32.0f / 3}, {"4/1", 16}, {"4/1d", 24},
      {"8/1t", 64.0f / 3}, {"8/1", 32}, {"8/1d", 48},
      {"16/1t", 128.0f / 3}, {"16/1", 64}, {"16/1d", 96},
      {"32/1t", 256.0f / 3}, {"32/1", 128}, {"32/1d", 192}
    }};
    for (size_t i = 1; i < values.size(); ++i) {
      const auto value = values[i];
      size_t j = i;
      while (j > 0 && values[j - 1].beats > value.beats) {
        values[j] = values[j - 1];
        --j;
      }
      values[j] = value;
    }
    return values;
  }
  inline constexpr auto divisions = makeDivisions();

  inline bool isTimeParameter(const std::string& name) {
    if (name.rfind("env_", 0) != 0)
      return false;
    const auto separator = name.find('_', 4);
    if (separator == std::string::npos)
      return false;
    const auto suffix = name.substr(separator + 1);
    return suffix == "delay" || suffix == "attack" || suffix == "hold" ||
           suffix == "decay" || suffix == "release";
  }

  // A stage shorter than half the shortest division is no time at all: it
  // reads as -1, which the controls show as None and the envelope plays as
  // the stage's free time, 0 for Delay and Hold.
  inline constexpr float kNoneThresholdBeats = divisions[0].beats * 0.5f;

  inline int closestDivision(float seconds, float beats_per_second) {
    const float beats = seconds * std::max(beats_per_second, .0001f);
    if (beats < kNoneThresholdBeats)
      return -1;
    const float target = 1.0f / std::max(beats, .000001f);
    float distance = 1.0e30f;
    int closest = 0;
    for (int i = 0; i < static_cast<int>(divisions.size()); ++i) {
      const float candidate = std::abs(1.0f / divisions[i].beats - target);
      if (candidate <= distance) {
        closest = i;
        distance = candidate;
      }
    }
    return closest;
  }
}
