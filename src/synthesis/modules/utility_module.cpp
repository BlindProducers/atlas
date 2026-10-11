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

#include "utility_module.h"

#include "futils.h"
#include "poly_utils.h"
#include "synth_constants.h"

#include <cmath>

namespace vital {

  UtilityModule::UtilityModule(std::string prefix) :
      SynthModule(0, 1), prefix_(prefix), input_gain_(nullptr), output_gain_(nullptr), width_(nullptr),
      low_cutoff_(nullptr), high_cutoff_(nullptr), slope_(nullptr), input_gain_smoothed_(1.0f),
      output_gain_smoothed_(1.0f), width_smoothed_(1.0f) { hardReset(); }

  void UtilityModule::init() {
    input_gain_ = createMonoModControl(prefix_ + "utility_input_gain", true, true);
    output_gain_ = createMonoModControl(prefix_ + "utility_output_gain", true, true);
    width_ = createMonoModControl(prefix_ + "utility_width");
    low_cutoff_ = createMonoModControl(prefix_ + "utility_low_cutoff", true, true);
    high_cutoff_ = createMonoModControl(prefix_ + "utility_high_cutoff", true, true);
    slope_ = createBaseControl(prefix_ + "utility_filter_slope");

    SynthModule::init();
  }

  void UtilityModule::hardReset() {
    low_cut_.reset(constants::kFullMask);
    high_cut_.reset(constants::kFullMask);
  }

  void UtilityModule::processWithInput(const poly_float* audio_in, int num_samples) {
    if (num_samples <= 0)
      return;
    SynthModule::process(num_samples);

    poly_float current_input_gain = input_gain_smoothed_;
    poly_float current_output_gain = output_gain_smoothed_;
    poly_float current_width = width_smoothed_;

    input_gain_smoothed_ = futils::dbToMagnitude(input_gain_->buffer[0]);
    output_gain_smoothed_ = futils::dbToMagnitude(output_gain_->buffer[0]);
    width_smoothed_ = utils::clamp(width_->buffer[0], 0.0f, 2.0f);

    poly_float input_gain_delta = (input_gain_smoothed_ - current_input_gain) * (1.0f / num_samples);
    poly_float output_gain_delta = (output_gain_smoothed_ - current_output_gain) * (1.0f / num_samples);
    poly_float width_delta = (width_smoothed_ - current_width) * (1.0f / num_samples);

    const mono_float low_cutoff = low_cutoff_->buffer[0][0];
    const mono_float high_cutoff = high_cutoff_->buffer[0][0];
    const int slope = static_cast<int>(slope_->value() + 0.5f);
    low_cut_.configure(low_cutoff, slope, getSampleRate());
    high_cut_.configure(high_cutoff, slope, getSampleRate());

    poly_float* audio_out = output()->buffer;
    for (int i = 0; i < num_samples; ++i) {
      current_input_gain += input_gain_delta;
      current_output_gain += output_gain_delta;
      current_width += width_delta;

      poly_float sample = audio_in[i] * current_input_gain;

      sample = high_cut_.tick(low_cut_.tick(sample));

      poly_float swapped = utils::swapStereo(sample);
      poly_float mid = (sample + swapped) * 0.5f;
      poly_float side = (sample - swapped) * 0.5f;
      audio_out[i] = (mid + side * current_width) * current_output_gain;
    }
  }
} // namespace vital
