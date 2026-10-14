/* Copyright 2013-2019 Matt Tytel
 *
 * vital is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * vital is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with vital.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "processor.h"
#include "utils.h"

#include <cstdint>
#include <memory>
#include <string>

namespace vital {

  class Envelope : public Processor {
    public:
      enum {
        kDelay,
        kAttack,
        kAttackPower,
        kHold,
        kDecay,
        kDecayPower,
        kSustain,
        kRelease,
        kReleasePower,
        kTrigger,
        kTempoSync,
        kBeatsPerSecond,
        kReset,
        // Analog mode: RC curves, and Age for worn timing components.
        kAnalogMode,
        kAnalogAge,
        kNumInputs
      };

      enum ProcessorOutput {
        kValue,
        kPhase,
        kNumOutputs
      };

      Envelope();
      virtual ~Envelope() { }

      virtual Processor* clone() const override { return new Envelope(*this); }
      virtual void process(int num_samples) override;
      void setRetriggerFromZero(bool retrigger_from_zero) { retrigger_from_zero_ = retrigger_from_zero; }

      // Salts this envelope's voice cards. Call before the voices are cloned.
      void setVoiceCardSalt(const std::string& salt);

    private:
      void processControlRate(int num_samples);
      void processAudioRate(int num_samples);
      void processAnalog(int num_samples);
      void prepareVoiceCards();
      poly_float tempoSyncedTime(poly_float time);

      poly_float processSection(poly_float* audio_out, int from, int to,
                                poly_float power, poly_float delta_power,
                                poly_float position, poly_float delta_position,
                                poly_float start, poly_float end, poly_float delta_end);

      poly_float current_value_;

      poly_float position_;
      poly_float value_;
      poly_float poly_state_;
      poly_float start_value_;

      poly_float attack_power_;
      poly_float decay_power_;
      poly_float release_power_;
      poly_float sustain_;
      bool retrigger_from_zero_;

      // Analog mode. Each voice clone is a voice card numbered in cloning
      // order, so its timing tolerances are the same every time a patch loads.
      struct VoiceCard {
        VoiceCard() : counter(std::make_shared<int>(0)) { }
        VoiceCard(const VoiceCard& other) : counter(other.counter), index((*counter)++), salt(other.salt) { }
        VoiceCard& operator=(const VoiceCard&) = delete;
        std::shared_ptr<int> counter;
        int index = 0;
        uint32_t salt = 0;
        bool ready = false;
        // Per lane, in [-1, 1]: attack, decay and release time errors, the
        // attack overshoot, the attack peak and the sustain leak.
        poly_float attack = 0.0f;
        poly_float decay = 0.0f;
        poly_float release = 0.0f;
        poly_float overshoot = 0.0f;
        poly_float peak = 0.0f;
        poly_float leak = 0.0f;
      };
      VoiceCard voice_card_;
      // Every stage's time also wanders slowly, like the oscillator's pitch.
      poly_int wander_random_ = 1;
      poly_float time_wander_ = 0.0f;
      // How long sustain has leaked, from 0 at note start toward 1.
      poly_float droop_ = 0.0f;

      JUCE_LEAK_DETECTOR(Envelope)
  };
} // namespace vital
