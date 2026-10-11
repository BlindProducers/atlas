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
#include "granular_source.h"

namespace vital {
  class GranularModule : public SynthModule, public SlotModulationTarget {
    public:
      enum {
        kReset,
        kVoiceEvent,
        kMidi,
        kNoteCount,
        kNumInputs
      };

      enum {
        kRaw,
        kLevelled,
        kNumOutputs
      };

      explicit GranularModule(std::string prefix = "granular_", std::shared_ptr<Sample> sample = nullptr,
                              const Output* beats_per_second = nullptr, std::string slot_prefix = "");
      virtual ~GranularModule() { }

      void init() override;
      void setModulatorOutput(int position, Output* output) override {
        granular_->modulation().setModulatorOutput(position, output);
      }
      void correctToTime(double seconds) override { granular_->correctToTime(seconds); }
      Processor* clone() const override { return new GranularModule(*this); }

      Sample* getSample() { return granular_->getSample(); }
      force_inline Output* getPhaseOutput() const { return granular_->getPhaseOutput(); }

    protected:
      std::string prefix_;
      std::string slot_prefix_;
      GranularSource* granular_;
      const Output* beats_per_second_;

      JUCE_LEAK_DETECTOR(GranularModule)
  };
} // namespace vital
