// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>

// Native ports of Alessio Scala's Saturator, Overdrive, Dynamic Tube and
// Multiband Dynamics JSFX (README.atlas.md). Each class is the script's @init,
// @slider and @sample sections line for line, in double precision as JSFX
// computes: the parameters hold the scripts' slider values, setParameters runs
// @slider, and process runs @sample on one stereo frame. Only the drawing code
// and Multiband Dynamics' sidechain are left out. These files are built
// outside the fast-math unity file so the arithmetic stays as written.
namespace atlas::drive_dynamics {
  // A TDF-II biquad's coefficients and one instance's state, as the scripts'
  // set_norm and bq functions keep them.
  struct BiquadCoefficients {
    double b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    void setNorm(double nb0, double nb1, double nb2, double a0, double na1, double na2) {
      b0 = nb0 / a0; b1 = nb1 / a0; b2 = nb2 / a0; a1 = na1 / a0; a2 = na2 / a0;
    }
    void setInverse(const BiquadCoefficients& c) { setNorm(1, c.a1, c.a2, c.b0, c.b1, c.b2); }
  };
  struct BiquadState {
    double s1 = 0, s2 = 0;
    double process(double x, const BiquadCoefficients& c) {
      const double y = c.b0 * x + s1;
      s1 = c.b1 * x - c.a1 * y + s2;
      s2 = c.b2 * x - c.a2 * y;
      return y;
    }
  };
  // The scripts' 1st-order DC blocker; b and a are shared by every instance.
  struct DcBlocker {
    double x1 = 0, y1 = 0;
    double process(double x, double b, double a) {
      const double y = b * (x - x1) - a * y1;
      x1 = x; y1 = y;
      return y;
    }
  };
  // Saturator's and Dynamic Tube's ap_chain / shape_os state for one channel:
  // the four allpass chains (three sections each) and the held odd branch.
  struct HalfBandState {
    std::array<double, 6> up0 {}, up1 {}, down0 {}, down1 {};
    double held = 0;
  };

  class Saturator {
    public:
      struct Parameters {
        double drive = 0, type = 0, color = 0, amt_lo = 0, frequency = 1000, width = 30, amt_hi = 0;
        double post_clip = 0, output = 0, dry_wet = 100, bass_threshold = -50;
        double ws_drive = 100, ws_linear = 50, ws_curve = 0, ws_damp = 0, ws_period = 0, ws_depth = 0;
        double dc_filter = 0, hi_quality = 1;
      };
      // The latency Hi-Quality reports and the dry path is delayed by.
      static constexpr int kHiQualityLatency = 3;
      static int latencyFor(const Parameters& p, double) { return p.hi_quality >= 0.5 ? kHiQualityLatency : 0; }
      void prepare(double sample_rate);
      void setParameters(const Parameters& parameters);
      void process(double& left, double& right);
      int latency() const { return static_cast<int>(lat_); }

    private:
      double shape(double u) const;
      double waveshaper(double u) const;
      double shapeOversampled(double u, HalfBandState& state) const;
      void setShelf(BiquadCoefficients& c, double gdb) const;
      void setPeak(BiquadCoefficients& c, double fc, double gdb, double q) const;
      void setHighPass(BiquadCoefficients& c, double fc, double q) const;

      Parameters p_;
      double srate_ = 44100;
      double wsdc_b_ = 0, wsdc_a_ = 0, sm_ = 0;
      bool sm_init_ = false;
      double last_key_ = -1;
      double t_gain_ = 0, t_out_ = 0, t_mix_ = 0, s_gain_ = 0, s_out_ = 0, s_mix_ = 0;
      double ctype_i_ = 0, bthr_lin_ = 0;
      double ws_drv_ = 0, ws_lin_ = 0, ws_damp_ = 0, ws_ca_ = 0, ws_dep_ = 0, ws_pf_ = 0;
      bool use_shelf_ = false, use_peak_ = false, os_on_ = false;
      double lat_ = 0;
      BiquadCoefficients dcc_, shf_, shfi_, pkf_, pkfi_;
      BiquadState dl_, dr_, sl_, sr_, pl_, pr_, pli_, pri_, sli_, sri_;
      DcBlocker wl1_, wl2_, wr1_, wr2_;
      HalfBandState st_l_, st_r_;
      std::array<double, 1024> dry_l_ {}, dry_r_ {};
      int dwp_ = 0;
  };

  class DynamicTube {
    public:
      struct Parameters {
        double drive = 0, tube_type = 0, bias = 0, envelope = 0, attack = 5, release = 35, tone = 0;
        double output = 0, dry_wet = 100, hi_quality = 0;
      };
      static constexpr int kHiQualityLatency = 4;
      static int latencyFor(const Parameters& p, double) { return p.hi_quality >= 0.5 ? kHiQualityLatency : 0; }
      void prepare(double sample_rate);
      void setParameters(const Parameters& parameters);
      void process(double& left, double& right);
      int latency() const { return static_cast<int>(lat_); }

    private:
      double shape(double u) const;
      double shapeOversampled(double u, double b, HalfBandState& state) const;
      void setShelf(BiquadCoefficients& c, bool high, double fc, double gdb, double s) const;
      double msCoefficient(double ms) const;

      Parameters p_;
      double srate_ = 44100;
      const double* cur_tab_ = nullptr;
      double dc_b_ = 0, dc_a_ = 0, sm_ = 0;
      bool sm_init_ = false;
      double last_tone_ = -100;
      double envl_ = 0, envr_ = 0;
      double t_gain_ = 0, t_out_ = 0, t_mix_ = 0, t_bias_ = 0, t_env_ = 0;
      double s_gain_ = 0, s_out_ = 0, s_mix_ = 0, s_bias_ = 0, s_env_ = 0;
      double att_c_ = 0, rel_c_ = 0;
      bool use_tone_ = false, os_on_ = false;
      double lat_ = 0;
      BiquadCoefficients tlo_, tloi_, thi_, thii_;
      BiquadState tll_, tlr_, thl_, thr_, tlli_, tlri_, thli_, thri_;
      DcBlocker dl1_, dl2_, dr1_, dr2_;
      HalfBandState st_l_, st_r_;
      std::array<double, 1024> dry_l_ {}, dry_r_ {};
      int dwp_ = 0;
  };

  class Overdrive {
    public:
      struct Parameters { double freq = 1250, width = 6.5, drive = 50, tone = 50, dynamics = 50, dry_wet = 50; };
      // The oversampling filters' 4 samples and half the look-ahead, which depends on the rate.
      static int latencyFor(const Parameters&, double sample_rate);
      void prepare(double sample_rate);
      void setParameters(const Parameters& parameters);
      void process(double& left, double& right);
      int latency() const { return static_cast<int>(lat_); }

    private:
      // A 1st-order allpass section, (c + z^-1) / (1 + c z^-1).
      struct Allpass {
        double x1 = 0, y1 = 0;
        double process(double x, double c) {
          const double y = c * (x - y1) + x1;
          x1 = x; y1 = y;
          return y;
        }
      };
      struct Chain {
        std::array<Allpass, 6> p;
        double chain0(double x);
        double chain1(double x);
      };
      struct Decimator {
        Chain d0, d1;
        double bprev = 0;
        double dec(double even, double odd);
      };
      struct Tilt {
        double x1 = 0, y1 = 0;
      };
      struct Channel {
        Chain up0, up1;
        Decimator gm, gs;
        Tilt tn;
        std::array<double, 64> buf {}, dbuf {};
        int wp = 0;
        double ed = 0, eds = 0, em = 0, m = 0;
      };
      struct Band { double lo = 0, hi = 0; };

      void updateTargets();
      void bandEdges(Band& band, double f, double w) const;
      void highPass(BiquadCoefficients& c, double fc) const;
      void lowPass(BiquadCoefficients& c, double fc) const;
      double rateCoefficient(double c96) const;
      double core(Channel& channel, double u, double dm, double dd);
      void corePair(double ul, double ur);
      double tilt(Tilt& t, double x) const;
      void driveOversampled(double xl, double xr);

      Parameters p_;
      double srate_ = 44100, osr_ = 88200;
      double attM_ = 0, relM_ = 0, relD_ = 0, smoD_ = 0;
      int look_ = 0;
      double lat_ = 0;
      double tone_b_ = 0, tone_a_ = 0, dc_b_ = 0, dc_a_ = 0;
      bool sm_init_ = false;
      double smc_ = 0;
      int coef_cnt_ = 0;
      double t_freq_ = 0, t_width_ = 0, t_gain_ = 0, t_dyn_ = 0, t_tone_ = 0, t_wet_ = 0, t_lfreq_ = 0;
      double s_lfreq_ = 0, s_width_ = 0, s_gain_ = 0, s_dyn_ = 0, s_tone_ = 0, s_wet_ = 0;
      double n0_ = 0, ithr_ = 0, bias_n0_ = 0, gl_ = 0, gh_ = 0, gdry_ = 0, gwet_ = 0;
      double out_l_ = 0, out_r_ = 0;
      Band bnd_;
      BiquadCoefficients hpc_, lpc_;
      BiquadState hl_, ll_, hr_, lr_;
      Channel cl_, cr_;
      DcBlocker dl1_, dl2_, dr1_, dr2_;
      std::array<double, 1024> dry_l_ {}, dry_r_ {};
      int dwp_ = 0;
  };

  class MultibandDynamics {
    public:
      // The script's sliders 1 to 39; its sidechain sliders (40 to 43) are not ported.
      struct BandParameters { double active = 1, solo = 0, above_threshold = -20, above_ratio = 0;
                              double below_threshold = -60, below_ratio = 0, attack = 50, release = 300;
                              double input = 0, output = 0; };
      struct Parameters {
        double split_low_mid = 120, split_mid_high = 2500, low_band = 1, high_band = 1;
        std::array<BandParameters, 3> bands { BandParameters {},
                                              BandParameters { 1, 0, -20, 0, -60, 0, 10, 200, 0, 0 },
                                              BandParameters { 1, 0, -20, 0, -60, 0, 5, 100, 0, 0 } };
        double amount = 100, time = 100, output = 0, peak = 0, knee = 1;
      };
      void prepare(double sample_rate);
      void setParameters(const Parameters& parameters);
      void process(double& left, double& right);
      // The script reports none: its half-band filters are minimum phase.
      static int latencyFor(const Parameters&, double) { return 0; }
      int latency() const { return 0; }

    private:
      struct AllpassChain {
        std::array<double, 4> x {}, y {};
        double process(double input, int first);
      };
      struct Crossover {
        BiquadState l1, l2, h1, h2, m1, m2, k1, k2, ap;
        double o0 = 0, o1 = 0, o2 = 0;
      };
      struct Band {
        bool act = false, solo = false;
        double athr = 0, bthr = 0, ar = 0, br = 0, ca = 0, cr = 0, gin = 1, gout = 1, gsig = 1;
        double env = 0, gdb = 0, g = 0;
      };

      void update();
      void setRbj(BiquadCoefficients& c, int type, double fc, double q) const;
      void split(Crossover& c, double x) const;
      double msCoefficient(double ms) const;
      double gcomp(const Band& band, double level) const;
      void bandGain(Band& band, double dl, double dr) const;
      void bandSetup(Band& band, const BandParameters& p) const;
      void osTick(double xl, double xr);

      Parameters p_;
      double srate_ = 44100, osr_ = 88200;
      bool lo_split_ = true, hi_split_ = true;
      BiquadCoefficients clp1_, chp1_, clp2_, chp2_, cap2_;
      std::array<Band, 3> b_;
      double w0_ = 0, w1_ = 0, w2_ = 0, out_g_ = 1;
      AllpassChain ule_, ulo_, ure_, uro_, dle_, dlo_, dre_, dro_;
      Crossover cl_, cr_;
      double ol_ = 0, or_ = 0, dlo_b_ = 0, dro_b_ = 0;
  };
}
