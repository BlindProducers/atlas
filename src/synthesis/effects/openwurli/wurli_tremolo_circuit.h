// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from OpenWurli 0.9.0, crates/openwurli-dsp/src/gen_tremolo.rs, which
// melange-solver 0.1.7 (7ecb36c) generated from OpenWurli's tremolo netlist
// ("* Wurlitzer 200A Tremolo Oscillator"; backward Euler, nodal solver,
// max_iter 100, no oversampling). Melange's notice on the generated code:
//
//   This code incorporates substantial portions of GPL-licensed source material
//   from the Melange project, including device model equations, Newton-Raphson
//   solver logic, matrix operations, and integration templates. It is not a
//   mechanical transformation of user input — it is a derived work of Melange's
//   copyrighted code and is therefore subject to the GNU General Public License
//   v3.0 or later. See https://www.gnu.org/licenses/gpl-3.0.html
#pragma once

namespace openwurli {
namespace tremolo_circuit {
  // The 200A's Twin-T tremolo oscillator: two BJTs (Ebers-Moll) and the LED
  // of the LG-1 opto, 8 nodes plus the supply branch. Nodal MNA with a
  // 5-dimensional Newton-Raphson on the device currents (Schur complement),
  // falling back to a backward-Euler re-solve when it does not converge.
  constexpr int kN = 9;  // augmented system size
  constexpr int kM = 5;  // nonlinear dimension
  // Matrices baked at this rate; other rates rebuild them.
  constexpr double kSampleRate = 48000.0;
  // Raw node voltages returned per sample: LED anode, LED cathode.
  constexpr int kNumTaps = 2;

  // Solver state. Holds no pointers; copying gives an identical oscillator.
  // Only the fields the default build reads are kept: the generated file's
  // diagnostic counters, chord-method LU cache, unused trapezoidal and
  // sub-step matrices, input/injection history and runtime device parameters
  // (never changed from their constants) are dropped.
  struct CircuitState {
    double v_prev[kN];
    double i_nl_prev[kM];
    double i_nl_prev_prev[kM];
    double a_neg[kN][kN];
    double a_neg_be[kN][kN];
    double s[kN][kN];
    double k[kM][kM];
    double s_ni[kN][kM];
    double s_be[kN][kN];
    double k_be[kM][kM];
    double s_ni_be[kN][kM];

    // CircuitState::default(): the DC operating point and the matrices at
    // kSampleRate, then 50 silent warm-up samples. The DC point is the
    // oscillator's unstable equilibrium; the tiny perturbation the warm-up
    // leaves is what starts it oscillating.
    static CircuitState makeDefault();
    // Rebuilds the matrices for another rate (keeps the transient state);
    // ignores rates that are not finite and positive.
    void setSampleRate(double sample_rate);
  };

  // One sample (the deck has no `.inject` sources). Writes the raw LED anode
  // and cathode voltages to `taps`.
  void processSample(double input, CircuitState& state, double (&taps)[kNumTaps]);
} // namespace tremolo_circuit
} // namespace openwurli
