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
#include "sample_source.h"

namespace vital {
  class SampleModule : public SynthModule, public SlotModulationTarget {
    public:
      enum {
        kReset,
        kRetrigger,
        kVoiceEvent,
        kMidi,
        kNoteCount,
        kNotePressed,
        kNumInputs
      };

      enum {
        kRaw,
        kLevelled,
        kNumOutputs
      };

      // `prefix` names this module's controls, e.g. "osc_5_sample_"; the sample is owned by the slot.
      // `slot_prefix` ("osc_5_") enables slot-level FM/RM from the slot's distortion controls.
      explicit SampleModule(std::string prefix = "sample_", std::shared_ptr<Sample> sample = nullptr,
                            std::string slot_prefix = "");
      virtual ~SampleModule() { }

      void init() override;
      void setModulatorOutput(int position, Output* output) override {
        sampler_->modulation().setModulatorOutput(position, output);
      }
      virtual Processor* clone() const override { return new SampleModule(*this); }

      Sample* getSample() { return sampler_->getSample(); }
      force_inline Output* getPhaseOutput() const { return sampler_->getPhaseOutput(); }

    protected:
      std::string prefix_;
      std::string slot_prefix_;
      SampleSource* sampler_;

      JUCE_LEAK_DETECTOR(SampleModule)
  };
} // namespace vital
