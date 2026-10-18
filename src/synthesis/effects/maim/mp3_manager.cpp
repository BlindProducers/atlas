// SPDX-License-Identifier: GPL-3.0-or-later
#include "mp3_manager.h"
#include <algorithm>
#include <chrono>
#include <cstring>

namespace atlas::maim {

Mp3Manager::Mp3Manager() {
  for (int i = 0; i < 2; ++i) {
    lame_[i] = std::make_unique<LameCodec>();
    blade_[i] = std::make_unique<BladeCodec>();
  }
}

Mp3Manager::~Mp3Manager() { stop(); }

void Mp3Manager::prepare(int codec_rate) {
  stop();
  codec_rate_ = codec_rate;
  reserveFrames(kMp3FrameSize);
  setFrameSize(kMp3FrameSize, codec_rate);
  for (auto& frames : previous_)
    for (auto& frame : frames) frame.assign(static_cast<size_t>(kMp3FrameSize), 0.0f);
  for (auto& frame : new_frame_) frame.assign(static_cast<size_t>(kMp3FrameSize), 0.0f);
  current_ = pending_ = nullptr;
  index_ = 0;
  switch_pending_ = false;
  bends_sent_ = false;
  request_.store(0, std::memory_order_relaxed);
  ready_.store(false, std::memory_order_relaxed);
  stop_.store(false, std::memory_order_relaxed);
  thread_ = std::thread([this] { workerLoop(); });
  prepared_ = true;
}

void Mp3Manager::stop() {
  if (thread_.joinable()) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stop_.store(true, std::memory_order_release);
    }
    signal_.notify_one();
    thread_.join();
  }
  for (int i = 0; i < 2; ++i) {
    lame_[i]->deinit();
    blade_[i]->deinit();
  }
  current_ = pending_ = nullptr;
  switch_pending_ = false;
  prepared_ = false;
}

void Mp3Manager::setTarget(Encoder encoder, int bitrate_kbps, bool realtime) {
  if (!prepared_) return;
  encoder = encoder == kBlade ? kBlade : kLame;
  const int bitrate = codecFor(encoder, 0)->closestBitrate(bitrate_kbps);
  desired_encoder_ = encoder;
  desired_bitrate_ = bitrate;
  realtime_ = realtime;
  if (switch_pending_) return;
  if (current_ != nullptr && current_->encoder() == encoder && current_->bitrate() == bitrate) return;
  // The first encoder takes slot 0; a replacement takes the other slot.
  pending_ = codecFor(encoder, current_ == nullptr ? index_ : 1 - index_);
  switch_pending_ = true;
  ready_.store(false, std::memory_order_release);
  requestBuild(realtime);
}

void Mp3Manager::requestBuild(bool realtime) {
  if (realtime) {
    request_.store(desired_bitrate_, std::memory_order_release);
    signal_.notify_one();
  }
  else {
    pending_->init(codec_rate_, desired_bitrate_);
    ready_.store(true, std::memory_order_release);
  }
}

void Mp3Manager::workerLoop() {
  std::unique_lock<std::mutex> lock(mutex_);
  while (!stop_.load(std::memory_order_acquire)) {
    // The deadline covers a request made between the predicate and the wait:
    // the audio thread never takes this mutex.
    signal_.wait_for(lock, std::chrono::milliseconds(10), [this] {
      return stop_.load(std::memory_order_acquire) || request_.load(std::memory_order_acquire) != 0;
    });
    if (stop_.load(std::memory_order_acquire)) break;
    const int bitrate = request_.exchange(0, std::memory_order_acq_rel);
    if (bitrate == 0) continue;
    lock.unlock();
    pending_->init(codec_rate_, bitrate);
    ready_.store(true, std::memory_order_release);
    lock.lock();
  }
}

void Mp3Manager::setBends(const Mp3Bends& bends) {
  if (bends_sent_ && bends == bends_) return;
  bends_ = bends;
  bends_sent_ = true;
  if (current_ != nullptr) current_->applyBends(bends_);
}

void Mp3Manager::codeFrame(const float* in_left, const float* in_right, float* out_left, float* out_right) {
  const size_t frame_bytes = sizeof(float) * kMp3FrameSize;
  if (switch_pending_ && ready_.load(std::memory_order_acquire)) {
    pending_->applyBends(bends_);
    if (current_ == nullptr) {
      // The first encoder: nothing to blend from, it just starts.
      current_ = pending_;
      current_->processFrame(in_left, in_right, out_left, out_right);
    }
    else {
      // Maim primes the new encoder on the last two frames, then blends
      // from the old encoder to the new one over this frame.
      pending_->processFrame(previous_[0][0].data(), previous_[0][1].data(), nullptr, nullptr);
      pending_->processFrame(previous_[1][0].data(), previous_[1][1].data(), nullptr, nullptr);
      pending_->processFrame(in_left, in_right, new_frame_[0].data(), new_frame_[1].data());
      current_->processFrame(in_left, in_right, out_left, out_right);
      for (int i = 0; i < kMp3FrameSize; ++i) {
        const float mix = static_cast<float>(i) / static_cast<float>(kMp3FrameSize);
        out_left[i] += (new_frame_[0][static_cast<size_t>(i)] - out_left[i]) * mix;
        out_right[i] += (new_frame_[1][static_cast<size_t>(i)] - out_right[i]) * mix;
      }
      current_ = pending_;
      index_ = 1 - index_;
    }
    pending_ = nullptr;
    switch_pending_ = false;
    ready_.store(false, std::memory_order_release);
    if (current_->encoder() != desired_encoder_ || current_->bitrate() != desired_bitrate_) {
      // The target moved on while this encoder was being built: build again.
      pending_ = codecFor(desired_encoder_, 1 - index_);
      switch_pending_ = true;
      requestBuild(realtime_);
    }
  }
  else if (current_ != nullptr) {
    current_->processFrame(in_left, in_right, out_left, out_right);
  }
  else {
    std::memset(out_left, 0, frame_bytes);
    std::memset(out_right, 0, frame_bytes);
  }
  std::memcpy(previous_[0][0].data(), previous_[1][0].data(), frame_bytes);
  std::memcpy(previous_[0][1].data(), previous_[1][1].data(), frame_bytes);
  std::memcpy(previous_[1][0].data(), in_left, frame_bytes);
  std::memcpy(previous_[1][1].data(), in_right, frame_bytes);
}
}
