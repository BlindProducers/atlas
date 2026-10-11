// SPDX-License-Identifier: GPL-3.0-or-later
// Suppressor: a spectral resonance suppressor. Each bin is compared with the
// sound's own spectral envelope (about a third of an octave wide); what sticks
// out above a small knee is pulled down by Amount, quickly, and let back up at
// a rate set by Release. A frequency window (Frequency, Width, Slope) chooses
// where the suppression acts, and Smoothing spreads the reduction across
// neighbouring bins so narrow notches do not sound surgical. Delta plays what
// was removed. Linked stereo, 2048-point STFT, 2048 samples of latency.
#pragma once
#include "../spectral_common/linked_stft.h"

namespace atlas::suppressor {
  struct Parameters {
    float amount = 0.25f;      // 0..1
    float smoothing = 0.25f;   // 0..1
    float release = 1.0f;      // 1..8; higher recovers more slowly
    float frequency = 192.0f;  // centre, on the position axis below
    float width = 128.0f;      // 0..1000, no further effect above 312.5
    float slope = 0.2f;        // 0..1 flat-top fraction of the window
    bool delta = false;
  };

  class Engine : public stft::LinkedGainStft<11> {
    public:
      static constexpr float kMaxCutDb = 24.0f;
      static constexpr float kKneeDb = 1.5f;
      static constexpr float kWidthPlateau = 312.5f;

      // The Frequency control keeps its sample-rate-relative axis: the centre
      // in Hz is (1 + position / 384)^12 * sample_rate / 8192.
      static double positionToHz(double position, double sample_rate);
      static double hzToPosition(double hz, double sample_rate);
      // Suppression weight of a frequency for the window settings (0..1).
      static float windowWeight(float hz, float centre_hz, float width, float slope);

      Engine() { prepare(48000.0); }
      void prepare(double sample_rate);
      void reset();
      void setParameters(const Parameters& parameters) { parameters_ = parameters; }
      const Parameters& parameters() const { return parameters_; }
      void process(const float* in_left, const float* in_right, float* out_left, float* out_right, int count);
      // Reduction applied to a bin in the latest frame, in dB (positive = cut).
      float reductionDb(int bin) const { return reduction_db_[std::clamp(bin, 0, kNumBins - 1)]; }

    private:
      void updateGains(const std::array<float, kNumBins>& power, std::array<float, kNumBins>& gain) override;

      Parameters parameters_;
      std::array<float, kNumBins> detector_ {};
      std::array<float, kNumBins> level_db_ {};
      std::array<float, kNumBins> target_db_ {};
      std::array<float, kNumBins> reduction_db_ {};
      std::array<float, kNumBins> weight_ {};
      stft::BoxAverage<kNumBins> levels_;
      stft::BoxAverage<kNumBins> targets_;
      float weight_frequency_ = -1.0f, weight_width_ = -1.0f, weight_slope_ = -1.0f;
  };
}
