// SPDX-License-Identifier: GPL-3.0-or-later
#include "spectral_balance_engine.h"
#include <algorithm>
#include <cmath>

namespace atlas::spectral_balance {
  namespace {
    constexpr float kPi = 3.14159265358979323846f;
    // Time constants at Speed 50%.
    constexpr float kFineAttackSeconds = 0.025f;
    constexpr float kFineReleaseSeconds = 0.25f;
    constexpr float kBroadSeconds = 1.0f;
    constexpr float kGainSmoothingSeconds = 0.02f;
    constexpr float kMixSmoothingSeconds = 0.005f;
    // Bands this far below the loudest band, or below the floor, are left alone.
    constexpr float kBandWindowDb = 45.0f;
    constexpr float kLiftFloorDb = Engine::kFloorDb + 20.0f;

    float powerToDb(float power) { return 10.0f * std::log10(power + 1.0e-12f); }
    float dbToGain(float db) { return std::exp(db * 0.11512925465f); }
    float coefficient(float seconds, float tau) { return 1.0f - std::exp(-seconds / std::max(tau, 1.0e-4f)); }
  }

  Engine::Engine() {
    for (int i = 0; i < kFftSize; ++i)
      window_[i] = 0.5f - 0.5f * std::cos(2.0f * kPi * i / kFftSize);
    prepare(48000.0);
  }

  float Engine::bandCentreHz(int band) {
    const float ratio = std::pow(kBandHighHz / kBandLowHz, 1.0f / kNumBands);
    return kBandLowHz * std::pow(ratio, band + 0.5f);
  }

  void Engine::prepare(double sample_rate) {
    sample_rate_ = sample_rate > 1000.0 ? sample_rate : 48000.0;
    const float octaves_per_band = std::log2(kBandHighHz / kBandLowHz) / kNumBands;
    const float first_centre = std::log2(bandCentreHz(0));
    band_weight_.fill(0.0f);
    for (int bin = 0; bin < kNumBins; ++bin) {
      const float hz = std::max(binFrequency(std::max(bin, 1), sample_rate_), 1.0f);
      log2_frequency_[bin] = std::log2(hz);
      // Position in band-index space: band b's centre is at b.
      const float x = (log2_frequency_[bin] - first_centre) / octaves_per_band;
      in_bands_[bin] = bin > 0 && x > -1.0f && x < float(kNumBands);
      const float clamped = std::clamp(x, 0.0f, float(kNumBands - 1));
      const int index = std::min(int(clamped), kNumBands - 2);
      band_index_[bin] = index;
      band_fraction_[bin] = std::clamp(clamped - index, 0.0f, 1.0f);
      if (in_bands_[bin]) {
        band_weight_[index] += 1.0f - band_fraction_[bin];
        band_weight_[index + 1] += band_fraction_[bin];
      }
    }
    for (int band = 0; band < kNumBands; ++band)
      band_octave_offset_db_[band] = 10.0f * std::log10(bandCentreHz(band) / 1000.0f);
    reset();
  }

  void Engine::reset() {
    in_left_.fill(0.0f); in_right_.fill(0.0f);
    out_left_.fill(0.0f); out_right_.fill(0.0f);
    fast_power_.fill(0.0f);
    level_db_.fill(kFloorDb);
    gain_db_.fill(0.0f);
    frame_gain_db_.fill(0.0f);
    slow_power_.fill(0.0f);
    band_db_.fill(kFloorDb);
    band_gain_db_.fill(0.0f);
    position_ = 0;
    hop_position_ = 0;
    mix_initialized_ = false;
  }

  float Engine::targetDb(int target, float bias_db_per_octave, float hz) {
    hz = std::max(hz, 1.0f);
    const float octaves = std::log2(hz / 1000.0f);
    float result = bias_db_per_octave * octaves;
    switch (target) {
      case kFlat: result += 3.0f * octaves; break;       // white: +3 dB/oct of energy per octave
      case kTilt15: result += 1.5f * octaves; break;
      case kTilt3: break;                                 // pink: flat energy per octave
      case kTilt45: result -= 1.5f * octaves; break;
      case kTilt6: result -= 3.0f * octaves; break;
      case kPerceptual:
      default: {
        // The long-term balance of finished music: flat per octave through the
        // low mids, -4.5 dB/oct above 250 Hz, -6 dB/oct above 6 kHz, and a
        // roll-off below 60 Hz so sub-bass is not lifted to the level of the bass.
        if (hz < 60.0f)
          result -= 6.0f * std::log2(60.0f / hz);
        else if (hz > 250.0f) {
          const float upper = std::min(hz, 6000.0f);
          result -= 1.5f * std::log2(upper / 250.0f);
          if (hz > 6000.0f)
            result -= 3.0f * std::log2(hz / 6000.0f);
        }
        break;
      }
    }
    return result;
  }

  void Engine::process(const float* in_left, const float* in_right, float* out_left, float* out_right, int count) {
    const float target_mix = std::clamp(parameters_.mix, 0.0f, 1.0f);
    if (!mix_initialized_) {
      mix_ = target_mix;
      mix_initialized_ = true;
    }
    const float mix_coefficient = coefficient(1.0f / float(sample_rate_), kMixSmoothingSeconds);
    for (int i = 0; i < count; ++i) {
      // The input ring doubles as the dry path, delayed by exactly kFftSize.
      const float dry_left = in_left_[position_], dry_right = in_right_[position_];
      const float wet_left = out_left_[position_], wet_right = out_right_[position_];
      in_left_[position_] = in_left[i];
      in_right_[position_] = in_right[i];
      out_left_[position_] = 0.0f;
      out_right_[position_] = 0.0f;
      mix_ += (target_mix - mix_) * mix_coefficient;
      out_left[i] = dry_left + (wet_left - dry_left) * mix_;
      out_right[i] = dry_right + (wet_right - dry_right) * mix_;
      position_ = (position_ + 1) & (kFftSize - 1);
      if (++hop_position_ == kHopSize) {
        hop_position_ = 0;
        processFrame();
      }
    }
  }

  void Engine::processFrame() {
    const Parameters& p = parameters_;
    constexpr int kLastBin = kNumBins - 1;
    for (int i = 0; i < kFftSize; ++i) {
      const int index = (position_ + i) & (kFftSize - 1);
      fft_left_[i] = in_left_[index] * window_[i];
      fft_right_[i] = in_right_[index] * window_[i];
    }
    fft_.performRealOnlyForwardTransform(fft_left_.data(), true);
    fft_.performRealOnlyForwardTransform(fft_right_.data(), true);

    // Analyse the sum. Magnitudes are normalised so a full-scale sine reads 0.5.
    const float hop_seconds = float(kHopSize) / float(sample_rate_);
    const float speed_factor = std::exp2((0.5f - std::clamp(p.speed, 0.0f, 1.0f)) * 4.0f);
    const float attack = coefficient(hop_seconds, kFineAttackSeconds * speed_factor);
    const float release = coefficient(hop_seconds, kFineReleaseSeconds * speed_factor);
    const float slow = coefficient(hop_seconds, kBroadSeconds * speed_factor);
    const float gain_coefficient = coefficient(hop_seconds, kGainSmoothingSeconds);
    constexpr float kNorm = 1.0f / kFftSize; // (2 / N) for the magnitude, / 2 for the sum
    for (int bin = 0; bin < kNumBins; ++bin) {
      const float real = (fft_left_[2 * bin] + fft_right_[2 * bin]) * kNorm;
      const float imaginary = (fft_left_[2 * bin + 1] + fft_right_[2 * bin + 1]) * kNorm;
      const float power = real * real + imaginary * imaginary;
      fast_power_[bin] += (power - fast_power_[bin]) * (power > fast_power_[bin] ? attack : release);
      level_db_[bin] = std::max(kFloorDb, powerToDb(fast_power_[bin]));
    }
    cumulative_[0] = 0.0f;
    for (int bin = 0; bin < kNumBins; ++bin)
      cumulative_[bin + 1] = cumulative_[bin] + level_db_[bin];
    const auto mean_db = [&](int low, int high) {
      low = std::max(low, 1); high = std::min(high, kLastBin);
      return (cumulative_[high + 1] - cumulative_[low]) / float(high - low + 1);
    };

    const float tame = p.tame_recover >= 0.0f ? 1.0f : 1.0f + std::max(p.tame_recover, -1.0f);
    const float recover = p.tame_recover <= 0.0f ? 1.0f : 1.0f - std::min(p.tame_recover, 1.0f);

    // Broad layer: band densities from the fast-smoothed power, integrated slowly.
    std::array<float, kNumBands> band_power {};
    for (int bin = 1; bin < kNumBins; ++bin) {
      if (!in_bands_[bin]) continue;
      band_power[band_index_[bin]] += fast_power_[bin] * (1.0f - band_fraction_[bin]);
      band_power[band_index_[bin] + 1] += fast_power_[bin] * band_fraction_[bin];
    }
    float loudest = kFloorDb;
    for (int band = 0; band < kNumBands; ++band) {
      const float density = band_weight_[band] > 0.0f ? band_power[band] / band_weight_[band] : 0.0f;
      slow_power_[band] += (density - slow_power_[band]) * slow;
      band_db_[band] = std::max(kFloorDb, powerToDb(slow_power_[band]) + band_octave_offset_db_[band]);
      loudest = std::max(loudest, band_db_[band]);
    }
    std::array<float, kNumBands> deviation {};
    std::array<bool, kNumBands> active {};
    float deviation_sum = 0.0f;
    int active_count = 0;
    for (int band = 0; band < kNumBands; ++band) {
      active[band] = band_db_[band] > kFloorDb + 10.0f && band_db_[band] > loudest - kBandWindowDb;
      if (!active[band]) continue;
      deviation[band] = band_db_[band] - targetDb(p.target, p.bias, bandCentreHz(band));
      deviation_sum += deviation[band];
      ++active_count;
    }
    const float deviation_mean = active_count > 0 ? deviation_sum / float(active_count) : 0.0f;
    for (int band = 0; band < kNumBands; ++band) {
      if (!active[band]) { band_gain_db_[band] = 0.0f; continue; }
      const float excess = deviation[band] - deviation_mean;
      band_gain_db_[band] = -excess * (excess > 0.0f ? tame : recover);
    }

    // Fine layer, range, limits and smoothing per bin.
    const float amount = std::clamp(p.amount, 0.0f, 1.0f);
    const float max_cut = -kMaxCutDb * amount, max_lift = kMaxLiftDb * amount;
    const float log2_low = std::log2(std::max(p.range_low, 1.0f));
    const float log2_high = std::log2(std::max(p.range_high, 1.0f));
    for (int bin = 1; bin < kNumBins; ++bin) {
      // Below about 560 Hz a resonance is narrower than a bin, so the bin
      // itself is compared; above, three bins are averaged against noise.
      const int local_width = bin < 24 ? 0 : 1;
      const float local = mean_db(bin - local_width, bin + local_width);
      const int width = std::max(8, int(bin * 0.1225f + 0.5f)); // about a third of an octave
      const float envelope = mean_db(bin - width, bin + width);
      const float difference = local - envelope;
      float fine = 0.0f;
      if (difference > kFineKneeDb)
        fine = -(difference - kFineKneeDb) * tame;
      else if (difference < -kFineKneeDb && local > kLiftFloorDb)
        fine = (-difference - kFineKneeDb) * recover;
      const int band = band_index_[bin];
      const float broad = band_gain_db_[band] + (band_gain_db_[band + 1] - band_gain_db_[band]) * band_fraction_[bin];
      const float fade = std::clamp((log2_frequency_[bin] - log2_low) * 2.0f + 1.0f, 0.0f, 1.0f) *
                         std::clamp((log2_high - log2_frequency_[bin]) * 2.0f + 1.0f, 0.0f, 1.0f);
      frame_gain_db_[bin] = std::clamp((fine + broad) * amount * fade, max_cut, max_lift);
    }
    frame_gain_db_[0] = frame_gain_db_[1];
    float previous = frame_gain_db_[0];
    for (int bin = 0; bin < kNumBins; ++bin) {
      const float next = bin < kLastBin ? frame_gain_db_[bin + 1] : frame_gain_db_[bin];
      const float smoothed = 0.15f * previous + 0.7f * frame_gain_db_[bin] + 0.15f * next;
      previous = frame_gain_db_[bin];
      gain_db_[bin] += (smoothed - gain_db_[bin]) * gain_coefficient;
      const float gain = dbToGain(gain_db_[bin]);
      fft_left_[2 * bin] *= gain; fft_left_[2 * bin + 1] *= gain;
      fft_right_[2 * bin] *= gain; fft_right_[2 * bin + 1] *= gain;
    }

    fft_.performRealOnlyInverseTransform(fft_left_.data());
    fft_.performRealOnlyInverseTransform(fft_right_.data());
    // Four overlapping squared Hann windows sum to 1.5.
    constexpr float kOverlapScale = 2.0f / 3.0f;
    for (int i = 0; i < kFftSize; ++i) {
      const int index = (position_ + i) & (kFftSize - 1);
      const float scale = window_[i] * kOverlapScale;
      out_left_[index] += fft_left_[i] * scale;
      out_right_[index] += fft_right_[i] * scale;
    }
  }
}
