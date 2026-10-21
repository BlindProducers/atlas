// SPDX-License-Identifier: GPL-3.0-or-later
// An LPC-10 style vocoder at 2.4 kbit/s. Each 22.5 ms frame is squeezed
// down to a tenth-order all-pole filter, a loudness and a pitch (or a flag
// saying the frame is noise); the decoder runs a pulse train or white noise
// through that filter. LPC-10 runs at 8 kHz; this one runs anywhere from 2
// to 22.05 kHz, keeping its frames, windows and pitch range the same length
// in time. From the LPC controller Alessio added to Maim.
#pragma once
#include "frame_codec.h"
#include "maim_common.h"
#include "packet_loss_model.h"
#include <array>
#include <cstdint>
#include <vector>

namespace atlas::maim {

class LpcCodec : public FrameCodec {
 public:
  static constexpr int kOrder = 10;
  static constexpr int kPitchBits = 7;
  static constexpr int kRmsBits = 5;
  static constexpr std::array<int, kOrder> kReflectionBits { 5, 5, 5, 5, 4, 4, 4, 4, 3, 2 };
  static constexpr int kFrameBits = 53;
  static constexpr int kFrameBytes = (kFrameBits + 7) / 8;
  using Packed = std::array<uint8_t, kFrameBytes>;

  // What travels over the wire, before packing into bits.
  struct Frame {
    int pitch = 0;  // 0 for an unvoiced frame, otherwise a log-spaced pitch period
    int rms = 0;    // 0 for silence, otherwise a loudness in 2.75 dB steps
    std::array<int, kOrder> reflection {};  // quantized log area ratios
  };

  // The pitch range, as periods in samples at 8 kHz: 400 Hz down to 51 Hz.
  static constexpr int kMinPitchPeriod8k = 20;
  static constexpr int kMaxPitchPeriod8k = 156;

  // 22.5 ms frames: 44.4 a second, which at 53 bits a frame is 2.4 kbit/s.
  static int frameSizeFor(int codec_rate) { return static_cast<int>(std::lround(codec_rate * 0.0225)); }
  // The codec can't run faster than the host, so the host's rate caps the choice.
  static int rateFor(int rate_index, int host_rate) {
    const int index = rate_index < 0 ? 0 : rate_index >= kNumLpcRates ? kNumLpcRates - 1 : rate_index;
    const int rate = kLpcRates[static_cast<size_t>(index)];
    return rate < host_rate ? rate : host_rate;
  }

  static Packed pack(const Frame& frame);
  static Frame unpack(const Packed& packed);
  static float pitchPeriod8kFor(int code);

  // Allocates for the highest rate; not for the audio thread.
  void prepare();
  bool prepared() const { return !window_.empty(); }
  // Safe on the audio thread: resets the vocoder for this rate.
  void setRate(int codec_rate);
  void setParameters(float error, int loss_mode);
  PacketLossModel& packetLoss() { return loss_; }
  int latencySamples() const override { return frameSize(); }
  void reset();

 protected:
  void codeFrame(const float* in_left, const float* in_right, float* out_left, float* out_right) override;

 private:
  struct Encoder {
    std::vector<float> emphasized;  // for the spectral envelope
    std::vector<float> lowpassed;   // for the pitch search
    float previous_input = 0;
    Biquad pitch_filter;
  };
  struct Decoder {
    std::array<float, kOrder> reflection {};
    float gain = 0;
    std::array<float, kOrder + 1> lattice {};
    float pulse_phase = 0;
    float previous_output = 0;
  };
  struct Channel {
    Encoder encoder;
    Decoder decoder;
    Packed encoded {};
  };

  // Lengths in seconds, which setRate turns into samples at the codec's rate.
  // The pitch search looks further back than the LPC window, so keep more history.
  static constexpr double kHistorySeconds = 0.04;
  static constexpr double kWindowSeconds = 0.03;
  static int samplesFor(double seconds, int rate) { return static_cast<int>(std::lround(seconds * rate)); }

  Frame analyze(Encoder& encoder, const float* in);
  void synthesize(Decoder& decoder, const Frame& frame, float* out);
  int estimatePitchPeriod(const std::vector<float>& signal);

  std::array<Channel, 2> channels_;
  int rate_ = 0, frame_size_ = 0, history_length_ = 0, window_length_ = 0;
  int min_pitch_period_ = 0, max_pitch_period_ = 0, interpolation_samples_ = 0;
  float preemphasis_ = 0.9375f;
  std::vector<float> window_, windowed_, correlation_;
  float error_ = 0.0f;
  bool stick_ = false;
  Random random_ { 0x1B873593u };
  PacketLossModel loss_;
};
}
