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

#include "limiter_module.h"

#include "futils.h"
#include "poly_utils.h"

namespace vital {

  LimiterModule::LimiterModule(std::string prefix) :
      SynthModule(0, 1), gain_(nullptr), ceiling_(nullptr), release_(nullptr), mix_(nullptr), prefix_(prefix),
      envelope_gain_(1.0f), mix_amount_(1.0f) { }

  void LimiterModule::init() {
    gain_ = createMonoModControl(prefix_ + "limiter_gain", true, true);
    ceiling_ = createMonoModControl(prefix_ + "limiter_ceiling", true, true);
    release_ = createMonoModControl(prefix_ + "limiter_release", true, true);
    mix_ = createMonoModControl(prefix_ + "limiter_dry_wet");

    SynthModule::init();
  }

  void LimiterModule::processWithInput(const poly_float* audio_in, int num_samples) {
    static constexpr float kMinPeak = 0.000001f;
    static constexpr float kMinReleaseSeconds = 0.001f;

    SynthModule::process(num_samples);

    poly_float input_gain = futils::dbToMagnitude(gain_->buffer[0]);
    poly_float ceiling = futils::dbToMagnitude(ceiling_->buffer[0]);
    poly_float release_seconds = utils::max(release_->buffer[0] * 0.001f, kMinReleaseSeconds);
    poly_float release_amount = poly_float(1.0f) -
                                futils::exp(poly_float(-1.0f) / (release_seconds * getSampleRate()));
    poly_float target_mix = utils::clamp(mix_->buffer[0], 0.0f, 1.0f);
    poly_float current_mix = mix_amount_;
    poly_float delta_mix = (target_mix - current_mix) * (1.0f / num_samples);
    poly_float current_gain = envelope_gain_;
    poly_float* audio_out = output()->buffer;

    for (int i = 0; i < num_samples; ++i) {
      current_mix += delta_mix;

      poly_float driven = audio_in[i] * input_gain;
      poly_float peak = utils::max(poly_float::abs(driven), kMinPeak);
      poly_float target_gain = utils::min(poly_float(1.0f), ceiling / peak);
      poly_mask reducing = poly_float::lessThan(target_gain, current_gain);
      current_gain = utils::maskLoad(current_gain + (target_gain - current_gain) * release_amount,
                                     target_gain, reducing);

      poly_float limited = driven * current_gain;
      limited = utils::clamp(limited, ceiling * -1.0f, ceiling);
      audio_out[i] = utils::interpolate(audio_in[i], limited, current_mix);
    }

    envelope_gain_ = current_gain;
    mix_amount_ = target_mix;
  }

  void LimiterModule::hardReset() {
    envelope_gain_ = 1.0f;
    mix_amount_ = 1.0f;
  }

  void LimiterModule::enable(bool enable) {
    SynthModule::enable(enable);
    if (!enable)
      hardReset();
  }
} // namespace vital
