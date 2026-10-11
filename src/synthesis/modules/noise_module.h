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

#include "noise_source.h"
#include "synth_module.h"

namespace vital {
  class NoiseModule : public SynthModule, public SlotModulationTarget {
    public:
      enum {
        kReset,
        kNotePressed,
        kNumInputs
      };

      enum {
        kRaw,
        kLevelled,
        kNumOutputs
      };

      explicit NoiseModule(std::string prefix = "noise_", std::string slot_prefix = "");
      virtual ~NoiseModule() { }

      void init() override;
      void setModulatorOutput(int position, Output* output) override {
        noise_->modulation().setModulatorOutput(position, output);
      }
      Processor* clone() const override { return new NoiseModule(*this); }

    protected:
      std::string prefix_;
      std::string slot_prefix_;
      NoiseSource* noise_;

      JUCE_LEAK_DETECTOR(NoiseModule)
  };
} // namespace vital
