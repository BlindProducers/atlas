// SPDX-License-Identifier: GPL-3.0-or-later
// Analogue Rotary: rotating-speaker cabinets.
//
// Organ 122: a tube amplifier drives a crossover; the treble goes to a
// spinning horn and the bass to a woofer firing into a spinning drum. Guitar
// 16: one full-range speaker inside a spinning drum. Each microphone hears
// every rotor through a Doppler delay (the rotor's distance to it changes
// as it turns) and a directivity that dims and darkens it as it turns away,
// plus cabinet reflections. Speed switches Slow / Fast / Stop; the rotors
// follow with their own inertia (the light horn quickly, the heavy drum
// slowly), scaled by Inertia (50% = the hardware). Stop lets the rotors coast
// to rest wherever they are.
#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace atlas::analogue::rotary {
  enum Model { kOrgan, kGuitar, kNumModels };
  enum Speed { kSlow, kFast, kStop, kNumSpeeds };

  struct Parameters {
    int model = kOrgan;
    int speed = kSlow;
    double inertia = 0.5;
    double drive = 0.3;
    double balance = 0.0;   // -1 drum only .. 1 horn only (Organ)
    double spread = 0.5;    // microphone angle: 0 mono .. 1 opposite sides
    double mix = 1.0;
  };

  // Rotor speeds in Hz and the time constants of their speed changes.
  struct RotorSpec {
    double slow_hz, fast_hz;
    double speed_up_seconds, slow_down_seconds;
    double radius_metres;         // source distance from the rotation axis
    double directivity;           // 0 = omni .. 1 = fully dim when facing away
    double rear_lowpass_hz;       // darkness when facing away
  };
  const RotorSpec& hornSpec();
  const RotorSpec& drumSpec(int model);

  class Engine {
   public:
    Engine();
    void prepare(double sample_rate);
    void reset();
    void setParameters(const Parameters& parameters);
    void process(const float* in_left, const float* in_right, float* out_left, float* out_right, int count);
    // Current rotor speeds in Hz (for tests).
    double hornHz() const { return rotors_[kHorn].hz; }
    double drumHz() const { return rotors_[parameters_.model == kGuitar ? kGuitarDrum : kOrganDrum].hz; }

   private:
    enum RotorIndex { kHorn, kOrganDrum, kGuitarDrum, kNumRotors };
    struct Rotor {
      double hz = 0.0, angle = 0.0;              // angle in cycles
      double cosine = 1.0, sine = 0.0;           // of the angle, advanced per sample
      double step_cosine = 1.0, step_sine = 0.0; // rotation per sample
      std::vector<float> line;
      int write = 0;
      std::array<double, 2> rear_state {};   // per microphone
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

    void configureCrossover();
    void updateSpeeds(int samples);
    void write(Rotor& rotor, double input);
    double readLine(const Rotor& rotor, double delay_samples) const;
    double amplifier(double x) const;
    void beginBlock(Rotor& rotor);
    double hear(Rotor& rotor, const RotorSpec& spec, int microphone, double facing_cosine);
    void renderModel(int model, const float* mono, float* left, float* right, int count);

    Parameters parameters_;
    double sample_rate_ = 48000.0;
    std::array<Rotor, kNumRotors> rotors_ {};
    // lowpass_: crossover, drum still band. highpass_: crossover, Guitar low cut.
    std::array<Biquad, 2> lowpass_ {}, highpass_ {};
    Biquad horn_voice_;
    double drive_ = 0.3, balance_ = 0.0, spread_ = 0.5, mix_ = 1.0;
    double smoothing_ = 0.01;
    int current_model_ = kOrgan, fading_model_ = -1, fade_remaining_ = 0, fade_length_ = 1;
  };
}
