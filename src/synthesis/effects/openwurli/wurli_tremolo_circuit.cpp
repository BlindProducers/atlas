// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from OpenWurli 0.9.0, crates/openwurli-dsp/src/gen_tremolo.rs, which
// melange-solver 0.1.7 (7ecb36c) generated from OpenWurli's tremolo netlist.
// Melange's notice on the generated code:
//
//   This code incorporates substantial portions of GPL-licensed source material
//   from the Melange project, including device model equations, Newton-Raphson
//   solver logic, matrix operations, and integration templates. It is not a
//   mechanical transformation of user input — it is a derived work of Melange's
//   copyrighted code and is therefore subject to the GNU General Public License
//   v3.0 or later. See https://www.gnu.org/licenses/gpl-3.0.html
//
// The constant matrices below are the generated file's decimal literals,
// copied verbatim. The solver keeps the generated structure and operand order
// (including its sparse sums and the NaN-honest `!(a <= b)` convergence
// tests); build without fast-math and with -ffp-contract=off. Helpers of the
// generated file that this deck never calls (fast_ln, fetlim, the diode
// series-resistance and breakdown models, BJT parasitics and the Gummel-Poon
// branch: both BJTs are Ebers-Moll) are not ported.
#include "wurli_tremolo_circuit.h"

#include "wurli_math.h"

#include <cmath>
#include <cstdint>

namespace openwurli {
namespace tremolo_circuit {
  namespace {
    // A finite per-sample state beyond this is a diverged solve (an exp()
    // clamp or a near-singular LU) and resets like a NaN would.
    constexpr double kStateMaxPlausibleMagnitude = 1e6;
    constexpr int kMaxIter = 100;
    constexpr int kInputNode = 0;
    constexpr double kInputResistance = 1.00000000000000000e7;
    constexpr int kTapNodes[kNumTaps] = { 6, 7 };

    // G: conductance matrix (sample-rate independent).
    constexpr double kG[kN][kN] = {
      {
        2.14336546682102638e-4, 0.00000000000000000e0, 0.00000000000000000e0,
        -1.47058823529411773e-6, 0.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, -2.12765957446808509e-4, 0.00000000000000000e0,
      },
      {
        0.00000000000000000e0, 3.70370380370370393e-5, 0.00000000000000000e0,
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
      },
      {
        0.00000000000000000e0, 0.00000000000000000e0, 2.94117747058823528e-6,
        -1.47058823529411773e-6, 0.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
      },
      {
        -1.47058823529411773e-6, 0.00000000000000000e0, -1.47058823529411773e-6,
        2.94117747058823528e-6, 0.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
      },
      {
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, 1.00000001000000001e-4, 0.00000000000000000e0,
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
      },
      {
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, 0.00000000000000000e0, 1.47058823629411762e-3,
        -1.47058823529411763e-3, 0.00000000000000000e0, 1.00000000000000000e0,
      },
      {
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, 0.00000000000000000e0, -1.47058823529411763e-3,
        1.47058823629411762e-3, 0.00000000000000000e0, 0.00000000000000000e0,
      },
      {
        -2.12765957446808509e-4, 0.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, 2.12765958446808505e-4, 0.00000000000000000e0,
      },
      {
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, 0.00000000000000000e0, 1.00000000000000000e0,
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
      },
    };

    // C: capacitance matrix (sample-rate independent).
    constexpr double kC[kN][kN] = {
      {
        1.20000999999999990e-7, -1.19999999999999989e-7, 0.00000000000000000e0,
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
      },
      {
        -1.19999999999999989e-7, 2.39999999999999979e-7, -1.19999999999999989e-7,
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
      },
      {
        0.00000000000000000e0, -1.19999999999999989e-7, 1.19999999999999989e-7,
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
      },
      {
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
        1.19999999999999989e-7, 0.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
      },
      {
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
      },
      {
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
      },
      {
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
      },
      {
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
      },
      {
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
      },
    };

    // N_v: extracts the controlling voltages from the node voltages (M x N).
    constexpr double kNV[kM][kN] = {
      {
        0.00000000000000000e0, 0.00000000000000000e0, 1.00000000000000000e0,
        0.00000000000000000e0, -1.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
      },
      {
        -1.00000000000000000e0, 0.00000000000000000e0, 1.00000000000000000e0,
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
      },
      {
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, 1.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
      },
      {
        -1.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, 1.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
      },
      {
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
        1.00000000000000000e0, -1.00000000000000000e0, 0.00000000000000000e0,
      },
    };

    // N_i: maps the nonlinear currents to node injections (N x M).
    constexpr double kNI[kN][kM] = {
      {
        -1.00000000000000000e0, 0.00000000000000000e0, -1.00000000000000000e0,
        0.00000000000000000e0, 0.00000000000000000e0,
      },
      {
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, 0.00000000000000000e0,
      },
      {
        0.00000000000000000e0, -1.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, 0.00000000000000000e0,
      },
      {
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, 0.00000000000000000e0,
      },
      {
        1.00000000000000000e0, 1.00000000000000000e0, 0.00000000000000000e0,
        -1.00000000000000000e0, 0.00000000000000000e0,
      },
      {
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, 0.00000000000000000e0,
      },
      {
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, -1.00000000000000000e0,
      },
      {
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, 1.00000000000000000e0,
      },
      {
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, 0.00000000000000000e0,
      },
    };

    // History matrix at SAMPLE_RATE.
    constexpr double kANegDefault[kN][kN] = {
      {
        5.76004799999999938e-3, -5.75999999999999950e-3, 0.00000000000000000e0,
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
      },
      {
        -5.75999999999999950e-3, 1.15199999999999990e-2, -5.75999999999999950e-3,
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
      },
      {
        0.00000000000000000e0, -5.75999999999999950e-3, 5.75999999999999950e-3,
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
      },
      {
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
        5.75999999999999950e-3, 0.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
      },
      {
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
      },
      {
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
      },
      {
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
      },
      {
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
      },
      {
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
      },
    };

    // Backward-Euler history matrix at SAMPLE_RATE.
    constexpr double kANegBeDefault[kN][kN] = {
      {
        5.76004799999999938e-3, -5.75999999999999950e-3, 0.00000000000000000e0,
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
      },
      {
        -5.75999999999999950e-3, 1.15199999999999990e-2, -5.75999999999999950e-3,
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
      },
      {
        0.00000000000000000e0, -5.75999999999999950e-3, 5.75999999999999950e-3,
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
      },
      {
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
        5.75999999999999950e-3, 0.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
      },
      {
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
      },
      {
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
      },
      {
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
      },
      {
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
      },
      {
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
      },
    };

    // S = A^-1 at SAMPLE_RATE.
    constexpr double kSDefault[kN][kN] = {
      {
        2.42023670543429798e4, 2.40355537935073698e4, 2.40232901368543025e4,
        1.23062307805524789e1, 0.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, 2.42023669405918481e4, 0.00000000000000000e0,
      },
      {
        2.40355537935073771e4, 2.40423047788092626e4, 2.40300376662970521e4,
        1.22653851820618520e1, -0.00000000000000000e0, -0.00000000000000000e0,
        -0.00000000000000000e0, 2.40355536805402735e4, -0.00000000000000000e0,
      },
      {
        2.40232901368543062e4, 2.40300376662970557e4, 2.41912993309964077e4,
        1.23034065171696465e1, -0.00000000000000000e0, -0.00000000000000000e0,
        -0.00000000000000000e0, 2.40232900239448390e4, -0.00000000000000000e0,
      },
      {
        1.23062307805524789e1, 1.22653851820618520e1, 1.23034065171696447e1,
        1.73528786750867141e2, 0.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, 1.23062307227131971e1, 0.00000000000000000e0,
      },
      {
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, 9.99999990000000071e3, 0.00000000000000000e0,
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
      },
      {
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, 0.00000000000000000e0, 1.00000000000000000e0,
      },
      {
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
        6.79999999537600047e2, 0.00000000000000000e0, 9.99999999320000055e-1,
      },
      {
        2.42023669405918554e4, 2.40355536805402735e4, 2.40232900239448390e4,
        1.23062307227131971e1, 0.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, 2.89023668047507344e4, 0.00000000000000000e0,
      },
      {
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, 0.00000000000000000e0, 1.00000000000000000e0,
        9.99999999320000055e-1, 0.00000000000000000e0, -1.99999999200839440e-12,
      },
    };

    // K = N_v S N_i at SAMPLE_RATE.
    constexpr double kKDefault[kM][kM] = {
      {
        -3.40232900368543051e4, -3.41912992309964102e4, -2.40232901368543062e4,
        9.99999990000000071e3, 2.40232900239448390e4,
      },
      {
        1.79076917488673644e2, -1.68009194142105116e2, 1.79076917488673644e2,
        0.00000000000000000e0, -1.79076916647009057e2,
      },
      {
        9.99999990000000071e3, 9.99999990000000071e3, 0.00000000000000000e0,
        -9.99999990000000071e3, 0.00000000000000000e0,
      },
      {
        3.42023669543429787e4, 3.40232900368543051e4, 2.42023670543429798e4,
        -9.99999990000000071e3, -2.42023669405918481e4,
      },
      {
        2.42023669405918554e4, 2.40232900239448390e4, 2.42023669405918554e4,
        0.00000000000000000e0, -2.95823668042883328e4,
      },
    };

    // S N_i at SAMPLE_RATE.
    constexpr double kSNiDefault[kN][kM] = {
      {
        -2.42023670543429798e4, -2.40232901368543025e4, -2.42023670543429798e4,
        0.00000000000000000e0, 2.42023669405918481e4,
      },
      {
        -2.40355537935073771e4, -2.40300376662970521e4, -2.40355537935073771e4,
        0.00000000000000000e0, 2.40355536805402735e4,
      },
      {
        -2.40232901368543062e4, -2.41912993309964077e4, -2.40232901368543062e4,
        0.00000000000000000e0, 2.40232900239448390e4,
      },
      {
        -1.23062307805524789e1, -1.23034065171696447e1, -1.23062307805524789e1,
        0.00000000000000000e0, 1.23062307227131971e1,
      },
      {
        9.99999990000000071e3, 9.99999990000000071e3, 0.00000000000000000e0,
        -9.99999990000000071e3, 0.00000000000000000e0,
      },
      {
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, 0.00000000000000000e0,
      },
      {
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, -6.79999999537600047e2,
      },
      {
        -2.42023669405918554e4, -2.40232900239448390e4, -2.42023669405918554e4,
        0.00000000000000000e0, 2.89023668047507344e4,
      },
      {
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, -9.99999999320000055e-1,
      },
    };

    // S_be = A_be^-1 at SAMPLE_RATE.
    constexpr double kSBeDefault[kN][kN] = {
      {
        2.42023670543429798e4, 2.40355537935073698e4, 2.40232901368543025e4,
        1.23062307805524789e1, 0.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, 2.42023669405918481e4, 0.00000000000000000e0,
      },
      {
        2.40355537935073771e4, 2.40423047788092626e4, 2.40300376662970521e4,
        1.22653851820618520e1, -0.00000000000000000e0, -0.00000000000000000e0,
        -0.00000000000000000e0, 2.40355536805402735e4, -0.00000000000000000e0,
      },
      {
        2.40232901368543062e4, 2.40300376662970557e4, 2.41912993309964077e4,
        1.23034065171696465e1, -0.00000000000000000e0, -0.00000000000000000e0,
        -0.00000000000000000e0, 2.40232900239448390e4, -0.00000000000000000e0,
      },
      {
        1.23062307805524789e1, 1.22653851820618520e1, 1.23034065171696447e1,
        1.73528786750867141e2, 0.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, 1.23062307227131971e1, 0.00000000000000000e0,
      },
      {
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, 9.99999990000000071e3, 0.00000000000000000e0,
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
      },
      {
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, 0.00000000000000000e0, 1.00000000000000000e0,
      },
      {
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
        6.79999999537600047e2, 0.00000000000000000e0, 9.99999999320000055e-1,
      },
      {
        2.42023669405918554e4, 2.40355536805402735e4, 2.40232900239448390e4,
        1.23062307227131971e1, 0.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, 2.89023668047507344e4, 0.00000000000000000e0,
      },
      {
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, 0.00000000000000000e0, 1.00000000000000000e0,
        9.99999999320000055e-1, 0.00000000000000000e0, -1.99999999200839440e-12,
      },
    };

    // K_be = N_v S_be N_i at SAMPLE_RATE.
    constexpr double kKBeDefault[kM][kM] = {
      {
        -3.40232900368543051e4, -3.41912992309964102e4, -2.40232901368543062e4,
        9.99999990000000071e3, 2.40232900239448390e4,
      },
      {
        1.79076917488673644e2, -1.68009194142105116e2, 1.79076917488673644e2,
        0.00000000000000000e0, -1.79076916647009057e2,
      },
      {
        9.99999990000000071e3, 9.99999990000000071e3, 0.00000000000000000e0,
        -9.99999990000000071e3, 0.00000000000000000e0,
      },
      {
        3.42023669543429787e4, 3.40232900368543051e4, 2.42023670543429798e4,
        -9.99999990000000071e3, -2.42023669405918481e4,
      },
      {
        2.42023669405918554e4, 2.40232900239448390e4, 2.42023669405918554e4,
        0.00000000000000000e0, -2.95823668042883328e4,
      },
    };

    // S_be N_i at SAMPLE_RATE.
    constexpr double kSNiBeDefault[kN][kM] = {
      {
        -2.42023670543429798e4, -2.40232901368543025e4, -2.42023670543429798e4,
        0.00000000000000000e0, 2.42023669405918481e4,
      },
      {
        -2.40355537935073771e4, -2.40300376662970521e4, -2.40355537935073771e4,
        0.00000000000000000e0, 2.40355536805402735e4,
      },
      {
        -2.40232901368543062e4, -2.41912993309964077e4, -2.40232901368543062e4,
        0.00000000000000000e0, 2.40232900239448390e4,
      },
      {
        -1.23062307805524789e1, -1.23034065171696447e1, -1.23062307805524789e1,
        0.00000000000000000e0, 1.23062307227131971e1,
      },
      {
        9.99999990000000071e3, 9.99999990000000071e3, 0.00000000000000000e0,
        -9.99999990000000071e3, 0.00000000000000000e0,
      },
      {
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, 0.00000000000000000e0,
      },
      {
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, -6.79999999537600047e2,
      },
      {
        -2.42023669405918554e4, -2.40232900239448390e4, -2.42023669405918554e4,
        0.00000000000000000e0, 2.89023668047507344e4,
      },
      {
        0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
        0.00000000000000000e0, -9.99999999320000055e-1,
      },
    };

    // RHS contribution of the DC sources.
    constexpr double kRhsConst[kN] = {
      0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
      0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
      0.00000000000000000e0, 0.00000000000000000e0, 1.45000000000000000e1,
    };

    // RHS contribution of the DC sources (backward Euler).
    constexpr double kRhsConstBe[kN] = {
      0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
      0.00000000000000000e0, 0.00000000000000000e0, 0.00000000000000000e0,
      0.00000000000000000e0, 0.00000000000000000e0, 1.45000000000000000e1,
    };

    // DC operating point: node voltages.
    constexpr double kDcOp[kN] = {
      4.36625634132295470e0, 0.00000000000000000e0, 1.23603689217946333e0,
      2.80114566436168344e0, 6.56905385625590865e-1, 1.45000000000000000e1,
      1.34136800666353189e1, 1.18746439971943616e1, -1.59752932827159078e-3,
    };

    // DC operating point: nonlinear device currents.
    constexpr double kDcNlI[kM] = {
      7.50088406372789279e-5, 4.83927999324380227e-7, 1.51934555887281463e-3,
      9.80222940729557764e-6, 1.59752930035797621e-3,
    };

    // Devices 0 and 1: the oscillator's two BJTs (Ebers-Moll). Device 2: the LED.
    struct BjtModel {
      double is;
      double vt;
      double beta_f;
      double beta_r;
      double nf;
      double nr;
      double ise;
      double ne;
      double isc;
      double nc;
      double sign;
      double vcrit;
    };

    constexpr BjtModel kDevice0 = {
      1.40000000000000003e-14, 2.58519910000000012e-2, 1.55000000000000000e2,
      3.00000000000000000e0, 1.00000000000000000e0, 1.00000000000000000e0,
      0.00000000000000000e0, 1.50000000000000000e0, 0.00000000000000000e0,
      2.00000000000000000e0, 1.0, 7.21213101001093038e-1
    };
    constexpr BjtModel kDevice1 = {
      1.40000000000000003e-14, 2.58519910000000012e-2, 1.55000000000000000e2,
      3.00000000000000000e0, 1.00000000000000000e0, 1.00000000000000000e0,
      0.00000000000000000e0, 1.50000000000000000e0, 0.00000000000000000e0,
      2.00000000000000000e0, 1.0, 7.21213101001093038e-1
    };
    constexpr double kDevice2Is = 2.23239999999999997e-29;
    constexpr double kDevice2NVt = 2.58519910000000012e-2;
    constexpr double kDevice2Vcrit = 1.60204656916718746e0;

    // pnjlim's thermal voltage and critical voltage for each nonlinear dimension.
    constexpr double kLimitVt[kM] = { kDevice0.vt, kDevice0.vt, kDevice1.vt, kDevice1.vt, kDevice2NVt };
    constexpr double kLimitVcrit[kM] = {
      kDevice0.vcrit, kDevice0.vcrit, kDevice1.vcrit, kDevice1.vcrit, kDevice2Vcrit
    };

    // exp() for |x| <= 40 by range reduction and a 5th-order polynomial
    // (< 0.0004 % error), the generated default (no `melange_precise_exp`).
    inline double fastExp(double x) {
      x = math::clamp(x, -40.0, 40.0);
      constexpr double kLn2Inv = 1.44269504088896340735992468100189214;  // LOG2_E
      constexpr double kLn2Hi = 0.6931471803691238;
      constexpr double kLn2Lo = 1.9082149292705877e-10;
      constexpr double kShift = 6755399441055744.0;  // 2^52 + 2^51
      const double z = x * kLn2Inv + kShift;
      const int64_t n_i64 = static_cast<int64_t>(math::toBits(z) - math::toBits(kShift));
      const double n = static_cast<double>(n_i64);
      const double f = (x - n * kLn2Hi) - n * kLn2Lo;
      const double p = 1.0
                       + f * (1.0
                              + f * (0.5
                                     + f * (0.16666666666666607
                                            + f * (0.04166666666665876 + f * 0.008333333333492337))));
      const double pow2n = math::fromBits((static_cast<uint64_t>(1023) + static_cast<uint64_t>(n_i64)) << 52);
      return p * pow2n;
    }

    // SPICE3f5 DEVpnjlim: log-compresses large forward steps on an exponential
    // junction.
    inline double pnjlim(double vnew, double vold, double vt, double vcrit) {
      if (vnew > vcrit && std::fabs(vnew - vold) > vt + vt) {
        if (vold >= 0.0) {
          const double arg = 1.0 + (vnew - vold) / vt;
          if (arg > 0.0)
            return vold + vt * std::log(arg);
          return vcrit;
        }
        return vt * std::log(vnew / vt);
      }
      return vnew;
    }

    // Above this forward current the diode's I-V curve continues linearly.
    constexpr double kDiodeMaxForwardCurrent = 1e3;

    // i = is (exp(v / n_vt) - 1), extended linearly below -40 n_vt, and above
    // 40 n_vt continued in log-current space up to kDiodeMaxForwardCurrent.
    inline double diodeCurrent(double v_d, double is, double n_vt) {
      const double x = v_d / n_vt;
      if (x < -40.0) {
        const double exp_clamp = fastExp(-40.0);
        const double i_clamp = is * (exp_clamp - 1.0);
        const double g_clamp = (is / n_vt) * exp_clamp;
        return i_clamp + g_clamp * (v_d + 40.0 * n_vt);
      }
      if (x <= 40.0)
        return is * (fastExp(x) - 1.0);
      const double ln_is = std::log(is);
      const double y = x + ln_is;
      const double y_max = std::fmax(40.0 + ln_is, std::log(kDiodeMaxForwardCurrent));
      if (y <= y_max)
        return std::exp(y) - is;
      const double i_c = std::exp(y_max);
      return (i_c - is) + i_c * (y - y_max);
    }

    // The derivative of diodeCurrent in all three regions.
    inline double diodeConductance(double v_d, double is, double n_vt) {
      const double x = v_d / n_vt;
      if (x <= 40.0)
        return (is / n_vt) * fastExp(std::fmax(x, -40.0));
      const double ln_is = std::log(is);
      const double y_max = std::fmax(40.0 + ln_is, std::log(kDiodeMaxForwardCurrent));
      const double y = std::fmin(x + ln_is, y_max);
      return std::exp(y) / n_vt;
    }

    struct BjtEval {
      double ic;
      double ib;
      double jacobian[4];  // dIc/dVbe, dIc/dVbc, dIb/dVbe, dIb/dVbc
    };

    // bjt_evaluate, Ebers-Moll path.
    inline BjtEval bjtEvaluate(double vbe, double vbc, const BjtModel& model) {
      const double is = model.is;
      const double vt = model.vt;
      const double vbe_eff = model.sign * vbe;
      const double vbc_eff = model.sign * vbc;
      const double nf_vt = model.nf * vt;
      const double nr_vt = model.nr * vt;

      const double exp_be = fastExp(vbe_eff / nf_vt);
      const double exp_bc = fastExp(vbc_eff / nr_vt);
      const double exp_be_leak = model.ise > 0.0 ? fastExp(vbe_eff / (model.ne * vt)) : 0.0;
      const double exp_bc_leak = model.isc > 0.0 ? fastExp(vbc_eff / (model.nc * vt)) : 0.0;

      const double i_cc = is * (exp_be - exp_bc);

      const double ib_fwd = is / model.beta_f * (exp_be - 1.0);
      const double ib_rev = is / model.beta_r * (exp_bc - 1.0);
      const double ib_leak_be = model.ise > 0.0 ? model.ise * (exp_be_leak - 1.0) : 0.0;
      const double ib_leak_bc = model.isc > 0.0 ? model.isc * (exp_bc_leak - 1.0) : 0.0;

      const double dib_fwd_dvbe = (is / (model.beta_f * nf_vt)) * exp_be;
      const double dib_rev_dvbc = (is / (model.beta_r * nr_vt)) * exp_bc;
      const double dib_leak_dvbe = model.ise > 0.0 ? (model.ise / (model.ne * vt)) * exp_be_leak : 0.0;
      const double dib_leak_dvbc = model.isc > 0.0 ? (model.isc / (model.nc * vt)) * exp_bc_leak : 0.0;

      BjtEval result;
      result.ic = model.sign * (i_cc - is / model.beta_r * (exp_bc - 1.0));
      result.ib = model.sign * (ib_fwd + ib_rev + ib_leak_be + ib_leak_bc);
      result.jacobian[0] = is / nf_vt * exp_be;
      result.jacobian[1] = -(is / nr_vt) * exp_bc - (is / (model.beta_r * nr_vt)) * exp_bc;
      result.jacobian[2] = dib_fwd_dvbe + dib_leak_dvbe;
      result.jacobian[3] = dib_rev_dvbc + dib_leak_dvbc;
      return result;
    }

    // Device currents for the controlling voltages `v_d`, and the block-
    // diagonal device Jacobian (only its BJT 2x2 blocks and the LED entry
    // are written).
    inline void evaluateDevices(const double (&v_d)[kM], double (&i_dev)[kM], double (&jdev)[kM][kM]) {
      const BjtEval bjt0 = bjtEvaluate(v_d[0], v_d[1], kDevice0);
      const BjtEval bjt1 = bjtEvaluate(v_d[2], v_d[3], kDevice1);
      i_dev[0] = bjt0.ic;
      i_dev[1] = bjt0.ib;
      jdev[0][0] = bjt0.jacobian[0];
      jdev[0][1] = bjt0.jacobian[1];
      jdev[1][0] = bjt0.jacobian[2];
      jdev[1][1] = bjt0.jacobian[3];
      i_dev[2] = bjt1.ic;
      i_dev[3] = bjt1.ib;
      jdev[2][2] = bjt1.jacobian[0];
      jdev[2][3] = bjt1.jacobian[1];
      jdev[3][2] = bjt1.jacobian[2];
      jdev[3][3] = bjt1.jacobian[3];
      i_dev[4] = diodeCurrent(v_d[4], kDevice2Is, kDevice2NVt);
      jdev[4][4] = diodeConductance(v_d[4], kDevice2Is, kDevice2NVt);
    }

    // NR Jacobian J = I - jdev K, written out per device block as generated.
    inline void buildJacobian(const double (&jdev)[kM][kM], const double (&k)[kM][kM],
                              double (&a)[kM][kM]) {
      for (int c = 0; c < kM; ++c) {
        a[0][c] = (c == 0 ? 1.0 : 0.0) - jdev[0][0] * k[0][c] - jdev[0][1] * k[1][c];
        a[1][c] = (c == 1 ? 1.0 : 0.0) - jdev[1][0] * k[0][c] - jdev[1][1] * k[1][c];
        a[2][c] = (c == 2 ? 1.0 : 0.0) - jdev[2][2] * k[2][c] - jdev[2][3] * k[3][c];
        a[3][c] = (c == 3 ? 1.0 : 0.0) - jdev[3][2] * k[2][c] - jdev[3][3] * k[3][c];
        a[4][c] = (c == 4 ? 1.0 : 0.0) - jdev[4][4] * k[4][c];
      }
    }

    // Gaussian elimination with partial pivoting; false if singular. The
    // solution replaces `b`.
    inline bool solve5(double (&a)[kM][kM], double (&b)[kM]) {
      for (int col = 0; col < kM; ++col) {
        int max_row = col;
        double max_val = std::fabs(a[col][col]);
        for (int row = col + 1; row < kM; ++row) {
          if (std::fabs(a[row][col]) > max_val) {
            max_val = std::fabs(a[row][col]);
            max_row = row;
          }
        }
        if (max_val < 1e-15)
          return false;
        if (max_row != col) {
          for (int j = 0; j < kM; ++j) {
            const double swap = a[col][j];
            a[col][j] = a[max_row][j];
            a[max_row][j] = swap;
          }
          const double swap = b[col];
          b[col] = b[max_row];
          b[max_row] = swap;
        }
        const double pivot = a[col][col];
        for (int row = col + 1; row < kM; ++row) {
          const double factor = a[row][col] / pivot;
          for (int j = col + 1; j < kM; ++j)
            a[row][j] -= factor * a[col][j];
          b[row] -= factor * b[col];
        }
      }
      for (int i = kM - 1; i >= 0; --i) {
        double sum = b[i];
        for (int j = i + 1; j < kM; ++j)
          sum -= a[i][j] * b[j];
        if (std::fabs(a[i][i]) < 1e-15)
          return false;
        b[i] = sum / a[i][i];
      }
      return true;
    }

    // invert_n: LU with partial pivoting, false if singular.
    bool invertN(const double (&a)[kN][kN], double (&result)[kN][kN]) {
      double lu[kN][kN];
      int perm[kN];
      for (int i = 0; i < kN; ++i) {
        perm[i] = i;
        for (int j = 0; j < kN; ++j)
          lu[i][j] = a[i][j];
      }

      for (int k = 0; k < kN; ++k) {
        int max_row = k;
        double max_val = std::fabs(lu[k][k]);
        for (int i = k + 1; i < kN; ++i) {
          const double v = std::fabs(lu[i][k]);
          if (v > max_val) {
            max_val = v;
            max_row = i;
          }
        }
        if (max_val < 1e-30)
          return false;
        if (max_row != k) {
          for (int j = 0; j < kN; ++j) {
            const double swap = lu[k][j];
            lu[k][j] = lu[max_row][j];
            lu[max_row][j] = swap;
          }
          const int swap = perm[k];
          perm[k] = perm[max_row];
          perm[max_row] = swap;
        }
        const double pivot = lu[k][k];
        for (int i = k + 1; i < kN; ++i) {
          const double m = lu[i][k] / pivot;
          lu[i][k] = m;
          for (int j = k + 1; j < kN; ++j)
            lu[i][j] -= m * lu[k][j];
        }
      }

      for (int col = 0; col < kN; ++col) {
        double b[kN] = {};
        int start = kN;
        for (int i = 0; i < kN; ++i) {
          if (perm[i] == col) {
            b[i] = 1.0;
            start = i;
            break;
          }
        }
        for (int i = start + 1; i < kN; ++i) {
          double sum = b[i];
          for (int j = start; j < i; ++j)
            sum -= lu[i][j] * b[j];
          b[i] = sum;
        }
        for (int i = kN - 1; i >= 0; --i) {
          double sum = b[i];
          for (int j = i + 1; j < kN; ++j)
            sum -= lu[i][j] * b[j];
          const double pivot = lu[i][i];
          if (std::fabs(pivot) < 1e-30)
            return false;
          b[i] = sum / pivot;
        }
        for (int i = 0; i < kN; ++i)
          result[i][col] = b[i];
      }
      return true;
    }

    // S N_i and K = N_v S N_i for a freshly inverted S.
    void computeSchur(const double (&s)[kN][kN], double (&s_ni)[kN][kM], double (&k)[kM][kM]) {
      for (int i = 0; i < kN; ++i) {
        for (int j = 0; j < kM; ++j) {
          double sum = 0.0;
          for (int a = 0; a < kN; ++a)
            sum += s[i][a] * kNI[a][j];
          s_ni[i][j] = sum;
        }
      }
      for (int i = 0; i < kM; ++i) {
        for (int j = 0; j < kM; ++j) {
          double sum = 0.0;
          for (int a = 0; a < kN; ++a)
            sum += kNV[i][a] * s_ni[a][j];
          k[i][j] = sum;
        }
      }
    }

    template <typename T, int kRows, int kColumns>
    void copyMatrix(const T (&source)[kRows][kColumns], T (&destination)[kRows][kColumns]) {
      for (int i = 0; i < kRows; ++i) {
        for (int j = 0; j < kColumns; ++j)
          destination[i][j] = source[i][j];
      }
    }

    void loadDefaultMatrices(CircuitState& state) {
      copyMatrix(kANegDefault, state.a_neg);
      copyMatrix(kANegBeDefault, state.a_neg_be);
      copyMatrix(kSDefault, state.s);
      copyMatrix(kKDefault, state.k);
      copyMatrix(kSNiDefault, state.s_ni);
      copyMatrix(kSBeDefault, state.s_be);
      copyMatrix(kKBeDefault, state.k_be);
      copyMatrix(kSNiBeDefault, state.s_ni_be);
    }

    // rebuild_matrices: A = G + C/T (backward Euler), its history C/T, and
    // their Schur products. The supply row has no history.
    void rebuildMatrices(CircuitState& state, double internal_rate) {
      const double alpha = internal_rate;
      const double alpha_be = internal_rate;

      double a[kN][kN];
      double a_be[kN][kN];
      for (int i = 0; i < kN; ++i) {
        for (int j = 0; j < kN; ++j) {
          a[i][j] = kG[i][j] + alpha * kC[i][j];
          state.a_neg[i][j] = alpha * kC[i][j];
          a_be[i][j] = kG[i][j] + alpha_be * kC[i][j];
          state.a_neg_be[i][j] = alpha_be * kC[i][j];
        }
      }
      for (int j = 0; j < kN; ++j) {
        state.a_neg[8][j] = 0.0;
        state.a_neg_be[8][j] = 0.0;
      }

      double inverse[kN][kN];
      if (invertN(a, inverse)) {
        copyMatrix(inverse, state.s);
        computeSchur(state.s, state.s_ni, state.k);
      }
      if (invertN(a_be, inverse)) {
        copyMatrix(inverse, state.s_be);
        computeSchur(state.s_be, state.s_ni_be, state.k_be);
      }
    }

    // The backward-Euler re-solve when the main Newton-Raphson does not
    // converge in kMaxIter iterations. Writes the node voltages to `v`.
    void solveBackwardEuler(double input, double input_conductance, CircuitState& state,
                            double (&i_nl)[kM], double (&v)[kN]) {
      double rhs_be[kN];
      for (int i = 0; i < kN; ++i) {
        double sum = kRhsConstBe[i];
        for (int j = 0; j < kN; ++j)
          sum += state.a_neg_be[i][j] * state.v_prev[j];
        rhs_be[i] = sum;
      }
      rhs_be[kInputNode] += input * input_conductance;

      double v_pred_be[kN];
      for (int i = 0; i < kN; ++i) {
        double sum = 0.0;
        for (int j = 0; j < kN; ++j)
          sum += state.s_be[i][j] * rhs_be[j];
        v_pred_be[i] = sum;
      }

      double p_be[kM];
      for (int i = 0; i < kM; ++i) {
        double sum = 0.0;
        for (int j = 0; j < kN; ++j)
          sum += kNV[i][j] * v_pred_be[j];
        p_be[i] = sum;
      }

      // Restart from the predictor.
      for (int i = 0; i < kM; ++i)
        i_nl[i] = 2.0 * state.i_nl_prev[i] - state.i_nl_prev_prev[i];

      const double (&k)[kM][kM] = state.k_be;
      for (int iteration = 0; iteration < kMaxIter; ++iteration) {
        double v_d[kM];
        for (int i = 0; i < kM; ++i) {
          v_d[i] = p_be[i] + k[i][0] * i_nl[0] + k[i][1] * i_nl[1] + k[i][2] * i_nl[2]
                   + k[i][3] * i_nl[3] + k[i][4] * i_nl[4];
        }

        double i_dev[kM];
        double jdev[kM][kM];
        evaluateDevices(v_d, i_dev, jdev);

        double f[kM];
        for (int i = 0; i < kM; ++i)
          f[i] = i_nl[i] - i_dev[i];

        double a[kM][kM];
        buildJacobian(jdev, k, a);
        double delta[kM] = { f[0], f[1], f[2], f[3], f[4] };
        if (solve5(a, delta)) {
          double dv[kM];
          for (int i = 0; i < kM; ++i) {
            dv[i] = -(k[i][0] * delta[0] + k[i][1] * delta[1] + k[i][2] * delta[2]
                      + k[i][3] * delta[3] + k[i][4] * delta[4]);
          }

          double alpha[kM] = { 1.0, 1.0, 1.0, 1.0, 1.0 };
          bool any_limited = false;
          for (int i = 0; i < kM; ++i) {
            if (std::fabs(dv[i]) > 1e-4) {
              const double v_lim = pnjlim(v_d[i] + dv[i], v_d[i], kLimitVt[i], kLimitVcrit[i]);
              const double ratio = std::fmax((v_lim - v_d[i]) / dv[i], 0.01);
              if (ratio < alpha[i]) {
                alpha[i] = ratio;
                if (ratio < 1.0)
                  any_limited = true;
              }
            }
          }
          double alpha_scalar = std::fmin(alpha[0], std::fmin(alpha[1], std::fmin(alpha[2],
                                          std::fmin(alpha[3], alpha[4]))));
          // Global backstop: no controlling voltage moves more than 3.5 V.
          double max_dv = std::fabs(dv[0] * alpha_scalar);
          for (int i = 1; i < kM; ++i)
            max_dv = std::fmax(max_dv, std::fabs(dv[i] * alpha_scalar));
          if (max_dv > 3.5)
            alpha_scalar *= std::fmax(3.5 / max_dv, 0.1);

          for (int i = 0; i < kM; ++i)
            i_nl[i] -= alpha_scalar * delta[i];

          // SPICE RELTOL = 0.001, VNTOL = 1e-6.
          if (!any_limited) {
            bool converged = true;
            for (int i = 0; i < kM; ++i) {
              const double step = dv[i] * alpha_scalar;
              const double v_new = v_d[i] + step;
              const double threshold = 1e-3 * std::fmax(std::fabs(v_d[i]), std::fabs(v_new)) + 1e-6;
              if (!(std::fabs(step) <= threshold))
                converged = false;
            }
            if (converged)
              break;
          }
        }
        else {
          for (int i = 0; i < kM; ++i)
            i_nl[i] -= math::clamp(f[i] * 0.5, -0.01, 0.01);
        }
      }

      for (int i = 0; i < kN; ++i) {
        v[i] = v_pred_be[i];
        for (int j = 0; j < kM; ++j)
          v[i] += state.s_ni_be[i][j] * i_nl[j];
      }
    }
  } // namespace

  CircuitState CircuitState::makeDefault() {
    CircuitState state;
    for (int i = 0; i < kN; ++i)
      state.v_prev[i] = kDcOp[i];
    for (int i = 0; i < kM; ++i) {
      state.i_nl_prev[i] = kDcNlI[i];
      state.i_nl_prev_prev[i] = kDcNlI[i];
    }
    loadDefaultMatrices(state);

    double taps[kNumTaps];
    for (int i = 0; i < 50; ++i)
      processSample(0.0, state, taps);
    return state;
  }

  void CircuitState::setSampleRate(double sample_rate) {
    if (!(sample_rate > 0.0 && std::isfinite(sample_rate)))
      return;

    // At the codegen rate: restore the baked matrices, keep the transient state.
    if (std::fabs(sample_rate - kSampleRate) < 0.5) {
      loadDefaultMatrices(*this);
      return;
    }

    const double internal_rate = sample_rate * 1.0;
    rebuildMatrices(*this, internal_rate);
  }

  void processSample(double input, CircuitState& state, double (&taps)[kNumTaps]) {
    input = std::isfinite(input) ? math::clamp(input, -100.0, 100.0) : 0.0;

    // Flush denormals from the history.
    for (double& v : state.v_prev)
      v = v + 1e-25 - 1e-25;
    for (double& i : state.i_nl_prev)
      i = i + 1e-25 - 1e-25;

    // Step 1: RHS = A_neg v_prev (its nonzero entries) + the sources.
    double rhs[kN];
    for (int i = 0; i < kN; ++i)
      rhs[i] = kRhsConst[i];
    rhs[0] += state.a_neg[0][0] * state.v_prev[0];
    rhs[0] += state.a_neg[0][1] * state.v_prev[1];
    rhs[1] += state.a_neg[1][0] * state.v_prev[0];
    rhs[1] += state.a_neg[1][1] * state.v_prev[1];
    rhs[1] += state.a_neg[1][2] * state.v_prev[2];
    rhs[2] += state.a_neg[2][1] * state.v_prev[1];
    rhs[2] += state.a_neg[2][2] * state.v_prev[2];
    rhs[3] += state.a_neg[3][3] * state.v_prev[3];

    const double input_conductance = 1.0 / kInputResistance;
    rhs[kInputNode] += input * input_conductance;

    // Step 2: linear prediction v_pred = S rhs.
    double v_pred[kN];
    for (int i = 0; i < kN; ++i) {
      double sum = 0.0;
      for (int j = 0; j < kN; ++j)
        sum += state.s[i][j] * rhs[j];
      v_pred[i] = sum;
    }

    // Step 3: controlling voltages p = N_v v_pred (nonzero entries).
    double p[kM];
    p[0] = kNV[0][2] * v_pred[2] + kNV[0][4] * v_pred[4];
    p[1] = kNV[1][0] * v_pred[0] + kNV[1][2] * v_pred[2];
    p[2] = kNV[2][4] * v_pred[4];
    p[3] = kNV[3][0] * v_pred[0] + kNV[3][4] * v_pred[4];
    p[4] = kNV[4][6] * v_pred[6] + kNV[4][7] * v_pred[7];

    // Step 4: Newton-Raphson on the device currents, from a first-order predictor.
    double i_nl[kM];
    for (int i = 0; i < kM; ++i)
      i_nl[i] = 2.0 * state.i_nl_prev[i] - state.i_nl_prev_prev[i];
    int last_nr_iterations = kMaxIter;

    const double (&k)[kM][kM] = state.k;
    for (int iteration = 0; iteration < kMaxIter; ++iteration) {
      // v_d = p + K i_nl, without the zero entries of K at codegen.
      double v_d[kM];
      v_d[0] = p[0] + k[0][0] * i_nl[0] + k[0][1] * i_nl[1] + k[0][2] * i_nl[2] + k[0][3] * i_nl[3]
               + k[0][4] * i_nl[4];
      v_d[1] = p[1] + k[1][0] * i_nl[0] + k[1][1] * i_nl[1] + k[1][2] * i_nl[2] + k[1][4] * i_nl[4];
      v_d[2] = p[2] + k[2][0] * i_nl[0] + k[2][1] * i_nl[1] + k[2][3] * i_nl[3];
      v_d[3] = p[3] + k[3][0] * i_nl[0] + k[3][1] * i_nl[1] + k[3][2] * i_nl[2] + k[3][3] * i_nl[3]
               + k[3][4] * i_nl[4];
      v_d[4] = p[4] + k[4][0] * i_nl[0] + k[4][1] * i_nl[1] + k[4][2] * i_nl[2] + k[4][4] * i_nl[4];

      double i_dev[kM];
      double jdev[kM][kM];
      evaluateDevices(v_d, i_dev, jdev);

      double f[kM];
      for (int i = 0; i < kM; ++i)
        f[i] = i_nl[i] - i_dev[i];

      double a[kM][kM];
      buildJacobian(jdev, k, a);
      double delta[kM] = { f[0], f[1], f[2], f[3], f[4] };
      if (solve5(a, delta)) {
        // Full NR step, then limit it on the controlling voltages.
        double i_trial[kM];
        for (int i = 0; i < kM; ++i)
          i_trial[i] = i_nl[i] - delta[i];
        double dv_trial[kM];
        double v_lim[kM];
        for (int i = 0; i < kM; ++i) {
          const double v_trial = p[i] + k[i][0] * i_trial[0] + k[i][1] * i_trial[1] + k[i][2] * i_trial[2]
                                 + k[i][3] * i_trial[3] + k[i][4] * i_trial[4];
          dv_trial[i] = v_trial - v_d[i];
          v_lim[i] = std::fabs(dv_trial[i]) > 1e-4 ? pnjlim(v_trial, v_d[i], kLimitVt[i], kLimitVcrit[i])
                                                   : v_trial;
        }

        bool any_limited = false;
        double global_alpha = 1.0;
        for (int i = 0; i < kM; ++i) {
          const double dv_lim = v_lim[i] - v_d[i];
          if (std::fabs(dv_trial[i]) > 1e-15) {
            const double r = dv_trial[i] * dv_lim < 0.0 ? 0.0 : math::clamp(dv_lim / dv_trial[i], 0.0, 1.0);
            if (r < global_alpha) {
              global_alpha = r;
              any_limited = true;
            }
          }
        }
        double max_dv = std::fabs(dv_trial[0] * global_alpha);
        for (int i = 1; i < kM; ++i)
          max_dv = std::fmax(max_dv, std::fabs(dv_trial[i] * global_alpha));
        if (max_dv > 3.5) {
          global_alpha *= 3.5 / max_dv;
          any_limited = true;
        }

        for (int i = 0; i < kM; ++i)
          i_nl[i] -= global_alpha * delta[i];

        // SPICE RELTOL = 0.001, VNTOL = 1e-6.
        if (!any_limited) {
          bool converged = true;
          for (int i = 0; i < kM; ++i) {
            const double dv = dv_trial[i] * global_alpha;
            const double threshold = 1e-3 * std::fmax(std::fabs(v_d[i]), std::fabs(v_d[i] + dv)) + 1e-6;
            if (!(std::fabs(dv) <= threshold))
              converged = false;
          }
          if (converged) {
            last_nr_iterations = iteration;
            break;
          }
        }
      }
      else {
        // Singular Jacobian: damped step on the residual, adaptive clamp.
        for (int i = 0; i < kM; ++i) {
          const double limit = std::fmax(std::fabs(i_nl[i]) * 0.1, 0.01);
          i_nl[i] -= math::clamp(f[i] * 0.5, -limit, limit);
        }
      }
    }

    // Step 5: node voltages v = v_pred + S_NI i_nl.
    double v[kN];
    for (int i = 0; i < kN; ++i) {
      v[i] = v_pred[i];
      for (int j = 0; j < kM; ++j)
        v[i] += state.s_ni[i][j] * i_nl[j];
    }

    if (last_nr_iterations >= kMaxIter)
      solveBackwardEuler(input, input_conductance, state, i_nl, v);

    // A NaN/Inf or implausibly large state resets to the DC point before it
    // can reach the history.
    bool v_is_finite = true;
    bool implausible = false;
    for (double value : v) {
      if (!std::isfinite(value))
        v_is_finite = false;
      if (std::fabs(value) > kStateMaxPlausibleMagnitude)
        implausible = true;
    }
    if (!v_is_finite || implausible) {
      for (int i = 0; i < kN; ++i)
        state.v_prev[i] = kDcOp[i];
      for (int i = 0; i < kM; ++i) {
        state.i_nl_prev[i] = kDcNlI[i];
        state.i_nl_prev_prev[i] = kDcNlI[i];
      }
      taps[0] = 1.34136800666353189e1;
      taps[1] = 1.18746439971943616e1;
      return;
    }

    for (int i = 0; i < kN; ++i)
      state.v_prev[i] = v[i];
    for (int i = 0; i < kM; ++i) {
      state.i_nl_prev_prev[i] = state.i_nl_prev[i];
      state.i_nl_prev[i] = i_nl[i];
    }

    for (int t = 0; t < kNumTaps; ++t) {
      const double raw = v[kTapNodes[t]];
      taps[t] = std::isfinite(raw) ? raw : 0.0;
    }
  }
} // namespace tremolo_circuit
} // namespace openwurli
