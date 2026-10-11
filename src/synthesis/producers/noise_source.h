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

#include "cut_filter.h"
#include "slot_modulation.h"

namespace vital {
  class NoiseSource : public Processor {
    public:
      static constexpr mono_float kMaxAmplitude = 1.41421356237f;

      enum {
        kReset,
        kNotePressed,
        kType,
        kLevel,
        kPan,
        kWidth,
        kLowCutoff,
        kHighCutoff,
        kFilterSlope,
        kDistortionType,
        kDistortionAmount,
        kSecondaryDistortionType,
        kSecondaryDistortionAmount,
        kNumInputs
      };

      enum {
        kRaw,
        kLevelled,
        kNumOutputs
      };

      enum NoiseType {
        kWhite,
        kBrightWhite,
        kPink,
        kBrown,
        kDark,
        kAir,
        kNumNoiseTypes
      };

      NoiseSource();

      void reset(poly_mask reset_mask) override;
      void process(int num_samples) override;
      Processor* clone() const override { return new NoiseSource(*this); }
      SlotModulation& modulation() { return modulation_; }

    private:

      poly_float shapeNoise(poly_float white, int type);

      poly_float pan_amplitude_;
      poly_float level_;
      poly_float width_;
      poly_float pink_state_1_;
      poly_float pink_state_2_;
      poly_float brown_state_;
      CutFilter low_cut_{true};
      CutFilter high_cut_{false};
      utils::RandomGenerator random_generator_;
      SlotModulation modulation_;

      JUCE_LEAK_DETECTOR(NoiseSource)
  };
} // namespace vital
