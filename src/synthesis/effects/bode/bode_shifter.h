// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <memory>

// Native port of Alessio Scala's Serum 2 Bode JSFX, the Frequency Shifter's
// Mode 2 (README.atlas.md). The class is the script's @init, @slider and
// @sample sections statement for statement, in double precision as JSFX
// computes, including the places EEL2 flushes an assigned value to zero. Only
// nine sliders are controls; the others keep their defaults. Built outside
// the fast-math unity file so the arithmetic stays as written.
namespace atlas::bode {
  // What differs between the EEL2 that REAPER runs and WDL's portable EEL2
  // (EEL_TARGET_PORTABLE), which the reference test runs. Atlas uses kReaper.
  struct Eel2Build {
    // An assignment flushes values whose exponent field is at most this to
    // zero (and inf and NaN): REAPER's x64 and arm64 code flush the smallest
    // normal binade too, the portable interpreter only denormals.
    int flush_exponent = 1;
    // `x % 2147483648` divides by this. REAPER converts both operands to
    // 64-bit integers; the portable interpreter converts the divisor to int,
    // which arm64 saturates to 2147483647.
    int64_t mod_2_31 = 2147483648LL;

    static Eel2Build reaper() { return {}; }
  };

  class Shifter {
    public:
      // The script's slider values for the nine sliders Atlas exposes:
      // Shift, Dir, Width, Feed, Blur and Mix in percent, Range in Hz, Delay
      // in seconds, Mono Input 0 or 1.
      struct Parameters {
        double shift = 0, range = 22.36, direction = 0, width = 100, mono_input = 0;
        double delay = 0, feedback = 0, blur = 0, mix = 100;
        bool operator==(const Parameters& other) const {
          return shift == other.shift && range == other.range && direction == other.direction &&
                 width == other.width && mono_input == other.mono_input && delay == other.delay &&
                 feedback == other.feedback && blur == other.blur && mix == other.mix;
        }
        bool operator!=(const Parameters& other) const { return !(*this == other); }
      };

      // pdc_delay: the script delays only its dry path by it.
      static constexpr int kLatency = 11;

      explicit Shifter(Eel2Build build = Eel2Build::reaper()) : build_(build) {}

      // @init at this rate. Allocates the 5 s feedback line, so call it off
      // the audio thread.
      void prepare(double sample_rate);
      bool prepared() const { return feedback_line_ != nullptr; }
      // @init again at the prepared rate, without allocating.
      void reset();
      // @slider.
      void setParameters(const Parameters& parameters);
      const Parameters& parameters() const { return parameters_; }
      // @sample on one stereo frame.
      void process(double& left, double& right);

    private:
      static constexpr int kSections = 22;
      static constexpr int kBlurTaps = 24;
      static constexpr int kBlurSize = 16384;

      double flush(double value) const;
      double freqRatio(double fa) const;
      void setOscillator();
      void setDelay();
      void setBlur();
      void setParams();

      Eel2Build build_;
      Parameters parameters_;
      double srate_ = 48000;

      // Allpass states (SI_L, SI_R, SQ_L, SQ_R), the chain inputs' delay
      // lines (INB_L, INB_R) and the dry delay lines (DRY_L, DRY_R).
      double si_l_[kSections] {}, si_r_[kSections] {}, sq_l_[kSections] {}, sq_r_[kSections] {};
      double inb_l_[8] {}, inb_r_[8] {};
      double dry_l_[32] {}, dry_r_[32] {};
      // Blur taps (BLK_L, BLK_R) and their signs (BLS_L, BLS_R).
      double blk_l_[kBlurTaps] {}, blk_r_[kBlurTaps] {}, bls_l_[kBlurTaps] {}, bls_r_[kBlurTaps] {};
      // BLUR_L and BLUR_R, kBlurSize each.
      std::unique_ptr<double[]> blur_;
      // FB, feedback_length_ samples. Only its first feedback_written_
      // samples have been written since @init; the rest read as zero.
      std::unique_ptr<double[]> feedback_line_;
      int feedback_length_ = 0, feedback_written_ = 0;

      // @slider's results.
      double w96_ = 0, dl_fr_ = 0, g_fb_ = 0, blur_g_ = 0;
      double bal_a_ = 0, bal_b_ = 0, dirv_ = 0, wdt_ = 0, gd_ = 0, gw_ = 0, glev_ = 0;
      int dl_ip_ = 0;
      bool blur_on_ = false, mono_ = false, swp_ = false;

      // The modulator and the write positions.
      double mc_ = 1, ms_ = 0;
      int quad_ = 0, dp_ = 0, dwp_ = 0, fwp_ = 0, bwp_ = 0;
  };
}
