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
#include "jsfx_ports.h"

#include <array>

namespace vital {

  class DimensionExpanderModule : public SynthModule {
    public:
      DimensionExpanderModule(std::string prefix = "");

      void init() override;
      void processWithInput(const poly_float* audio_in, int num_samples) override;
      void hardReset() override;
      void enable(bool enable) override;
      void setSampleRate(int rate) override;
      void setOversampleAmount(int amount) override;

      Processor* clone() const override { VITAL_ASSERT(false); return nullptr; }

    private:
      static constexpr int kMaxDelaySamples = 32768;

      static mono_float onePoleCoefficient(mono_float midi_cutoff, int sample_rate);
      static force_inline poly_float processLowPass(poly_float input, poly_float& state,
                                                    mono_float coefficient) {
        state += (input - state) * coefficient;
        return state;
      }

      poly_float readDelay(mono_float delay_samples) const;

      Output* amount_;
      Output* delay_ = nullptr;
      mono_float delay_smoothed_ = 0.0f;
      bool delay_initialized_ = false;
      Value* mode_ = nullptr;
      Value* quorax_slope_ = nullptr;
      std::array<Output*, 6> quorax_controls_ {};
      jsfx::Quorax quorax_;
      int last_mode_ = -1;
      Output* low_cutoff_;
      Output* high_cutoff_;
      std::string prefix_;

      std::array<poly_float, kMaxDelaySamples> delay_buffer_;
      int write_index_;

      poly_float amount_smoothed_;
      poly_float low_cut_state_;
      poly_float high_cut_state_;

      JUCE_LEAK_DETECTOR(DimensionExpanderModule)
  };
} // namespace vital
