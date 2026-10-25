// SPDX-License-Identifier: GPL-3.0-or-later
// DynamicTube.jsfx (Alessio Scala), ported line for line; see README.atlas.md.
#include "drive_dynamics.h"
#include "drive_dynamics_common.h"

#include <algorithm>
#include <cmath>

namespace atlas::drive_dynamics {
  namespace {
#include "dynamic_tube_tables.inc"
    // ---------------- measured constants ----------------
    constexpr double BIAS_K = 1.3;                         // curve offset per unit of Bias
    constexpr double TONE_LO_FC = 40, TONE_HI_FC = 18500;  // Tone: tilting shelves (+/-15 dB at Tone = +/-1), slope 0.1
    constexpr double TONE_DB = 15, TONE_S = 0.1;
    constexpr double DC_FC = 7.022;                        // two 1st-order DC blockers after the tube
    constexpr double LUT_U0 = -2.5, LUT_U1 = 10.5, LUT_K = 1040, LUT_SCALE = LUT_K / (LUT_U1 - LUT_U0);
    // 2x polyphase IIR half-band used by Hi-Quality (coefficients alternate between the two branches)
    constexpr double HB[6] = { 0.0178514446, 0.1322267046, 0.3768012742, 0.6375699163, 0.8740288713, 0.9955406713 };

    // measured tube curve: Catmull-Rom spline through evenly spaced knots, flat outside
    double tcurve(const double* tab, double u) {
      const double p = (std::min(std::max(u, LUT_U0), LUT_U1) - LUT_U0) * LUT_SCALE;
      const double i = std::min(std::floor(p), LUT_K - 1);
      const double f = p - i;
      const int k = static_cast<int>(i);
      const double p0 = tab[k], p1 = tab[k + 1], p2 = tab[k + 2], p3 = tab[k + 3];
      const double m1 = 0.5 * (p2 - p0), m2 = 0.5 * (p3 - p1);
      const double f2 = f * f, f3 = f2 * f;
      return (2 * f3 - 3 * f2 + 1) * p1 + (f3 - 2 * f2 + f) * m1 + (-2 * f3 + 3 * f2) * p2 + (f3 - f2) * m2;
    }
  }

  double DynamicTube::shape(double u) const { return tcurve(cur_tab_, u); }

  // RBJ shelving filter; high = false low shelf, high = true high shelf
  void DynamicTube::setShelf(BiquadCoefficients& c, bool high, double fc, double gdb, double s) const {
    const double amp = std::pow(10.0, gdb / 40), w0 = 2 * kPi * std::min(fc, 0.49 * srate_) / srate_, cs = std::cos(w0);
    const double al = std::sin(w0) / 2 * std::sqrt((amp + 1 / amp) * (1 / s - 1) + 2), sq = 2 * std::sqrt(amp) * al;
    if (high)
      c.setNorm(amp * ((amp + 1) + (amp - 1) * cs + sq), -2 * amp * ((amp - 1) + (amp + 1) * cs), amp * ((amp + 1) + (amp - 1) * cs - sq),
                (amp + 1) - (amp - 1) * cs + sq, 2 * ((amp - 1) - (amp + 1) * cs), (amp + 1) - (amp - 1) * cs - sq);
    else
      c.setNorm(amp * ((amp + 1) - (amp - 1) * cs + sq), 2 * amp * ((amp - 1) - (amp + 1) * cs), amp * ((amp + 1) - (amp - 1) * cs - sq),
                (amp + 1) + (amp - 1) * cs + sq, -2 * ((amp - 1) + (amp + 1) * cs), (amp + 1) + (amp - 1) * cs - sq);
  }

  // one host-rate sample through the 2x oversampled tube (offset b added after upsampling)
  double DynamicTube::shapeOversampled(double u, double b, HalfBandState& st) const {
    const double a = allpassChain(HB, shape(allpassChain(HB, u, 0, st.up0) + b), 0, st.down0);
    const double c = allpassChain(HB, shape(allpassChain(HB, u, 1, st.up1) + b), 1, st.down1);
    const double y = 0.5 * (a + st.held);
    st.held = c;
    return y;
  }

  double DynamicTube::msCoefficient(double ms) const {
    return 1 - std::exp(-1 / (std::max(ms, 0.001) * 0.001 * srate_));
  }

  void DynamicTube::prepare(double sample_rate) {
    *this = DynamicTube();
    srate_ = sample_rate;
    cur_tab_ = kDynamicTubeTabA;
    const double k = std::tan(kPi * DC_FC / srate_);
    dc_b_ = 1 / (1 + k);
    dc_a_ = (k - 1) / (k + 1);
    sm_ = 1 - std::exp(-1 / (0.02 * srate_));
    sm_init_ = false;
    last_tone_ = -100;
    envl_ = envr_ = 0;
  }

  void DynamicTube::setParameters(const Parameters& parameters) {
    p_ = parameters;
    // update_targets()
    t_gain_ = std::pow(10.0, p_.drive / 20);
    t_out_ = std::pow(10.0, p_.output / 20);
    t_mix_ = p_.dry_wet / 100;
    t_bias_ = BIAS_K * p_.bias / 100;
    t_env_ = p_.envelope;
    att_c_ = msCoefficient(p_.attack); rel_c_ = msCoefficient(p_.release);
    const double ttype_i = std::min(std::max(std::floor(p_.tube_type + 0.5), 0.0), 2.0);
    cur_tab_ = eelEqual(ttype_i, 0) ? kDynamicTubeTabA : eelEqual(ttype_i, 1) ? kDynamicTubeTabB : kDynamicTubeTabC;
    use_tone_ = std::abs(p_.tone) > 0.00001;
    if (!eelEqual(p_.tone, last_tone_)) {
      last_tone_ = p_.tone;
      setShelf(tlo_, false, TONE_LO_FC, TONE_DB * p_.tone, TONE_S); tloi_.setInverse(tlo_);
      setShelf(thi_, true, TONE_HI_FC, -TONE_DB * p_.tone, TONE_S); thii_.setInverse(thi_);
    }
    os_on_ = p_.hi_quality >= 0.5;
    lat_ = os_on_ ? kHiQualityLatency : 0;
    // @slider
    if (!sm_init_) {
      sm_init_ = true; s_gain_ = t_gain_; s_out_ = t_out_; s_mix_ = t_mix_; s_bias_ = t_bias_; s_env_ = t_env_;
    }
  }

  void DynamicTube::process(double& spl0, double& spl1) {
    s_gain_ += sm_ * (t_gain_ - s_gain_);
    s_out_  += sm_ * (t_out_ - s_out_);
    s_mix_  += sm_ * (t_mix_ - s_mix_);
    s_bias_ += sm_ * (t_bias_ - s_bias_);
    s_env_  += sm_ * (t_env_ - s_env_);

    double xl = spl0, xr = spl1, yl, yr;
    if (use_tone_) {
      xl = thl_.process(tll_.process(xl, tlo_), thi_);
      xr = thr_.process(tlr_.process(xr, tlo_), thi_);
    }
    // envelope follower on the (tone-shaped) input moves the bias
    const double vl = std::abs(xl); envl_ += (vl > envl_ ? att_c_ : rel_c_) * (vl - envl_);
    const double vr = std::abs(xr); envr_ += (vr > envr_ ? att_c_ : rel_c_) * (vr - envr_);
    // curve offset (Envelope is not scaled by Drive)
    const double bl = -s_bias_ - s_env_ * envl_, br = -s_bias_ - s_env_ * envr_;
    if (os_on_) {
      yl = shapeOversampled(s_gain_ * xl, bl, st_l_);
      yr = shapeOversampled(s_gain_ * xr, br, st_r_);
    }
    else {
      yl = shape(s_gain_ * xl + bl);
      yr = shape(s_gain_ * xr + br);
    }
    yl /= s_gain_; yr /= s_gain_;
    if (use_tone_) {
      yl = thli_.process(tlli_.process(yl, tloi_), thii_);
      yr = thri_.process(tlri_.process(yr, tloi_), thii_);
    }
    yl = dl2_.process(dl1_.process(yl, dc_b_, dc_a_), dc_b_, dc_a_);
    yr = dr2_.process(dr1_.process(yr, dc_b_, dc_a_), dc_b_, dc_a_);
    yl *= s_out_; yr *= s_out_;

    // dry path delayed to match the oversampling latency
    dry_l_[dwp_] = spl0; dry_r_[dwp_] = spl1;
    const int rp = (dwp_ - static_cast<int>(lat_) + 1024) % 1024;
    dwp_ = (dwp_ + 1) % 1024;
    spl0 = (1 - s_mix_) * dry_l_[rp] + s_mix_ * yl;
    spl1 = (1 - s_mix_) * dry_r_[rp] + s_mix_ * yr;
  }
}
