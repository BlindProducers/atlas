// SPDX-License-Identifier: GPL-3.0-or-later
// The Opus encoder and decoder at 48 kHz in 20 ms frames. Turbo splits a
// frame into 10 or 5 ms packets at random, Error flips bits in the packets,
// and the packet loss model drops packets, which the decoder conceals, or
// repeats the last one. Adapted from Maim's OpusController.
#pragma once
#include "frame_codec.h"
#include "maim_common.h"
#include "packet_loss_model.h"
#include <array>
#include <vector>

struct OpusEncoder;
struct OpusDecoder;

namespace atlas::maim {

class OpusCodec : public FrameCodec {
 public:
  static constexpr int kFrame = 960;  // 20 ms at 48 kHz
  // Opus's own delay at 48 kHz: 2.5 ms of overlap plus 4 ms of lookahead.
  static constexpr int kLookahead = 312;

  ~OpusCodec() override;
  // Builds the encoder and decoder. Allocates; not for the audio thread.
  bool prepare();
  void release();
  bool prepared() const { return encoder_ != nullptr && decoder_ != nullptr; }
  // The lookahead Opus reports, to check kLookahead against.
  int reportedLookahead() const;

  void setParameters(int bitrate_kbps, float turbo, float error, int loss_mode);
  PacketLossModel& packetLoss() { return loss_; }
  int latencySamples() const override { return kFrame + kLookahead; }
  void reset();

 protected:
  void codeFrame(const float* in_left, const float* in_right, float* out_left, float* out_right) override;

 private:
  OpusEncoder* encoder_ = nullptr;
  OpusDecoder* decoder_ = nullptr;
  std::vector<float> input_, output_;
  std::array<unsigned char, 4096> encoded_ {};
  int encoded_bytes_ = 0;
  int bitrate_kbps_ = -1;
  float turbo_ = 0.0f;
  float error_ = 0.0f;
  bool stick_ = false;
  Random random_ { 0x7F4A7C15u };
  PacketLossModel loss_;
};
}
