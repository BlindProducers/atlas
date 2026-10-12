// SPDX-License-Identifier: GPL-3.0-or-later
// OpenB3's scanner vibrato (setBfree's Beatrix vibrato.c): the tonewheel
// organ's vibrato and chorus. A delay line whose write position swings ahead
// of and behind its read position at the scanner's rate, 7.25 Hz by default:
// V1, V2 and V3 swing 3, 6 and 9 samples either way at 48 kHz; C1, C2 and C3
// mix that vibrato with the dry signal, as the organ's chorus does.
#pragma once

#include <array>
#include <cstdint>

namespace atlas::openb3 {
  enum ScannerMode { kVibrato1, kVibrato2, kVibrato3, kChorus1, kChorus2, kChorus3, kNumScannerModes };

  class ScannerVibrato {
   public:
    static constexpr int kTableSize = 2048;     // INCTBL_SIZE
    static constexpr int kBufferSize = 1024;    // BUF_SIZE_BYTES (in samples)
    static constexpr int kNumChannels = 2;
    static constexpr double kDefaultHz = 7.25;
    static constexpr double kMinimumHz = 4.0;
    static constexpr double kMaximumHz = 22.0;
    // scanner.modulation.v1-v3, in samples at the reference rate.
    static constexpr double kReferenceRate = 48000.0;

    ScannerVibrato();
    // The swing is 3, 6 or 9 samples at 48 kHz and the same time at other rates.
    void prepare(double sample_rate);
    void reset();
    void setMode(int mode);
    void setFrequency(double hz);
    // Each channel runs its own delay line under the one scanner.
    void process(const float* const* input, float* const* output, int channels, int samples);

   private:
    void buildTables();

    double sample_rate_ = kReferenceRate;
    double hz_ = kDefaultHz;
    std::array<std::array<unsigned int, kTableSize>, 3> offsets_ {};
    int table_ = 2;
    bool chorus_ = false;
    unsigned int stator_ = 0;
    unsigned int stator_increment_ = 0;
    unsigned int out_position_ = 0;
    std::array<std::array<float, kBufferSize>, kNumChannels> buffers_ {};
  };
}
