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

#include "spectral_source.h"
#include "synth_module.h"

namespace vital {
  class SpectralModule : public SynthModule, public SlotModulationTarget {
    public:
      enum {
        kReset,
        kMidi,
        kNotePressed,
        kActiveVoices,
        kRetrigger,
        kNumInputs
      };

      enum {
        kRaw,
        kLevelled,
        kNumOutputs
      };

      explicit SpectralModule(std::string prefix = "spectral_", std::shared_ptr<Sample> sample = nullptr,
                              std::string slot_prefix = "");
      virtual ~SpectralModule() { }

      void init() override;
      void setModulatorOutput(int position, Output* output) override {
        spectral_->modulation().setModulatorOutput(position, output);
      }
      Processor* clone() const override { return new SpectralModule(*this); }

      Sample* getSample() { return spectral_->getSample(); }

    protected:
      std::string prefix_;
      std::string slot_prefix_;
      SpectralSource* spectral_;

      JUCE_LEAK_DETECTOR(SpectralModule)
  };
} // namespace vital
