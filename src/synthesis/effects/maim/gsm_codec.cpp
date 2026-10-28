// SPDX-License-Identifier: GPL-3.0-or-later
#include "gsm_codec.h"
#include "maim_gsm.h"
#include <cstring>

namespace atlas::maim {

GsmCodec::~GsmCodec() { release(); }

bool GsmCodec::prepare() {
  release();
  for (auto& channel : channels_) {
    channel.encoder = gsm_create();
    channel.decoder = gsm_create();
    if (channel.encoder == nullptr || channel.decoder == nullptr) {
      release();
      return false;
    }
    channel.encoded.fill(0);
  }
  reserveFrames(kFrame);
  setFrameSize(kFrame, kGsmRate);
  loss_.prepare(kGsmRate);
  return true;
}

void GsmCodec::release() {
  for (auto& channel : channels_) {
    if (channel.encoder != nullptr) gsm_destroy(channel.encoder);
    if (channel.decoder != nullptr) gsm_destroy(channel.decoder);
    channel.encoder = channel.decoder = nullptr;
  }
}

bool GsmCodec::prepared() const { return channels_[0].encoder != nullptr; }

void GsmCodec::reset() {
  loss_.reset();
  resetFraming();
}

void GsmCodec::setParameters(float error, int loss_mode) {
  error_ = error * error * error;
  stick_ = loss_mode == kStick;
}

void GsmCodec::codeFrame(const float* in_left, const float* in_right, float* out_left, float* out_right) {
  const bool delivered = loss_.processPacket(kFrame);
  for (int ch = 0; ch < 2; ++ch) {
    auto& channel = channels_[static_cast<size_t>(ch)];
    const float* in = ch == 0 ? in_left : in_right;
    float* out = ch == 0 ? out_left : out_right;
    if (!prepared()) {
      std::memset(out, 0, sizeof(float) * kFrame);
      continue;
    }
    if (delivered) {
      for (int i = 0; i < kFrame; ++i) {
        const float sample = in[i] < -1.0f ? -1.0f : in[i] > 1.0f ? 1.0f : in[i];
        channel.in[static_cast<size_t>(i)] = static_cast<short>(sample * 32767.0f);
      }
      gsm_encode(channel.encoder, channel.in.data(), channel.encoded.data());
      if (error_ > 0.0f) {
        for (size_t byte = 0; byte < channel.encoded.size(); ++byte) {
          if (random_.nextFloat() < error_) {
            // The high nibble of the first byte is the GSM magic number,
            // which the decoder checks before decoding anything.
            const int bit = byte == 0 ? random_.nextInt(4) : random_.nextInt(8);
            channel.encoded[byte] ^= static_cast<unsigned char>(1 << bit);
          }
        }
      }
    }
    // A lost packet is silence in Drop mode. In Stick mode the last packet
    // that got through is decoded again; the decoder's state moves on, so
    // it stutters rather than holds.
    if ((delivered || stick_) && gsm_decode(channel.decoder, channel.encoded.data(), channel.out.data()) == 0) {
      for (int i = 0; i < kFrame; ++i) out[i] = static_cast<float>(channel.out[static_cast<size_t>(i)]) / 32768.0f;
    }
    else {
      std::memset(out, 0, sizeof(float) * kFrame);
    }
  }
}
}
