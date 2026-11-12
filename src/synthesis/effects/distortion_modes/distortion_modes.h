// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>

// Native port of eight modes of Alessio Scala's distortion JSFX
// (README.atlas.md): Tube, Diode 1, Diode 2, Zero-Square, Asym, Stomp Box,
// Tape Sat and Soft Sat. The class is the script's @init, @slider,
// @block and @sample sections for those modes, statement for statement, in
// double precision as JSFX computes, including the places EEL2 flushes an
// assigned value to zero. Built outside the fast-math unity file so the
// arithmetic stays as written.
//
// @sample is split in two so Atlas's own filter can sit before or after the
// shaper: wet() is the shaper and output() the dry delay, Mix and Level.
// process() runs both, which is the script with its filter off.
//
// Atlas doubles the wet signal before the mix (unity_wet), since the
// script's wet path plays the input at half its level at Drive 0. With
// unity_wet off the class is the script alone, bit for bit.
namespace atlas::distortion_modes {
  // What differs between the EEL2 that REAPER runs and WDL's portable EEL2
  // (EEL_TARGET_PORTABLE), which the reference test runs. Atlas uses reaper().
  struct Eel2Build {
    // An assignment flushes values whose exponent field is at most this to
    // zero (and inf and NaN): REAPER's x64 and arm64 code flush the smallest
    // normal binade too, the portable interpreter only denormals.
    int flush_exponent = 1;

    static Eel2Build reaper() { return {}; }
  };

  // The script's Mode numbers of the ported modes.
  enum Mode {
    kTube = 0,
    kDiode1 = 3,
    kDiode2 = 4,
    kZeroSquare = 7,
    kAsym = 9,
    kStompBox = 14,
    kTapeSat = 15,
    kSoftSat = 17
  };

  class Engine {
    public:
      // The script's slider values: Mode (one of Mode), Drive and Mix in
      // percent, Level 0 to 1.
      struct Parameters {
        double mode = kTube, drive = 25, mix = 100, level = 0.5;

        bool operator==(const Parameters& other) const {
          return mode == other.mode && drive == other.drive && mix == other.mix && level == other.level;
        }
        bool operator!=(const Parameters& other) const { return !(*this == other); }
      };

      // pdc_delay: the script delays only its dry path by it.
      static constexpr int kLatency = 7;

      explicit Engine(Eel2Build build = Eel2Build::reaper(), bool unity_wet = true);

      // @init at this rate.
      void prepare(double sample_rate);
      // @slider.
      void setParameters(const Parameters& parameters);
      const Parameters& parameters() const { return parameters_; }
      // @block for a block of this many samples: starts a Mode crossfade and
      // moves Drive along its glide.
      void block(int samples);

      // The first half of @sample: silence detection and the wet path.
      // spl0 and spl1 are the input, pre_l and pre_r what the shaper gets
      // (the input, or a pre filter's output).
      void wet(double spl0, double spl1, double pre_l, double pre_r, double& wl, double& wr);
      // The second half: the dry delay, Mix, Level and the output clip.
      void output(double spl0, double spl1, double wl, double wr, double& out_l, double& out_r);
      // All of @sample on one stereo frame.
      void process(double& left, double& right) {
        double wl, wr;
        wet(left, right, left, right, wl, wr);
        output(left, right, wl, wr, left, right);
      }

    private:
      // The script's BANK: oversampler L [0, 32) and R [32, 64), DC blocker
      // [64, 68), wet delay lines L [68, 84) and R [84, 100).
      using Bank = std::array<double, 100>;

      double flush(double value) const;
      double cr(const double* t, int n, double f) const;
      void setDrive(double D);
      void setParams();
      double shape(double x, int m) const;
      void up2(double v, double* s, double& up_a, double& up_b) const;
      double down2(double e, double o, double* s) const;
      double osShape(double x, double* s, int m) const;
      void modeWet(int mm, int b, double pre_l, double pre_r, double& w_l, double& w_r);

      Eel2Build build_;
      // 2 with unity_wet, else 1.
      double wet_gain_;
      // The Asym and Zero-Square curves (T_ASYM, T_ZSQ), shared by every engine.
      const double* asym_ = nullptr;
      const double* zsq_ = nullptr;
      Parameters parameters_;
      double srate_ = 48000;

      // @init's results.
      double dcb_p_ = 0, smooth_ = 0, xf_step_ = 0;

      // set_drive's results.
      double tube_a_ = 0, ssat_g_ = 0, tape_g_ = 0, tape_x_ = 0, tape_w_ = 0, stomp_k_ = 0, stomp_x_ = 0;
      double stomp_s_ = 0, d1_m_ = 0, d1_al_ = 0, d2_g_ = 0, zsq_off_ = 0, asym_g_ = 0;
      bool tape_on_ = false;

      // @slider's results.
      int tm_ = kTube;
      double mix_t_ = 0, lvl_t_ = 0;

      // State.
      std::array<Bank, 2> banks_ {};
      std::array<double, 8> dry_l_ {}, dry_r_ {};
      double drive_s_ = 0, mix_s_ = 0, lvl_g_ = 0, xf_ = 1, silent_run_ = 0;
      int cur_m_ = kTube, old_m_ = kTube, cur_b_ = 0, dwp_ = 0, wwp_ = 0;
  };
}
