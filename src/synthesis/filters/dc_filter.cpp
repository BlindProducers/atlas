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

#include "dc_filter.h"

namespace vital {

  DcFilter::DcFilter() : Processor(DcFilter::kNumInputs, 1) {
    coefficient_ = DcBlocker::coefficient(kDefaultSampleRate);
  }

  void DcFilter::setSampleRate(int sample_rate) {
    Processor::setSampleRate(sample_rate);
    coefficient_ = DcBlocker::coefficient(getSampleRate());
  }

  void DcFilter::process(int num_samples) {
    VITAL_ASSERT(inputMatchesBufferSize(kAudio));
    processWithInput(input(kAudio)->source->buffer, num_samples);
  }

  void DcFilter::processWithInput(const poly_float* audio_in, int num_samples) {
    poly_mask reset_mask = getResetMask(kReset);
    if (reset_mask.anyMask())
      reset(reset_mask);

    poly_float* audio_out = output()->buffer;
    for (int i = 0; i < num_samples; ++i)
      audio_out[i] = blocker_.tick(audio_in[i], coefficient_);
  }

  void DcFilter::reset(poly_mask reset_mask) {
    blocker_.reset(reset_mask);
  }
} // namespace vital
