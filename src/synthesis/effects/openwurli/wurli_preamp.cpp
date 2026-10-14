// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from OpenWurli 0.9.0, crates/openwurli-dsp/src/dk_preamp_legacy.rs
// and preamp.rs (GPL-3.0-or-later). Floating-point expressions keep the Rust's
// operand order; build without fast-math and with -ffp-contract=off.
#include "wurli_preamp.h"

#include "wurli_math.h"

#include <cmath>

namespace openwurli {
  namespace {
    constexpr int kN = DkPreamp::kNumNodes;

    // Supply: the service manual's "+14.5 volts regulated" (the drawing marks
    // +15 V; 14.5 V closes the drawing's own TR-2 collector KCL).
    constexpr double kVcc = 14.5;

    // Resistors (ohms).
    constexpr double kR1 = 22'000.0;     // source to the R-1/C-1 junction
    constexpr double kR2 = 1'000'000.0;  // polarizing feed, junction to AC ground
    constexpr double kR3 = 470'000.0;    // base1 to emit2b: TR-1's only bias path
    constexpr double kRe1 = 33'000.0;    // emit1 to ground
    constexpr double kRc1 = 150'000.0;   // coll1 to Vcc
    constexpr double kRe2a = 270.0;      // emit2 to emit2b
    constexpr double kRe2b = 820.0;      // emit2b to ground
    constexpr double kRc2 = 1'800.0;     // coll2 to Vcc
    constexpr double kR9 = 6'800.0;      // node_c6 to out, outside the feedback loop
    constexpr double kR10 = 56'000.0;    // node_c6 to fb
    // Bench load at OUT, so gains compare with OpenWurli's SPICE testbench.
    // The real load (R-11 into the volume pot) is applied downstream through
    // openCircuitOutputFactor().
    constexpr double kRLoad = 100'000.0;

    // The R1/R2/Cin input two-port seen from base1 is exactly a Thevenin
    // source input * R2 / (R1 + R2) behind Cin + (R1 || R2).
    constexpr double kRInEff = kR1 * kR2 / (kR1 + kR2);
    constexpr double kKInDiv = kR2 / (kR1 + kR2);

    // Capacitors (farads).
    constexpr double kCin = 0.022e-6;  // input coupling, in series with kRInEff
    constexpr double kC2 = 220.0e-12;  // base1 to ground
    constexpr double kC3 = 100.0e-12;  // Miller, stage 1 (coll1 to base1)
    constexpr double kC4 = 100.0e-12;  // Miller, stage 2 (coll2 to coll1)
    constexpr double kCe1 = 4.7e-6;    // feedback coupling (emit1 to fb)
    constexpr double kCe2 = 22.0e-6;   // stage 2 emitter bypass (emit2 to ground)
    constexpr double kC6 = 4.7e-6;     // output coupling (coll2 to node_c6)

    // 2N5089, forward active: Gummel-Poon base charge for high injection
    // (IKF) and the ISE/NE recombination base current. No Early effect.
    // IS, BF and NF were fitted with VT = 0.026: change them together.
    constexpr double kIs = 3.03e-14;
    constexpr double kVt = 0.026;
    constexpr double kNf = 1.005;
    constexpr double kBf = 1434.0;
    constexpr double kIse = 2.88e-15;
    constexpr double kNe = 1.262;
    constexpr double kIkf = 0.01358;
    constexpr double kVtf = kNf * kVt;
    constexpr double kNeVt = kNe * kVt;
    constexpr double kIsOverIkf = kIs / kIkf;
    // Vbe clamp against exp overflow; a real 2N5089 stays under ~0.8 V.
    constexpr double kVbeMax = 0.85;

    enum Node { kBase1, kEmit1, kColl1, kEmit2, kEmit2b, kColl2, kNodeC6, kOut, kFb };

    struct Incidence {
      int node;
      double coefficient;
    };

    // Controlling voltages: vbe1 = base1 - emit1, vbe2 = coll1 - emit2
    // (TR-2's base is TR-1's collector).
    constexpr Incidence kNv[2][2] = {
      { { kBase1, 1.0 }, { kEmit1, -1.0 } }, { { kColl1, 1.0 }, { kEmit2, -1.0 } }
    };
    // Collector currents flow from the collector into the device and out of
    // the emitter; base currents likewise from the base.
    constexpr Incidence kNic[2][2] = {
      { { kEmit1, 1.0 }, { kColl1, -1.0 } }, { { kEmit2, 1.0 }, { kColl2, -1.0 } }
    };
    constexpr Incidence kNib[2][2] = {
      { { kEmit1, 1.0 }, { kBase1, -1.0 } }, { { kEmit2, 1.0 }, { kColl1, -1.0 } }
    };

    using Matrix = double[kN][kN];
    using Vector = double[kN];

    void matVecMul(const Matrix& a, const Vector& x, Vector& y) {
      for (int i = 0; i < kN; ++i) {
        double sum = 0.0;
        for (int j = 0; j < kN; ++j)
          sum += a[i][j] * x[j];
        y[i] = sum;
      }
    }

    // Gauss-Jordan inverse. The Rust panics on a singular matrix; that only
    // happens for a degenerate sample rate, and here gives non-finite values.
    void matInverse(const Matrix& m, Matrix& inverse) {
      double aug[kN][kN * 2];
      for (int i = 0; i < kN; ++i) {
        for (int j = 0; j < kN; ++j) {
          aug[i][j] = m[i][j];
          aug[i][kN + j] = i == j ? 1.0 : 0.0;
        }
      }

      for (int col = 0; col < kN; ++col) {
        double max_val = std::fabs(aug[col][col]);
        int max_row = col;
        for (int row = col + 1; row < kN; ++row) {
          if (std::fabs(aug[row][col]) > max_val) {
            max_val = std::fabs(aug[row][col]);
            max_row = row;
          }
        }

        if (max_row != col) {
          for (int j = 0; j < kN * 2; ++j) {
            const double swap = aug[col][j];
            aug[col][j] = aug[max_row][j];
            aug[max_row][j] = swap;
          }
        }

        const double pivot = aug[col][col];
        for (int j = 0; j < kN * 2; ++j)
          aug[col][j] /= pivot;

        for (int row = 0; row < kN; ++row) {
          if (row != col) {
            const double factor = aug[row][col];
            for (int j = 0; j < kN * 2; ++j)
              aug[row][j] -= factor * aug[col][j];
          }
        }
      }

      for (int i = 0; i < kN; ++i) {
        for (int j = 0; j < kN; ++j)
          inverse[i][j] = aug[i][kN + j];
      }
    }

    // Rust's f64 Sum folds from -0.0.
    double sumIncidence(const Incidence (&incidence)[2], const double* values) {
      double sum = -0.0;
      for (const Incidence& entry : incidence)
        sum += entry.coefficient * values[entry.node];
      return sum;
    }

    // K = N_v S N_i for one incidence map.
    void computeK(const Matrix& s, const Incidence (&ni)[2][2], double (&k)[2][2]) {
      for (int i = 0; i < 2; ++i) {
        for (int j = 0; j < 2; ++j) {
          double acc = 0.0;
          for (const Incidence& row : kNv[i]) {
            for (const Incidence& col : ni[j])
              acc += row.coefficient * col.coefficient * s[row.node][col.node];
          }
          k[i][j] = acc;
        }
      }
    }

    // Column j of S N_i.
    void sTimesNi(const Matrix& s, const Incidence (&ni)[2], double (&result)[kN]) {
      for (int i = 0; i < kN; ++i) {
        double sum = -0.0;
        for (const Incidence& entry : ni)
          sum += entry.coefficient * s[i][entry.node];
        result[i] = sum;
      }
    }

    void stampResistor(Matrix& g, int i, int j, double r) {
      const double conductance = 1.0 / r;
      g[i][i] += conductance;
      g[j][j] += conductance;
      g[i][j] -= conductance;
      g[j][i] -= conductance;
    }

    void stampCapacitor(Matrix& c, int i, int j, double capacitance) {
      c[i][i] += capacitance;
      c[j][j] += capacitance;
      c[i][j] -= capacitance;
      c[j][i] -= capacitance;
    }

    void stampCapacitorToGround(Matrix& c, int i, double capacitance) {
      c[i][i] += capacitance;
    }

    struct BjtResult {
      double ic;
      double ib;
      double gic;  // dIc/dVbe
      double gib;  // dIb/dVbe
    };

    // Ic = Is (exp(Vbe / (NF Vt)) - 1) / qb, qb = (1 + sqrt(1 + 4 q2)) / 2,
    // q2 = (Is / IKF) exp(Vbe / (NF Vt)); Ib = Icc / BF + Ise (exp(Vbe / (NE Vt)) - 1).
    inline BjtResult bjt(double vbe) {
      const double v = math::clamp(vbe, -1.0, kVbeMax);
      const double ef = std::exp(v / kVtf);
      const double icc = kIs * (ef - 1.0);

      const double q2 = kIsOverIkf * ef;
      const double root = std::sqrt(1.0 + 4.0 * q2);
      const double qb = 0.5 * (1.0 + root);
      const double ic = icc / qb;

      const double ee = std::exp(v / kNeVt);
      const double ib = icc / kBf + kIse * (ee - 1.0);

      const double dicc = kIs * ef / kVtf;
      const double dq2 = q2 / kVtf;
      const double dqb = dq2 / root;
      const double gic = (dicc * qb - icc * dqb) / (qb * qb);
      const double gib = dicc / kBf + kIse * ee / kNeVt;

      return { ic, ib, gic, gib };
    }
  } // namespace

  DkPreamp::DkPreamp(double sample_rate) {
    const double t = 1.0 / sample_rate;
    const double two_over_t = 2.0 / t;

    // Input companion: Thevenin of the R1/R2/Cin two-port seen from base1.
    const double alpha_cin = 2.0 * kRInEff * kCin * sample_rate;
    g_cin_ = (2.0 * kCin * sample_rate) / (1.0 + alpha_cin);
    c_cin_ = (1.0 - alpha_cin) / (1.0 + alpha_cin);
    gc_1pc_ = g_cin_ * (1.0 + c_cin_);

    // G without the LDR. base1 has no resistive path to a supply: R-3 to
    // emit2b is TR-1's only DC bias path, as drawn.
    Matrix g_base = {};
    Vector w = {};
    stampResistor(g_base, kBase1, kEmit2b, kR3);
    g_base[kEmit1][kEmit1] += 1.0 / kRe1;
    g_base[kColl1][kColl1] += 1.0 / kRc1;
    w[kColl1] += kVcc / kRc1;
    stampResistor(g_base, kEmit2, kEmit2b, kRe2a);
    g_base[kEmit2b][kEmit2b] += 1.0 / kRe2b;
    g_base[kColl2][kColl2] += 1.0 / kRc2;
    w[kColl2] += kVcc / kRc2;
    stampResistor(g_base, kNodeC6, kOut, kR9);
    stampResistor(g_base, kNodeC6, kFb, kR10);
    g_base[kOut][kOut] += 1.0 / kRLoad;

    // The DC solves use G without the input companion.
    for (int i = 0; i < kN; ++i) {
      for (int j = 0; j < kN; ++j)
        g_dc_base_[i][j] = g_base[i][j];
    }

    g_base[kBase1][kBase1] += g_cin_;

    Matrix c = {};
    stampCapacitor(c, kColl1, kBase1, kC3);
    stampCapacitor(c, kColl2, kColl1, kC4);
    stampCapacitor(c, kEmit1, kFb, kCe1);
    stampCapacitor(c, kColl2, kNodeC6, kC6);
    stampCapacitorToGround(c, kEmit2, kCe2);
    stampCapacitorToGround(c, kBase1, kC2);

    for (int i = 0; i < kN; ++i)
      two_w_[i] = 2.0 * w[i];

    // The fixed transient matrices (no LDR).
    Matrix a_base;
    for (int i = 0; i < kN; ++i) {
      for (int j = 0; j < kN; ++j) {
        const double two_c_over_t = two_over_t * c[i][j];
        a_base[i][j] = two_c_over_t + g_base[i][j];
        a_neg_base_[i][j] = two_c_over_t - g_base[i][j];
      }
    }
    matInverse(a_base, s_base_);
    computeK(s_base_, kNic, k_c_);
    computeK(s_base_, kNib, k_b_);

    // Sherman-Morrison projections onto the FB node.
    Vector s_fb_row;
    for (int i = 0; i < kN; ++i) {
      s_fb_col_[i] = s_base_[i][kFb];
      s_fb_row[i] = s_base_[kFb][i];
    }
    s_fb_fb_ = s_base_[kFb][kFb];

    for (int i = 0; i < 2; ++i)
      nv_sfb_[i] = sumIncidence(kNv[i], s_fb_col_);
    for (int j = 0; j < 2; ++j)
      sfb_nic_[j] = sumIncidence(kNic[j], s_fb_row);
    for (int j = 0; j < 2; ++j)
      sfb_nib_[j] = sumIncidence(kNib[j], s_fb_row);

    for (int j = 0; j < 2; ++j)
      sTimesNi(s_base_, kNic[j], s_nic_[j]);
    for (int j = 0; j < 2; ++j)
      sTimesNi(s_base_, kNib[j], s_nib_[j]);

    // Start at the DC operating point with the LDR dark.
    constexpr double kRLdrInit = 1'000'000.0;
    double v_nl_dc[2];
    Vector v_dc;
    fullDcSolve(g_dc_base_, w, kRLdrInit, v_nl_dc, v_dc);
    resetStateAtDc(v_nl_dc, v_dc);

    r_ldr_ = kRLdrInit;
    g_ldr_ = 1.0 / kRLdrInit;
    g_ldr_prev_ = 1.0 / kRLdrInit;
  }

  void DkPreamp::fullDcSolve(const Matrix& g_dc_base, const Vector& w, double r_ldr,
                             double (&v_nl)[2], Vector& v_dc) {
    Matrix g_full;
    for (int i = 0; i < kN; ++i) {
      for (int j = 0; j < kN; ++j)
        g_full[i][j] = g_dc_base[i][j];
    }
    g_full[kFb][kFb] += 1.0 / r_ldr;

    Matrix s_dc;
    matInverse(g_full, s_dc);
    double k_c_dc[2][2];
    double k_b_dc[2][2];
    computeK(s_dc, kNic, k_c_dc);
    computeK(s_dc, kNib, k_b_dc);
    Vector sv;
    matVecMul(s_dc, w, sv);
    const double p_dc[2] = { sv[kBase1] - sv[kEmit1], sv[kColl1] - sv[kEmit2] };

    v_nl[0] = 0.56;
    v_nl[1] = 0.66;
    for (int iteration = 0; iteration < 200; ++iteration) {
      const BjtResult d0 = bjt(v_nl[0]);
      const BjtResult d1 = bjt(v_nl[1]);
      const double f0 = v_nl[0] - p_dc[0] - k_c_dc[0][0] * d0.ic - k_c_dc[0][1] * d1.ic
                        - k_b_dc[0][0] * d0.ib - k_b_dc[0][1] * d1.ib;
      const double f1 = v_nl[1] - p_dc[1] - k_c_dc[1][0] * d0.ic - k_c_dc[1][1] * d1.ic
                        - k_b_dc[1][0] * d0.ib - k_b_dc[1][1] * d1.ib;
      if (std::fabs(f0) < 1e-13 && std::fabs(f1) < 1e-13)
        break;

      const double j00 = 1.0 - k_c_dc[0][0] * d0.gic - k_b_dc[0][0] * d0.gib;
      const double j01 = -k_c_dc[0][1] * d1.gic - k_b_dc[0][1] * d1.gib;
      const double j10 = -k_c_dc[1][0] * d0.gic - k_b_dc[1][0] * d0.gib;
      const double j11 = 1.0 - k_c_dc[1][1] * d1.gic - k_b_dc[1][1] * d1.gib;
      const double det = j00 * j11 - j01 * j10;
      const double inv_det = 1.0 / det;
      const double dv0 = inv_det * (j11 * f0 - j01 * f1);
      const double dv1 = inv_det * (j00 * f1 - j10 * f0);
      const double max_step = 2.0 * kVt;
      v_nl[0] -= math::clamp(dv0, -max_step, max_step);
      v_nl[1] -= math::clamp(dv1, -max_step, max_step);
    }

    const BjtResult d0 = bjt(v_nl[0]);
    const BjtResult d1 = bjt(v_nl[1]);
    const double ic[2] = { d0.ic, d1.ic };
    const double ib[2] = { d0.ib, d1.ib };
    Vector dc_rhs;
    for (int i = 0; i < kN; ++i)
      dc_rhs[i] = w[i];
    for (int j = 0; j < 2; ++j) {
      for (const Incidence& entry : kNic[j])
        dc_rhs[entry.node] += entry.coefficient * ic[j];
      for (const Incidence& entry : kNib[j])
        dc_rhs[entry.node] += entry.coefficient * ib[j];
    }
    matVecMul(s_dc, dc_rhs, v_dc);
  }

  void DkPreamp::resetStateAtDc(const double (&v_nl_dc)[2], const Vector& v_dc) {
    const BjtResult d0 = bjt(v_nl_dc[0]);
    const BjtResult d1 = bjt(v_nl_dc[1]);
    main_.j_cin = g_cin_ * v_dc[kBase1];
    main_.cin_rhs_prev = g_cin_ * v_dc[kBase1];
    for (int i = 0; i < kN; ++i)
      main_.v[i] = v_dc[i];
    main_.i_c[0] = d0.ic;
    main_.i_c[1] = d1.ic;
    main_.i_b[0] = d0.ib;
    main_.i_b[1] = d1.ib;
    main_.v_nl[0] = v_nl_dc[0];
    main_.v_nl[1] = v_nl_dc[1];
  }

  // dk_step: one trapezoidal step of the full system.
  double DkPreamp::step(double input) {
    State& state = main_;

    // History: A_neg_base v[n] plus the sources.
    Vector rhs;
    matVecMul(a_neg_base_, state.v, rhs);

    // The previous LDR current, explicit (trapezoidal backward term).
    rhs[kFb] -= g_ldr_prev_ * state.v[kFb];

    // Input companion, driven by the two-port's Thevenin source.
    const double vin_eff = input * kKInDiv;
    const double cin_rhs_now = g_cin_ * vin_eff + state.j_cin;
    rhs[kBase1] += cin_rhs_now + state.cin_rhs_prev;

    for (int j = 0; j < 2; ++j) {
      for (const Incidence& entry : kNic[j])
        rhs[entry.node] += entry.coefficient * state.i_c[j];
      for (const Incidence& entry : kNib[j])
        rhs[entry.node] += entry.coefficient * state.i_b[j];
    }

    for (int i = 0; i < kN; ++i)
      rhs[i] += two_w_[i];

    // Prediction without the LDR, then its Sherman-Morrison correction.
    Vector v_pred_base;
    matVecMul(s_base_, rhs, v_pred_base);
    const double sm_k = g_ldr_ / (1.0 + s_fb_fb_ * g_ldr_);
    const double sm_vpred = sm_k * v_pred_base[kFb];
    Vector v_pred;
    for (int i = 0; i < kN; ++i)
      v_pred[i] = v_pred_base[i] - sm_vpred * s_fb_col_[i];

    const double p[2] = { v_pred[kBase1] - v_pred[kEmit1], v_pred[kColl1] - v_pred[kEmit2] };

    // Newton-Raphson on the 2x2 kernel, corrected for the LDR.
    double kc[2][2];
    double kb[2][2];
    for (int i = 0; i < 2; ++i) {
      for (int j = 0; j < 2; ++j) {
        kc[i][j] = k_c_[i][j] - sm_k * nv_sfb_[i] * sfb_nic_[j];
        kb[i][j] = k_b_[i][j] - sm_k * nv_sfb_[i] * sfb_nib_[j];
      }
    }

    double v_nl[2] = { state.v_nl[0], state.v_nl[1] };
    for (int iteration = 0; iteration < 6; ++iteration) {
      const BjtResult d0 = bjt(v_nl[0]);
      const BjtResult d1 = bjt(v_nl[1]);

      const double f0 = v_nl[0] - p[0] - kc[0][0] * d0.ic - kc[0][1] * d1.ic
                        - kb[0][0] * d0.ib - kb[0][1] * d1.ib;
      const double f1 = v_nl[1] - p[1] - kc[1][0] * d0.ic - kc[1][1] * d1.ic
                        - kb[1][0] * d0.ib - kb[1][1] * d1.ib;
      if (std::fabs(f0) < 1e-9 && std::fabs(f1) < 1e-9)
        break;

      const double j00 = 1.0 - kc[0][0] * d0.gic - kb[0][0] * d0.gib;
      const double j01 = -kc[0][1] * d1.gic - kb[0][1] * d1.gib;
      const double j10 = -kc[1][0] * d0.gic - kb[1][0] * d0.gib;
      const double j11 = 1.0 - kc[1][1] * d1.gic - kb[1][1] * d1.gib;

      const double det = j00 * j11 - j01 * j10;
      if (std::fabs(det) < 1e-30)
        break;
      const double inv_det = 1.0 / det;

      v_nl[0] -= inv_det * (j11 * f0 - j01 * f1);
      v_nl[1] -= inv_det * (j00 * f1 - j10 * f0);
    }

    const BjtResult d0 = bjt(v_nl[0]);
    const BjtResult d1 = bjt(v_nl[1]);
    const double ic_new[2] = { d0.ic, d1.ic };
    const double ib_new[2] = { d0.ib, d1.ib };

    // Node update.
    const double sfb_dot = sfb_nic_[0] * ic_new[0] + sfb_nic_[1] * ic_new[1]
                           + sfb_nib_[0] * ib_new[0] + sfb_nib_[1] * ib_new[1];
    for (int i = 0; i < kN; ++i) {
      const double s_ni_i = ic_new[0] * s_nic_[0][i] + ic_new[1] * s_nic_[1][i]
                            + ib_new[0] * s_nib_[0][i] + ib_new[1] * s_nib_[1][i];
      state.v[i] = v_pred[i] + s_ni_i - sm_k * sfb_dot * s_fb_col_[i];
    }

    // Input companion update.
    state.cin_rhs_prev = cin_rhs_now;
    const double dv_cin = vin_eff - state.v[kBase1];
    state.j_cin = -gc_1pc_ * dv_cin - c_cin_ * state.j_cin;

    state.i_c[0] = ic_new[0];
    state.i_c[1] = ic_new[1];
    state.i_b[0] = ib_new[0];
    state.i_b[1] = ib_new[1];
    state.v_nl[0] = v_nl[0];
    state.v_nl[1] = v_nl[1];

    return state.v[kOut];
  }

  double DkPreamp::openCircuitOutputFactor() const {
    return (kR9 + kRLoad) / kRLoad;
  }

  double DkPreamp::processSample(double input) {
    const double result = step(input);
    // The step consumed g_ldr_prev.
    g_ldr_prev_ = g_ldr_;

    // Only a diverged solve gets here.
    if (!std::isfinite(result)) {
      reset();
      return 0.0;
    }
    return result;
  }

  void DkPreamp::setLdrResistance(double r_ldr_path) {
    const double new_r = std::fmax(r_ldr_path, 1000.0);
    if (std::fabs(new_r - r_ldr_) > 0.01) {
      r_ldr_ = new_r;
      g_ldr_ = 1.0 / new_r;
    }
  }

  void DkPreamp::reset() {
    Vector w;
    for (int i = 0; i < kN; ++i)
      w[i] = two_w_[i] * 0.5;
    double v_nl_dc[2];
    Vector v_dc;
    fullDcSolve(g_dc_base_, w, r_ldr_, v_nl_dc, v_dc);

    g_ldr_ = 1.0 / r_ldr_;
    g_ldr_prev_ = g_ldr_;
    resetStateAtDc(v_nl_dc, v_dc);
  }
} // namespace openwurli
