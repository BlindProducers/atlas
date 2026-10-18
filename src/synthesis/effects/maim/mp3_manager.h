// SPDX-License-Identifier: GPL-3.0-or-later
// The MP3 side of Maim: frames of 1152 samples through the current encoder,
// and a change of encoder or bitrate handled as Maim does it, with a second
// encoder primed on the last two frames and crossfaded in over one frame.
// Unlike Maim, the second encoder is built on its own thread while the
// current one keeps playing, so the audio thread never allocates.
// Adapted from Maim's Mp3ControllerManager.
#pragma once
#include "frame_codec.h"
#include "mp3_codec.h"
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace atlas::maim {

class Mp3Manager : public FrameCodec {
 public:
  Mp3Manager();
  ~Mp3Manager() override;

  // Allocates the encoder pairs for this rate and starts the build thread.
  // Nothing is encoded until setTarget names an encoder. Not for the audio thread.
  void prepare(int codec_rate);
  // Joins the build thread and frees the encoders.
  void stop();
  bool prepared() const { return prepared_; }

  // Audio thread: the encoder and bitrate to run. A change builds the new
  // encoder off thread (in realtime) or right here (offline), then swaps it
  // in at the next frame.
  void setTarget(Encoder encoder, int bitrate_kbps, bool realtime);
  void setBends(const Mp3Bends& bends);
  // True once the encoder setTarget asked for is the one coding frames.
  bool settled() const { return current_ != nullptr && !switch_pending_; }
  Encoder currentEncoder() const { return current_ ? current_->encoder() : desired_encoder_; }
  int currentBitrate() const { return current_ ? current_->bitrate() : desired_bitrate_; }
  int latencySamples() const override { return latencyFor(currentEncoder()); }
  static int latencyFor(Encoder encoder) {
    return encoder == kBlade ? BladeCodec::kLatencySamples : LameCodec::kLatencySamples;
  }

 protected:
  void codeFrame(const float* in_left, const float* in_right, float* out_left, float* out_right) override;

 private:
  void requestBuild(bool realtime);
  void buildPending();
  void workerLoop();
  Mp3Codec* codecFor(Encoder encoder, int index) {
    return (encoder == kBlade ? blade_[index] : lame_[index]).get();
  }

  std::unique_ptr<Mp3Codec> lame_[2], blade_[2];
  Mp3Codec* current_ = nullptr;
  Mp3Codec* pending_ = nullptr;
  int index_ = 0;
  Encoder desired_encoder_ = kLame;
  int desired_bitrate_ = 112;
  bool switch_pending_ = false;
  bool prepared_ = false;
  bool realtime_ = false;
  int codec_rate_ = 44100;
  Mp3Bends bends_;
  bool bends_sent_ = false;
  // The last two frames, to prime a new encoder with.
  std::vector<float> previous_[2][2];
  std::vector<float> new_frame_[2];

  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable signal_;
  std::atomic<int> request_ { 0 };     // the bitrate to build pending_ for, 0 for none
  std::atomic<bool> ready_ { false };  // pending_ is built
  std::atomic<bool> stop_ { false };
};
}
