// SPDX-License-Identifier: GPL-3.0-or-later
// Analogue Spring: Accutronics-style spring tanks behind a Fender 6G15-style
// tube driver and recovery stage.
//
// Each spring is modelled after Valimaki, Parker & Abel ("Parametric Spring
// Reverberation Effect", JAES 2010) as Parker describes it (EURASIP 2011): a
// feedback loop holding a cascade of 100 stretched allpass sections, whose
// group delay grows from about 2 ms at DC to about 80 ms at the spring's
// transition frequency, so every echo is a rising chirp ending near that
// frequency: the drip. The loop's delay sets the echo period, its gain the
// decay, a few pre-echo taps and a slow jitter break up the regularity. The
// springs carry nothing above their transition (about 2.5-5 kHz), so the loops
// run at a reduced sample rate, all springs side by side.
//
// Dwell sets the drive into the tank through the 6G15's bass cut and a
// saturating driver; the recovered signal passes the 6G15 tone network,
// computed from its components. Mono (one pickup, as on a real tank) feeds one
// tank from both inputs and Pan places it; stereo feeds a second, slightly
// different tank from the right input and Pan balances the two. The dry
// signal is never panned.
#pragma once

#include "poly_values.h"

#include <array>
#include <cstdint>
#include <vector>

namespace atlas::analogue::spring {
  enum Tank { kShort, kMedium, kLong, kNumTanks };
  constexpr int kMaxSpringsPerTank = 3;
  constexpr int kMaxLanes = 2 * kMaxSpringsPerTank;
  // Spring lanes are processed in SIMD groups of poly_float::kSize.
  constexpr int kLaneStride = 8;
  constexpr int kGroups = kLaneStride / static_cast<int>(vital::poly_float::kSize);
  static_assert(kLaneStride % vital::poly_float::kSize == 0, "lane groups must fill SIMD vectors");
  constexpr int kSections = 100;

  struct Parameters {
    int tank = kLong;
    double dwell = 0.5;
    double tone = 0.5;
    bool mono = true;
    double pan = 0.0;
    double noise = 0.5;
    double mix = 0.3;
  };

  struct TankSpec {
    int springs;
    std::array<double, kMaxSpringsPerTank> echo_seconds;   // echo period of each spring
    std::array<double, kMaxSpringsPerTank> transition_hz;  // dispersion transition of each spring
    double decay_seconds;                                   // RT60
  };
  const TankSpec& tankSpec(int tank);

  // 6G15 wet tone network response (analog, magnitude) at a frequency, for tests.
  double toneResponse(double tone, double hz);

  class Engine {
   public:
    Engine();
    void prepare(double sample_rate);
    void reset();
    void setParameters(const Parameters& parameters);
    void process(const float* in_left, const float* in_right, float* out_left, float* out_right, int count);

    int decimation() const { return decimation_; }

   private:
    struct Biquad {
      double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0, z1 = 0.0, z2 = 0.0;
      double tick(double x) {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
      }
    };
    // Up to 3rd order IIR, transposed direct form II.
    struct Iir3 {
      std::array<double, 4> b {}, a {};
      std::array<double, 3> z {};
      double tick(double x) {
        const double y = b[0] * x + z[0];
        z[0] = b[1] * x - a[1] * y + z[1];
        z[1] = b[2] * x - a[2] * y + z[2];
        z[2] = b[3] * x - a[3] * y;
        return y;
      }
    };
    // A biquad running both tanks side by side. Double precision: the
    // elliptic filter's sharpest section has a Q near 80.
    struct PairBiquad {
      double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
      double z1[2] = { 0.0, 0.0 }, z2[2] = { 0.0, 0.0 };
      void tick(double* x) {
        for (int i = 0; i < 2; ++i) {
          const double y = b0 * x[i] + z1[i];
          z1[i] = b1 * x[i] - a1 * y + z2[i];
          z2[i] = b2 * x[i] - a2 * y;
          x[i] = y;
        }
      }
      void clear() { z1[0] = z1[1] = z2[0] = z2[1] = 0.0; }
    };
    static constexpr int kEllipticSections = 4;
    struct TankIo {
      double dc_state = 0.0;
      Iir3 tone;
      double held = 0.0;
    };

    void configureTank(int tank);
    void clearSprings();
    void clearLanes(int first, int last);
    void configureTone(double tone);
    void runSprings(const std::array<double, 2>& inputs, std::array<double, 2>& outputs, int lanes);
    void runChain(vital::poly_float* chain, int groups);
    double drive(double x) const;
    double random();

    Parameters parameters_;
    double sample_rate_ = 48000.0, reduced_rate_ = 16000.0;
    int decimation_ = 3;
    int phase_ = 0;

    // Spring lanes: tank A springs first, then tank B's. Allpass history is a
    // ring of 8 per section; delay lines are interleaved by lane.
    int configured_tank_ = -1, springs_ = 2, history_offset_ = 1;
    std::array<float, kLaneStride> fraction_coefficient_ {}, allpass_coefficient_ {};
    std::vector<vital::poly_float> history_, previous_;
    std::vector<float> lines_;
    int line_length_ = 1, line_write_ = 0, history_index_ = 0;
    std::array<double, kMaxLanes> loop_gain_ {}, main_delay_ {}, modulation_ {}, tap_gain_ {};
    std::array<int, kMaxLanes> echo_tap_ {}, ripple_tap_ {}, base_tap_ {};
    std::array<double, kMaxLanes> feedback_ {};
    // The allpass fractional delay of each loop's drift.
    std::array<double, kMaxLanes> allpass_input_ {}, allpass_output_ {};
    // Per-sample loss of every delay element in the loops (see configureTank).
    float decay_per_sample_ = 1.0f, history_loss_ = 1.0f, previous_loss_ = 1.0f;
    std::array<float, kMaxLanes> output_ {};

    std::array<TankIo, 2> io_ {};
    std::array<PairBiquad, kEllipticSections> decimator_ {}, interpolator_ {};
    PairBiquad driver_highpass_;
    double tone_ = -1.0, dwell_ = 0.5, pan_ = 0.0, noise_ = 0.5, mix_ = 0.3;
    // Output gains: mono pan (left, right) and stereo balance (left, right),
    // computed per block and interpolated across it.
    std::array<double, 4> pan_gains_ { 0.70710678, 0.70710678, 1.0, 1.0 };
    double stereo_ = 0.0;          // smoothed 0 mono .. 1 stereo
    bool tank_b_live_ = false;     // tank B has run since it was last cleared
    double switch_gain_ = 1.0;     // fades out and back in around a tank change
    int pending_tank_ = -1;
    double smoothing_ = 0.01, switch_step_ = 0.01, modulation_coefficient_ = 0.07, modulation_depth_ = 1.0;
    double dc_coefficient_ = 0.01, wet_gain_ = 1.0;
    std::uint32_t random_ = 0x5eed1234u;
  };
}
