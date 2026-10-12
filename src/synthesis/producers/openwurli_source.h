// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "processor.h"
#include "slot_modulation.h"
#include "wurli_voice.h"

#include <array>
#include <cstdint>

namespace vital {
  // The per-voice engine of the OpenWurli oscillator type. Each note-on strikes
  // a Wurlitzer 200A reed: OpenWurli's modal reed, hammer and electrostatic
  // pickup, ported in src/synthesis/effects/openwurli. The reed for the nearest
  // of the 200A's 64 keys gives the timbre, and its pitch follows the voice, so
  // bends, glides and tuning move it. The output is the pickup voltage times
  // kPickupToOutput; the OpenWurli Amp effect divides it back out before its
  // preamp, as the real instrument feeds every reed into one amplifier.
  class OpenWurliSource : public Processor {
    public:
      static constexpr int kVoicesPerProcessor = poly_float::kSize / 2;
      static constexpr mono_float kMaxAmplitude = 1.41421356237f;
      // A fortissimo note peaks at roughly 1 to 1.7 before the slot level.
      static constexpr double kPickupToOutput = 100.0;
      // A re-struck or killed reed fades out over 5 ms, as engine.rs crossfades
      // a stolen voice.
      static constexpr double kFadeSeconds = 0.005;
      // Bends and glides retune the reed every kPitchStep samples.
      static constexpr int kPitchStep = 16;

      // Amplitude envelope modes (osc_N_wurli_amp_env).
      enum AmpEnvMode { kReedEnvelope, kEnvelope1, kNumAmpEnvModes };

      enum {
        kReset,
        kVoiceEvent,
        kMidi,
        kVelocity,
        kAmpEnvMode,
        kMlp,
        kBark,
        kDamping,
        kCondition,
        kTrebleVoicing,
        kRegisterBalance,
        kPickupLowCut,
        kTranspose,
        kTune,
        kLevel,
        kPan,
        kDistortionType,
        kDistortionAmount,
        kSecondaryDistortionType,
        kSecondaryDistortionAmount,
        kNumInputs
      };

      enum {
        kRaw,
        kLevelled,
        // 1 per voice while a reed still sounds there (Reed envelope mode only).
        kHold,
        kNumOutputs
      };

      OpenWurliSource();

      void process(int num_samples) override;
      Processor* clone() const override { return new OpenWurliSource(*this); }
      SlotModulation& modulation() { return modulation_; }

    private:
      struct VoiceState {
        openwurli::Voice reed;
        // The lane's previous reed, fading out after a re-strike or a kill.
        openwurli::Voice fading;
        int fade_remaining = 0;
        int fade_length = 1;
        // The key the reed was struck as; its pitch moves relative to it.
        int key = 60;
        // The previous block's note, for a ramp across this block.
        float note = 60.0f;
      };

      // The voicing controls at the moment a key is struck.
      openwurli::VoiceSettings voicing(int lane) const;
      void strike(VoiceState& state, float note, float velocity, bool mlp, const openwurli::VoiceSettings& settings);
      void fadeOut(VoiceState& state);
      // Renders samples [start, end) of the lane into `output`, the note
      // ramping from `from` at sample 0 to `to` at `count`.
      void render(VoiceState& state, double* output, int start, int end, int count, float from, float to);

      std::array<VoiceState, kVoicesPerProcessor> voices_;
      double sample_rate_ = 0.0;
      uint32_t strike_count_ = 0;
      poly_float pan_amplitude_ = 0.0f;
      bool pan_initialized_ = false;
      SlotModulation modulation_;

      JUCE_LEAK_DETECTOR(OpenWurliSource)
  };
} // namespace vital
