// SPDX-License-Identifier: GPL-3.0-or-later
// Shared types of the Maim effect: the encoders, the choices the editor
// offers, every setting the engine takes, and a small deterministic random
// generator. Adapted from Maim by Arden Butterfield (GPL v3); see README.atlas.md.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace atlas::maim {

// In Maim's order: Blade and LAME are MP3 encoders, the rest their own codecs.
enum Encoder { kBlade, kLame, kOpus, kGsm, kLpc, kNumEncoders };

// What the decoder does with a packet that was lost: silence (Opus conceals
// it), or decode the last packet that got through again.
enum LossMode { kDrop, kStick, kNumLossModes };

constexpr int kNumBitrates = 17;
constexpr std::array<int, kNumBitrates> kBitrates {
  8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320 };
constexpr int kDefaultBitrateIndex = 10;  // 112 kbps

// The LPC vocoder's sample rates, in Hz. LPC-10 runs at 8 kHz.
constexpr int kNumLpcRates = 10;
constexpr std::array<int, kNumLpcRates> kLpcRates {
  2000, 3000, 4000, 5000, 6000, 8000, 11025, 12000, 16000, 22050 };
constexpr int kDefaultLpcRateIndex = 5;

constexpr int kMp3FrameSize = 1152;
constexpr int kOpusRate = 48000;
constexpr int kGsmRate = 8000;

inline bool isMp3(int encoder) { return encoder == kBlade || encoder == kLame; }

// Every setting, as the engine reads them once per block. Levels are plain
// fractions, gains in decibels, frequencies in hertz.
struct Parameters {
  int encoder = kLame;
  int bitrate_index = kDefaultBitrateIndex;
  int lpc_rate_index = kDefaultLpcRateIndex;
  float turbo = 0.0f;         // 0..1: squeezes the MP3 frames, splits Opus frames
  float tilt = 0.0f;          // -1..1: biases the psychoacoustic threshold
  float error = 0.0f;         // 0..1: shuffles MP3 lines, flips Opus, GSM and LPC bits
  float mix = 1.0f;
  int shift = 0;              // -100..100 MDCT lines
  float level_shift = 0.0f;   // -1..1
  float feedback = 0.0f;      // 0..1
  float butterfly_straight = 1.0f;
  float butterfly_crossed = 0.0f;
  int band_step = 18;         // 8..18, LAME only
  bool band_invert = false;   // LAME only
  int window_step = 64;       // -64..64, LAME only
  int band_shift = 0;         // -31..31 MDCT bands
  bool band_reverse = false;
  float loss_period = 2.05f;  // seconds, one loss cycle
  float loss_width = 1.0f;    // 0..1, the share of the cycle that gets through
  float loss_jitter = 0.0f;   // 0..1
  int loss_mode = kDrop;
  float drive_db = 0.0f;
  float makeup_db = 0.0f;
  float low_cut_hz = 10.0f;
  float high_cut_hz = 18000.0f;
};

// xorshift32: one per instance, so two Maim effects never share a generator
// and a preset's errors fall the same way on every platform.
class Random {
 public:
  explicit Random(uint32_t seed = 0x9E3779B9u) : state_(seed ? seed : 0x9E3779B9u) {}
  uint32_t next() {
    uint32_t x = state_;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    state_ = x;
    return x;
  }
  // [0, 1)
  float nextFloat() { return static_cast<float>(next() >> 8) * (1.0f / 16777216.0f); }
  // [0, n)
  int nextInt(int n) { return n <= 1 ? 0 : static_cast<int>(next() % static_cast<uint32_t>(n)); }
  // [low, high)
  int nextInt(int low, int high) { return high <= low ? low : low + nextInt(high - low); }
 private:
  uint32_t state_;
};

// Transposed direct form II biquad with RBJ's low and high pass shapes,
// Q of 1/sqrt(2): the filters Maim builds with JUCE's IIRCoefficients.
struct Biquad {
  float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
  void lowPass(float rate, float cutoff);
  void highPass(float rate, float cutoff);
  void reset() { z1 = z2 = 0; }
  float process(float x) {
    const float y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
  }
};

inline float dbToGain(float db) { return std::pow(10.0f, db * 0.05f); }
inline float gainToDb(float gain, float floor_db) {
  return gain > 0.0f ? std::max(floor_db, 20.0f * std::log10(gain)) : floor_db;
}
inline bool finite(float value) { return std::isfinite(value); }
}
