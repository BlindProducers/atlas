// SPDX-License-Identifier: GPL-3.0-or-later
// BladeEnc's header defines FLOAT, MIN, MAX and PI as macros, so it is
// included last and only here.
#include "mp3_codec.h"
#include "maim_blade.h"

namespace atlas::maim {
namespace {
  float levelShiftFor(float level_shift) { return level_shift > 0.0f ? level_shift / 100.0f : level_shift / 20.0f; }
  float squishFor(float turbo) { const float t = 1.0f - turbo; return t * t * t; }
}

int BladeCodec::closestBitrate(int kbps) const {
  static const int bitrates[] = { 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320 };
  return closest(kbps, bitrates, 14);
}

bool BladeCodec::initEncoder() {
  blade_ = blade_init(sample_rate_, bitrate_);
  if (blade_ == nullptr) return false;
  blade_clear_bends(blade_);
  return true;
}

void BladeCodec::deinitEncoder() {
  if (blade_ != nullptr) {
    blade_deinit(blade_);
    blade_ = nullptr;
  }
}

int BladeCodec::encodeFrame(const float* left, const float* right) {
  return blade_encode_chunk(blade_, const_cast<float*>(left), const_cast<float*>(right),
                            reinterpret_cast<char*>(mp3_buffer_.data()));
}

void BladeCodec::sendBends(const Mp3Bends& b) {
  blade_set_error_bends(blade_, b.error);
  blade_set_butterfly_bends(blade_, b.butterfly_straight, b.butterfly_crossed, b.butterfly_crossed, b.butterfly_straight);
  // Blade has no band step, inversion or window increment bends.
  blade_set_mdct_feedback_bends(blade_, b.feedback);
  blade_set_mdct_post_shift_bends(blade_, b.shift, levelShiftFor(b.level_shift));
  blade_set_bitrate_squish_bends(blade_, squishFor(b.turbo));
  blade_set_threshold_bias_bends(blade_, b.tilt);
  int order[32];
  for (int i = 0; i < 32; ++i) order[i] = b.band_order[static_cast<size_t>(i)];
  blade_set_mdct_band_reassignment_bends(blade_, order);
}
}
