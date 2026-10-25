// SPDX-License-Identifier: GPL-3.0-or-later
// MultibandDynamics.jsfx (Alessio Scala), ported line for line without its
// sidechain (inputs 3/4, sliders 40 to 43, always off here); see README.atlas.md.
#include "drive_dynamics.h"
#include "drive_dynamics_common.h"

#include <algorithm>
#include <cmath>

namespace atlas::drive_dynamics {
  namespace {
    // ---------------- measured constants ----------------
    constexpr double XO_Q = 0.7071067811865476;
    constexpr double KNEE_W = 20.0;
    constexpr double GAIN_MAX = 36.12359947967774;
    // Everything runs at 2x the host rate through a polyphase IIR half-band (8 coefficients,
    // transition 0.01), identified from Live's band responses.
    constexpr int HB_PER = 4;
    constexpr double HB[8] = { 0.0771150798324, 0.265968526521, 0.482070625061, 0.665104153263,
                               0.796820471332, 0.884101508551, 0.941251427774, 0.982005414189 };
    const double FLOOR = std::pow(10.0, -30.0);
  }

  // chain of first-order allpasses y = c*x + x1 - c*y1 over HB[first], HB[first + 2], ...
  double MultibandDynamics::AllpassChain::process(double input, int first) {
    for (int i = 0; i < HB_PER; ++i) {
      const double c = HB[first + 2 * i];
      const double out = c * input + x[i] - c * y[i];
      x[i] = input; y[i] = out; input = out;
    }
    return input;
  }

  // RBJ low pass (type 0), high pass (type 1) and all pass (type 2) at the oversampled rate
  void MultibandDynamics::setRbj(BiquadCoefficients& c, int type, double fc, double q) const {
    const double w0 = 2 * kPi * std::min(fc, 0.49 * osr_) / osr_, cs = std::cos(w0), al = std::sin(w0) / (2 * q);
    if (type == 0) c.setNorm((1 - cs) / 2, 1 - cs, (1 - cs) / 2, 1 + al, -2 * cs, 1 - al);
    else if (type == 1) c.setNorm((1 + cs) / 2, -(1 + cs), (1 + cs) / 2, 1 + al, -2 * cs, 1 - al);
    else c.setNorm(1 - al, -2 * cs, 1 + al, 1 + al, -2 * cs, 1 - al);
  }

  // 3-way Linkwitz-Riley (4th order) split of one channel
  void MultibandDynamics::split(Crossover& c, double x) const {
    double r;
    if (lo_split_) {
      c.o0 = c.l2.process(c.l1.process(x, clp1_), clp1_);
      r = c.h2.process(c.h1.process(x, chp1_), chp1_);
    }
    else { c.o0 = 0; r = x; }
    if (hi_split_) {
      c.o1 = c.m2.process(c.m1.process(r, clp2_), clp2_);
      c.o2 = c.k2.process(c.k1.process(r, chp2_), chp2_);
      if (lo_split_) c.o0 = c.ap.process(c.o0, cap2_);
    }
    else { c.o1 = r; c.o2 = 0; }
  }

  // Attack / Release are the time for the follower to move 80 dB: tau = T / ln(1e4)
  double MultibandDynamics::msCoefficient(double ms) const {
    return 1 - std::exp(-std::log(10000.0) / (std::max(ms * p_.time / 100, 0.001) * 0.001 * osr_));
  }

  // static curve in dB; quadratic soft knee KNEE_W wide, centred on each threshold
  double MultibandDynamics::gcomp(const Band& band, double level) const {
    const double w = p_.knee >= 0.5 ? KNEE_W : 0;
    double g = 0;
    double d = level - band.athr;
    if (w > 0 && std::abs(d) <= w / 2) g += band.ar * std::pow(d + w / 2, 2.0) / (2 * w);
    else if (d > 0) g += band.ar * d;
    d = band.bthr - level;
    if (w > 0 && std::abs(d) <= w / 2) g += band.br * std::pow(d + w / 2, 2.0) / (2 * w);
    else if (d > 0) g += band.br * d;
    return std::min(g * p_.amount / 100, GAIN_MAX);
  }

  // linked detector (mean of the channels' power for RMS, of their magnitude for Peak) -> gain in band.g
  void MultibandDynamics::bandGain(Band& band, double dl, double dr) const {
    const bool peak = p_.peak >= 0.5;
    const double e = peak ? 0.5 * (std::abs(dl) + std::abs(dr)) : 0.5 * (dl * dl + dr * dr);
    band.env += (e > band.env ? band.ca : band.cr) * (e - band.env);
    const double level = peak ? 20 * std::log10(std::max(band.env, FLOOR)) : 10 * std::log10(std::max(band.env, FLOOR));
    band.gdb = gcomp(band, level);
    band.g = std::pow(10.0, band.gdb / 20);
  }

  // Amount scales every gain in dB: the static curve, the band's Input and Output gains and the
  // global Output. An inactive band passes through untouched (no Input / Output gain).
  void MultibandDynamics::bandSetup(Band& band, const BandParameters& p) const {
    band.act = p.active >= 0.5; band.solo = p.solo >= 0.5;
    band.athr = p.above_threshold; band.bthr = p.below_threshold;     // Live does not keep Below under Above
    band.ar = p.above_ratio;            // gain change per dB above the threshold
    band.br = p.below_ratio;            // gain change per dB below the threshold
    band.ca = msCoefficient(p.attack); band.cr = msCoefficient(p.release);
    band.gin = std::pow(10.0, p.input * p_.amount / 100 / 20);
    band.gout = band.act ? std::pow(10.0, p.output * p_.amount / 100 / 20) : 1;
    band.gsig = band.act ? band.gin : 1;   // input gain on the audio path
  }

  void MultibandDynamics::update() {
    lo_split_ = p_.low_band >= 0.5; hi_split_ = p_.high_band >= 0.5;
    // a Low-Mid split above the Mid-High split is pulled down to it (closest match to Live)
    const double xo1e = std::min(p_.split_low_mid, p_.split_mid_high);
    setRbj(clp1_, 0, xo1e, XO_Q); setRbj(chp1_, 1, xo1e, XO_Q);
    setRbj(clp2_, 0, p_.split_mid_high, XO_Q); setRbj(chp2_, 1, p_.split_mid_high, XO_Q);
    setRbj(cap2_, 2, p_.split_mid_high, XO_Q);
    for (int i = 0; i < 3; ++i) bandSetup(b_[i], p_.bands[i]);
    // as in Live, soloing a band whose split is off still mutes the other bands
    const bool any_solo = b_[0].solo || b_[1].solo || b_[2].solo;
    w0_ = (!any_solo || b_[0].solo) * b_[0].gout;
    w1_ = (!any_solo || b_[1].solo) * b_[1].gout;
    w2_ = (!any_solo || b_[2].solo) * b_[2].gout;
    out_g_ = std::pow(10.0, p_.output * p_.amount / 100 / 20);
  }

  void MultibandDynamics::prepare(double sample_rate) {
    *this = MultibandDynamics();
    srate_ = sample_rate;
    osr_ = 2 * srate_;
  }

  void MultibandDynamics::setParameters(const Parameters& parameters) {
    p_ = parameters;
    update();
  }

  // one oversampled sample through crossovers, detectors and gains -> ol / or_
  void MultibandDynamics::osTick(double xl, double xr) {
    split(cl_, xl); split(cr_, xr);
    Band& bl = b_[0];
    Band& bm = b_[1];
    Band& bh = b_[2];
    // detector inputs: the band after its input gain
    const double d0l = cl_.o0 * bl.gin, d0r = cr_.o0 * bl.gin;
    const double d1l = cl_.o1 * bm.gin, d1r = cr_.o1 * bm.gin;
    const double d2l = cl_.o2 * bh.gin, d2r = cr_.o2 * bh.gin;
    double l0 = cl_.o0 * bl.gsig, r0 = cr_.o0 * bl.gsig;
    double l1 = cl_.o1 * bm.gsig, r1 = cr_.o1 * bm.gsig;
    double l2 = cl_.o2 * bh.gsig, r2 = cr_.o2 * bh.gsig;
    if (lo_split_) bandGain(bl, d0l, d0r);
    bandGain(bm, d1l, d1r);
    if (hi_split_) bandGain(bh, d2l, d2r);
    if (bl.act) { l0 *= bl.g; r0 *= bl.g; }
    if (bm.act) { l1 *= bm.g; r1 *= bm.g; }
    if (bh.act) { l2 *= bh.g; r2 *= bh.g; }
    ol_ = w0_ * l0 + w1_ * l1 + w2_ * l2;
    or_ = w0_ * r0 + w1_ * r1 + w2_ * r2;
  }

  void MultibandDynamics::process(double& spl0, double& spl1) {
    // upsample: the two polyphase branches give the even and odd oversampled samples
    const double ule = ule_.process(spl0, 0);
    osTick(ule, ure_.process(spl1, 0));
    const double ea = dle_.process(ol_, 0), eb = dre_.process(or_, 0);
    const double ulo = ulo_.process(spl0, 1);
    osTick(ulo, uro_.process(spl1, 1));
    const double oa = dlo_.process(ol_, 1), ob = dro_.process(or_, 1);
    // downsample: average the even branch with the previous odd branch
    spl0 = 0.5 * (ea + dlo_b_) * out_g_; dlo_b_ = oa;
    spl1 = 0.5 * (eb + dro_b_) * out_g_; dro_b_ = ob;
  }
}
