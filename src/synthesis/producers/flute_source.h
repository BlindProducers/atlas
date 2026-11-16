// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "processor.h"
#include "slot_modulation.h"
#include "flute_instrument.h"

#include <array>
#include <memory>

namespace vital {
  // The voices' side of the Flute oscillator type. The slot's one flute
  // (atlas::flute::Instrument) has rendered the block before the voices run;
  // the lane of the voice the voice handler started last carries that audio
  // (the instrument's owner, by activation number), every other lane is
  // silent, and when the audio changes lanes in a block the old lane carries
  // it up to the sample and the new one from there, so the flute plays on
  // through Atlas's voice stealing and retriggering. In Flute envelope mode
  // the carrying lane holds its voice open while the flute sounds.
  class FluteSource : public Processor {
    public:
      static constexpr int kVoicesPerProcessor = poly_float::kSize / 2;
      static constexpr mono_float kMaxAmplitude = 1.41421356237f;
      // The flute's output times this is the slot's raw output. A slot's
      // audio reaches the effects at half its raw level, so the level's
      // default (70.7%, 0.5 after the level curve) plays the flute at the
      // original plugin's Output of 0 dB.
      static constexpr float kOutputScale = 2.0f;

      // Amplitude envelope modes (osc_N_flute_amp_env).
      enum AmpEnvMode { kFluteEnvelope, kEnvelope1, kNumAmpEnvModes };

      enum {
        kReset,
        kVoiceEvent,
        // The voice's activation number, which names the lane carrying the flute.
        kNoteCount,
        // 1 in the lanes of voices the voice handler is playing.
        kActiveVoices,
        kAmpEnvMode,
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
        // 1 per voice while it carries a sounding flute (Flute envelope mode only).
        kHold,
        kNumOutputs
      };

      explicit FluteSource(std::shared_ptr<atlas::flute::Instrument> instrument);

      void process(int num_samples) override;
      Processor* clone() const override { return new FluteSource(*this); }
      SlotModulation& modulation() { return modulation_; }

    private:
      struct VoiceState {
        // The activation number the lane had when the previous block ended,
        // so a retriggered voice knows it carried the flute before.
        int id = -1;
        // The voice was killed: it fades under its own fade and no longer holds.
        bool killed = false;
      };

      std::shared_ptr<atlas::flute::Instrument> instrument_;
      std::array<VoiceState, kVoicesPerProcessor> voices_;
      poly_float pan_amplitude_ = 0.0f;
      bool pan_initialized_ = false;
      SlotModulation modulation_;

      JUCE_LEAK_DETECTOR(FluteSource)
  };
} // namespace vital
