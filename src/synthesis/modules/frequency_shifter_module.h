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

#include <array>
#include <vector>

namespace vital {

  class FrequencyShifterModule : public SynthModule {
    public:
      FrequencyShifterModule(std::string prefix = "");

      void init() override;
      void processWithInput(const poly_float* audio_in, int num_samples) override;
      void hardReset() override;
      void enable(bool enable) override;
      void setSampleRate(int sample_rate) override;
      void setOversampleAmount(int oversample) override;

      Processor* clone() const override { VITAL_ASSERT(false); return nullptr; }

    private:
      static constexpr int kNumHilbertCoefficients = 16;
      static constexpr int kNumHilbertSections = kNumHilbertCoefficients / 2;
      static constexpr int kNumDownshiftFilterSections = 4;
      static constexpr float kMaxDelayMilliseconds = 50.0f;

      struct AllpassSection {
        poly_float input_delay_1 = 0.0f;
        poly_float input_delay_2 = 0.0f;
        poly_float output_delay_1 = 0.0f;
        poly_float output_delay_2 = 0.0f;

        force_inline poly_float process(poly_float input, float coefficient) {
          poly_float output = (input + output_delay_2) * coefficient - input_delay_2;
          input_delay_2 = input_delay_1;
          input_delay_1 = input;
          output_delay_2 = output_delay_1;
          output_delay_1 = output;
          return output;
        }

        void reset() {
          input_delay_1 = 0.0f;
          input_delay_2 = 0.0f;
          output_delay_1 = 0.0f;
          output_delay_2 = 0.0f;
        }
      };

      struct QuadraturePair {
        poly_float real;
        poly_float quadrature;
      };

      struct BiquadCoefficients {
        mono_float b0 = 1.0f;
        mono_float b1 = 0.0f;
        mono_float b2 = 0.0f;
        mono_float a1 = 0.0f;
        mono_float a2 = 0.0f;
      };

      struct BiquadState {
        poly_float state_1 = 0.0f;
        poly_float state_2 = 0.0f;

        force_inline poly_float process(poly_float input, const BiquadCoefficients& coefficients) {
          poly_float output = input * coefficients.b0 + state_1;
          state_1 = input * coefficients.b1 - output * coefficients.a1 + state_2;
          state_2 = input * coefficients.b2 - output * coefficients.a2;
          return output;
        }

        void reset() {
          state_1 = 0.0f;
          state_2 = 0.0f;
        }
      };

      void updateHilbertCoefficients();
      void updateDownshiftFilterCoefficients(mono_float cutoff);
      void resetHilbertState();
      void resetDownshiftFilters();
      void resizeDelayBuffer();
      QuadraturePair processHilbert(poly_float audio);
      QuadraturePair protectDownshiftFromDcFold(QuadraturePair pair);
      poly_float processAlignedDry(poly_float audio);
      poly_float processWetDelay(poly_float wet, mono_float delay_samples);

      Output* shift_;
      Output* feedback_;
      Output* delay_;
      Output* mix_;
      std::string prefix_;

      std::array<float, kNumHilbertCoefficients> hilbert_coefficients_;
      std::array<AllpassSection, kNumHilbertSections> real_sections_;
      std::array<AllpassSection, kNumHilbertSections> quadrature_sections_;
      std::array<AllpassSection, kNumHilbertSections> dry_sections_;
      std::array<BiquadCoefficients, kNumDownshiftFilterSections> downshift_coefficients_;
      std::array<BiquadState, kNumDownshiftFilterSections> downshift_real_filters_;
      std::array<BiquadState, kNumDownshiftFilterSections> downshift_quadrature_filters_;
      std::vector<poly_float> wet_delay_buffer_;
      int wet_delay_write_index_;
      poly_float quadrature_delay_;
      poly_float phase_;
      poly_float feedback_sample_;
      poly_float feedback_amount_;
      poly_float mix_amount_;
      mono_float delay_milliseconds_;
      mono_float downshift_cutoff_;

      JUCE_LEAK_DETECTOR(FrequencyShifterModule)
  };
} // namespace vital
