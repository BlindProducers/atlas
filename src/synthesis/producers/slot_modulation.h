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
#include "synth_oscillator.h"
#include "utils.h"

namespace vital {
  // Slot-level FM and ring modulation for the oscillator types that are not
  // SynthOscillator (Sample, Granular, Spectral, Noise). The slot's distortion
  // controls (`osc_N_distortion_type/amount` and the secondary pair) are shared
  // by every type, so "FM from Oscillator 3" means the same thing whatever the
  // slot plays. Modulator position k is the k-th other slot, as in SynthOscillator.
  //
  // FM scales the playback rate: rate x (1 + kFmDepth x amount^2 x modulator),
  // through zero, so a sample can run backwards on the modulator's troughs.
  // Ring modulation multiplies the output by lerp(1, modulator, amount), the
  // same curve the analog oscillator uses.
  class SlotModulation {
    public:
      static constexpr int kNumPositions = SynthOscillator::kNumModulatorPositions;
      static constexpr mono_float kFmDepth = 12.0f;

      struct Block {
        const poly_float* fm[2] = { nullptr, nullptr };
        poly_float fm_depth[2] = { 0.0f, 0.0f };
        const poly_float* rm[2] = { nullptr, nullptr };
        poly_float rm_amount[2] = { 0.0f, 0.0f };

        force_inline bool anyFm() const { return fm[0] != nullptr || fm[1] != nullptr; }
        force_inline bool anyRm() const { return rm[0] != nullptr || rm[1] != nullptr; }

        force_inline poly_float rateScale(int i) const {
          poly_float scale = 1.0f;
          if (fm[0])
            scale += fm_depth[0] * fm[0][i];
          if (fm[1])
            scale += fm_depth[1] * fm[1][i];
          return scale;
        }

        force_inline poly_float ringGain(int i) const {
          poly_float gain = 1.0f;
          if (rm[0])
            gain *= utils::interpolate(poly_float(1.0f), rm[0][i], rm_amount[0]);
          if (rm[1])
            gain *= utils::interpolate(poly_float(1.0f), rm[1][i], rm_amount[1]);
          return gain;
        }

        void applyRingMod(poly_float* buffer, int num_samples) const {
          if (!anyRm())
            return;
          for (int i = 0; i < num_samples; ++i)
            buffer[i] *= ringGain(i);
        }
      };

      void setModulatorOutput(int position, Output* output) {
        if (position >= 0 && position < kNumPositions)
          modulators_[position] = output;
      }

      // Reads the distortion inputs of `processor` for this block. Unplugged
      // inputs read None, so a source used standalone is unmodulated. Types a
      // slot cannot do (every warp; FM when `allow_fm` is false) read as None.
      Block resolve(const Processor& processor, int type_input, int amount_input,
                    int secondary_type_input, int secondary_amount_input, bool allow_fm) const {
        Block block;
        const int type_inputs[2] = { type_input, secondary_type_input };
        const int amount_inputs[2] = { amount_input, secondary_amount_input };
        for (int k = 0; k < 2; ++k) {
          const int type = utils::iclamp(static_cast<int>(processor.input(type_inputs[k])->at(0)[0]),
                                         0, SynthOscillator::kNumDistortionTypes - 1);
          const int position = SynthOscillator::modulatorPosition(type);
          if (position < 0 || modulators_[position] == nullptr)
            continue;
          const poly_float amount = utils::clamp(processor.input(amount_inputs[k])->at(0), 0.0f, 1.0f);
          if (SynthOscillator::isFmType(type) && allow_fm) {
            block.fm[k] = modulators_[position]->buffer;
            block.fm_depth[k] = amount * amount * kFmDepth;
          }
          else if (SynthOscillator::isRmType(type)) {
            block.rm[k] = modulators_[position]->buffer;
            block.rm_amount[k] = amount;
          }
        }
        return block;
      }

    private:
      Output* modulators_[kNumPositions] = {};
  };

  // Implemented by the type modules whose source carries a SlotModulation.
  class SlotModulationTarget {
    public:
      virtual ~SlotModulationTarget() = default;
      virtual void setModulatorOutput(int position, Output* output) = 0;
  };
} // namespace vital
