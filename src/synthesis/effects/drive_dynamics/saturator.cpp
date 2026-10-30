// SPDX-License-Identifier: GPL-3.0-or-later
// Saturator.jsfx (Alessio Scala), ported line for line; see README.atlas.md.
#include "drive_dynamics.h"
#include "drive_dynamics_common.h"

#include <algorithm>
#include <cmath>

namespace atlas::drive_dynamics {
  namespace {
#include "saturator_tables.inc"
    // ---------------- measured constants ----------------
    constexpr double SHELF_FC = 405.4785, SHELF_K = -0.465936, SHELF_FDC = 2.715171;   // Amt Lo shelf
    constexpr double BELL_Q0 = 1.71109, BELL_Q1 = 0.3008, BELL_QE = 2.22389;   // Width 0..1 -> bell Q
    constexpr double DC_FC = 27.33, DC_Q = 0.486;                   // Pre-DC filter (2nd-order high-pass)
    constexpr double WSDC_FC = 7.022;                               // Waveshaper output DC blockers (2 stages)
    constexpr double WS_DAMP_K = 0.00624, WS_PERIOD_K = 60, WS_CLAMP = 10;
    constexpr double UMAX_A = 8, UMAX_M = 4, UMAX_H = 16;
    constexpr int LUTN = 128;
    // 2x polyphase IIR half-band used by Hi-Quality (coefficients alternate between the two branches)
    constexpr double HB[6] = { 0.017851445, 0.1322267, 0.37680127, 0.63756992, 0.87402887, 0.99554067 };

    double hermite(const double* tab, double s) {
      const double p = std::min(std::max(s, 0.0), 1.0) * LUTN;
      const double i = std::min(std::floor(p), LUTN - 1.0);
      const double f = p - i;
      const int k = static_cast<int>(i);
      const double p1 = tab[k], p2 = tab[k + 1];
      const double p0 = i > 0 ? tab[k - 1] : -tab[1];
      const double p3 = i < LUTN - 1 ? tab[k + 2] : 2 * tab[LUTN] - tab[LUTN - 1];
      const double m1 = 0.5 * (p2 - p0), m2 = 0.5 * (p3 - p1);
      const double f2 = f * f, f3 = f2 * f;
      return (2 * f3 - 3 * f2 + 1) * p1 + (f3 - 2 * f2 + f) * m1 + (-2 * f3 + 3 * f2) * p2 + (f3 - f2) * m2;
    }
    double lutcurve(const double* tab, double umax, double u) {
      const double a = std::abs(u);
      return a >= umax ? (u < 0 ? -tab[LUTN] : tab[LUTN]) : (u < 0 ? -1 : 1) * hermite(tab, std::sqrt(a / umax));
    }
  }

  double Saturator::waveshaper(double u) const {
    u = std::min(std::max(u, -WS_CLAMP), WS_CLAMP);
    double s = ws_lin_ * u + std::sin(ws_ca_ * u * u) + ws_dep_ * std::sin(ws_pf_ * u);
    if (ws_damp_ > 0) {
      const double u4 = u * u * u * u;
      s *= u4 / (u4 + WS_DAMP_K * ws_damp_);
    }
    return (1 - ws_drv_) * u + ws_drv_ * s;
  }

  double Saturator::shape(double u) const {
    if (eelEqual(ctype_i_, 0)) return lutcurve(kSaturatorLutA, UMAX_A, u);
    if (eelEqual(ctype_i_, 1)) return std::sin(0.5 * kPi * std::min(std::max(u, -1.0), 1.0));
    if (eelEqual(ctype_i_, 2)) {
      const double a = std::abs(u), t = bthr_lin_;
      return a <= t ? u : (u < 0 ? -1 : 1) * (t + (1 - t) * (1 - std::exp(-(a - t) / std::max(1 - t, 0.000000001))));
    }
    if (eelEqual(ctype_i_, 3)) return lutcurve(kSaturatorLutM, UMAX_M, u);
    if (eelEqual(ctype_i_, 4)) return lutcurve(kSaturatorLutH, UMAX_H, u);
    if (eelEqual(ctype_i_, 5)) return std::sin(0.5 * kPi * std::min(std::max(u, -9.0), 9.0));
    if (eelEqual(ctype_i_, 6)) return std::min(std::max(u, -1.0), 1.0);
    return waveshaper(u);
  }

  // Amt Lo: 1st-order low shelf whose corner moves with gain, with its boost/cut
  // returned to unity at DC by a 1st-order high-pass:  H = 1 + (S - 1) * HP_dc
  void Saturator::setShelf(BiquadCoefficients& c, double gdb) const {
    const double g = std::pow(10.0, gdb / 20);
    const double K = std::tan(kPi * std::min(SHELF_FC * std::pow(g, SHELF_K), 0.49 * srate_) / srate_);
    const double a = (K - 1) / (K + 1);
    const double bs0 = (g * K + 1) / (K + 1) - 1, bs1 = (g * K - 1) / (K + 1) - a;    // S - 1 numerator
    const double Kd = std::tan(kPi * SHELF_FDC / srate_), bh = 1 / (1 + Kd), ad = (Kd - 1) / (Kd + 1);
    const double n0 = bs0 * bh, n1 = bs1 * bh - bs0 * bh;                              // (S - 1) * HP_dc numerator
    c.setNorm(1 + n0, a + ad + n1, a * ad - bs1 * bh, 1, a + ad, a * ad);
  }

  void Saturator::setPeak(BiquadCoefficients& c, double fc, double gdb, double q) const {
    const double A = std::pow(10.0, gdb / 40), w0 = 2 * kPi * std::min(fc, 0.49 * srate_) / srate_;
    const double cs = std::cos(w0), al = std::sin(w0) / (2 * q);
    c.setNorm(1 + al * A, -2 * cs, 1 - al * A, 1 + al / A, -2 * cs, 1 - al / A);
  }

  void Saturator::setHighPass(BiquadCoefficients& c, double fc, double q) const {
    const double w0 = 2 * kPi * fc / srate_, cs = std::cos(w0), al = std::sin(w0) / (2 * q);
    c.setNorm((1 + cs) / 2, -(1 + cs), (1 + cs) / 2, 1 + al, -2 * cs, 1 - al);
  }

  // one host-rate sample through the 2x oversampled shaper
  double Saturator::shapeOversampled(double u, HalfBandState& st) const {
    const double a = allpassChain(HB, shape(allpassChain(HB, u, 0, st.up0)), 0, st.down0);
    const double b = allpassChain(HB, shape(allpassChain(HB, u, 1, st.up1)), 1, st.down1);
    const double y = 0.5 * (a + st.held);
    st.held = b;
    return y;
  }

  void Saturator::prepare(double sample_rate) {
    *this = Saturator(remove_dc_);
    srate_ = sample_rate;
    const double k = std::tan(kPi * WSDC_FC / srate_);
    wsdc_b_ = 1 / (1 + k);
    wsdc_a_ = (k - 1) / (k + 1);
    // Atlas's wet DC blocker, the scripts' DC blocker form at kDcBlockerHz: unity at Nyquist.
    const double kd = std::tan(kPi * kDcBlockerHz / srate_);
    out_dc_b_ = 1 / (1 + kd);
    out_dc_a_ = (kd - 1) / (kd + 1);
    setHighPass(dcc_, DC_FC, DC_Q);
    sm_ = 1 - std::exp(-1 / (0.02 * srate_));
    sm_init_ = false;
    last_key_ = -1;
  }

  void Saturator::setParameters(const Parameters& parameters) {
    p_ = parameters;
    // update_targets()
    t_gain_ = std::pow(10.0, p_.drive / 20);
    t_out_ = std::pow(10.0, p_.output / 20);
    t_mix_ = p_.dry_wet / 100;
    ctype_i_ = std::floor(p_.type + 0.5);
    bthr_lin_ = std::pow(10.0, p_.bass_threshold / 20);
    ws_drv_ = p_.ws_drive / 100; ws_lin_ = p_.ws_linear / 100; ws_damp_ = p_.ws_damp / 100;
    ws_ca_ = 2 * kPi * p_.ws_curve / 100;
    ws_dep_ = 0.5 * std::pow(p_.ws_depth / 100, 2.0);
    ws_pf_ = kPi * (1 + WS_PERIOD_K * std::pow(p_.ws_period / 100, 2.0));
    use_shelf_ = p_.color >= 0.5 && std::abs(p_.amt_lo) > 0.0001;
    use_peak_ = p_.color >= 0.5 && std::abs(p_.amt_hi) > 0.0001;
    const double key = p_.amt_lo * 7 + p_.frequency * 13 + p_.width * 17 + p_.amt_hi * 19 + p_.color;
    if (!eelEqual(key, last_key_)) {
      last_key_ = key;
      setShelf(shf_, p_.amt_lo); shfi_.setInverse(shf_);
      setPeak(pkf_, p_.frequency, p_.amt_hi, BELL_Q0 * std::pow(BELL_Q1 / BELL_Q0, std::pow(p_.width / 100, BELL_QE)));
      pkfi_.setInverse(pkf_);
    }
    os_on_ = p_.hi_quality >= 0.5;
    lat_ = os_on_ ? kHiQualityLatency : 0;
    // @slider
    if (!sm_init_) { sm_init_ = true; s_gain_ = t_gain_; s_out_ = t_out_; s_mix_ = t_mix_; }
  }

  void Saturator::process(double& spl0, double& spl1) {
    s_gain_ += sm_ * (t_gain_ - s_gain_);
    s_out_  += sm_ * (t_out_ - s_out_);
    s_mix_  += sm_ * (t_mix_ - s_mix_);

    double xl = spl0, xr = spl1, yl, yr;
    if (p_.dc_filter >= 0.5) { xl = dl_.process(xl, dcc_); xr = dr_.process(xr, dcc_); }
    if (use_shelf_) { xl = sl_.process(xl, shf_); xr = sr_.process(xr, shf_); }
    if (use_peak_) { xl = pl_.process(xl, pkf_); xr = pr_.process(xr, pkf_); }
    if (os_on_) {
      yl = shapeOversampled(s_gain_ * xl, st_l_);
      yr = shapeOversampled(s_gain_ * xr, st_r_);
    }
    else {
      yl = shape(s_gain_ * xl);
      yr = shape(s_gain_ * xr);
    }
    if (eelEqual(ctype_i_, 7)) {
      yl = wl2_.process(wl1_.process(yl, wsdc_b_, wsdc_a_), wsdc_b_, wsdc_a_);
      yr = wr2_.process(wr1_.process(yr, wsdc_b_, wsdc_a_), wsdc_b_, wsdc_a_);
    }
    if (use_peak_) { yl = pli_.process(yl, pkfi_); yr = pri_.process(yr, pkfi_); }
    if (use_shelf_) { yl = sli_.process(yl, shfi_); yr = sri_.process(yr, shfi_); }
    if (p_.post_clip >= 1.5) { yl = std::min(std::max(yl, -1.0), 1.0); yr = std::min(std::max(yr, -1.0), 1.0); }
    else if (p_.post_clip >= 0.5) { yl = lutcurve(kSaturatorLutA, UMAX_A, yl); yr = lutcurve(kSaturatorLutA, UMAX_A, yr); }
    yl *= s_out_; yr *= s_out_;
    if (remove_dc_) { yl = out_dc_l_.process(yl, out_dc_b_, out_dc_a_); yr = out_dc_r_.process(yr, out_dc_b_, out_dc_a_); }

    // dry path delayed to match the oversampling latency
    dry_l_[dwp_] = spl0; dry_r_[dwp_] = spl1;
    const int rp = (dwp_ - static_cast<int>(lat_) + 1024) % 1024;
    dwp_ = (dwp_ + 1) % 1024;
    spl0 = (1 - s_mix_) * dry_l_[rp] + s_mix_ * yl;
    spl1 = (1 - s_mix_) * dry_r_[rp] + s_mix_ * yr;
  }
}
