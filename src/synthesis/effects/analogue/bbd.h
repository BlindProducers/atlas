// SPDX-License-Identifier: GPL-3.0-or-later
// Bucket-brigade delay line with its anti-aliasing and reconstruction filters,
// after Holters & Parker, "A Combined Model for a Bucket Brigade Device and
// its Input and Output Filters" (DAFx-18).
//
// The input filter's continuous-time response is evaluated at each BBD clock
// instant, the bucket chain holds one value per clock period, and the output
// filter integrates the held staircase exactly. The clock is independent of
// the audio rate, so bandwidth, aliasing, imaging and clock leakage follow the
// delay time as they do in hardware: a slower clock gives a longer, darker,
// grainier delay.
//
// An N-stage two-phase BBD delays by N / (2 f_clock) and stores N / 2 values.
// The fractional-timing weights e^{p d T} are read from per-term tables over
// d in [0, 1] with linear interpolation (as in jpcima's bbd-delay), so audio
// samples without a clock tick cost only the filters' own recursions.
#pragma once

#include "analog_filter.h"

#include <cstdint>
#include <vector>

namespace atlas::analogue {
  struct BbdChip {
    int stages = 1024;
    // Signal level (relative to the digital input) where the chip's output
    // stage starts to compress. 1.0 = 0 dBFS.
    double headroom = 1.0;
    // Second-order distortion coefficient at the headroom level.
    double even_distortion = 0.0;
    // Output-referred noise density, as RMS of the held value per tick.
    double noise = 0.0;
    // Clock feedthrough amplitude before the output filter.
    double clock_leakage = 0.0;
    // Charge left behind per transfer. Over N stages it acts as a one-pole
    // low pass at the clock rate, so longer chips lose more top end.
    double transfer_inefficiency = 2.5e-5;
    // When nonzero, clock ticks beyond this many per audio sample are processed
    // in groups of buckets: one input sample written to, and one output read
    // as the mean of, a power-of-two run of buckets, with the noise scaled to
    // keep its density. The FIFO keeps full resolution, so delay changes stay
    // seamless; in band this acts like a BBD clocked at the group rate.
    double max_events_per_sample = 0.0;
  };

  // Per-tick noise RMS that gives a chip's datasheet signal-to-noise ratio
  // (re a full-scale sine) through the given output filter at a clock rate.
  // Noise injected per bucket has a density proportional to 1 / clock, so a
  // slower clock is noisier, as on the datasheet curves.
  double noiseForSignalToNoise(double signal_to_noise_db, const AnalogFilter& output_filter, double clock_hz);

  class BbdLine {
   public:
    // Audio samples between updates of the clock-leakage level; callers
    // evaluating LFOs need not set the delay more often than this.
    static constexpr int kRefreshInterval = 16;
    static constexpr int kTableSize = 128;

    void configure(const BbdChip& chip, const AnalogFilter& input_filter,
                   const AnalogFilter& output_filter, double sample_rate);
    void reset();

    // Target delay in seconds; takes effect at the next sample.
    void setDelay(double seconds);
    double delay() const { return 0.5 * chip_.stages / clock_; }
    double clockHz() const { return clock_; }
    // Multiplies the chip's noise and clock leakage (Age / Noise controls).
    void setNoiseScale(double noise, double leakage) { noise_scale_ = noise; leakage_scale_ = leakage; }

    double process(double input);

   private:
    struct Term {
      Complex pole_step;        // e^{pT}
      Complex state;
    };

    void refresh();
    void updateGroup();
    double tick(double delta);
    double randomGaussian();

    BbdChip chip_;
    AnalogFilter input_filter_, output_filter_;
    std::vector<Term> input_terms_, output_terms_;
    // Per term, kTableSize + 1 weights over d in [0, 1], conjugate-pair
    // doubling folded in. Input: T r e^{p d T}. Output: (r / p) e^{p (1 - d) T}.
    std::vector<Complex> input_table_, output_table_;
    std::vector<double> buckets_;
    double sample_rate_ = 48000.0, period_ = 1.0 / 48000.0;
    double clock_ = 1.0e4, target_clock_ = 1.0e4;
    double samples_per_tick_ = 1.0;      // P: samples between ticks
    double next_tick_ = 0.0;             // position of the next tick, in samples from the previous sample
    double held_ = 0.0, output_dc_gain_ = 1.0;
    double transfer_coefficient_ = 0.0, transfer_state_ = 0.0;
    double group_transfer_coefficient_ = 0.0, group_noise_ = 1.0;
    int group_ = 1, bucket_mask_ = 0;
    double leakage_gain_ = 0.0, clock_phase_ = 0.0;
    double noise_scale_ = 1.0, leakage_scale_ = 1.0;
    int write_ = 0, refresh_counter_ = 0;
    uint32_t random_ = 0x6d2b79f5u;
  };
}
