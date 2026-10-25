// SPDX-License-Identifier: GPL-3.0-or-later
// Overdrive.jsfx (Alessio Scala), ported line for line; see README.atlas.md.
#include "drive_dynamics.h"
#include "drive_dynamics_common.h"

#include <algorithm>
#include <cmath>

namespace atlas::drive_dynamics {
  namespace {
    // ---- model constants (fitted against the original at 48 kHz) ----
    constexpr double DRIVE_DB0   = -3.4863;   // pre-gain at 0 % drive (dB)
    constexpr double DRIVE_DBPCT = 0.36;      // dB of pre-gain per % drive
    constexpr double OUT_GAIN    = 0.69123;   // clipper output scale
    constexpr double BIAS        = 0.11856;   // clipper asymmetry, relative to the ceiling floor
    constexpr double DYN_EXP     = 0.88206;   // envelope -> ceiling power law
    constexpr double DYN_T0      = -0.0019;   // dynamics threshold (dB) at 0 %
    constexpr double DYN_TK      = -31.984;   // threshold change (dB) from 0 to 100 %
    constexpr double DYN_N0K     = -18.066;   // ceiling floor change (dB) from 0 to 100 %
    constexpr double ATT_M96     = 0.74884;   // make-up envelope attack coefficient @96k
    constexpr double REL_M96     = 0.30915;   // make-up envelope release coefficient @96k
    constexpr double REL_D96     = 0.30134;   // clipper-input envelope release coefficient @96k
    constexpr double SMO_D96     = 0.93876;   // clipper-input envelope smoothing coefficient @96k
    constexpr double LOOK96      = 8;         // make-up envelope look-ahead (samples @96k)
    constexpr double LIVE_LAT    = 4;         // latency reported for the oversampling filters
    constexpr double TONE_FC     = 320;       // tone tilt corner (Hz)
    constexpr double DC_FC       = 7;         // DC blockers (two 1st-order stages)
    constexpr double FMIN = 50, FMAX = 20000;
    // 2x polyphase IIR half-band (two chains of 1st-order allpass sections)
    constexpr double HB0 = 0.0098, HB1 = 0.97431, HB2 = 0.24303, HB3 = 0.47357, HB4 = 0.77317, HB5 = 0.07963;

    double onepoleK(double fc, double rate) { return std::tan(kPi * std::min(fc, 0.49 * rate) / rate); }

    double shaper(double v) {
      v = std::min(std::max(v, -1.0), 1.0);
      return 1.5 * v - 0.5 * v * v * v;
    }
  }

  double Overdrive::Chain::chain0(double x) { return p[0].process(p[2].process(p[4].process(x, HB4), HB2), HB0); }
  double Overdrive::Chain::chain1(double x) { return p[1].process(p[3].process(p[5].process(x, HB5), HB3), HB1); }

  // half-band decimation of one 2x-rate pair (even, odd) -> one host-rate sample
  double Overdrive::Decimator::dec(double even, double odd) {
    const double a = d0.chain0(even);
    const double b = d1.chain1(odd);
    const double y = 0.5 * (a + bprev);
    bprev = b;
    return y;
  }

  double Overdrive::rateCoefficient(double c96) const { return 1 - std::pow(1 - c96, 96000 / osr_); }

  // RBJ Butterworth (Q = 1/sqrt 2) high-pass / low-pass, bilinear with pre-warping
  void Overdrive::highPass(BiquadCoefficients& c, double fc) const {
    const double w = 2 * kPi * std::min(fc, 0.49 * srate_) / srate_, cs = std::cos(w);
    const double al = std::sin(w) * 0.5 * std::sqrt(2.0), a0 = 1 + al;
    c.b0 = (1 + cs) * 0.5 / a0; c.b1 = -(1 + cs) / a0; c.b2 = c.b0;
    c.a1 = -2 * cs / a0; c.a2 = (1 - al) / a0;
  }
  void Overdrive::lowPass(BiquadCoefficients& c, double fc) const {
    const double w = 2 * kPi * std::min(fc, 0.49 * srate_) / srate_, cs = std::cos(w);
    const double al = std::sin(w) * 0.5 * std::sqrt(2.0), a0 = 1 + al;
    c.b0 = (1 - cs) * 0.5 / a0; c.b1 = (1 - cs) / a0; c.b2 = c.b0;
    c.a1 = -2 * cs / a0; c.a2 = (1 - al) / a0;
  }

  void Overdrive::bandEdges(Band& band, double f, double w) const {
    band.lo = std::min(std::max(f * std::pow(2.0, -w / 2), FMIN), FMAX);
    band.hi = std::min(std::max(f * std::pow(2.0, w / 2), FMIN), FMAX);
  }

  // one 2x-rate step of the drive stage for one channel.
  // u: signal, dm/dd: make-up / clipper-input detector inputs
  double Overdrive::core(Channel& ch, double u, double dm, double dd) {
    ch.buf[ch.wp] = u; ch.dbuf[ch.wp] = dd;
    const int rp = (ch.wp - look_ + 64) % 64;
    const double main = ch.buf[rp], a = ch.dbuf[rp];
    ch.wp = (ch.wp + 1) % 64;
    // clipper-input envelope is aligned with the main path, then smoothed
    if (a > ch.ed) ch.ed = a; else ch.ed += relD_ * (a - ch.ed);
    ch.eds += smoD_ * (ch.ed - ch.eds);
    // make-up envelope sees the signal "look" samples ahead of the main path
    if (dm > ch.em) ch.em += attM_ * (dm - ch.em); else ch.em += relM_ * (dm - ch.em);
    const double d = n0_ * std::pow(std::max(1.0, ch.eds * ithr_), DYN_EXP);
    const double m = n0_ * std::pow(std::max(1.0, ch.em * ithr_), DYN_EXP);
    ch.m = m;
    return shaper((s_gain_ * main + bias_n0_) / d);
  }

  // both channels share one envelope detector: the average of |L| and |R|
  void Overdrive::corePair(double ul, double ur) {
    const double det = 0.5 * (std::abs(ul) + std::abs(ur));
    out_l_ = core(cl_, ul, det, det);
    out_r_ = core(cr_, ur, det, det);
  }

  // tilt tone: equal-power blend of complementary 1-pole low/high-pass
  double Overdrive::tilt(Tilt& t, double x) const {
    const double lp = tone_b_ * (x + t.x1) - tone_a_ * t.y1;
    t.x1 = x; t.y1 = lp;
    return gl_ * lp + gh_ * (x - lp);
  }

  // one host-rate stereo sample through the drive stage -> out_l, out_r.
  // The clipper runs at 2x; its output and the make-up gain are decimated, then
  // the tone filter sits between them at the host rate.
  void Overdrive::driveOversampled(double xl, double xr) {
    const double ul0 = cl_.up0.chain0(xl);
    corePair(ul0, cr_.up0.chain0(xr));
    const double sl0 = out_l_, sr0 = out_r_, ml0 = cl_.m, mr0 = cr_.m;
    const double ul1 = cl_.up1.chain1(xl);
    corePair(ul1, cr_.up1.chain1(xr));
    const double gml = cl_.gm.dec(ml0, cl_.m);
    out_l_ = OUT_GAIN * gml * tilt(cl_.tn, cl_.gs.dec(sl0, out_l_));
    const double gmr = cr_.gm.dec(mr0, cr_.m);
    out_r_ = OUT_GAIN * gmr * tilt(cr_.tn, cr_.gs.dec(sr0, out_r_));
  }

  int Overdrive::latencyFor(const Parameters&, double sample_rate) {
    const double osr = 2 * sample_rate;
    return static_cast<int>(LIVE_LAT + std::floor(LOOK96 * osr / 96000 / 2 + 0.5));
  }

  void Overdrive::prepare(double sample_rate) {
    *this = Overdrive();
    srate_ = sample_rate;
    // coefficients follow the analysis rate (96k at 48k host rate)
    osr_ = 2 * srate_;
    attM_ = rateCoefficient(ATT_M96);
    relM_ = rateCoefficient(REL_M96);
    relD_ = rateCoefficient(REL_D96);
    smoD_ = rateCoefficient(SMO_D96);
    // even, so the delay is whole host samples
    look_ = static_cast<int>(2 * std::floor(LOOK96 * osr_ / 96000 / 2 + 0.5));
    lat_ = LIVE_LAT + look_ / 2;
    double k = onepoleK(TONE_FC, srate_); tone_b_ = k / (1 + k); tone_a_ = (k - 1) / (k + 1);
    k = onepoleK(DC_FC, srate_); dc_b_ = 1 / (1 + k); dc_a_ = (k - 1) / (k + 1);
    sm_init_ = false;
    smc_ = 1 - std::exp(-1 / (0.02 * srate_));
    coef_cnt_ = 0;
  }

  void Overdrive::updateTargets() {
    t_freq_  = std::min(std::max(p_.freq, FMIN), FMAX);
    t_width_ = std::min(std::max(p_.width, 0.5), 9.0);
    t_gain_  = std::pow(10.0, (DRIVE_DB0 + DRIVE_DBPCT * std::min(std::max(p_.drive, 0.0), 100.0)) / 20);
    t_dyn_   = std::min(std::max(p_.dynamics, 0.0), 100.0) / 100;
    t_tone_  = std::min(std::max(p_.tone, 0.0), 100.0) / 100;
    t_wet_   = std::min(std::max(p_.dry_wet, 0.0), 100.0) / 100;
    t_lfreq_ = std::log(t_freq_);
  }

  void Overdrive::setParameters(const Parameters& parameters) {
    p_ = parameters;
    updateTargets();
    if (!sm_init_) {
      sm_init_ = true;
      s_lfreq_ = std::log(t_freq_); s_width_ = t_width_; s_gain_ = t_gain_; s_dyn_ = t_dyn_; s_tone_ = t_tone_;
      s_wet_ = t_wet_;
      bandEdges(bnd_, t_freq_, t_width_); highPass(hpc_, bnd_.lo); lowPass(lpc_, bnd_.hi);
    }
  }

  void Overdrive::process(double& spl0, double& spl1) {
    // --- parameter smoothing ---
    s_lfreq_ += smc_ * (t_lfreq_ - s_lfreq_);
    s_width_ += smc_ * (t_width_ - s_width_);
    s_gain_  += smc_ * (t_gain_  - s_gain_);
    s_dyn_   += smc_ * (t_dyn_   - s_dyn_);
    s_tone_  += smc_ * (t_tone_  - s_tone_);
    s_wet_   += smc_ * (t_wet_   - s_wet_);
    if ((coef_cnt_ -= 1) <= 0) {
      coef_cnt_ = 16;
      bandEdges(bnd_, std::exp(s_lfreq_), s_width_);
      highPass(hpc_, bnd_.lo); lowPass(lpc_, bnd_.hi);
      n0_      = std::pow(10.0, DYN_N0K * s_dyn_ / 20);
      ithr_    = std::pow(10.0, -(DYN_T0 + DYN_TK * s_dyn_) / 20);
      bias_n0_ = BIAS * n0_;
      gl_ = std::sqrt(2.0) * std::cos(s_tone_ * kPi / 2);
      gh_ = std::sqrt(2.0) * std::sin(s_tone_ * kPi / 2);
      gdry_ = std::cos(s_wet_ * kPi / 2);
      gwet_ = std::sin(s_wet_ * kPi / 2);
    }

    // --- wet path: band-pass, oversampled drive stage, DC blockers ---
    double xl = hl_.process(spl0, hpc_); xl = ll_.process(xl, lpc_);
    double xr = hr_.process(spl1, hpc_); xr = lr_.process(xr, lpc_);
    driveOversampled(xl, xr);
    double yl = out_l_, yr = out_r_;
    yl = dl2_.process(dl1_.process(yl, dc_b_, dc_a_), dc_b_, dc_a_);
    yr = dr2_.process(dr1_.process(yr, dc_b_, dc_a_), dc_b_, dc_a_);

    // --- dry path delayed to line up with the reported latency ---
    dry_l_[dwp_] = spl0; dry_r_[dwp_] = spl1;
    const int rp = (dwp_ - static_cast<int>(lat_) + 1024) % 1024;
    const double dl = dry_l_[rp], dr = dry_r_[rp];
    dwp_ = (dwp_ + 1) % 1024;

    spl0 = gdry_ * dl + gwet_ * yl;
    spl1 = gdry_ * dr + gwet_ * yr;
  }
}
