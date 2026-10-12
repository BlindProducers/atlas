// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from setBfree's vibrato.c as used by OpenB3 (GPL v2 or later):
// Copyright (C) 2003-2004 Fredrik Kilander, 2008-2018 Robin Gareus,
// 2012 Will Panther. See README.atlas.md.
#include "scanner_vibrato.h"

#include <algorithm>
#include <cmath>

namespace atlas::openb3 {
  namespace {
    constexpr double kPi = 3.14159265358979323846;
    constexpr unsigned int kTableMask = 0x07ffffff;   // INCTBL_MASK, fixed point
    constexpr unsigned int kBufferMask = 0x000003FF;  // BUF_MASK_SAMPLES
    constexpr unsigned int kPositionMask = 0x03FFFFFF; // BUF_MASK_POSN, fixed point
    // vib1OffAmp, vib2OffAmp, vib3OffAmp.
    constexpr double kSwing[3] = { 3.0, 6.0, 9.0 };
  }

  ScannerVibrato::ScannerVibrato() {
    prepare(kReferenceRate);
  }

  void ScannerVibrato::prepare(double sample_rate) {
    sample_rate_ = sample_rate;
    buildTables();
    setFrequency(hz_);
    reset();
  }

  // initIncrementTables(): fixed-point write offsets from the read position,
  // ahead for half a scanner cycle and behind for the other half.
  void ScannerVibrato::buildTables() {
    const double S = 65536.0;
    const double scale = sample_rate_ / kReferenceRate;
    for (int t = 0; t < 3; ++t) {
      const double swing = kSwing[t] * scale;
      for (int i = 0; i < kTableSize; ++i) {
        const double m = std::sin((2.0 * kPi * i) / kTableSize);
        offsets_[static_cast<size_t>(t)][static_cast<size_t>(i)] =
            static_cast<unsigned int>((1.0 + swing + (m * swing)) * S);
      }
    }
  }

  void ScannerVibrato::reset() {
    stator_ = 0;
    // reset_vibrato(): the reader starts half the buffer away from the writes.
    out_position_ = kBufferMask / 2;
    for (auto& buffer : buffers_)
      buffer.fill(0.0f);
  }

  void ScannerVibrato::setMode(int mode) {
    mode = std::clamp(mode, 0, kNumScannerModes - 1);
    table_ = mode % 3;
    chorus_ = mode >= kChorus1;
  }

  // setScannerFrequency().
  void ScannerVibrato::setFrequency(double hz) {
    hz_ = std::clamp(hz, kMinimumHz, kMaximumHz);
    stator_increment_ = static_cast<unsigned int>(((hz_ * kTableSize) / sample_rate_) * 65536.0);
  }

  // vibratoProc(), one delay line per channel.
  void ScannerVibrato::process(const float* const* input, float* const* output, int channels, int samples) {
    const float fnorm = 1.0 / 65536.0;
    const float mixnorm = 0.7071067811865475;  // 1/sqrt(2)
    channels = std::clamp(channels, 0, kNumChannels);
    const unsigned int* offsets = offsets_[static_cast<size_t>(table_)].data();
    for (int i = 0; i < samples; ++i) {
      const unsigned int j = ((out_position_ << 16) + offsets[stator_ >> 16]) & kPositionMask;
      const int h = static_cast<int>(j >> 16);
      const int k = (h + 1) & static_cast<int>(kBufferMask);
      const float f = fnorm * static_cast<float>(j & 0xFFFF);
      for (int c = 0; c < channels; ++c) {
        float* buffer = buffers_[static_cast<size_t>(c)].data();
        const float x = input[c][i];
        const float g = f * x;
        buffer[h] += x - g;
        buffer[k] += g;
        output[c][i] = chorus_ ? (x + buffer[out_position_]) * mixnorm : buffer[out_position_];
        buffer[out_position_] = 0.0f;
      }
      out_position_ = (out_position_ + 1) & kBufferMask;
      stator_ = (stator_ + stator_increment_) & kTableMask;
    }
  }
}
