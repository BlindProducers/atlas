// SPDX-License-Identifier: GPL-3.0-or-later
// Maim's fixed-size FIFO: an empty queue hands out zeros, an over-full one
// drops its oldest samples. Adapted from Maim's QueueBuffer.
#pragma once
#include <algorithm>
#include <vector>

namespace atlas::maim {

class QueueBuffer {
 public:
  // Allocates; not for the audio thread.
  void reserve(int capacity) {
    buffer_.assign(static_cast<size_t>(std::max(2, capacity + 1)), 0.0f);
    clear();
  }
  void clear() {
    std::fill(buffer_.begin(), buffer_.end(), 0.0f);
    read_ = write_ = 0;
  }
  int capacity() const { return static_cast<int>(buffer_.size()) - 1; }
  int size() const {
    const int n = static_cast<int>(buffer_.size());
    return (write_ + n - read_) % n;
  }
  void push(float value) {
    const int n = static_cast<int>(buffer_.size());
    buffer_[static_cast<size_t>(write_)] = value;
    write_ = (write_ + 1) % n;
    if (write_ == read_) read_ = (read_ + 1) % n;
  }
  float pop() {
    if (read_ == write_) return 0.0f;
    const float value = buffer_[static_cast<size_t>(read_)];
    read_ = (read_ + 1) % static_cast<int>(buffer_.size());
    return value;
  }
 private:
  std::vector<float> buffer_;
  int read_ = 0, write_ = 0;
};
}
