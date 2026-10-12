// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <memory>

namespace braids {
  class MacroOscillator;
}

namespace vital {
namespace braids_engine {
  // Mutable Instruments Braids renders 16-bit audio at 96 kHz in blocks of 24
  // samples. This header keeps stmlib's macros out of the rest of the build:
  // only braids_engine.cpp includes the original sources.
  constexpr double kNativeSampleRate = 96000.0;
  constexpr int kBlockSize = 24;
  // Every shape of Braids 1.9, in firmware order, the hidden "????" last.
  constexpr int kNumModels = 48;
  // The firmware's BITS and RATE settings.
  constexpr int kNumResolutions = 7;
  constexpr int kNumSampleRates = 7;
  constexpr int kFullResolution = kNumResolutions - 1;
  constexpr int kFullSampleRate = kNumSampleRates - 1;
  // Oscillators in a voice that keep a phase: three analog, one digital.
  constexpr int kNumPhases = 4;

  // One Braids voice: a MacroOscillator plus the firmware's bit and sample
  // rate reduction. Copying gives a fresh, silent voice with the same settings
  // (the oscillator itself is not copyable).
  class Voice {
    public:
      Voice();
      Voice(const Voice& other);
      Voice& operator=(const Voice&) = delete;
      ~Voice();

      // Back to the power-on state, with every oscillator at its start phase
      // (a full cycle is 2^32). Without `start_phases` every cycle starts at 0,
      // as at power-on.
      void reset(const uint32_t* start_phases = nullptr);
      // A trigger: plucks, strikes and blows the physical and percussion models.
      void strike();
      void setModel(int model);
      // Timbre and Color, 0 to 1.
      void setParameters(float timbre, float color);
      // MIDI note number (60 is middle C), fractional for bends.
      void setNote(float note);
      void setResolution(int resolution);
      void setSampleRate(int sample_rate);

      // One block of kBlockSize samples at 96 kHz, scaled to -1..1.
      void render(float* output);

    private:
      std::unique_ptr<braids::MacroOscillator> oscillator_;
      int model_ = 0;
      int resolution_ = kFullResolution;
      int sample_rate_ = kFullSampleRate;
      int16_t timbre_ = 16384;
      int16_t color_ = 16384;
      int16_t pitch_ = 60 << 7;
  };
} // namespace braids_engine
} // namespace vital
