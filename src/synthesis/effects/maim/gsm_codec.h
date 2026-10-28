// SPDX-License-Identifier: GPL-3.0-or-later
// GSM 06.10 full rate, the 13 kbit/s telephone codec, through libgsm at
// 8 kHz in 160-sample frames, one coder a channel. From the GSM controller
// Alessio added to Maim.
#pragma once
#include "frame_codec.h"
#include "maim_common.h"
#include "packet_loss_model.h"
#include <array>

struct gsm_state;

namespace atlas::maim {

class GsmCodec : public FrameCodec {
 public:
  static constexpr int kFrame = 160;
  static constexpr int kBytes = 33;

  ~GsmCodec() override;
  // Allocates the coders; not for the audio thread.
  bool prepare();
  void release();
  bool prepared() const;
  void setParameters(float error, int loss_mode);
  PacketLossModel& packetLoss() { return loss_; }
  int latencySamples() const override { return kFrame; }
  void reset();

 protected:
  void codeFrame(const float* in_left, const float* in_right, float* out_left, float* out_right) override;

 private:
  struct Channel {
    gsm_state* encoder = nullptr;
    gsm_state* decoder = nullptr;
    std::array<short, kFrame> in {};
    std::array<short, kFrame> out {};
    std::array<unsigned char, kBytes> encoded {};
  };
  std::array<Channel, 2> channels_;
  float error_ = 0.0f;
  bool stick_ = false;
  Random random_ { 0x3C6EF372u };
  PacketLossModel loss_;
};
}
