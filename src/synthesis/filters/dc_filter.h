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
#include "synth_constants.h"

namespace vital {

  // A one-pole high-pass at 5 Hz that removes DC: y[n] = x[n] - x[n-1] + r y[n-1].
  struct DcBlocker {
    static constexpr mono_float kCutoff = 5.0f;

    static mono_float coefficient(mono_float sample_rate) {
      return 1.0f - 2.0f * kPi * kCutoff / sample_rate;
    }

    force_inline poly_float tick(poly_float audio_in, mono_float coefficient) {
      last_out = utils::mulAdd(audio_in - last_in, last_out, coefficient);
      last_in = audio_in;
      return last_out;
    }

    force_inline void reset(poly_mask reset_mask) {
      last_in = utils::maskLoad(last_in, 0.0f, reset_mask);
      last_out = utils::maskLoad(last_out, 0.0f, reset_mask);
    }

    poly_float last_in = 0.0f;
    poly_float last_out = 0.0f;
  };

  class DcFilter : public Processor {
    public:
      enum {
        kAudio,
        kReset,
        kNumInputs
      };

      DcFilter();
      virtual ~DcFilter() { }

      virtual Processor* clone() const override { return new DcFilter(*this); }
      virtual void process(int num_samples) override;
      virtual void processWithInput(const poly_float* audio_in, int num_samples) override;

      void setSampleRate(int sample_rate) override;

    private:
      void reset(poly_mask reset_mask) override;

      mono_float coefficient_;
      DcBlocker blocker_;

      JUCE_LEAK_DETECTOR(DcFilter)
  };
} // namespace vital
