// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "processor.h"
#include "multisample_player.h"

#include <array>
#include <memory>

namespace vital {
  class MultiSampleSlot;

  // The per-voice engine of the Multi-sample oscillator type. On note-on it
  // picks the instrument's regions for the key, velocity, CC state, round
  // robin and keyswitch, and plays them together; on note-off it releases
  // them and starts release-trigger regions.
  class MultiSampleSource : public Processor {
    public:
      static constexpr int kMaxPlayersPerVoice = 32;
      static constexpr int kVoicesPerProcessor = poly_float::kSize / 2;
      // Amplitude envelope modes (osc_N_multisample_amp_env).
      enum AmpEnvMode { kInstrumentEnvelope, kEnvelope1, kNumAmpEnvModes };

      enum {
        kReset,
        kRetrigger,
        kVoiceEvent,
        kMidi,
        kNote,
        kMidiKey,
        kVelocity,
        kLevel,
        kPan,
        kTranspose,
        kTune,
        kAmpEnvMode,
        kNumInputs
      };

      enum {
        kRaw,
        kLevelled,
        // 1 per voice while the instrument still sounds there (Instrument mode only).
        kHold,
        kNumOutputs
      };

      explicit MultiSampleSource(std::shared_ptr<MultiSampleSlot> slot);

      void process(int num_samples) override;
      Processor* clone() const override { return new MultiSampleSource(*this); }

    private:
      struct VoiceState {
        std::array<MultiSamplePlayer, kMaxPlayersPerVoice> players;
        int key = -1;
        int velocity = 0;
        float tuned_note = 60.0f;
        // Samples since note-on, for release-trigger decay.
        int64_t held_samples = 0;
        bool held = false;
      };

      void noteOn(int voice, int key, int velocity, float tuned_note, int offset);
      void noteOff(int voice, int offset);
      void startRegion(int voice, int region_index, int velocity, float random, int offset, float gain,
                       bool ignore_note_off);
      MultiSamplePlayer& freePlayer(int voice);

      std::shared_ptr<MultiSampleSlot> slot_;
      uint32_t generation_ = 0;
      std::array<VoiceState, kVoicesPerProcessor> voices_;
      poly_float pan_amplitude_ = 0.0f;
      bool pan_initialized_ = false;

      JUCE_LEAK_DETECTOR(MultiSampleSource)
  };
} // namespace vital
