// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 - zsliu98
// This file is adapted from ZLCompressor (https://github.com/ZL-Audio/ZLCompressor,
// commit c11049e), which is free software under the GNU Affero General Public
// License version 3 as published by the Free Software Foundation, and is
// distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
// without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
// PARTICULAR PURPOSE. See the GNU Affero General Public License for details.
#include "zl_compressor_engine.h"

#include <algorithm>
#include <cmath>

namespace atlas::zl {
  namespace {
    constexpr float kLn10 = 2.30258509299404568402f;
  }

  Engine::Engine() {
    prepare(sample_rate_);
    setParameters(Parameters {});
    reset();
  }

  void Engine::prepare(double sample_rate) {
    sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
    for (auto& follower : follower_)
      follower.prepare(sample_rate_);
    gain_increment_ = static_cast<float>(1.0 / (sample_rate_ * kGainRampSeconds));
  }

  void Engine::reset() {
    last_reduction_db_ = {};
    seed_pending_ = false;
    seedStyle();
    gain_current_ = gain_target_;
  }

  // compress_controller.cpp resets the chosen style's followers to no gain
  // change whenever the style changes, which jumps the gain while compressing.
  // Atlas starts them from the gain change they were applying instead; after a
  // reset that is zero, as in ZL.
  void Engine::seedStyle() {
    for (int channel = 0; channel < 2; ++channel) {
      auto& follower = follower_[channel];
      const float reduction = std::min(0.0f, last_reduction_db_[channel]);
      switch (style_) {
        case kClassic:
          classic_[channel].reset(follower);
          follower.reset(-reduction);
          break;
        case kOptical:
          OpticalCompressor<float>::reset(follower);
          follower.reset(levelForReduction(reduction));
          break;
        case kVocal:
          vocal_[channel].reset(follower);
          follower.reset(-chore::decibelsToGain(reduction));
          break;
        default:
          CleanCompressor<float>::reset(follower);
          follower.reset(-reduction);
          break;
      }
    }
  }

  float Engine::levelForReduction(float reduction_db) const {
    if (reduction_db >= 0.0f)
      return 0.0f;
    // The computer's gain change falls as the level rises: bisect for it.
    float low = -240.0f, high = 40.0f;
    if (computer_.eval(high) > reduction_db)
      return chore::decibelsToGain(high);
    for (int i = 0; i < 40; ++i) {
      const float middle = 0.5f * (low + high);
      if (computer_.eval(middle) > reduction_db)
        low = middle;
      else
        high = middle;
    }
    return chore::decibelsToGain(0.5f * (low + high));
  }

  void Engine::setParameters(const Parameters& p) {
    const int style = std::clamp(p.style, 0, kNumStyles - 1);
    if (style != style_) {
      style_ = style;
      seed_pending_ = true;
    }
    // compress_attach.cpp: the computer takes the dB values as they are, and
    // only the first follower is set; the second copies it each block.
    computer_.setThreshold(static_cast<float>(p.threshold));
    computer_.setRatio(static_cast<float>(p.ratio));
    computer_.setKneeW(static_cast<float>(p.knee));
    follower_[0].setAttack(static_cast<float>(p.attack));
    follower_[0].setRelease(static_cast<float>(p.release));

    // compress_controller.cpp's prepareBuffer: Wet scales the gain change
    // (0.05 turns dB into a power of ten), the range and the makeup in dB.
    const float wet = static_cast<float>(std::clamp(p.wet, 0.0, 1.0));
    gain_scale_ = wet * 0.05f * kLn10;
    range_ = kRange * wet;
    gain_target_ = chore::decibelsToGain(static_cast<float>(p.makeup) * wet);
  }

  void Engine::process(const float* in_left, const float* in_right, float* out_left, float* out_right, int samples) {
    for (int offset = 0; offset < samples; offset += kChunk) {
      const int count = std::min(kChunk, samples - offset);
      processChunk(in_left + offset, in_right + offset, out_left + offset, out_right + offset, count);
    }
  }

  void Engine::processChunk(const float* in_left, const float* in_right, float* out_left, float* out_right,
                            int samples) {
    const size_t num_samples = static_cast<size_t>(samples);
    std::array<float, kChunk> mid, side, side_chain_mid, side_chain_side;

    // The side chain is a copy of the input; both are split into mid and side
    // (InplaceMSSplitter, GainMode::kPre).
    for (size_t i = 0; i < num_samples; ++i) {
      const float m = 0.5f * (in_left[i] + in_right[i]);
      const float s = m - in_right[i];
      mid[i] = side_chain_mid[i] = m;
      side[i] = side_chain_side[i] = s;
    }

    if (follower_[0].prepareBuffer())
      follower_[1].copyFrom(follower_[0]);
    computer_.prepareBuffer();
    if (seed_pending_) {
      seedStyle();
      seed_pending_ = false;
    }

    switch (style_) {
      case kClassic:
        classic_[0].process(computer_, follower_[0], side_chain_mid.data(), num_samples);
        classic_[1].process(computer_, follower_[1], side_chain_side.data(), num_samples);
        break;
      case kOptical:
        OpticalCompressor<float>::process(computer_, follower_[0], side_chain_mid.data(), num_samples);
        OpticalCompressor<float>::process(computer_, follower_[1], side_chain_side.data(), num_samples);
        break;
      case kVocal:
        vocal_[0].process(computer_, follower_[0], side_chain_mid.data(), num_samples);
        vocal_[1].process(computer_, follower_[1], side_chain_side.data(), num_samples);
        break;
      default:
        CleanCompressor<float>::process(computer_, follower_[0], side_chain_mid.data(), num_samples);
        CleanCompressor<float>::process(computer_, follower_[1], side_chain_side.data(), num_samples);
        break;
    }

    last_reduction_db_ = { side_chain_mid[num_samples - 1], side_chain_side[num_samples - 1] };

    // appleSideBuffer: clamp the gain change to the range, turn it into a gain
    // and apply it. The stereo link at 0% leaves each channel's change alone.
    const float range_low = -range_;
    const float range_high = std::min(40.0f, range_);
    for (size_t i = 0; i < num_samples; ++i) {
      mid[i] *= std::exp(std::clamp(side_chain_mid[i], range_low, range_high) * gain_scale_);
      side[i] *= std::exp(std::clamp(side_chain_side[i], range_low, range_high) * gain_scale_);
    }

    // Join mid and side back to left and right, then the makeup gain.
    for (size_t i = 0; i < num_samples; ++i) {
      if (gain_current_ < gain_target_)
        gain_current_ = std::min(gain_target_, gain_current_ + gain_increment_);
      else if (gain_current_ > gain_target_)
        gain_current_ = std::max(gain_target_, gain_current_ - gain_increment_);
      out_left[i] = (mid[i] + side[i]) * gain_current_;
      out_right[i] = (mid[i] - side[i]) * gain_current_;
    }
  }
} // namespace atlas::zl
