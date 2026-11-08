// SPDX-License-Identifier: GPL-3.0-or-later
#include "destroy.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <math.h>

namespace atlas::destroy {
  namespace {
    // The C library's functions, called through pointers as EEL2 calls them,
    // so the compiler never inlines, folds or fuses them.
    double (*volatile libm_sin)(double) = ::sin;
    double (*volatile libm_cos)(double) = ::cos;
    double (*volatile libm_exp)(double) = ::exp;
    double (*volatile libm_log)(double) = ::log;
    double (*volatile libm_pow)(double, double) = ::pow;
    double (*volatile libm_sqrt)(double) = ::sqrt;

    // $pi
    constexpr double kPi = 3.141592653589793;

    // @init's measured constants.
    constexpr double HP1_FC = 30.0;
    constexpr double HP2_FC = 20.0;
    constexpr double GAIN_DB0 = 0.9;
    constexpr double GAIN_DBPCT = 0.39;
    constexpr double RAIL = 0.98837;
    constexpr double KNEE_FRAC = 0.9;
    constexpr double AGC_ATT = 0.01;
    constexpr double AGC_REL = 0.3;
    constexpr double AGC_P = 0.9;
    constexpr double T_DBPCT = 0.05;
    constexpr double SVF_F0 = 8.175800000000001;
    constexpr double SVF_RATIO = 2697.0;
    constexpr double SUSPEND_SAMPLES = 8192.0;
    constexpr double OUT_CLIP = 4.0;
    constexpr double SILENCE_EPS = 0.0000000001;

    // The Q table: Serum's Q knob values and the measured 1 / damping.
    constexpr int QTAB_N = 18;
    constexpr double QTAB_X[QTAB_N] = {
      0.05, 0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 1.0, 1.5, 2.0, 2.5, 3.0, 4.0, 5.0, 6.0, 7.0, 7.6 };
    constexpr double QTAB_Y[QTAB_N] = {
      0.731245384013513, 0.757357730350354, 0.839052877112316, 0.90447807092917, 0.962528755546572,
      1.016477093688694, 1.067919692439129, 1.117793029442668, 1.263375993329375, 1.50918338087261,
      1.776546039190606, 2.081338717062815, 2.441287046530931, 3.435009618026931, 5.171165580721895,
      9.070294784580499, 23.446658851113718, 73.313782991202345 };

    // EEL2's min, max and fuzzy ==.
    double eelMin(double a, double b) { return a > b ? b : a; }
    double eelMax(double a, double b) { return a < b ? b : a; }
    bool eelEqual(double a, double b) { return std::fabs(a - b) < 0.00001; }

    // tanh_(x)
    double tanh_(double x) { return x > 20 ? 1 : x < -20 ? -1 : 1 - 2 / (libm_exp(2 * x) + 1); }
  }

  // An assignment of a value EEL2 judges could be denormal: it stores zero
  // instead of a value below the flush exponent, inf or NaN.
  double Destroy::flush(double value) const {
    uint64_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    const int exponent = static_cast<int>((bits >> 52) & 0x7ff);
    return exponent > build_.flush_exponent && exponent != 0x7ff ? value : 0.0;
  }

  // EEL2 does not group as C does: / binds tighter than *, and - tighter than
  // +, so `a * b / c` is a * (b / c) and `a + b - c` is a + (b - c). The
  // expressions here are bracketed as EEL2 reads the script's.
  //
  // Each `flush` is an assignment EEL2 compiles with its denormal check: one
  // whose value is computed by + or - without a constant, by * without a
  // constant of at least 1, by ^, or by a compound operator. Copies, memory
  // reads, abs, min and max of those, comparisons and a user function whose
  // last expression is one of them (knee and svf) are stored as they are, and
  // so are the arguments a user function receives.

  // Butterworth high-pass biquad coefficients (RBJ, Q = 1/sqrt 2)
  void Destroy::highPass(HighPass& filter, double fc) const {
    const double w = (2 * kPi) * (fc / srate_), cs = libm_cos(w);
    const double al = libm_sin(w) * 0.5 * libm_sqrt(2), a0 = 1 + al;
    filter.b0 = (1 + cs) * (0.5 / a0); filter.b1 = -(1 + cs) / a0; filter.b2 = filter.b0;
    filter.a1 = -2 * (cs / a0); filter.a2 = (1 - al) / a0;
  }

  double Destroy::knee(double w) const {
    double a = std::fabs(w);
    if (a > knee_) a = flush(knee_ + knee_w_ * tanh_((a - knee_) / knee_w_));
    return w < 0 ? -a : a;
  }

  void Destroy::prepare(double sample_rate) {
    srate_ = sample_rate;
    // REAPER starts a script with every variable and memory slot at zero.
    dry_l_.fill(0); dry_r_.fill(0);
    stage_l_.fill({}); stage_r_.fill({});
    svf_l_ = {}; svf_r_ = {};
    dc_l_ = {}; dc_r_ = {};
    stg_l_ = 0; stg_r_ = 0;
    dwp_ = 0;
    highPass(hp_a_, HP1_FC); highPass(hp_b_, HP2_FC);
    // gain control coefficients (one-pole on |v|)
    agc_att_ = 1 - libm_exp(-1 / (AGC_ATT * srate_));
    agc_rel_ = 1 - libm_exp(-1 / (AGC_REL * srate_));
    knee_ = KNEE_FRAC * RAIL; knee_w_ = RAIL - knee_;
    // Atlas's DC blocker, unity gain at Nyquist.
    dc_coefficient_ = std::exp(-2 * kPi * kDcBlockerHz / srate_);
    dc_gain_ = 0.5 * (1 + dc_coefficient_);
    setParams();
    silent_run_ = 0;
  }

  void Destroy::setParameters(const Parameters& parameters) {
    parameters_ = parameters;
    setParams();
  }

  // 2x oversampled Chamberlin state-variable filter (input held for both
  // sub-steps). The three outputs are read at different points of the double
  // step: LP = the low-pass state one sample late, BP = the band-pass after the
  // first half step, HP = the high-pass average of both half steps smoothed by [0.75, 0.25].
  double Destroy::qDamping(double q) const {
    q = eelMin(eelMax(q, QTAB_X[0]), QTAB_X[QTAB_N - 1]);
    int i = 0;
    for (int n = 0; n < QTAB_N - 2; ++n) {
      if (q > QTAB_X[i + 1]) i += 1;
    }
    const double t = (q - QTAB_X[i]) / (QTAB_X[i + 1] - QTAB_X[i]);
    // interpolate 1/k linearly in log
    const double a = libm_log(QTAB_Y[i]), b = libm_log(QTAB_Y[i + 1]);
    return 1 / libm_exp(a + t * (b - a));
  }

  void Destroy::svfCoefficients(double fnorm, double q, double morph) {
    const double fc = SVF_F0 * libm_pow(SVF_RATIO, fnorm);
    svf_f_ = 2 * libm_sin(kPi * (eelMin(fc, (0.49 * 2) * srate_) / (2 * srate_)));
    svf_k_ = qDamping(q);
    const double mm = morph / 50;
    svf_kl_ = eelMax(0, 1 - mm); svf_kb_ = mm <= 1 ? mm : 2 - mm; svf_kh_ = eelMax(0, mm - 1);
  }

  double Destroy::svf(double x, SvfState& state) const {
    double* s = state.s;
    const double hp1 = flush(x - s[0] - svf_k_ * s[1]);
    s[1] = flush(s[1] + svf_f_ * hp1); s[0] = flush(s[0] + svf_f_ * s[1]);
    const double bp1 = s[1];
    const double hp2 = flush(x - s[0] - svf_k_ * s[1]);
    s[1] = flush(s[1] + svf_f_ * hp2); s[0] = flush(s[0] + svf_f_ * s[1]);
    const double hpa = flush(0.5 * (hp1 + hp2));
    const double o = flush(svf_kl_ * s[2] + svf_kb_ * bp1 + svf_kh_ * (0.75 * hpa + 0.25 * s[3]));
    s[2] = s[0]; s[3] = hpa;
    return o;
  }

  void Destroy::setParams() {
    const double d = parameters_.drive;
    g_stage_ = libm_pow(10, (GAIN_DB0 + GAIN_DBPCT * d) / 20);
    agc_t_ = RAIL * libm_pow(10, d * (T_DBPCT / 20));
    nstages_ = static_cast<int>(eelMax(1, eelMin(16, std::floor(parameters_.stack + 0.5))));
    svfCoefficients(parameters_.frequency, parameters_.q, parameters_.morph);
    mix_s_ = libm_pow(libm_sin(kPi * (parameters_.mix / 200)), 2);
    out_g_ = libm_pow(2 * parameters_.level, 2) / 3;
    dry_g_ = 1 - mix_s_ * mix_s_;
    fmode_ = std::floor(parameters_.filter + 0.5);
  }

  // one Overdrive stage for both channels. The gain control is shared: its
  // detector follows the louder channel (max of |L|, |R|).
  void Destroy::stage(StageState& left, StageState& right) {
    double* sl = left.s;
    double* sr = right.s;
    double u = flush(hp_a_.b0 * stg_l_ + sl[0]);
    sl[0] = flush(hp_a_.b1 * stg_l_ - hp_a_.a1 * u + sl[1]); sl[1] = flush(hp_a_.b2 * stg_l_ - hp_a_.a2 * u);
    double vl = flush(hp_b_.b0 * u + sl[2]);
    sl[2] = flush(hp_b_.b1 * u - hp_b_.a1 * vl + sl[3]); sl[3] = flush(hp_b_.b2 * u - hp_b_.a2 * vl);
    u = flush(hp_a_.b0 * stg_r_ + sr[0]);
    sr[0] = flush(hp_a_.b1 * stg_r_ - hp_a_.a1 * u + sr[1]); sr[1] = flush(hp_a_.b2 * stg_r_ - hp_a_.a2 * u);
    double vr = flush(hp_b_.b0 * u + sr[2]);
    sr[2] = flush(hp_b_.b1 * u - hp_b_.a1 * vr + sr[3]); sr[3] = flush(hp_b_.b2 * u - hp_b_.a2 * vr);
    vl = flush(vl * g_stage_); vr = flush(vr * g_stage_);
    const double a = eelMax(std::fabs(vl), std::fabs(vr));
    double env = sl[4];
    env = flush(env + (a > env ? agc_att_ : agc_rel_) * (a - env)); sl[4] = env;
    const double G = env <= agc_t_ ? 1 : flush(libm_pow(agc_t_ / env, AGC_P));
    stg_l_ = knee(G * vl); stg_r_ = knee(G * vr);
  }

  // the wet path: optional pre filter, the stage stack, optional post filter (stg_l / stg_r in and out)
  void Destroy::wet() {
    if (eelEqual(fmode_, 1)) { stg_l_ = svf(stg_l_, svf_l_); stg_r_ = svf(stg_r_, svf_r_); }
    for (int k = 0; k < nstages_; ++k)
      stage(stage_l_[static_cast<size_t>(k)], stage_r_[static_cast<size_t>(k)]);
    if (eelEqual(fmode_, 2)) { stg_l_ = svf(stg_l_, svf_l_); stg_r_ = svf(stg_r_, svf_r_); }
  }

  double Destroy::blockDc(double x, DcBlocker& state) const {
    const double y = dc_gain_ * (x - state.x1) + dc_coefficient_ * state.y1;
    state.x1 = x;
    // Kept out of the denormal range so a decaying tail never slows the CPU.
    state.y1 = std::fabs(y) < 1.0e-30 ? 0.0 : y;
    return y;
  }

  void Destroy::process(double& left, double& right) {
    const double spl0 = left, spl1 = right;
    // dry path: delayed by the reported latency (Serum does the same)
    dry_l_[static_cast<size_t>(dwp_)] = spl0; dry_r_[static_cast<size_t>(dwp_)] = spl1;
    const double dryl = dry_l_[static_cast<size_t>((dwp_ + 1) & 7)], dryr = dry_r_[static_cast<size_t>((dwp_ + 1) & 7)];
    dwp_ = (dwp_ + 1) & 7;
    // Serum suspends the effect after 8192 samples of digital silence and freezes its state until signal returns.
    const bool nz = std::fabs(spl0) > SILENCE_EPS || std::fabs(spl1) > SILENCE_EPS;
    if (nz) silent_run_ = 0;
    else silent_run_ += 1;
    double wl, wr;
    if (silent_run_ > SUSPEND_SAMPLES) {
      wl = 0; wr = 0;
    }
    else {
      stg_l_ = spl0; stg_r_ = spl1; wet();
      wl = flush(stg_l_ * out_g_); wr = flush(stg_r_ * out_g_);
      if (remove_dc_) {
        wl = blockDc(wl, dc_l_); wr = blockDc(wr, dc_r_);
      }
    }
    // Serum clips its output hard at +/- 4
    left = flush(eelMin(eelMax(dry_g_ * dryl + mix_s_ * wl, -OUT_CLIP), OUT_CLIP));
    right = flush(eelMin(eelMax(dry_g_ * dryr + mix_s_ * wr, -OUT_CLIP), OUT_CLIP));
  }
}
