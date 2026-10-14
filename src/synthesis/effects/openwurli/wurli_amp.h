// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from OpenWurli 0.9.0, crates/openwurli-dsp/src/engine.rs (the shared
// signal chain of WurliEngine) and tables.rs (the volume network and output
// mapping) (GPL-3.0-or-later).
#pragma once

#include "wurli_oversampler.h"
#include "wurli_power_amp.h"
#include "wurli_preamp.h"
#include "wurli_speaker.h"
#include "wurli_tremolo.h"

#include <cstdint>

namespace openwurli {
  // The drawn network between the preamp and the power amp (#203720-S-3):
  // preamp out (R-9, 6.8K) -> R-11 "reed bar volume" trimmer -> the 10K
  // volume pot to ground -> wiper -> power amp input (R-27, 15K), with C-9
  // (1 nF) at the amp input. The pot taper and the R-11 setting are
  // OpenWurli's assumptions pending bench readings; see tables.rs.
  struct VolumeNetwork {
    // R-11 setting, transferred from the non-A Model 200 factory calibration.
    static constexpr double kR11ReedBarVolumeDefault = 17'200.0;
    static constexpr double kR11ReedBarVolumeMax = 25'000.0;
    static constexpr double kVolumePotR = 10'000.0;
    static constexpr double kPreampOutputR9 = 6'800.0;
    static constexpr double kPowerAmpRIn = 15'000.0;
    static constexpr double kPowerAmpC9 = 1.0e-9;

    // Volts at the power amp output that map to full scale: the rail.
    static constexpr double kFullScaleVolts = kHeadroomV;
    static constexpr double kPostSpeakerGainDb = 0.0;
    // The amp's normalized output (+-1 = +-kHeadroomV) referred to kFullScaleVolts.
    static constexpr double kPostSpeakerGain = kHeadroomV / kFullScaleVolts;

    // Wiper-to-ground fraction at rotation `pos` (0..1): the standard "15 % at
    // center" two-slope audio taper.
    static double audioTaper(double pos);
    // Gain from the preamp's open-circuit output to the power amp input, for
    // pot position `volume` and R-11 = `r11` ohms (0.143 at 0.8, 0.200 at 1.0
    // with the default trim).
    static double potGain(double volume, double r11);
    // Corner of the C-9 pole against the wiper's Thevenin resistance (~35 kHz
    // at full volume, higher below; infinite with the wiper at ground).
    static double potPoleHz(double volume, double r11);
  };

  // The 200A's shared chain after the reeds, as WurliEngine runs it (engine.rs):
  // 2x oversampling (off at 88.2 kHz and up) around the preamp, with the
  // tremolo's LDR in its feedback loop, the volume network and the power amp;
  // then the speaker at the base rate.
  //
  // Input: the summed pickup voltage (volts). Output: the power amp's rail
  // mapped to full scale (kPostSpeakerGain), i.e. what WurliEngine::render
  // writes, kept in double, before the OpenWurli plugin's +14 dB output
  // alignment.
  //
  // Holds no heap memory and no pointers: copying gives an identical chain.
  class AmpChain {
    public:
      static constexpr double kDefaultVolume = 0.8;
      static constexpr double kDefaultTremoloDepth = 0.5;
      static constexpr double kDefaultSpeakerCharacter = 0.0;

      AmpChain();

      // WurliEngine::set_sample_rate: rebuilds every stage for this rate and
      // warms up on 0.6 s of silence. The smoothers first snap to the values
      // set so far, so the warm-up runs at the current settings. Slow (it also
      // settles the tremolo oscillator for 2 s of samples); allocation-free.
      // A rate that is not finite and positive leaves the chain unprepared.
      void prepare(double sample_rate);
      // Restores the state saved at the end of prepare() (a copy, no warm-up)
      // and snaps the smoothers to their targets. Allocation-free and cheap.
      void reset();
      // WurliEngine::reset: resets every stage, snaps the smoothers and warms
      // up again. Slow; for tests and offline use.
      void resetAndWarmUp();

      // Ramped over 5 ms, as WurliEngine does.
      void setVolume(double volume);
      void setTremoloDepth(double depth);
      void setSpeakerCharacter(double character);

      // Any num_samples >= 0; `input` and `output` may be the same buffer.
      // The result does not depend on how a signal is split into calls. A
      // non-finite input sample is read as 0, and a non-finite output (as a
      // float) resets the preamp, the C-9 pole, the resampler, the power amp
      // and the speaker and gives 0, as WurliEngine's NaN guards do. Writes
      // zeros before prepare().
      void process(const double* input, double* output, int num_samples);

      // The rate prepare() was last given (0 before the first call).
      double sampleRate() const { return sample_rate_; }

    private:
      // engine.rs LinearSmoother: a linear ramp over `ramp_samples` base-rate
      // samples to each new target.
      class LinearSmoother {
        public:
          LinearSmoother(double initial, uint32_t ramp_samples);

          void setTarget(double target);
          void snapTo(double value);
          // Keeps an in-flight ramp, re-timed to the new length.
          void setRampSamples(uint32_t ramp_samples);
          double target() const { return target_; }

          double next() {
            if (samples_remaining_ > 0) {
              current_ += step_;
              --samples_remaining_;
              if (samples_remaining_ == 0)
                current_ = target_;
            }
            return current_;
          }

        private:
          double current_;
          double target_;
          double step_ = 0.0;
          uint32_t samples_remaining_ = 0;
          uint32_t ramp_samples_;
      };

      // Everything prepare() builds and process() advances; reset() restores
      // a copy taken after the warm-up.
      struct Stages {
        DkPreamp preamp;
        Tremolo tremolo;
        Oversampler oversampler;
        PowerAmp power_amp;
        Speaker speaker;
        // The C-9 pole at the amp input: one-pole state and coefficient.
        double c9_state = 0.0;
        double c9_alpha = 1.0;
        // volume -> (drive gain, C-9 coefficient), recomputed only when the
        // smoothed volume changes (bitwise); the functions are pure.
        bool volume_cached = false;
        uint64_t cached_volume_bits = 0;
        double drive_gain = 0.0;
      };

      double processSample(double input);
      void updateVolumeNetwork(double volume);
      void warmUp();
      void snapSmoothers();

      Stages stages_;
      Stages prepared_;
      LinearSmoother volume_;
      LinearSmoother tremolo_depth_;
      LinearSmoother speaker_character_;
      double sample_rate_ = 0.0;
      double os_sample_rate_ = 0.0;
      bool oversample_ = true;
      bool prepared_valid_ = false;
  };
} // namespace openwurli
