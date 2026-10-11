// SPDX-License-Identifier: GPL-3.0-or-later
// Analogue Wah: inductor wah pedals and an optical envelope filter.
//
// Cry Baby (Dunlop GCB-95) and V846 (Vox V846-HW): the same two-transistor
// circuit with different values. A 500 mH inductor resonates with a 0.01 uF
// capacitor whose apparent size the pot multiplies through Q2 (a Miller
// effect), sweeping about 400 Hz to 2.2 kHz. The circuit is simulated from its
// component values (linear_circuit.h) with Q1's base-emitter junction as a
// nonlinear element, solved every sample: that is the pedal's smooth,
// mostly second-harmonic grit, strongest toe down.
//
// Mu-Tron III: a three op-amp state-variable filter whose two integrator
// resistors are shunted by light-dependent resistors lit by one LED. A
// half-wave envelope follower drives the LED; the cells answer with their
// own lag. Mode (LP / BP / HP), Range (Lo / Hi) and Peak as on the unit.
//
// Every model shares the Mu-Tron's envelope follower: Envelope opens
// (positive) or closes (negative) the wah from Position as the input gets
// louder; Decay scales its release (50% = the Mu-Tron).
#pragma once

#include "linear_circuit.h"

#include <array>
#include <complex>
#include <vector>

namespace atlas::analogue::wah {
  enum Model { kCryBaby, kVox, kMuTron, kNumModels };
  enum Mode { kLowPass, kBandPass, kHighPass, kNumModes };
  enum Range { kLow, kHigh, kNumRanges };

  // 0 dBFS peak is this many volts at the input, about the Cry Baby's rated
  // maximum input (-8 dBV) and a strong pickup's peak.
  constexpr double kInstrumentVolts = 0.5;

  struct Parameters {
    int model = kCryBaby;
    double position = 0.0;   // 0 heel .. 1 toe; Mu-Tron: resting control voltage
    double envelope = 0.5;   // -1 .. 1: sensitivity; sign = Up / Down
    double decay = 0.5;
    int mode = kBandPass;
    int range = kLow;
    double peak = 0.5;
    double mix = 1.0;
  };

  // A pedal's small-signal circuit (Q1 as a nonlinear port) with handles for
  // its pot; for tests and the engine's tables.
  struct PedalCircuit {
    LinearCircuit circuit;
    int pot_top = 0, pot_bottom = 0, output = 0;
    double buffer_gain = 1.0, buffer_hz = 0.0;  // the GCB-95's input buffer
    void setPot(double fraction);  // wiper-to-ground share of the track
    // Small-signal response from the input jack, buffer included.
    std::complex<double> response(double hz) const;
  };
  PedalCircuit makePedalCircuit(int model);
  // The pot's wiper-to-ground share at a rocker position (0 heel .. 1 toe),
  // through the pot's taper and the rack's travel.
  double potFraction(int model, double position);

  // Mu-Tron III filter tuning and the vactrol's static law, for tests.
  double muTronFrequency(double ldr_ohms, int range);
  double muTronQ(double peak);
  double vactrolOhms(double led_amps);

  class Engine {
   public:
    Engine();
    void prepare(double sample_rate);
    void reset();
    void setParameters(const Parameters& parameters);
    void process(const float* in_left, const float* in_right, float* out_left, float* out_right, int count);

    // For tests.
    double envelopeVolts() const { return envelope_volts_; }
    double pedalPosition() const { return pedal_position_; }
    double ldrOhms() const { return ldr_ohms_; }

    static constexpr int kTableSize = 65;
    static constexpr int kJunctionPoints = 512;
    static constexpr int kControlInterval = 8;

   private:
    static constexpr int kStates = 6;
    // One discretised pedal circuit: 6 states, Q1's port, one output.
    struct PedalCoefficients {
      std::array<std::array<double, kStates>, kStates> a {};
      std::array<double, kStates> b {}, c {}, d {}, g {};
      double e = 0.0, f = 0.0, h = 0.0, k = 0.0;
    };
    struct Pedal {
      std::vector<PedalCoefficients> table;
      // Q1's solved junction law per table entry: kJunctionPoints pairs of
      // (current, slope per step) over the drive from junction_start, one
      // step being 1 / junction_scale volts.
      std::vector<float> junction;
      std::array<double, kTableSize> junction_start {}, junction_scale {};
      PedalCoefficients now;
      int entry = 0;
      double entry_fraction = 0.0, polarity = 1.0;
      bool buffered = false;
      double position = -1.0;  // where `now` was interpolated
      double buffer_gain = 1.0, buffer_a = 1.0, buffer_c = 0.0;
      std::array<std::array<double, kStates>, 2> state {};
      std::array<std::array<double, 2>, 2> buffer_state {};

    };
    struct MuTron {
      std::array<double, 2> s1 {}, s2 {};
      double g = 0.0, g_target = 0.0, damping = 1.0, input_gain = 1.0;
      std::array<double, kNumModes> mode_gain { 0.0, 1.0, 0.0 };
    };

    void buildPedal(int model);
    void follow(const float* in_left, const float* in_right, int count);
    void control();
    void controlMuTron(bool settle);
    void renderPedal(int model, const float* in_left, const float* in_right, double* left, double* right, int count);
    void renderMuTron(const float* in_left, const float* in_right, double* left, double* right, int count);
    void render(int model, const float* in_left, const float* in_right, double* left, double* right, int count);
    void startModel(int model);

    Parameters parameters_;
    double sample_rate_ = 48000.0;
    std::array<Pedal, 2> pedals_ {};
    MuTron mutron_;

    // Envelope follower (Mu-Tron III C9 node, volts) and the LED / vactrol.
    double envelope_volts_ = 0.0, ldr_log_ = 0.0, ldr_ohms_ = 1.0e7;
    double vactrol_on_ = 0.0, vactrol_off_ = 0.0;
    // Smoothed controls.
    double position_ = 0.0, pedal_position_ = 0.0, peak_ = 0.5, mix_ = 1.0, capacitance_ = 4.0e-9;
    double smoothing_ = 0.01, control_smoothing_ = 0.1;
    int current_model_ = kCryBaby, fading_model_ = -1, fade_remaining_ = 0, fade_length_ = 1;
  };
}
