// SPDX-License-Identifier: GPL-3.0-or-later
// Spectral Compressor: 512 band compressors, each acting on one STFT bin.
// A band above its threshold is pulled down by Downward, a band below is
// lifted by Upward (up to 24 dB, never out of the noise floor). The threshold
// is Target, tilted by Tilt per octave around 1 kHz and shaped by Curve.
// Band levels are calibrated to broadband material: white noise at -X dBFS
// reads -X in every band, so a single tone reads well above its own dBFS and
// is compressed hard, which is what makes the effect spectral. Attack
// and Release set the per-band level detector (zero makes it instant, so with
// no release the effect clips spectrally; with release only, it limits).
// Smooth averages the gain curve across neighbouring bands. Linked stereo,
// 1024-point STFT, 1024 samples of latency.
#pragma once
#include "../spectral_common/linked_stft.h"

namespace atlas::spectral_compressor {
  enum Curve { kFlat, kSmooth, kLoud, kNumCurves };

  struct Parameters {
    float downward = 0.0f;    // 0..1 (1 = infinite ratio)
    float upward = 0.0f;      // 0..1
    float target_db = -16.0f; // -64..0 dBFS on the broadband band scale
    float attack = 0.25f;     // 0..1 -> 0..100 ms
    float release = 0.25f;    // 0..1 -> 0..1 s
    float smooth = 0.5f;      // 0..1
    int curve = kFlat;
    float tilt = -3.0f;       // dB per octave, -6..0
    float makeup_db = 0.0f;   // -6..18
    float mix = 1.0f;         // 0..1
  };

  class Engine : public stft::LinkedGainStft<10> {
    public:
      static constexpr float kMaxLiftDb = 24.0f;
      static constexpr float kMaxAttackSeconds = 0.1f;
      static constexpr float kMaxReleaseSeconds = 1.0f;
      // Below this level a band is never lifted; the lift fades in over 20 dB.
      static constexpr float kLiftFloorDb = -90.0f;
      // Per-bin power times this reads the signal's RMS level for white noise:
      // the Hann window's power gain is 3/8 and the linked sum doubles the
      // amplitude, so a bin holds 6 / N of the variance.
      static constexpr float kBandScale = kFftSize / 6.0f;

      // The threshold shape added to Target at `hz` for a curve, before Tilt.
      static float curveDb(int curve, float hz);
      static float thresholdDb(const Parameters& parameters, float hz);

      Engine() { prepare(48000.0); }
      void prepare(double sample_rate);
      void reset();
      void setParameters(const Parameters& parameters) { parameters_ = parameters; }
      const Parameters& parameters() const { return parameters_; }
      void process(const float* in_left, const float* in_right, float* out_left, float* out_right, int count);
      // Gain applied to a bin in the latest frame, in dB (before makeup).
      float gainDb(int bin) const { return gain_db_[std::clamp(bin, 0, kNumBins - 1)]; }
      float levelDb(int bin) const { return level_db_[std::clamp(bin, 0, kNumBins - 1)]; }

    private:
      void updateGains(const std::array<float, kNumBins>& power, std::array<float, kNumBins>& gain) override;

      Parameters parameters_;
      std::array<float, kNumBins> detector_ {};
      std::array<float, kNumBins> level_db_ {};
      std::array<float, kNumBins> raw_gain_db_ {};
      std::array<float, kNumBins> gain_db_ {};
      std::array<float, kNumBins> threshold_db_ {};
      stft::BoxAverage<kNumBins> gains_;
      float threshold_target_ = 1.0e9f, threshold_tilt_ = 1.0e9f;
      int threshold_curve_ = -1;
      float mix_ = 1.0f, makeup_ = 1.0f;
      bool smoothed_initialised_ = false;
  };
}
