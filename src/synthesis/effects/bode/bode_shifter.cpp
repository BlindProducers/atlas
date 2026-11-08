// SPDX-License-Identifier: GPL-3.0-or-later
#include "bode_shifter.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <math.h>

namespace atlas::bode {
  namespace {
    // The C library's functions, called through pointers as EEL2 calls them,
    // so the compiler never inlines, folds or fuses them (sin and cos into sincos).
    double (*volatile libm_sin)(double) = ::sin;
    double (*volatile libm_cos)(double) = ::cos;
    double (*volatile libm_log)(double) = ::log;
    double (*volatile libm_pow)(double, double) = ::pow;
    double (*volatile libm_sqrt)(double) = ::sqrt;

    // $pi
    constexpr double kPi = 3.14159265358979323846;

    // @init's measured constants.
    constexpr double GAIN = 0.999846104893149;
    constexpr double PSI0 = 0.000065;
    constexpr double LOOP_LAT = 23.0;
    constexpr double LOOP_FRAC = 0.04;
    constexpr double BLUR_K = 0.18;
    constexpr double BLUR_TAPS = 24.0;
    constexpr int DQ = 3;

    constexpr double AI[22] = {
      -0.999810780072733, -0.999501760123473, -0.999039677349058, -0.998281950350413,
      -0.996997170046136, -0.994791806705936, -0.990995808884053, -0.984469635183461,
      -0.973276767754129, -0.954220578989486, -0.922080410743966, -0.868903689091314,
      -0.783291121857243, -0.651600637484587, -0.46080387024636, -0.204927716171037,
      0.04305833940261, 0.109309741348372, 0.165895777160516, 0.374071156153417,
      0.540689087435953, 0.733268797327206 };
    constexpr double AQ[22] = {
      -0.999987897994469, -0.999643868462584, -0.999232708434688, -0.998764301681091,
      -0.997704141768094, -0.996053005095589, -0.993142958071573, -0.988178902596468,
      -0.979609398053627, -0.965017652260712, -0.94017859826469, -0.898815168899885,
      -0.830847159766582, -0.724379148270128, -0.563531549794462, -0.34288003792547,
      -0.023954297700375, 0.179748017502924, 0.29958211283515, 0.380772867355423,
      0.733192292854594, 0.823221334958439 };
    constexpr int FREQ_N = 14;
    constexpr double FREQ_F[FREQ_N] = {
      1.0, 3.0, 10.0, 22.359999999999999, 33.0, 100.0, 300.0, 600.0, 1000.0, 1200.0,
      2400.0, 3000.0, 4800.0, 5000.0 };
    constexpr double FREQ_R[FREQ_N] = {
      0.999923361, 0.999926795, 0.9999930569, 1.000008325178891, 0.999957379181818,
      0.9997837558, 0.998893906333333, 0.996919170725, 0.993474465078, 0.9914851404275,
      0.976848625, 0.968212333333333, 0.939399291666667, 0.9362278 };

    // The sliders Atlas leaves at their defaults: Swap A/B, BPM sync,
    // Balance, Retrig, Level and Osc phase at start.
    constexpr double kSwap = 0, kBalance = 0, kLevel = 0.5, kOscPhase = 0;

    // The drawing code's slider table (PRM_MIN, PRM_MAX, PRM_DEF and PRM_KIND
    // at 30000, 30100, 30200 and 30300, for sliders 1 to 16). @init writes
    // it inside BLUR_R (18384 to 34767), so until @sample has written over
    // them, these values are what the right blur taps read there.
    constexpr int kPrmInBlurR = 30000 - 18384;
    constexpr double kPrm[16][4] = {
      { -100.0, 100.0, 0.0, 8 }, { 0.1, 5000.0, 22.359999999999999, 11 }, { -100.0, 100.0, 0.0, 8 },
      { 0.0, 100.0, 100.0, 7 }, { 0.0, 1.0, 0.0, 1 }, { 0.0, 1.0, 0.0, 1 }, { 0.0, 5.0, 0.0, 13 },
      { 0.0, 1.0, 0.0, 1 }, { 0.0, 23.0, 15.0, 1 }, { 0.0, 100.0, 0.0, 7 }, { -100.0, 100.0, 0.0, 8 },
      { 0.0, 100.0, 0.0, 7 }, { 0.0, 1.0, 0.0, 1 }, { 0.0, 100.0, 100.0, 7 }, { 0.0, 1.0, 0.5, 12 },
      { 0.0, 1.0, 0.0, 9 } };

    // EEL2's min and max.
    double eelMin(double a, double b) { return a > b ? b : a; }
    double eelMax(double a, double b) { return a < b ? b : a; }
    // tan_(x)
    double tan_(double x) { return libm_sin(x) / libm_cos(x); }
  }

  // An assignment of a computed value: EEL2 stores zero instead of a value
  // below the flush exponent, inf or NaN.
  double Shifter::flush(double value) const {
    uint64_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    const int exponent = static_cast<int>((bits >> 52) & 0x7ff);
    return exponent > build_.flush_exponent && exponent != 0x7ff ? value : 0.0;
  }

  void Shifter::prepare(double sample_rate) {
    srate_ = sample_rate;
    // FB_LEN = floor(5 * srate) + 128
    feedback_length_ = static_cast<int>(std::floor(5 * srate_) + 128);
    feedback_line_.reset(new double[static_cast<size_t>(feedback_length_)]);
    if (!blur_)
      blur_.reset(new double[2 * kBlurSize]);
    reset();
  }

  void Shifter::reset() {
    if (!prepared())
      return;
    // REAPER starts a script with every variable and memory slot at zero.
    std::fill(std::begin(si_l_), std::end(si_l_), 0.0); std::fill(std::begin(si_r_), std::end(si_r_), 0.0);
    std::fill(std::begin(sq_l_), std::end(sq_l_), 0.0); std::fill(std::begin(sq_r_), std::end(sq_r_), 0.0);
    std::fill(std::begin(inb_l_), std::end(inb_l_), 0.0); std::fill(std::begin(inb_r_), std::end(inb_r_), 0.0);
    std::fill(std::begin(dry_l_), std::end(dry_l_), 0.0); std::fill(std::begin(dry_r_), std::end(dry_r_), 0.0);
    std::fill(blur_.get(), blur_.get() + 2 * kBlurSize, 0.0);
    double* blur_r = blur_.get() + kBlurSize;
    for (int id = 1; id <= 16; ++id) {
      for (int column = 0; column < 4; ++column)
        blur_r[kPrmInBlurR + 100 * column + id] = kPrm[id - 1][column];
    }
    feedback_written_ = 0;

    // modulator state (both channels share the oscillator)
    mc_ = libm_cos(PSI0 + 2 * kPi * kOscPhase); ms_ = libm_sin(PSI0 + 2 * kPi * kOscPhase); quad_ = 0;
    dp_ = 0; dwp_ = 0; fwp_ = 0; bwp_ = 0;
    setParams();
  }

  void Shifter::setParameters(const Parameters& parameters) {
    parameters_ = parameters;
    if (prepared())
      setParams();
  }

  // measured ratio f_eff / f, interpolated linearly in log f between the table points
  double Shifter::freqRatio(double fa) const {
    fa = eelMin(eelMax(fa, FREQ_F[0]), FREQ_F[FREQ_N - 1]);
    int i = 0;
    for (int n = 0; n < FREQ_N - 2; ++n) {
      if (fa > FREQ_F[i + 1]) i += 1;
    }
    const double t = libm_log(fa / FREQ_F[i]) / libm_log(FREQ_F[i + 1] / FREQ_F[i]);
    return FREQ_R[i] + t * (FREQ_R[i + 1] - FREQ_R[i]);
  }

  void Shifter::setOscillator() {
    const double f = parameters_.shift / 100 * parameters_.range;
    const double feff = std::fabs(f) > 0.000000000001 ? f * freqRatio(std::fabs(f)) : 0;
    w96_ = flush(tan_(kPi * (feff / srate_)));
  }

  void Shifter::setDelay() {
    // BPM sync is off: the delay is Delay's seconds.
    const double dsec = parameters_.delay;
    double pos = dsec * srate_ - LOOP_LAT + LOOP_FRAC;
    if (pos < 3 + LOOP_FRAC) pos = 3 + LOOP_FRAC;
    if (pos > feedback_length_ - 4) pos = feedback_length_ - 4;
    const double floored = std::floor(pos);
    dl_ip_ = static_cast<int>(floored);
    dl_fr_ = pos - floored;
  }

  // blur: random multitap of unit energy, taps between 0.3 L and 1.3 L, L = 0.18 blur^2 samples at 48 kHz
  void Shifter::setBlur() {
    const double blur = parameters_.blur;
    blur_on_ = blur > 0.05;
    double L = BLUR_K * blur * blur * (srate_ / 48000);
    if (L > 16000) L = 16000;
    // EEL2's %: both operands as integers, the left one's magnitude.
    auto next = [this](double r) {
      return static_cast<double>(static_cast<int64_t>(std::fabs(r * 1103515245 + 12345)) % build_.mod_2_31);
    };
    double r = 12345;
    for (int k = 0; k < kBlurTaps; ++k) {
      r = next(r); blk_l_[k] = std::floor(0.3 * L + (r / 2147483648.0) * L + 0.5);
      r = next(r); bls_l_[k] = (r / 2147483648.0) < 0.5 ? -1 : 1;
      r = next(r); blk_r_[k] = std::floor(0.3 * L + (r / 2147483648.0) * L + 0.5);
      r = next(r); bls_r_[k] = (r / 2147483648.0) < 0.5 ? -1 : 1;
    }
    blur_g_ = 1 / libm_sqrt(BLUR_TAPS);
  }

  void Shifter::setParams() {
    setOscillator(); setDelay(); setBlur();
    g_fb_ = parameters_.feedback / 100;
    bal_a_ = (1 - kBalance / 100) * 0.5; bal_b_ = (1 + kBalance / 100) * 0.5;
    dirv_ = parameters_.direction / 100; wdt_ = parameters_.width / 100;
    const double m = parameters_.mix / 100;
    gd_ = eelMin(1, 2 * (1 - m)); gw_ = eelMin(1, 2 * m);
    glev_ = libm_pow(2 * kLevel, 2) * GAIN;
    mono_ = parameters_.mono_input > 0.5; swp_ = kSwap > 0.5;
  }

  // EEL2 does not group as C does: / binds tighter than *, and - tighter than
  // +, so `a * b / c` is a * (b / c) and `a + b - c` is a + (b - c). The
  // expressions here are bracketed as EEL2 reads the script's.
  //
  // Each `flush` is an assignment EEL2 compiles with its denormal check: one
  // whose value is computed by + or - (without a constant), *, / or a
  // compound operator. Copies, memory reads, floor and constants are not checked.
  void Shifter::process(double& left, double& right) {
    const double spl0 = left, spl1 = right;
    double xl = spl0, xr = spl1;
    if (mono_) {
      const double xm = flush(0.5 * (xl + xr));
      xl = xm; xr = xm;
    }
    // feedback read (linear interpolation)
    const double* fb = feedback_line_.get();
    const int i0 = (fwp_ - dl_ip_ + feedback_length_) % feedback_length_;
    const int i1 = (fwp_ - dl_ip_ - 1 + feedback_length_) % feedback_length_;
    const double r0 = i0 < feedback_written_ ? fb[i0] : 0.0;
    const double r1 = i1 < feedback_written_ ? fb[i1] : 0.0;
    const double fbv = flush(g_fb_ * ((1 - dl_fr_) * r0 + dl_fr_ * r1));
    const double uL = flush(xl + fbv), uR = flush(xr + fbv);

    // analytic signal: I chain (input delayed 2) and Q chain (input delayed DQ)
    inb_l_[dp_] = uL; inb_r_[dp_] = uR;
    double vIL = inb_l_[(dp_ - 2) & 7], vIR = inb_r_[(dp_ - 2) & 7];
    double vQL = inb_l_[(dp_ - DQ) & 7], vQR = inb_r_[(dp_ - DQ) & 7];
    dp_ = (dp_ + 1) & 7;
    for (int k = 0; k < kSections; ++k) {
      const double a = AI[k];
      double t = flush(vIL - a * si_l_[k]); vIL = flush(a * t + si_l_[k]); si_l_[k] = t;
      t = flush(vIR - a * si_r_[k]); vIR = flush(a * t + si_r_[k]); si_r_[k] = t;
    }
    for (int k = 0; k < kSections; ++k) {
      const double a = AQ[k];
      double t = flush(vQL - a * sq_l_[k]); vQL = flush(a * t + sq_l_[k]); sq_l_[k] = t;
      t = flush(vQR - a * sq_r_[k]); vQR = flush(a * t + sq_r_[k]); sq_r_[k] = t;
    }

    // modulator: two Euler half-steps per sample, renormalised at quadrant crossings
    for (int step = 0; step < 2; ++step) {
      const double c2 = flush(mc_ - w96_ * ms_), s2 = flush(ms_ + w96_ * mc_);
      mc_ = c2; ms_ = s2;
      const int q = mc_ >= 0 ? (ms_ >= 0 ? 0 : 3) : (ms_ >= 0 ? 1 : 2);
      if (q != quad_) {
        quad_ = q;
        const double nrm = flush(1 / libm_sqrt(std::fabs(mc_ * mc_ + ms_ * ms_)));
        mc_ = flush(mc_ * nrm); ms_ = flush(ms_ * nrm);
      }
    }
    // A = down-shift of the left input, B = up-shift of the right input
    double A = flush(vIL * mc_ + vQL * ms_), B = flush(vIR * mc_ - vQR * ms_);

    // blur (approximate random multitap on the wet signal)
    if (blur_on_) {
      double* blur_l = blur_.get();
      double* blur_r = blur_l + kBlurSize;
      blur_l[bwp_] = A; blur_r[bwp_] = B;
      double sa = 0, sb = 0;
      for (int k = 0; k < kBlurTaps; ++k) {
        sa = flush(sa + bls_l_[k] * blur_l[static_cast<int64_t>(bwp_ - blk_l_[k]) & (kBlurSize - 1)]);
        sb = flush(sb + bls_r_[k] * blur_r[static_cast<int64_t>(bwp_ - blk_r_[k]) & (kBlurSize - 1)]);
      }
      bwp_ = (bwp_ + 1) & (kBlurSize - 1);
      A = flush(sa * blur_g_); B = flush(sb * blur_g_);
    }

    // feedback write: Balance pans the loop input between the two sides
    feedback_line_[fwp_] = flush(bal_a_ * A + bal_b_ * B);
    fwp_ += 1;
    feedback_written_ = std::max(feedback_written_, fwp_);
    if (fwp_ >= feedback_length_) fwp_ = 0;
    double oL = swp_ ? B : A, oR = swp_ ? A : B;
    // Dir: d > 0 crossfades the left output towards the right one, d < 0 the right output towards the left one
    if (dirv_ >= 0)
      oL = flush((1 - dirv_) * oL + dirv_ * oR);
    else
      oR = flush((1 + dirv_) * oR - dirv_ * oL);
    // Width: mid/side blend
    const double mid = flush(0.5 * (oL + oR)), side = flush(0.5 * (oL - oR));
    const double wL = flush(mid + wdt_ * side), wR = flush(mid - wdt_ * side);
    // dry path delayed by the reported latency
    dry_l_[dwp_] = spl0; dry_r_[dwp_] = spl1;
    const double dL = dry_l_[(dwp_ - kLatency) & 31], dR = dry_r_[(dwp_ - kLatency) & 31];
    dwp_ = (dwp_ + 1) & 31;
    left = flush(glev_ * (gd_ * dL + gw_ * wL));
    right = flush(glev_ * (gd_ * dR + gw_ * wR));
  }
}
