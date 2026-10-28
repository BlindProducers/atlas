// SPDX-License-Identifier: GPL-3.0-or-later
#include "mp3_codec.h"
#include "maim_lame.h"
#include <cstring>
#include <limits>

namespace atlas::maim {
namespace {
  constexpr int kDecodedCapacity = 20000;
  float pcmToFloat(short sample) { return static_cast<float>(sample) / static_cast<float>(std::numeric_limits<short>::max()); }
  // Maim's scale for the level shift: a hundredth up, a twentieth down.
  float levelShiftFor(float level_shift) { return level_shift > 0.0f ? level_shift / 100.0f : level_shift / 20.0f; }
  float squishFor(float turbo) { const float t = 1.0f - turbo; return t * t * t; }
}

Mp3Codec::~Mp3Codec() {
  // The encoder is gone by now: each subclass's destructor releases it,
  // while deinitEncoder can still reach it.
  if (decoder_ != nullptr) hip_decode_exit(decoder_);
}

int Mp3Codec::closest(int target, const int* options, int count) {
  int lower = options[0], upper = options[count - 1];
  for (int i = 0; i < count; ++i) {
    if (options[i] < target) lower = options[i];
    else {
      upper = options[i];
      return (target - lower) < (upper - target) ? lower : upper;
    }
  }
  return lower;
}

int Mp3Codec::closestSampleRate(int rate) {
  static const int rates[] = { 32000, 44100, 48000 };
  return closest(rate, rates, 3);
}

bool Mp3Codec::init(int sample_rate, int bitrate_kbps) {
  deinit();
  sample_rate_ = closestSampleRate(sample_rate);
  bitrate_ = closestBitrate(bitrate_kbps);
  // LAME's rule of thumb: 1.25 bytes a sample plus 7200.
  mp3_buffer_.assign(static_cast<size_t>(kMp3FrameSize * 5 / 4 + 7200), 0);
  pcm_left_.assign(kDecodedCapacity, 0);
  pcm_right_.assign(kDecodedCapacity, 0);
  if (!initEncoder()) return false;
  decoder_ = hip_decode_init();
  if (decoder_ == nullptr) {
    deinitEncoder();
    return false;
  }
  flush();
  initialized_ = true;
  return true;
}

void Mp3Codec::deinit() {
  initialized_ = false;
  deinitEncoder();
  if (decoder_ != nullptr) {
    hip_decode_exit(decoder_);
    decoder_ = nullptr;
  }
}

void Mp3Codec::flush() {
  // Silence through the encoder until the decoder gives a frame back, so
  // that from here on every encoded frame decodes to one frame of audio.
  std::vector<float> silence(static_cast<size_t>(kMp3FrameSize), 0.0f);
  for (int attempt = 0; attempt < 16; ++attempt) {
    const int bytes = encodeFrame(silence.data(), silence.data());
    const int decoded = hip_decode(decoder_, mp3_buffer_.data(), static_cast<size_t>(bytes < 0 ? 0 : bytes),
                                   pcm_left_.data(), pcm_right_.data());
    if (decoded != 0) break;
  }
}

bool Mp3Codec::processFrame(const float* in_left, const float* in_right, float* out_left, float* out_right) {
  const auto silence = [&] {
    if (out_left) std::memset(out_left, 0, sizeof(float) * kMp3FrameSize);
    if (out_right) std::memset(out_right, 0, sizeof(float) * kMp3FrameSize);
  };
  if (!initialized_) {
    silence();
    return false;
  }
  const int bytes = encodeFrame(in_left, in_right);
  if (bytes <= 0) {
    silence();
    return false;
  }
  const int decoded = hip_decode(decoder_, mp3_buffer_.data(), static_cast<size_t>(bytes),
                                 pcm_left_.data(), pcm_right_.data());
  if (decoded <= 0) {
    silence();
    return false;
  }
  const int count = decoded < kMp3FrameSize ? decoded : kMp3FrameSize;
  for (int ch = 0; ch < 2; ++ch) {
    float* out = ch == 0 ? out_left : out_right;
    const short* pcm = ch == 0 ? pcm_left_.data() : pcm_right_.data();
    if (out == nullptr) continue;
    for (int i = 0; i < count; ++i) out[i] = pcmToFloat(pcm[i]);
    for (int i = count; i < kMp3FrameSize; ++i) out[i] = 0.0f;
  }
  return true;
}

void Mp3Codec::applyBends(const Mp3Bends& bends) {
  if (initialized_) sendBends(bends);
}

// ------------------------------------------------------------------ LAME

int LameCodec::closestBitrate(int kbps) const {
  // 8 to 24 kbps exist only for MPEG-2's rates; at 32 kHz and above LAME
  // takes the nearest it has, 32.
  static const int bitrates[] = { 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320 };
  return closest(kbps, bitrates, 17);
}

bool LameCodec::initEncoder() {
  lame_ = lame_init();
  if (lame_ == nullptr) return false;
  lame_clear_bends(lame_);
  lame_set_in_samplerate(lame_, sample_rate_);
  lame_set_out_samplerate(lame_, sample_rate_);
  lame_set_brate(lame_, bitrate_);
  // Constant bitrate with no bit reservoir, to cut down on latency (Maim).
  lame_set_VBR(lame_, vbr_off);
  lame_set_disable_reservoir(lame_, 1);
  if (lame_init_params(lame_) != 0) {
    lame_close(lame_);
    lame_ = nullptr;
    return false;
  }
  return true;
}

void LameCodec::deinitEncoder() {
  if (lame_ != nullptr) {
    lame_close(lame_);
    lame_ = nullptr;
  }
}

int LameCodec::encodeFrame(const float* left, const float* right) {
  return lame_encode_buffer_ieee_float(lame_, left, right, kMp3FrameSize, mp3_buffer_.data(),
                                       static_cast<int>(mp3_buffer_.size()));
}

void LameCodec::sendBends(const Mp3Bends& b) {
  lame_set_error_bends(lame_, b.error);
  lame_set_butterfly_bends(lame_, b.butterfly_straight, b.butterfly_crossed, b.butterfly_crossed, b.butterfly_straight);
  lame_set_mdct_step_bends(lame_, b.band_invert ? 1 : 0, b.band_step);
  lame_set_mdct_feedback_bends(lame_, b.feedback);
  lame_set_mdct_post_shift_bends(lame_, b.shift, levelShiftFor(b.level_shift));
  lame_set_mdct_window_increment_bends(lame_, b.window_step);
  lame_set_bitrate_squish_bends(lame_, squishFor(b.turbo));
  lame_set_threshold_bias_bends(lame_, b.tilt);
  int order[32];
  for (int i = 0; i < 32; ++i) order[i] = b.band_order[static_cast<size_t>(i)];
  lame_set_mdct_band_reassignment_bends(lame_, order);
}
}
