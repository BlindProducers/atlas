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

#include "synth_module.h"
#include "synth_oscillator.h"

namespace vital {
  class Wavetable;

  class OscillatorModule : public SynthModule {
    public:
      enum {
        kReset,
        kRetrigger,
        kMidi,
        kActiveVoices,
        kNotePressed,
        kSoundingVoices,
        kNumInputs
      };

      enum {
        kRaw,
        kLevelled,
        kNumOutputs
      };

      // The wavetable may be owned by the slot so it survives type changes.
      OscillatorModule(std::string prefix = "", std::shared_ptr<Wavetable> wavetable = nullptr);
      virtual ~OscillatorModule() { }

      void init() override;
      virtual Processor* clone() const override { return new OscillatorModule(*this); }

      Wavetable* getWavetable() { return wavetable_.get(); }
      force_inline SynthOscillator* oscillator() { return oscillator_; }
      SynthOscillator::DistortionType getDistortionType() {
        int val = distortion_type_->value();
        return static_cast<SynthOscillator::DistortionType>(val);
      }

      SynthOscillator::DistortionType getSecondaryDistortionType() const {
        return static_cast<SynthOscillator::DistortionType>(static_cast<int>(secondary_distortion_type_->value()));
      }

    protected:
      std::string prefix_;
      std::shared_ptr<Wavetable> wavetable_;

      SynthOscillator* oscillator_;
      Value* distortion_type_;
      Value* secondary_distortion_type_ = nullptr;

      JUCE_LEAK_DETECTOR(OscillatorModule)
  };
} // namespace vital
