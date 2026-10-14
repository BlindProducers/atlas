// SPDX-License-Identifier: GPL-3.0-or-later
// OpenB3's rotary speaker (setBfree's Beatrix whirl.c and eqcomp.c): a horn
// and a drum turning in front of two microphones. The horn's feed passes a
// 4.5 kHz low pass and a 300 Hz low shelf, and part of it leaks out unturned;
// the drum's output passes a high shelf that leaves its bass. Each rotor
// reaches each microphone along a direct path and two reflections, through a
// Doppler delay that follows its distance and, for the horn, an
// angle-dependent response (Smith, Serafin, Abel and Berners, DAFx-02).
// Speeds and their first-order changes follow Herrera, Hanson and Abel (AES
// 127th Convention, 2009), as setBfree has them.
//
// Speeds change every 64 samples, as OpenB3 runs whirl.c in 64-sample
// fragments. Brake positions stay at free stop and the microphone width at
// stereo, OpenB3's settings; that code is not ported.
#pragma once

#include <array>
#include <vector>

namespace atlas::openb3 {
  class Whirl {
   public:
    // Atlas's speed order (Analogue Rotary's Speed).
    enum Speed { kSlow, kFast, kStop, kNumSpeeds };
    static constexpr int kDisplacementSize = 1 << 14;  // WHIRL_DISPLC_SIZE
    static constexpr int kFragment = 64;               // OpenB3's BUFFER_SIZE_SAMPLES

    Whirl();
    // initWhirl(): builds the rate's tables and resets.
    void prepare(double sample_rate);
    // Clears the delay lines and filters. `spinning`: the rotors start at
    // the selected speed; otherwise at rest, as setBfree starts.
    void reset(bool spinning);
    // useRevOption() for the horn and drum together: both slow, both fast or
    // both stopped.
    void setSpeed(int speed);
    // Scales the acceleration and deceleration times (1 = setBfree's).
    void setInertia(double scale);
    // whirl.horn.mic.angle in degrees: 180 (the default) places the
    // microphones on opposite sides, 0 together.
    void setMicAngle(double degrees);

    // whirlProc2(): the horn (with its leakage) and the drum at each
    // microphone. OpenB3's output is horn plus drum per side.
    void process(const float* input, float* horn_left, float* horn_right, float* drum_left, float* drum_right,
                 int samples);

    // Rotational speeds in revolutions per second, and angles (0-1).
    double hornHz() const { return horn_increment_ * sample_rate_; }
    double drumHz() const { return drum_increment_ * sample_rate_; }
    double hornAngle() const { return horn_angle_; }
    double drumAngle() const { return drum_angle_; }

   private:
    struct Filter {  // iir_t W[8]: a0 a1 a2 b0 b1 b2 z0 z1
      float w[8] {};
    };

    static void setFilter(Filter& filter, int type, double hz, double q, double gain, double rate);
    void useRevOption(int option);
    void updateSpeeds();
    void renderFragment(const float* input, float* horn_left, float* horn_right, float* drum_left,
                        float* drum_right, int samples);

    double sample_rate_ = 48000.0;
    // computeRotationSpeeds(): target speeds per revoption (horn, drum).
    std::array<std::array<double, 2>, 9> rev_options_ {};
    int speed_ = kSlow;
    double inertia_ = 1.0;
    float horn_acceleration_ = 0.161f, horn_deceleration_ = 0.321f;
    float drum_acceleration_ = 4.127f, drum_deceleration_ = 1.371f;
    int horn_acdc_ = 0, drum_acdc_ = 0;
    double horn_increment_ = 0.0, drum_increment_ = 0.0;
    double horn_target_ = 0.0, drum_target_ = 0.0;
    double horn_angle_ = 0.0, drum_angle_ = 0.0;
    double mic_angle_ = 0.0;

    std::vector<float> horn_forward_, horn_backward_, drum_forward_, drum_backward_;
    std::array<int, 6> horn_phase_ {}, drum_phase_ {};
    std::array<float, 6> horn_spacing_ {}, drum_spacing_ {};

    std::vector<float> horn_left_, horn_right_, drum_left_, drum_right_;
    unsigned int buffer_mask_ = 0;
    unsigned int out_position_ = 0;
    std::array<float, 8> history_0_ {}, history_1_ {}, history_2_ {};
    int history_index_0_ = 0, history_index_1_ = 0, history_index_2_ = 0;
    std::array<float, 4> z_ {};

    Filter drum_filter_left_, drum_filter_right_, horn_filter_a_, horn_filter_b_;
    float horn_level_ = 0.7f;
    float leakage_ = 0.0f;
    int fragment_position_ = 0;
  };
}
