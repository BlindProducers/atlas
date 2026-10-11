// SPDX-License-Identifier: GPL-3.0-or-later
// Analogue Chorus: four classic BBD choruses on the shared BBD engine.
//
// Every model sums the input to mono for its wet path, as the hardware does;
// the dry signal keeps its stereo image. Rate and Depth scale each model's
// authentic LFO speed and sweep (1.0 = the original), Noise scales the chips'
// hiss and clock leakage (0.5 = the original level), Width scales the wet
// path's side signal and Mix is an equal-power dry/wet blend.
//
// As on the hardware, the LFO sweeps the BBD clock period, so the delay
// itself follows the LFO waveform.
#pragma once

#include "bbd.h"
#include "compander.h"

#include <array>

namespace atlas::analogue::chorus {
  enum Model { kJuno, kDimension, kCe, kEnsemble, kNumModels };
  enum JunoMode { kJunoI, kJunoII, kJunoBoth, kNumJunoModes };
  constexpr int kNumDimensionModes = 4;
  constexpr int kMaxLines = 3;

  struct Parameters {
    int model = kJuno;
    int juno_mode = kJunoI;
    int dimension_mode = 0;
    double rate = 1.0;
    double depth = 1.0;
    double noise = 0.5;
    double width = 1.0;
    double mix = 0.5;
  };

  enum class LfoShape { kTriangle, kSine };

  // One mode of one unit, as the hardware runs it.
  struct ModeSpec {
    int lines = 1;
    // Up to two LFO components summed (Ensemble blends slow and fast).
    std::array<double, 2> rate_hz { 0.5, 0.0 };
    std::array<double, 2> weight { 1.0, 0.0 };
    LfoShape shape = LfoShape::kTriangle;
    // Phase offset of each line's LFO, in cycles.
    std::array<double, kMaxLines> phase { 0.0, 0.5, 0.0 };
    // Delay at the LFO extremes, in seconds, at 100% depth.
    double min_delay = 0.00166, max_delay = 0.00535;
    // Wet output matrix: line i to left / right, and the same lines through a
    // high-pass (the Dimension's inverted cross-feed).
    std::array<double, kMaxLines> to_left { 1.0, 0.0, 0.0 };
    std::array<double, kMaxLines> to_right { 0.0, 1.0, 0.0 };
    std::array<double, kMaxLines> cross_left { 0.0, 0.0, 0.0 };
    std::array<double, kMaxLines> cross_right { 0.0, 0.0, 0.0 };
    double cross_highpass_hz = 0.0;
    // Wet level relative to dry in the unit's output mixer.
    double wet_gain = 1.0;
  };

  // Hardware data for a model's mode (index clamped to the model's modes).
  const ModeSpec& modeSpec(int model, int mode);
  int numModes(int model);
  int stages(int model);

  class Engine {
   public:
    Engine();
    void prepare(double sample_rate);
    void reset();
    void setParameters(const Parameters& parameters);
    void process(const float* in_left, const float* in_right, float* out_left, float* out_right, int count);

   private:
    struct Voice {
      std::array<BbdLine, kMaxLines> lines;
      std::array<Compander, kMaxLines> companders;
      std::array<double, kMaxLines> cross_state {}, output_highpass {};
      double input_highpass = 0.0;
      // Smoothed sweep endpoints, LFO weights and mixer gains, so mode changes glide.
      std::array<double, 2> weight {};
      double min_delay = 0.0, max_delay = 0.0, wet_gain = 1.0;
      std::array<double, kMaxLines> to_left {}, to_right {}, cross_left {}, cross_right {};
      bool primed = false, settled = false;
      int control_countdown = 0;
    };

    void updateTargets(Voice& voice, const ModeSpec& spec, double coefficient);
    void renderModel(int model, Voice& voice, const float* mono, float* wet_left, float* wet_right, int count);
    void resetVoice(Voice& voice);

    std::array<Voice, kNumModels> voices_ {};
    Parameters parameters_;
    double sample_rate_ = 48000.0;
    std::array<double, 2> lfo_phase_ {};
    int current_model_ = kJuno, fading_model_ = -1;
    int fade_remaining_ = 0, fade_length_ = 1, fade_wait_ = 0, prime_length_ = 1;
    double rate_ = 1.0, depth_ = 1.0, width_ = 1.0, mix_ = 0.5, noise_ = 0.5;
    double smoothing_ = 0.01;
  };
}
