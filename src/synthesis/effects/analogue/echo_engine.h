// SPDX-License-Identifier: GPL-3.0-or-later
// Analogue Echo: a Space Echo-style tape loop or a Memory Man-style BBD echo.
//
// Tape: one record head and three playback heads. A motor moves the tape; the
// heads sit at fixed distances, so changing the repeat time changes the tape
// speed and every echo already on the tape bends in pitch. The model stores
// each recorded sample with the tape position it was written at and reads a
// head where the tape now passing it was recorded (Zavalishin & Parker,
// DAFx-18), so pitch follows any speed trajectory. Free heads sit at the
// RE-201's measured 1 : 1.90 : 2.75; synced heads at exactly 1 : 2 : 3.
// Playback losses follow the tape speed, so slower (longer) echoes are darker.
//
// BBD: 8192 stages (a Deluxe Memory Man's two MN3005s) between NE570
// compander halves; the clock follows the delay, so long delays are dark and
// grainy and whine at the clock. Width runs two lines with independently
// wobbling clocks.
//
// Both: Drive (the Tape effect's saturation) and the Bass / Treble shelves sit
// inside the feedback loop; above about 100% Feedback the loop self-oscillates
// and the saturation bounds it. Inertia slews the motor or clock: slowing
// takes 1.89 s and speeding 1.07 s at 50%, as measured on an RE-201.
#pragma once

#include "bbd.h"
#include "compander.h"

#include <array>
#include <cstdint>
#include <vector>

namespace atlas::analogue::echo {
  enum Medium { kTape, kBbd, kNumMedia };
  constexpr int kNumHeads = 3;

  struct Parameters {
    int medium = kTape;
    bool synced = true;
    double time = 0.25;            // head 1 / BBD delay in seconds (tempo resolved)
    std::array<bool, kNumHeads> heads { true, false, true };
    std::array<double, kNumHeads> pans { -1.0, 1.0, 0.0 };
    double width = 0.5;            // BBD only
    double feedback = 0.4;         // 0 - 1.2
    double inertia = 0.5;          // 0.5 = RE-201 motor
    double drive = 0.3;
    double bass_db = 0.0, treble_db = 0.0;
    double wow = 0.25, flutter = 0.25;
    double noise = 0.5;            // 0.5 = original level
    double age = 0.2;
    double mix = 0.3;
  };

  // Head 1 / BBD delay limits in seconds.
  struct Range { double min_delay, max_delay; };
  Range tapeRange();
  Range bbdRange();
  // Head spacing relative to head 1, free and synced.
  std::array<double, kNumHeads> headRatios(bool synced);

  class Engine {
   public:
    Engine();
    void prepare(double sample_rate);
    void reset();
    void setParameters(const Parameters& parameters);
    void process(const float* in_left, const float* in_right, float* out_left, float* out_right, int count);

   private:
    struct OnePole {
      double state = 0.0;
      double lowpass(double x, double coefficient) { return state += (x - state) * coefficient; }
    };
    struct Biquad {
      double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0, z1 = 0.0, z2 = 0.0;
      double tick(double x) {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
      }
    };
    struct Tone {
      Biquad bass, treble;
      double tick(double x) { return treble.tick(bass.tick(x)); }
    };
    struct Head {
      std::uint64_t read = 0;      // history index at or before the head's target
      OnePole loss1, loss2;        // speed-dependent playback loss
      double gain = 0.0;           // on/off ramp
      double contact = 1.0;        // Age: slowly uneven tape contact
      double contact_target = 1.0;
      int dropout_wait = 1, dropout_remaining = 0, dropout_length = 1;
    };

    void configureTone(double bass_db, double treble_db);
    double random();
    double gaussian();
    double tapeRead(std::uint64_t index, double fraction) const;
    double locate(Head& head, double target);
    // Input gains ramp linearly from start to end over the block.
    void renderTape(const float* mono, float* wet_left, float* wet_right, int count, double gain_start, double gain_end);
    void renderBbd(const float* mono, float* wet_left, float* wet_right, int count, double gain_start, double gain_end);
    void clearMedium(int medium);
    double inertiaCoefficient(double target, double current) const;
    double saturate(double x) const;

    Parameters parameters_;
    double sample_rate_ = 48000.0;

    // Tape loop: recorded audio and the tape position it was recorded at.
    std::vector<float> tape_audio_;
    std::vector<double> tape_position_;
    std::uint64_t write_ = 0;
    std::uint64_t valid_from_ = 0;  // older history reads as blank tape
    double position_ = 0.0;
    double speed_ = 1.0;           // tape cells per sample (1 = head 1 at 100 ms)
    std::array<Head, kNumHeads> heads_ {};
    std::array<double, kNumHeads> ratio_ {};
    double tape_feedback_ = 0.0;
    double feedback_select_ = 2.0; // smoothed index of the furthest active head
    OnePole record_highpass_;
    double wow_phase_ = 0.0, flutter_phase_ = 0.0, drift_ = 0.0;
    OnePole drift_filter_;

    // BBD echo.
    std::array<BbdLine, 2> lines_;
    Compander compressor_;
    std::array<Compander, 2> expanders_;
    std::array<double, 2> bbd_delay_ {};
    double bbd_rate_ = 4.0;        // smoothed 1 / delay
    double bbd_feedback_ = 0.0;
    std::array<double, 2> bbd_wow_phase_ {}, bbd_flutter_phase_ {};
    std::array<OnePole, 2> bbd_drift_ {};
    OnePole bbd_highpass_;
    int control_countdown_ = 0;

    std::array<Tone, 3> tone_;     // left wet, right wet, feedback
    double tone_bass_ = 1.0e9, tone_treble_ = 1.0e9;

    double feedback_ = 0.4, drive_ = 0.3, mix_ = 0.3, width_ = 0.5;
    double wow_ = 0.25, flutter_ = 0.25, noise_ = 0.5, age_ = 0.2;
    std::array<double, kNumHeads> pan_ {};
    double smoothing_ = 0.01;
    int current_medium_ = kTape, fading_medium_ = -1, fade_remaining_ = 0, fade_length_ = 1;
    std::uint32_t random_ = 0x2545f491u;
    bool primed_ = false;
  };
}
