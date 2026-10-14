// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from OpenWurli 0.9.0, crates/openwurli-dsp/src/dk_preamp_legacy.rs
// (the DkPreamp that dk_preamp/mod.rs exports by default) and preamp.rs
// (GPL-3.0-or-later).
#pragma once

namespace openwurli {
  // The 200A's two-stage direct-coupled 2N5089 preamp (schematic #203720-S-3,
  // 2026-09 drawn-topology revision) as a 9-node MNA system: trapezoidal
  // companions for the capacitors, Newton-Raphson on the 2x2 Vbe kernel.
  // The tremolo's LDR shunt sits in the emitter feedback loop; it is kept out
  // of the system matrix and applied as a Sherman-Morrison correction, so a
  // moving LDR never disturbs the Ce1 companion's charge (a stamped LDR made
  // large DC transients). See OpenWurli's docs/research/dk-preamp-derivation.md.
  //
  // Run it at the oversampled rate. Input: pickup voltage. Output: the voltage
  // at OUT, loaded by RLOAD (100K).
  class DkPreamp {
    public:
      static constexpr int kNumNodes = 9;

      // Unusable until assigned from DkPreamp(sample_rate).
      DkPreamp() = default;
      // Builds the fixed matrices for `sample_rate` and starts at the DC
      // operating point with the LDR dark (1 MΩ).
      explicit DkPreamp(double sample_rate);

      // PreampModel::open_circuit_output_factor: turns the loaded OUT voltage
      // into the open-circuit voltage at the R-9 terminal, (R9 + RLOAD) / RLOAD,
      // which tables::volume_pot_gain then loads with R-9 as source resistance.
      double openCircuitOutputFactor() const;
      // One sample. A non-finite result resets to the DC point and returns 0.
      double processSample(double input);
      // The LDR path's resistance in ohms (from Tremolo::process), floored at 1K.
      void setLdrResistance(double r_ldr_path);
      // Back to the DC operating point at the current LDR resistance.
      void reset();

    private:
      using Matrix = double[kNumNodes][kNumNodes];
      using Vector = double[kNumNodes];

      // Per-sample solver state.
      struct State {
        double j_cin = 0.0;
        double cin_rhs_prev = 0.0;
        double v[kNumNodes] = {};  // node voltages
        double i_c[2] = {};        // collector currents
        double i_b[2] = {};        // base currents
        double v_nl[2] = {};       // Vbe of both transistors (NR warm start)
      };

      static void fullDcSolve(const Matrix& g_dc_base, const Vector& w, double r_ldr,
                              double (&v_nl)[2], Vector& v_dc);
      void resetStateAtDc(const double (&v_nl_dc)[2], const Vector& v_dc);
      double step(double input);

      // Fixed after construction: S_base = (2C/T + G_base)^-1 without the
      // LDR, the history matrix, the DK kernels and the Sherman-Morrison
      // projections onto the FB node.
      Matrix s_base_ = {};
      Matrix a_neg_base_ = {};
      double k_c_[2][2] = {};
      double k_b_[2][2] = {};
      Vector two_w_ = {};
      double s_nic_[2][kNumNodes] = {};
      double s_nib_[2][kNumNodes] = {};
      Vector s_fb_col_ = {};
      double s_fb_fb_ = 0.0;
      double nv_sfb_[2] = {};
      double sfb_nic_[2] = {};
      double sfb_nib_[2] = {};
      // G for the DC solves: no LDR, no input companion.
      Matrix g_dc_base_ = {};

      // Input companion: Cin in series with R1 || R2.
      double g_cin_ = 0.0;
      double c_cin_ = 0.0;
      double gc_1pc_ = 0.0;

      State main_;

      double r_ldr_ = 0.0;
      double g_ldr_ = 0.0;
      double g_ldr_prev_ = 0.0;
  };
} // namespace openwurli
