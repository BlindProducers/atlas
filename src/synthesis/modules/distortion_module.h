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

#include "distortion_modes/distortion_modes.h"
#include "synth_constants.h"
#include "synth_module.h"

namespace vital {

  class Distortion;
  class DigitalSvf;

  class DistortionModule : public SynthModule {
    public:
      DistortionModule(std::string prefix = "");
      virtual ~DistortionModule();

      virtual void init() override;
      virtual void setSampleRate(int sample_rate) override;
      virtual void setOversampleAmount(int oversample) override;
      virtual void hardReset() override;
      virtual void processWithInput(const poly_float* audio_in, int num_samples) override;
      virtual Processor* clone() const override { return new DistortionModule(*this); }

    protected:
      // The Types from Tube on: the JSFX modes, with the script's own Mix
      // law, Level and latency.
      void processJsfxMode(int type, const poly_float* audio_in, int num_samples);

      Distortion* distortion_;
      std::string prefix_;
      Value* distortion_type_;
      Value* filter_order_;
      DigitalSvf* filter_;
      Output* distortion_mix_;
      Output* jsfx_drive_;
      Output* jsfx_level_;
      poly_float mix_;
      atlas::distortion_modes::Engine jsfx_;
      // Whether jsfx_ has run its @init since the last reset, rate change or Type from before Tube.
      bool jsfx_active_;

      JUCE_LEAK_DETECTOR(DistortionModule)
  };
} // namespace vital
