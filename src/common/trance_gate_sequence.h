// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cmath>

namespace vital::trance_gate {
  constexpr int kSteps = 32;
  constexpr int kModes = 5;
  constexpr int kGrids = 8;
  struct Step { double value = 0.0; int mode = 0; double curve = 0.0; };
  struct Envelope { double attack = 0.002, decay = 0.0, sustain = 1.0; };
  inline double beatsPerStep(int grid) { return std::ldexp(4.0, -std::clamp(grid, 0, kGrids - 1)); }
  inline double precisionCurve(double t, double c) {
    return c == 0.0 ? t : c > 0.0 ? 1.0 - std::pow(1.0 - t, 1.0 + c * 5.0)
                                      : std::pow(t, 1.0 + std::abs(c) * 5.0);
  }
  // BP StepGate 1.1's five retained shapes and attack/decay/sustain.
  // Medium/Short silence the shape at the gate boundary, so release was inaudible.
  inline double targetGain(const Step& step, const Envelope& adsr, double phase, double duration) {
    const double base = step.value + 1.0;
    const double gate = step.mode == 1 ? 0.5 : step.mode == 2 ? 0.25 : 1.001;
    double shape = phase < gate ? base : 0.0;
    if (step.mode >= 3) {
      const double curved = precisionCurve(phase, step.curve);
      if (step.mode == 3) shape = base * (1.0 - curved);
      else shape = base + (1.0 - base) * curved;
    }
    const double seconds = phase * duration;
    double env = 0.0;
    if (seconds < gate * duration) {
      if (seconds < adsr.attack) env = adsr.attack > 0.0 ? seconds / adsr.attack : 1.0;
      else env = adsr.decay > 0.0
          ? std::max(adsr.sustain, 1.0 - (1.0 - adsr.sustain) * ((seconds - adsr.attack) / adsr.decay))
          : adsr.sustain;
    }
    return shape * env;
  }
  inline int stepIndex(double whole_steps, int start, int stop) {
    const int range = std::abs(stop - start) + 1;
    // Positive modulo also supports host count-ins at negative PPQ positions.
    int position = static_cast<int>(std::fmod(whole_steps, range));
    if (position < 0) position += range;
    return start - 1 + (start <= stop ? position : -position);
  }
}
