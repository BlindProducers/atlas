// SPDX-License-Identifier: GPL-3.0-or-later
// Maim's packet loss pattern: a cycle of a set length, of which the first
// part delivers packets and the rest loses them, with optional jitter on
// both lengths. Adapted from Maim's PacketLossModel.
#pragma once
#include "maim_common.h"

namespace atlas::maim {

class PacketLossModel {
 public:
  void prepare(int sample_rate) {
    sample_rate_ = sample_rate;
    reset();
  }
  void reset() {
    counter_ = 0;
    next_one_must_be_on_ = false;
    random_ = Random(0x2545F491u);
    setJitteredLengths();
  }
  // The cycle length in seconds, the delivered share of it, and the jitter.
  void setParameters(float length_seconds, float pulse_width, float jitter) {
    const float cycle = length_seconds * static_cast<float>(sample_rate_);
    const int on = static_cast<int>(cycle * pulse_width);
    const int off = static_cast<int>(cycle * (1.0f - pulse_width));
    if (on == on_length_ && off == off_length_ && jitter == jitter_) return;
    on_length_ = on;
    off_length_ = off;
    jitter_ = jitter;
    setJitteredLengths();
  }
  // Whether a packet of this many samples gets through.
  bool processPacket(int length_in_samples) {
    const bool result = counter_ < on_length_with_jitter_ || next_one_must_be_on_;
    next_one_must_be_on_ = false;
    const int total = on_length_with_jitter_ + off_length_with_jitter_;
    counter_ += length_in_samples;
    if (counter_ > total) {
      counter_ -= total;
      setJitteredLengths();
      next_one_must_be_on_ = true;
    }
    return result;
  }
 private:
  void setJitteredLengths() {
    if (jitter_ > 0.0f) {
      const int on_spread = static_cast<int>(jitter_ * static_cast<float>(on_length_));
      const int off_spread = static_cast<int>(jitter_ * static_cast<float>(off_length_));
      on_length_with_jitter_ = random_.nextInt(on_length_ - on_spread, on_length_ + on_spread + 1);
      off_length_with_jitter_ = random_.nextInt(off_length_ - off_spread, off_length_ + off_spread + 1);
    }
    else {
      on_length_with_jitter_ = on_length_;
      off_length_with_jitter_ = off_length_;
    }
  }
  int sample_rate_ = 44100;
  int counter_ = 0;
  int on_length_ = 0, off_length_ = 0;
  int on_length_with_jitter_ = 0, off_length_with_jitter_ = 0;
  float jitter_ = 0.0f;
  bool next_one_must_be_on_ = false;
  Random random_ { 0x2545F491u };
};
}
