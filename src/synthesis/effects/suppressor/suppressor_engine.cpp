// SPDX-License-Identifier: GPL-3.0-or-later
#include "suppressor_engine.h"

namespace atlas::suppressor {
  namespace {
    constexpr float kDetectorAttackSeconds = 0.001f;
    constexpr float kDetectorReleaseSeconds = 0.03f;
    constexpr float kAttackSeconds = 0.003f;
    constexpr float kFloorDb = -100.0f;
    constexpr float kPi = 3.14159265358979323846f;
  }

  double Engine::positionToHz(double position, double sample_rate) {
    return std::pow(1.0 + position / 384.0, 12.0) * sample_rate / 8192.0;
  }

  double Engine::hzToPosition(double hz, double sample_rate) {
    return 384.0 * (std::pow(std::max(0.0, hz) * 8192.0 / sample_rate, 1.0 / 12.0) - 1.0);
  }

  float Engine::windowWeight(float hz, float centre_hz, float width, float slope) {
    // Width is in the control's own units: 62.5 per octave of half-width,
    // never less than a quarter octave, and flat above the plateau.
    const float half_width_octaves = std::max(0.25f, std::min(width, kWidthPlateau) / 62.5f);
    const float distance = std::abs(std::log2(std::max(hz, 1.0f) / std::max(centre_hz, 1.0f))) / half_width_octaves;
    const float flat = std::clamp(slope, 0.0f, 0.98f);
    if (distance <= flat)
      return 1.0f;
    if (distance >= 1.0f)
      return 0.0f;
    const float t = (distance - flat) / (1.0f - flat);
    return 0.5f + 0.5f * std::cos(kPi * t);
  }

  void Engine::prepare(double sample_rate) {
    LinkedGainStft::prepare(sample_rate);
    weight_frequency_ = -1.0f;
    reset();
  }

  void Engine::reset() {
    resetStft();
    detector_.fill(0.0f);
    level_db_.fill(kFloorDb);
    target_db_.fill(0.0f);
    reduction_db_.fill(0.0f);
  }

  void Engine::process(const float* in_left, const float* in_right, float* out_left, float* out_right, int count) {
    if (parameters_.delta) {
      processStft(in_left, in_right, out_left, out_right, count,
                  [](float dry_l, float dry_r, float wet_l, float wet_r, float& out_l, float& out_r) {
                    out_l = dry_l - wet_l;
                    out_r = dry_r - wet_r;
                  });
    }
    else {
      processStft(in_left, in_right, out_left, out_right, count,
                  [](float, float, float wet_l, float wet_r, float& out_l, float& out_r) {
                    out_l = wet_l;
                    out_r = wet_r;
                  });
    }
  }

  void Engine::updateGains(const std::array<float, kNumBins>& power, std::array<float, kNumBins>& gain) {
    const Parameters& p = parameters_;
    const float hop = hopSeconds();
    const float detector_attack = stft::smoothingCoefficient(hop, kDetectorAttackSeconds);
    const float detector_release = stft::smoothingCoefficient(hop, kDetectorReleaseSeconds);
    const float release_factor = std::clamp(p.release, 1.0f, 8.0f);
    const float attack = stft::smoothingCoefficient(hop, kAttackSeconds);
    const float release = stft::smoothingCoefficient(hop, 0.02f * release_factor * std::sqrt(release_factor));
    const float amount = std::clamp(p.amount, 0.0f, 1.0f);
    const float smoothing = std::clamp(p.smoothing, 0.0f, 1.0f);

    if (p.frequency != weight_frequency_ || p.width != weight_width_ || p.slope != weight_slope_) {
      weight_frequency_ = p.frequency; weight_width_ = p.width; weight_slope_ = p.slope;
      const float centre = float(positionToHz(p.frequency, sampleRate()));
      for (int bin = 0; bin < kNumBins; ++bin)
        weight_[bin] = windowWeight(binFrequency(bin), centre, p.width, p.slope);
    }

    for (int bin = 0; bin < kNumBins; ++bin) {
      const float coefficient = power[bin] > detector_[bin] ? detector_attack : detector_release;
      detector_[bin] += (power[bin] - detector_[bin]) * coefficient;
      level_db_[bin] = std::max(kFloorDb, stft::powerToDb(detector_[bin]));
    }
    levels_.load(level_db_.data());

    // What a bin sticks out above its surroundings, scaled by Amount and the window.
    for (int bin = 0; bin < kNumBins; ++bin) {
      if (bin == 0 || weight_[bin] <= 0.0f) {
        target_db_[bin] = 0.0f;
        continue;
      }
      // Below about 560 Hz a resonance is narrower than a bin; above, average
      // three bins against noise. The envelope is the third of an octave on
      // either side, excluding the bins being judged, so a resonance does not
      // raise the level it is compared with.
      const int local_width = bin < 24 ? 0 : 1;
      const int local_count = 2 * local_width + 1;
      const int envelope_width = std::max(8, int(bin * 0.26f + 0.5f));
      const int low = std::max(0, bin - envelope_width), high = std::min(kNumBins - 1, bin + envelope_width);
      const int envelope_count = high - low + 1;
      const float local = levels_.mean(bin - local_width, bin + local_width);
      const float envelope = envelope_count > local_count
          ? (levels_.mean(low, high) * envelope_count - local * local_count) / float(envelope_count - local_count)
          : local;
      const float excess = local - envelope - kKneeDb;
      target_db_[bin] = excess > 0.0f ? std::min(kMaxCutDb, excess * amount) * weight_[bin] : 0.0f;
    }

    // Smoothing spreads a cut over its neighbours (up to a sixth of an octave each side).
    targets_.load(target_db_.data());
    for (int bin = 0; bin < kNumBins; ++bin) {
      const int half_width = int(smoothing * std::max(2.0f, bin * 0.06f) + 0.5f);
      const float target = half_width > 0 ? targets_.mean(bin - half_width, bin + half_width) : target_db_[bin];
      const float coefficient = target > reduction_db_[bin] ? attack : release;
      reduction_db_[bin] += (target - reduction_db_[bin]) * coefficient;
      gain[bin] = stft::dbToGain(-reduction_db_[bin]);
    }
  }
}
