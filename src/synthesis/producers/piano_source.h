// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "processor.h"
#include "slot_modulation.h"
#include "piano_instrument.h"

#include <array>
#include <memory>

namespace vital {
  // The per-voice engine of the Piano oscillator type: each note-on strikes
  // one of the 88 notes of the modelled grand (src/synthesis/effects/piano)
  // in the voice's lane. The hammer, the strings, the knock of the action
  // and the note's share of the soundboard render here, so Envelope 1 and
  // the voice filters shape each note; the sympathetic strings, which every
  // note drives, render once per block in the voice handler (the slot's
  // Instrument). All voices of the slot share that Instrument.
  //
  // The engine plays A0 to C8; a note outside that stays silent. Pitch bend
  // and glide do not move a struck note: the note is the key nearest the
  // voice's pitch at the strike, moved by Transpose.
  class PianoSource : public Processor {
    public:
      static constexpr int kVoicesPerProcessor = poly_float::kSize / 2;
      static constexpr mono_float kMaxAmplitude = 1.41421356237f;
      // The engine's output times this is the slot's raw output. A slot's
      // audio reaches the effects at half its raw level, so the level's
      // default (70.7%, 0.5 after the level curve) plays the piano at the
      // original plugin's default Volume of -6 dB, and 100% at 0 dB.
      static constexpr float kOutputScale = 2.0f;

      // Amplitude envelope modes (osc_N_piano_amp_env).
      enum AmpEnvMode { kPianoEnvelope, kEnvelope1, kNumAmpEnvModes };

      enum {
        kReset,
        kVoiceEvent,
        kMidi,
        kVelocity,
        // 1 in the lanes of voices the voice handler is playing.
        kActiveVoices,
        // 0 while the voice's key is down; 1 plus the release velocity (0 to
        // 1) once it is up, also while the sustain pedal keeps the voice on.
        kKeyLifted,
        kAmpEnvMode,
        kDynamics,
        kHardness,
        kActionNoise,
        kSympathetic,
        kWidth,
        kTranspose,
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
        // 1 per voice while its note still sounds (Piano envelope mode only).
        kHold,
        kNumOutputs
      };

      explicit PianoSource(std::shared_ptr<pm::Instrument> instrument);

      void process(int num_samples) override;
      Processor* clone() const override { return new PianoSource(*this); }
      SlotModulation& modulation() { return modulation_; }

    private:
      struct VoiceState {
        pm::Instrument::Lane lane;
        // The voice was killed: the note plays on under the voice's own fade
        // and no longer holds it open.
        bool killed = false;
        // The previous block's width, for a ramp across this block.
        float width = 1.0f;
      };

      std::shared_ptr<pm::Instrument> instrument_;
      std::array<VoiceState, kVoicesPerProcessor> voices_;
      poly_float pan_amplitude_ = 0.0f;
      bool pan_initialized_ = false;
      SlotModulation modulation_;

      JUCE_LEAK_DETECTOR(PianoSource)
  };
} // namespace vital
