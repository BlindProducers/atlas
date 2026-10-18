// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 - zsliu98
// This file is adapted from ZLCompressor (https://github.com/ZL-Audio/ZLCompressor,
// commit c11049e), which is free software under the GNU Affero General Public
// License version 3 as published by the Free Software Foundation, and is
// distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
// without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
// PARTICULAR PURPOSE. See the GNU Affero General Public License for details.
//
// The Analogue Compressor: ZLCompressor's compress controller
// (source/zlp/compress_controller.cpp) with its default routing. The side
// chain is the input itself; both are split into mid and side, each detected
// on its own (Mid Side mode, no stereo link), and the gain change is applied
// and the result joined back to left and right before the makeup gain.
// Atlas exposes the style, threshold, ratio, knee, attack, release, makeup
// and wet; everything else stays at ZL's defaults. README.atlas.md lists what
// is left out.
#pragma once

#include "zl_compressor_dsp.h"

#include <array>

namespace atlas::zl {
  // ZL's Compressor Style menu, in its order.
  enum Style { kClean, kClassic, kOptical, kVocal, kNumStyles };

  struct Parameters {
    int style = kClean;
    // The computer, in dB; Knee is ZL's knee width, half the soft knee.
    double threshold = -18.0;
    double ratio = 3.0;
    double knee = 8.0;
    // The follower, in ZL's milliseconds (see PSFollower).
    double attack = 100.0;
    double release = 500.0;
    // Makeup gain in dB, and ZL's Wet from 0 to 1: it scales the gain change,
    // the range and the makeup in dB, so 0 is the dry signal.
    double makeup = 0.0;
    double wet = 1.0;
  };

  class Engine {
    public:
      // ZL's Range: the gain change never exceeds 80 dB.
      static constexpr float kRange = 80.0f;
      // ZL's output gain ramps linearly at one unit of gain per 100 ms.
      static constexpr double kGainRampSeconds = 0.1;

      Engine();

      void prepare(double sample_rate);
      void reset();
      void setParameters(const Parameters& parameters);
      // Any block length; the input and output may be the same buffers.
      void process(const float* in_left, const float* in_right, float* out_left, float* out_right, int samples);

      int style() const { return style_; }

    private:
      static constexpr int kChunk = 256;

      // Starts the style's followers from the gain change of the last sample
      // (zero after a reset), so changing style does not jump the gain.
      void seedStyle();
      // The level whose gain change is reduction_db: the Optical follower holds
      // a level, not a gain change.
      float levelForReduction(float reduction_db) const;
      void processChunk(const float* in_left, const float* in_right, float* out_left, float* out_right, int samples);

      double sample_rate_ = 48000.0;
      int style_ = kClean;
      CompressionComputer<float> computer_;
      std::array<PSFollower<float>, 2> follower_ {};
      std::array<ClassicCompressor<float>, 2> classic_ {};
      std::array<VocalCompressor<float>, 2> vocal_ {};
      std::array<float, 2> last_reduction_db_ {};
      bool seed_pending_ = false;
      // The gain change in dB times wet / 20 * ln 10, for exp().
      float gain_scale_ = 0.0f;
      float range_ = kRange;
      float gain_target_ = 1.0f;
      float gain_current_ = 1.0f;
      float gain_increment_ = 0.0f;
  };
} // namespace atlas::zl
