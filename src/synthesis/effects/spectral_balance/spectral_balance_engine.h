// SPDX-License-Identifier: GPL-3.0-or-later
// Spectral Balance: an automatic spectral balancer. One Amount knob moves the
// spectrum toward a target tonal balance in two layers under one gain curve:
//   - a fine layer compares each bin with the sound's own smoothed spectral
//     envelope (about a third of an octave wide) and cuts resonances and
//     boomy notes or lifts narrow holes, with fast time constants;
//   - a broad layer levels 24 log-spaced bands toward a target curve (a
//     preset tilt, tilted further by Bias), slowly, so the overall tone
//     settles without pumping.
// Fixed 2048-point STFT, Hann window, 75% overlap, linked stereo: the sum is
// analysed and one gain curve is applied to both channels.
#pragma once
#include "atlas_fft.h"
#include <array>
#include <memory>

namespace atlas::spectral_balance {
  enum Target { kPerceptual, kFlat, kTilt15, kTilt3, kTilt45, kTilt6, kNumTargets };

  struct Parameters {
    float amount = 0.25f;         // 0..1
    int target = kPerceptual;
    float bias = 0.0f;            // dB per octave added to the target, -3..3
    float speed = 0.5f;           // 0..1; 0.5 is the designed time constants, each end is 4x
    float range_low = 30.0f;      // Hz; no gain below (half-octave fade)
    float range_high = 16000.0f;  // Hz; no gain above
    float tame_recover = 0.0f;    // -1 recover only .. 0 both .. 1 tame only
    float mix = 1.0f;             // 0..1, dry is latency-aligned
  };

  class Engine {
    public:
      static constexpr int kFftOrder = 11;
      static constexpr int kFftSize = 1 << kFftOrder;
      static constexpr int kHopSize = kFftSize / 4;
      static constexpr int kNumBins = kFftSize / 2 + 1;
      static constexpr int kNumBands = 24;
      static constexpr float kBandLowHz = 30.0f;
      static constexpr float kBandHighHz = 16000.0f;
      static constexpr int kLatencySamples = kFftSize;
      // Gain limits at Amount 100%; they scale linearly with Amount.
      static constexpr float kMaxCutDb = 12.0f;
      static constexpr float kMaxLiftDb = 6.0f;
      // Power dB of a bin below which nothing is lifted or balanced
      // (a full-scale sine reads about -6 dB on this scale).
      static constexpr float kFloorDb = -90.0f;
      static constexpr float kFineKneeDb = 1.5f;

      Engine();
      void prepare(double sample_rate);
      void reset();
      void setParameters(const Parameters& parameters) { parameters_ = parameters; }
      const Parameters& parameters() const { return parameters_; }
      void process(const float* in_left, const float* in_right, float* out_left, float* out_right, int count);

      double sampleRate() const { return sample_rate_; }
      static float binFrequency(int bin, double sample_rate) { return float(bin * sample_rate / kFftSize); }
      // Target level in dB for energy per octave at `hz` (an arbitrary offset:
      // only the shape matters, levels are compared after removing the mean).
      static float targetDb(int target, float bias_db_per_octave, float hz);
      static float bandCentreHz(int band);
      // The gain curve (dB per bin) applied to the most recent frame.
      const std::array<float, kNumBins>& gainsDb() const { return gain_db_; }
      // Band levels (energy per octave, dB) after slow integration.
      const std::array<float, kNumBands>& bandLevelsDb() const { return band_db_; }

    private:
      void processFrame();

      Parameters parameters_;
      double sample_rate_ = 48000.0;
      atlas::Fft fft_ { kFftOrder };
      std::array<float, kFftSize> window_ {};
      alignas(16) std::array<float, 2 * kFftSize> fft_left_ {};
      alignas(16) std::array<float, 2 * kFftSize> fft_right_ {};
      std::array<float, kFftSize> in_left_ {}, in_right_ {}, out_left_ {}, out_right_ {};
      int position_ = 0;
      int hop_position_ = 0;
      float mix_ = 1.0f;
      bool mix_initialized_ = false;

      // Analysis state.
      std::array<float, kNumBins> fast_power_ {};
      std::array<float, kNumBins> level_db_ {};
      std::array<float, kNumBins + 1> cumulative_ {};
      std::array<float, kNumBins> gain_db_ {};
      std::array<float, kNumBins> log2_frequency_ {};
      std::array<int, kNumBins> band_index_ {};
      std::array<float, kNumBins> band_fraction_ {};
      std::array<bool, kNumBins> in_bands_ {};
      std::array<float, kNumBands> band_weight_ {};
      std::array<float, kNumBands> band_octave_offset_db_ {};
      std::array<float, kNumBands> slow_power_ {};
      std::array<float, kNumBands> band_db_ {};
      std::array<float, kNumBands> band_gain_db_ {};
      std::array<float, kNumBins> frame_gain_db_ {};
  };
}
