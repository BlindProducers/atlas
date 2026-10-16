// SPDX-License-Identifier: GPL-3.0-or-later
#include "spectral_compressor_engine.h"

namespace atlas::spectral_compressor {
  namespace {
    constexpr float kFloorDb = -120.0f;
    constexpr float kGainSmoothingSeconds = 0.004f;
    constexpr float kControlSmoothingSeconds = 0.005f;
  }

  float Engine::curveDb(int curve, float hz) {
    hz = std::max(hz, 1.0f);
    switch (curve) {
      case kSmooth: {
        // Louder-allowed lows and highs, like an equal-loudness contour:
        // +6 dB two octaves below 150 Hz, +4 dB an octave above 8 kHz.
        const float low = std::clamp(std::log2(150.0f / hz), 0.0f, 2.0f) * 3.0f;
        const float high = std::clamp(std::log2(hz / 8000.0f), 0.0f, 1.0f) * 4.0f;
        return low + high;
      }
      case kLoud: {
        // A 6 dB dip in the mids, two octaves wide around 1.5 kHz, so the
        // mids are compressed hardest and pushed forward.
        const float octaves = std::log2(hz / 1500.0f);
        return -6.0f * std::exp(-octaves * octaves * 0.5f);
      }
      case kFlat:
      default:
        return 0.0f;
    }
  }

  float Engine::thresholdDb(const Parameters& parameters, float hz) {
    const float tilt = std::clamp(parameters.tilt, -6.0f, 0.0f);
    return std::clamp(parameters.target_db, -64.0f, 0.0f) + tilt * std::log2(std::max(hz, 1.0f) / 1000.0f) +
           curveDb(parameters.curve, hz);
  }

  void Engine::prepare(double sample_rate) {
    LinkedGainStft::prepare(sample_rate);
    threshold_curve_ = -1;
    reset();
  }

  void Engine::reset() {
    resetStft();
    detector_.fill(0.0f);
    level_db_.fill(kFloorDb);
    raw_gain_db_.fill(0.0f);
    gain_db_.fill(0.0f);
    smoothed_initialised_ = false;
  }

  void Engine::process(const float* in_left, const float* in_right, float* out_left, float* out_right, int count) {
    const float target_mix = std::clamp(parameters_.mix, 0.0f, 1.0f);
    const float target_makeup = stft::dbToGain(std::clamp(parameters_.makeup_db, -6.0f, 18.0f));
    if (!smoothed_initialised_) {
      mix_ = target_mix;
      makeup_ = target_makeup;
      smoothed_initialised_ = true;
    }
    const float coefficient = stft::smoothingCoefficient(1.0f / float(sampleRate()), kControlSmoothingSeconds);
    processStftRuns(in_left, in_right, out_left, out_right, count, [&](auto run) {
      // Once a smoothing step leaves Mix and Makeup where they are, every
      // later step does too, so the run can treat them as constants. The
      // stepped values are kept, so a -0 mix becomes +0 as it would per sample.
      float mix = mix_, makeup = makeup_;
      mix += (target_mix - mix) * coefficient;
      makeup += (target_makeup - makeup) * coefficient;
      if (mix == mix_ && makeup == makeup_) {
        mix_ = mix;
        makeup_ = makeup;
        run([mix, makeup](float dry_l, float dry_r, float wet_l, float wet_r, float& out_l, float& out_r) {
          out_l = dry_l + (wet_l * makeup - dry_l) * mix;
          out_r = dry_r + (wet_r * makeup - dry_r) * mix;
        });
      }
      else {
        run([&](float dry_l, float dry_r, float wet_l, float wet_r, float& out_l, float& out_r) {
          mix_ += (target_mix - mix_) * coefficient;
          makeup_ += (target_makeup - makeup_) * coefficient;
          out_l = dry_l + (wet_l * makeup_ - dry_l) * mix_;
          out_r = dry_r + (wet_r * makeup_ - dry_r) * mix_;
        });
      }
    });
  }

  void Engine::updateGains(const std::array<float, kNumBins>& power, std::array<float, kNumBins>& gain) {
    const Parameters& p = parameters_;
    const float hop = hopSeconds();
    const float attack = stft::smoothingCoefficient(hop, std::clamp(p.attack, 0.0f, 1.0f) * kMaxAttackSeconds);
    const float release = stft::smoothingCoefficient(hop, std::clamp(p.release, 0.0f, 1.0f) * kMaxReleaseSeconds);
    const float gain_coefficient = stft::smoothingCoefficient(hop, kGainSmoothingSeconds);
    const float downward = std::clamp(p.downward, 0.0f, 1.0f);
    const float upward = std::clamp(p.upward, 0.0f, 1.0f);
    const float smooth = std::clamp(p.smooth, 0.0f, 1.0f);

    if (p.target_db != threshold_target_ || p.tilt != threshold_tilt_ || p.curve != threshold_curve_) {
      threshold_target_ = p.target_db; threshold_tilt_ = p.tilt; threshold_curve_ = p.curve;
      for (int bin = 0; bin < kNumBins; ++bin)
        threshold_db_[bin] = thresholdDb(p, binFrequency(std::max(bin, 1)));
    }

    for (int bin = 0; bin < kNumBins; ++bin) {
      // One bin per band, on the broadband scale (see the header).
      const float band_power = power[bin] * kBandScale;
      const float coefficient = band_power > detector_[bin] ? attack : release;
      detector_[bin] += (band_power - detector_[bin]) * coefficient;
      level_db_[bin] = std::max(kFloorDb, stft::powerToDb(detector_[bin]));
      const float over = level_db_[bin] - threshold_db_[bin];
      float gain_db = 0.0f;
      if (over > 0.0f)
        gain_db = -over * downward;
      else if (upward > 0.0f) {
        const float lift_gate = std::clamp((level_db_[bin] - kLiftFloorDb) / 20.0f, 0.0f, 1.0f);
        gain_db = std::min(kMaxLiftDb, -over) * upward * lift_gate;
      }
      raw_gain_db_[bin] = gain_db;
    }

    // Smooth averages the gain curve over up to a third of an octave at 100%;
    // the square law keeps low settings nearly per-band.
    gains_.load(raw_gain_db_.data());
    const float smooth_width = smooth * smooth;
    for (int bin = 0; bin < kNumBins; ++bin) {
      const int half_width = int(smooth_width * std::max(1.0f, bin * 0.17f) + 0.5f);
      const float target = half_width > 0 ? gains_.mean(bin - half_width, bin + half_width) : raw_gain_db_[bin];
      gain_db_[bin] += (target - gain_db_[bin]) * gain_coefficient;
      gain[bin] = stft::dbToGain(gain_db_[bin]);
    }
  }
}
