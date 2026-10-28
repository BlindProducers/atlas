// SPDX-License-Identifier: GPL-3.0-or-later
#include "opus_codec.h"
#include "opus.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace atlas::maim {

OpusCodec::~OpusCodec() { release(); }

bool OpusCodec::prepare() {
  release();
  int error = 0;
  encoder_ = opus_encoder_create(kOpusRate, 2, OPUS_APPLICATION_VOIP, &error);
  if (error != OPUS_OK || encoder_ == nullptr) {
    release();
    return false;
  }
  decoder_ = opus_decoder_create(kOpusRate, 2, &error);
  if (error != OPUS_OK || decoder_ == nullptr) {
    release();
    return false;
  }
  input_.assign(static_cast<size_t>(kFrame) * 2, 0.0f);
  output_.assign(static_cast<size_t>(kFrame) * 2, 0.0f);
  reserveFrames(kFrame);
  setFrameSize(kFrame, kOpusRate);
  loss_.prepare(kOpusRate);
  bitrate_kbps_ = -1;
  encoded_bytes_ = 0;
  return true;
}

void OpusCodec::release() {
  if (encoder_ != nullptr) opus_encoder_destroy(encoder_);
  if (decoder_ != nullptr) opus_decoder_destroy(decoder_);
  encoder_ = nullptr;
  decoder_ = nullptr;
}

int OpusCodec::reportedLookahead() const {
  opus_int32 lookahead = 0;
  if (encoder_ != nullptr) opus_encoder_ctl(encoder_, OPUS_GET_LOOKAHEAD(&lookahead));
  return static_cast<int>(lookahead);
}

void OpusCodec::reset() {
  if (!prepared()) return;
  opus_encoder_ctl(encoder_, OPUS_RESET_STATE);
  opus_decoder_ctl(decoder_, OPUS_RESET_STATE);
  std::fill(output_.begin(), output_.end(), 0.0f);
  encoded_bytes_ = 0;
  loss_.reset();
  resetFraming();
}

void OpusCodec::setParameters(int bitrate_kbps, float turbo, float error, int loss_mode) {
  if (!prepared()) return;
  if (bitrate_kbps != bitrate_kbps_) {
    bitrate_kbps_ = bitrate_kbps;
    // Maim's scale: half the nominal rate, in bits a second.
    opus_encoder_ctl(encoder_, OPUS_SET_BITRATE(bitrate_kbps * 500));
  }
  turbo_ = turbo;
  error_ = error * error * error;
  stick_ = loss_mode == kStick;
}

void OpusCodec::codeFrame(const float* in_left, const float* in_right, float* out_left, float* out_right) {
  if (!prepared()) {
    std::memset(out_left, 0, sizeof(float) * kFrame);
    std::memset(out_right, 0, sizeof(float) * kFrame);
    return;
  }
  for (int i = 0; i < kFrame; ++i) {
    input_[static_cast<size_t>(i) * 2] = in_left[i];
    input_[static_cast<size_t>(i) * 2 + 1] = in_right[i];
  }
  int split = static_cast<int>(std::lround(random_.nextFloat() * turbo_ * 3.0f));
  split = 1 << std::min(std::max(0, split), 2);
  const int packet = kFrame / split;
  for (int s = 0; s < split; ++s) {
    float* out = output_.data() + static_cast<size_t>(s * packet) * 2;
    const float* in = input_.data() + static_cast<size_t>(s * packet) * 2;
    float max_in = 0.0f;
    for (int i = 0; i < packet * 2; ++i) max_in = std::max(max_in, std::abs(in[i]));

    const bool delivered = loss_.processPacket(packet);
    if (delivered) {
      const int bytes = opus_encode_float(encoder_, in, packet, encoded_.data(), static_cast<opus_int32>(encoded_.size()));
      encoded_bytes_ = bytes > 0 ? bytes : 0;
      if (error_ > 0.0f) {
        for (int byte = 0; byte < encoded_bytes_; ++byte) {
          if (random_.nextFloat() < error_)
            encoded_[static_cast<size_t>(byte)] ^= static_cast<unsigned char>(1 << random_.nextInt(8));
        }
      }
    }
    // A lost packet repeats the last one in Stick mode; in Drop mode the
    // decoder conceals it from what it heard before.
    const bool decode_last = delivered || stick_;
    const int decoded = opus_decode_float(decoder_, decode_last && encoded_bytes_ > 0 ? encoded_.data() : nullptr,
                                          decode_last ? encoded_bytes_ : 0, out, packet, 0);
    if (decoded != packet) std::memset(out, 0, sizeof(float) * static_cast<size_t>(packet) * 2);

    // A corrupted packet can decode far louder than the input; hold it near.
    float max_out = 0.0f;
    for (int i = 0; i < packet * 2; ++i) max_out = std::max(max_out, std::abs(out[i]));
    const float ceiling = std::min(1.0f, max_in * 1.5f);
    if (max_out > ceiling && max_out > 1.0e-4f) {
      const float gain = ceiling / max_out;
      for (int i = 0; i < packet * 2; ++i) out[i] *= gain;
    }
  }
  for (int i = 0; i < kFrame; ++i) {
    out_left[i] = output_[static_cast<size_t>(i) * 2];
    out_right[i] = output_[static_cast<size_t>(i) * 2 + 1];
  }
}
}
