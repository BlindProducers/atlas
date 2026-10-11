// SPDX-License-Identifier: GPL-3.0-or-later
// Analogue Flanger: a BBD flanger with regeneration, or two-machine tape
// flanging that sweeps through zero.
//
// BBD: the mono-summed input (plus feedback) feeds two BBD lines whose LFOs
// are offset by Stereo Phase. The bucket soft clip bounds the regeneration.
//
// Tape: one tape loop is read by a reference head at a fixed delay and by a
// moving head swept around it, both with the same tape coloration. Where the
// two coincide the comb collapses: positive Feedback sums them in phase (the
// classic reinforcing "jet"); negative Feedback inverts the moving machine so
// the zero point cancels. Tape-mode Mix at 100% is the classic two-machine
// sound; the dry input is never delayed, so 0% still nulls.
#pragma once

#include "bbd.h"

#include <array>
#include <cstdint>
#include <vector>

namespace atlas::analogue::flanger {
  enum Medium { kBbd, kTape, kNumMedia };

  struct Parameters {
    int medium = kBbd;
    double lfo_frequency = 0.25;  // Hz, already resolved from free rate or tempo
    double depth = 0.5;           // 0-1 sweep width
    double manual = 0.5;          // 0-1 sweep centre / static position
    double feedback = 0.5;        // -1 to 1
    double stereo_phase = 0.5;    // 0-1 = 0-180 degrees
    double noise = 0.5;           // 0.5 = original level
    double age = 0.0;
    double mix = 0.5;
  };

  // Delay range of each medium, in seconds, for tests and documentation.
  struct Range { double min_delay, max_delay; };
  Range bbdRange();
  Range tapeRange();       // moving head, relative to the reference head
  double tapeReferenceDelay();

  class Engine {
   public:
    Engine();
    void prepare(double sample_rate);
    void reset();
    void setParameters(const Parameters& parameters);
    // Sets the LFO phase in cycles (tempo sync anchors it to the timeline).
    void alignLfo(double cycles);
    void process(const float* in_left, const float* in_right, float* out_left, float* out_right, int count);

   private:
    void renderBbd(const float* mono, float* wet_left, float* wet_right, int count);
    void renderTape(const float* mono, float* wet_left, float* wet_right, int count);
    double readTape(double delay_samples) const;
    double noiseSample();

    Parameters parameters_;
    double sample_rate_ = 48000.0;
    std::array<BbdLine, 2> lines_;
    std::array<double, 2> bbd_feedback_ {}, dc_block_ {};
    std::vector<float> tape_;
    int tape_write_ = 0;
    std::array<double, 3> playback_lowpass_ {};  // reference, moving left, moving right
    double tape_feedback_ = 0.0, flutter_phase_ = 0.0, wow_phase_ = 0.0;
    double lfo_phase_ = 0.0;
    double depth_ = 0.5, manual_ = 0.5, feedback_ = 0.5, stereo_phase_ = 0.5;
    double noise_ = 0.5, age_ = 0.0, mix_ = 0.5;
    double smoothing_ = 0.01, dc_coefficient_ = 0.0;
    int current_medium_ = kBbd, fading_medium_ = -1, fade_remaining_ = 0, fade_length_ = 1;
    int fade_wait_ = 0, prime_length_ = 1;
    uint32_t random_ = 0x9e3779b9u;
  };
}
