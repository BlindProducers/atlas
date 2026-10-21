// SPDX-License-Identifier: GPL-3.0-or-later
// One bent MP3 encoder (LAME or BladeEnc) with LAME's hip decoder behind it:
// a frame of audio in, the same frame back after its trip through MP3.
// Adapted from Maim's MP3Controller, LameController and BladeController.
#pragma once
#include "maim_common.h"
#include <array>
#include <vector>

struct hip_global_struct;
struct lame_global_struct;
struct encoder_flags_and_data_struct;

namespace atlas::maim {

// Every circuit bend the two encoders take, as Maim's controllers take them.
struct Mp3Bends {
  float error = 0.0f;
  float butterfly_straight = 1.0f;
  float butterfly_crossed = 0.0f;
  int band_step = 18;
  bool band_invert = false;
  int window_step = 64;
  int shift = 0;
  float level_shift = 0.0f;   // -1..1; mapped to the encoders' scale here
  float feedback = 0.0f;
  float turbo = 0.0f;         // 0..1; the encoders take (1 - turbo)^3
  float tilt = 0.0f;          // -1..1
  std::array<int, 32> band_order {};
  Mp3Bends() { for (int i = 0; i < 32; ++i) band_order[static_cast<size_t>(i)] = i; }
  bool operator==(const Mp3Bends& other) const {
    return error == other.error && butterfly_straight == other.butterfly_straight &&
           butterfly_crossed == other.butterfly_crossed && band_step == other.band_step &&
           band_invert == other.band_invert && window_step == other.window_step && shift == other.shift &&
           level_shift == other.level_shift && feedback == other.feedback && turbo == other.turbo &&
           tilt == other.tilt && band_order == other.band_order;
  }
  bool operator!=(const Mp3Bends& other) const { return !(*this == other); }
};

class Mp3Codec {
 public:
  virtual ~Mp3Codec();
  // Builds the encoder and decoder for this rate and bitrate and primes them
  // with silence until the decoder answers every frame. Allocates.
  bool init(int sample_rate, int bitrate_kbps);
  void deinit();
  bool initialized() const { return initialized_; }
  // 1152 samples a channel in; the decoded frame out (either output may be
  // null). False, with silence out, when the codec refused the frame.
  bool processFrame(const float* in_left, const float* in_right, float* out_left, float* out_right);
  void applyBends(const Mp3Bends& bends);
  int bitrate() const { return bitrate_; }
  int sampleRate() const { return sample_rate_; }
  virtual Encoder encoder() const = 0;
  // The delay from input to decoded output, the frame of priming included:
  // Maim's measurements, LAME's corrected by the sample a cross-correlation
  // with sub-sample interpolation finds (see README.atlas.md).
  virtual int latencySamples() const = 0;
  virtual int closestBitrate(int kbps) const = 0;
  // The MP3 rates both encoders take: 32, 44.1 or 48 kHz.
  static int closestSampleRate(int rate);

 protected:
  virtual bool initEncoder() = 0;
  virtual void deinitEncoder() = 0;
  // Encodes a frame into mp3_buffer_; the byte count, or 0 and below on failure.
  virtual int encodeFrame(const float* left, const float* right) = 0;
  virtual void sendBends(const Mp3Bends& bends) = 0;
  static int closest(int target, const int* options, int count);

  int sample_rate_ = 44100;
  int bitrate_ = 112;
  std::vector<unsigned char> mp3_buffer_;

 private:
  void flush();
  hip_global_struct* decoder_ = nullptr;
  std::vector<short> pcm_left_, pcm_right_;
  bool initialized_ = false;
};

class LameCodec : public Mp3Codec {
 public:
  ~LameCodec() override { deinit(); }
  Encoder encoder() const override { return kLame; }
  int latencySamples() const override { return kLatencySamples; }
  int closestBitrate(int kbps) const override;
  static constexpr int kLatencySamples = 2881;
 protected:
  bool initEncoder() override;
  void deinitEncoder() override;
  int encodeFrame(const float* left, const float* right) override;
  void sendBends(const Mp3Bends& bends) override;
 private:
  lame_global_struct* lame_ = nullptr;
};

class BladeCodec : public Mp3Codec {
 public:
  ~BladeCodec() override { deinit(); }
  Encoder encoder() const override { return kBlade; }
  int latencySamples() const override { return kLatencySamples; }
  int closestBitrate(int kbps) const override;
  static constexpr int kLatencySamples = 2209;
 protected:
  bool initEncoder() override;
  void deinitEncoder() override;
  int encodeFrame(const float* left, const float* right) override;
  void sendBends(const Mp3Bends& bends) override;
 private:
  encoder_flags_and_data_struct* blade_ = nullptr;
};
}
