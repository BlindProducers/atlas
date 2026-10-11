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

  class LimiterModule : public SynthModule {
    public:
      LimiterModule(std::string prefix = "");

      void init() override;
      void processWithInput(const poly_float* audio_in, int num_samples) override;
      void hardReset() override;
      void enable(bool enable) override;

      Processor* clone() const override { VITAL_ASSERT(false); return nullptr; }

    private:
      Output* gain_;
      Output* ceiling_;
      Output* release_;
      Output* mix_;
      std::string prefix_;

      poly_float envelope_gain_;
      poly_float mix_amount_;

      JUCE_LEAK_DETECTOR(LimiterModule)
  };
} // namespace vital
