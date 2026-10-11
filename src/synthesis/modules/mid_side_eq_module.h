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

namespace vital {

  class EqualizerModule;

  // Two full equalizers, one on the mid (mono) signal and one on the side
  // (stereo difference) signal. Same bands and controls as the regular EQ.
  class MidSideEqModule : public SynthModule {
    public:
      MidSideEqModule(std::string prefix = "");
      virtual ~MidSideEqModule() { }

      void processWithInput(const poly_float* audio_in, int num_samples) override;
      void setOversampleAmount(int oversampling) override;
      void hardReset() override;
      void enable(bool enable) override;

      Processor* clone() const override { VITAL_ASSERT(false); return nullptr; }

    private:
      EqualizerModule* mid_eq_;
      EqualizerModule* side_eq_;
      Output mid_input_;
      Output side_input_;

      JUCE_LEAK_DETECTOR(MidSideEqModule)
  };
} // namespace vital
