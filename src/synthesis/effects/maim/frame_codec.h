// SPDX-License-Identifier: GPL-3.0-or-later
// A codec that works a frame at a time, fed one sample at a time at its own
// rate. The input FIFO is primed with a frame of silence, as Maim primes
// its MP3 controllers, so the output lags the input by one frame plus
// whatever delay the codec itself has.
#pragma once
#include "queue_buffer.h"
#include <vector>

namespace atlas::maim {

class FrameCodec {
 public:
  virtual ~FrameCodec() = default;
  int frameSize() const { return frame_size_; }
  int codecRate() const { return codec_rate_; }

  // One stereo sample in, one out, both at the codec's rate.
  void processSample(float& left, float& right) {
    in_[0].push(left);
    in_[1].push(right);
    while (frame_size_ > 0 && in_[0].size() >= frame_size_) {
      for (int i = 0; i < frame_size_; ++i) {
        frame_in_[0][static_cast<size_t>(i)] = in_[0].pop();
        frame_in_[1][static_cast<size_t>(i)] = in_[1].pop();
      }
      codeFrame(frame_in_[0].data(), frame_in_[1].data(), frame_out_[0].data(), frame_out_[1].data());
      for (int i = 0; i < frame_size_; ++i) {
        out_[0].push(frame_out_[0][static_cast<size_t>(i)]);
        out_[1].push(frame_out_[1][static_cast<size_t>(i)]);
      }
    }
    left = out_[0].pop();
    right = out_[1].pop();
  }

  // Empties the FIFOs and primes the input with a frame of silence again.
  // Safe on the audio thread.
  void resetFraming() {
    for (auto& fifo : in_) {
      fifo.clear();
      for (int i = 0; i < frame_size_; ++i) fifo.push(0.0f);
    }
    for (auto& fifo : out_) fifo.clear();
  }

  // The delay at the codec's rate, the frame included.
  virtual int latencySamples() const = 0;

 protected:
  // Room for frames up to max_frame samples. Allocates; not for the audio thread.
  void reserveFrames(int max_frame) {
    max_frame_ = max_frame;
    for (int ch = 0; ch < 2; ++ch) {
      in_[ch].reserve(2 * max_frame + 8);
      out_[ch].reserve(2 * max_frame + 8);
      frame_in_[ch].assign(static_cast<size_t>(max_frame), 0.0f);
      frame_out_[ch].assign(static_cast<size_t>(max_frame), 0.0f);
    }
  }
  // Within the reserved room; safe on the audio thread. Primes the framing again.
  void setFrameSize(int frame, int codec_rate) {
    frame_size_ = frame > max_frame_ ? max_frame_ : frame;
    codec_rate_ = codec_rate;
    resetFraming();
  }
  virtual void codeFrame(const float* in_left, const float* in_right, float* out_left, float* out_right) = 0;

 private:
  QueueBuffer in_[2], out_[2];
  std::vector<float> frame_in_[2], frame_out_[2];
  int frame_size_ = 0, codec_rate_ = 0, max_frame_ = 0;
};
}
