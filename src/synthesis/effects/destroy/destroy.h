// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>

// Native port of Alessio Scala's Serum 2 Overdrive JSFX, the Destroy effect
// (README.atlas.md). The class is the script's @init, @slider and @sample
// sections statement for statement, in double precision as JSFX computes,
// including the places EEL2 flushes an assigned value to zero. All eight
// sliders are controls. Built outside the fast-math unity file so the
// arithmetic stays as written.
//
// The one addition is a DC blocker on the wet signal, which Atlas always runs:
// the script's last clipper is symmetric, so asymmetric material leaves an
// offset on the wet signal that only the next stage's high-pass would remove.
// With remove_dc off the class is the script alone, bit for bit.
namespace atlas::destroy {
  // What differs between the EEL2 that REAPER runs and WDL's portable EEL2
  // (EEL_TARGET_PORTABLE), which the reference test runs. Atlas uses reaper().
  struct Eel2Build {
    // An assignment flushes values whose exponent field is at most this to
    // zero (and inf and NaN): REAPER's x64 and arm64 code flush the smallest
    // normal binade too, the portable interpreter only denormals.
    int flush_exponent = 1;

    static Eel2Build reaper() { return {}; }
  };

  class Destroy {
    public:
      // The script's slider values: Drive, Filter LP-HP and Mix in percent,
      // Stack 1 to 16, Filter 0 (Off), 1 (Pre) or 2 (Post), Filter Freq and
      // Level 0 to 1, Filter Q 0.05 to 7.6.
      struct Parameters {
        double drive = 25, stack = 1, filter = 0, frequency = 0.5, q = 1.9, morph = 0, mix = 100, level = 0.5;
      };

      // pdc_delay: the script delays only its dry path by it.
      static constexpr int kLatency = 7;
      static int latencyFor(const Parameters&, double) { return kLatency; }
      // The wet signal's DC blocker: a first-order high-pass at this frequency.
      static constexpr double kDcBlockerHz = 5.0;

      explicit Destroy(Eel2Build build = Eel2Build::reaper(), bool remove_dc = true) :
          build_(build), remove_dc_(remove_dc) { }

      // @init at this rate.
      void prepare(double sample_rate);
      // @slider.
      void setParameters(const Parameters& parameters);
      const Parameters& parameters() const { return parameters_; }
      // @sample on one stereo frame.
      void process(double& left, double& right);

    private:
      static constexpr int kMaxStages = 16;

      // A biquad's coefficients, as hp_set leaves them in its namespace.
      struct HighPass { double b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0; };
      // One channel of one stage: the two high-passes' states, and on the left
      // channel the shared gain control's envelope (the script's NST values 0 to 4).
      struct StageState { double s[5] {}; };
      // The SVF's s[0] lp, s[1] bp, s[2] lp of the previous sample, s[3] previous hp average.
      struct SvfState { double s[4] {}; };
      struct DcBlocker { double x1 = 0, y1 = 0; };

      double flush(double value) const;
      void highPass(HighPass& filter, double fc) const;
      double knee(double w) const;
      double qDamping(double q) const;
      void svfCoefficients(double fnorm, double q, double morph);
      double svf(double x, SvfState& state) const;
      void setParams();
      void stage(StageState& left, StageState& right);
      void wet();
      double blockDc(double x, DcBlocker& state) const;

      Eel2Build build_;
      bool remove_dc_;
      Parameters parameters_;
      double srate_ = 48000;

      // @init's results.
      HighPass hp_a_, hp_b_;
      double agc_att_ = 0, agc_rel_ = 0, knee_ = 0, knee_w_ = 0;
      double dc_coefficient_ = 0, dc_gain_ = 0;

      // @slider's results.
      double g_stage_ = 0, agc_t_ = 0, svf_f_ = 0, svf_k_ = 0, svf_kl_ = 0, svf_kb_ = 0, svf_kh_ = 0;
      double mix_s_ = 0, out_g_ = 0, dry_g_ = 0, fmode_ = 0;
      int nstages_ = 1;

      // State: the dry delay lines (DRY_L, DRY_R), the stages (STATE), the
      // filter (SVF_L, SVF_R), the wet path's frame (stg_l, stg_r) and the silence count.
      std::array<double, 8> dry_l_ {}, dry_r_ {};
      std::array<StageState, kMaxStages> stage_l_ {}, stage_r_ {};
      SvfState svf_l_, svf_r_;
      DcBlocker dc_l_, dc_r_;
      double stg_l_ = 0, stg_r_ = 0, silent_run_ = 0;
      int dwp_ = 0;
  };
}
