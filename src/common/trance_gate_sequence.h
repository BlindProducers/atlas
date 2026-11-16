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
  // Ramp up differs from the script: it rises from silence to the step's
  // level, as Ramp down falls from it, where the script rose from the step's
  // level to full (so a full-level step did not ramp at all).
  inline double targetGain(const Step& step, const Envelope& adsr, double phase, double duration) {
    const double base = step.value + 1.0;
    const double gate = step.mode == 1 ? 0.5 : step.mode == 2 ? 0.25 : 1.001;
    double shape = phase < gate ? base : 0.0;
    if (step.mode >= 3) {
      const double curved = precisionCurve(phase, step.curve);
      shape = step.mode == 3 ? base * (1.0 - curved) : base * curved;
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
  // Keeps a Ramp up step saved before behaviour version 4 sounding as it did:
  // a muted one rose from silence to full, which it now does at full level,
  // and a full-level one held its level, which Whole does. One in between
  // rose to full, and now does so from silence, the closest the shapes offer.
  // Returns false for a step that is not Ramp up.
  inline bool migrateRampUp(double& value, int& mode) {
    if (mode != 4) return false;
    mode = std::abs(value) < 0.005 ? 0 : 4;
    value = 0.0;
    return true;
  }
  inline int stepIndex(double whole_steps, int start, int stop) {
    const int range = std::abs(stop - start) + 1;
    // Positive modulo also supports host count-ins at negative PPQ positions.
    int position = static_cast<int>(std::fmod(whole_steps, range));
    if (position < 0) position += range;
    return start - 1 + (start <= stop ? position : -position);
  }
}
