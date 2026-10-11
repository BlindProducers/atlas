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

#include "noise_source.h"

#include "futils.h"
#include "poly_utils.h"

namespace vital {

  NoiseSource::NoiseSource() :
      Processor(kNumInputs, kNumOutputs), pan_amplitude_(1.0f), level_(0.0f), width_(1.0f),
      pink_state_1_(0.0f), pink_state_2_(0.0f), brown_state_(0.0f),
      random_generator_(-1.0f, 1.0f) {
    low_cut_.reset(constants::kFullMask);
    high_cut_.reset(constants::kFullMask);
  }

  void NoiseSource::reset(poly_mask reset_mask) {
    pink_state_1_ = utils::maskLoad(pink_state_1_, 0.0f, reset_mask);
    pink_state_2_ = utils::maskLoad(pink_state_2_, 0.0f, reset_mask);
    brown_state_ = utils::maskLoad(brown_state_, 0.0f, reset_mask);
    low_cut_.reset(reset_mask);
    high_cut_.reset(reset_mask);
  }

  poly_float NoiseSource::shapeNoise(poly_float white, int type) {
    pink_state_1_ += (white - pink_state_1_) * 0.055f;
    pink_state_2_ += (pink_state_1_ - pink_state_2_) * 0.012f;
    brown_state_ = utils::clamp(brown_state_ + white * 0.018f, -1.0f, 1.0f);

    switch (type) {
      case kBrightWhite:
        return utils::clamp((white - pink_state_1_) * 1.75f, -1.0f, 1.0f);
      case kPink:
        return utils::clamp((white * 0.5f + pink_state_1_ * 0.7f + pink_state_2_ * 0.35f) * 0.75f, -1.0f, 1.0f);
      case kBrown:
        return brown_state_;
      case kDark:
        return pink_state_2_;
      case kAir:
        return utils::clamp((white - pink_state_2_) * 1.25f, -1.0f, 1.0f);
      case kWhite:
      default:
        return white;
    }
  }

  void NoiseSource::process(int num_samples) {
    if (num_samples <= 0)
      return;

    poly_float current_pan_amplitude = pan_amplitude_;
    poly_float current_level = level_;
    poly_float current_width = width_;

    pan_amplitude_ = futils::panAmplitude(utils::clamp(input(kPan)->at(0), -1.0f, 1.0f));
    const Output* level_source = input(kLevel)->source;
    const bool audio_rate_level = level_source->buffer_size > 1;
    level_ = utils::clamp(level_source->buffer[audio_rate_level ? num_samples - 1 : 0], 0.0f, kMaxAmplitude);
    width_ = utils::clamp(input(kWidth)->at(0), 0.0f, 1.0f);

    const poly_float delta_pan = (pan_amplitude_ - current_pan_amplitude) * (1.0f / num_samples);
    const poly_float delta_level = (level_ - current_level) * (1.0f / num_samples);
    const poly_float delta_width = (width_ - current_width) * (1.0f / num_samples);

    const int type = utils::iclamp(static_cast<int>(input(kType)->at(0)[0] + 0.5f), 0, kNumNoiseTypes - 1);
    poly_float low_cutoff = utils::clamp(input(kLowCutoff)->at(0), 8.0f, 136.0f);
    poly_float high_cutoff = utils::clamp(input(kHighCutoff)->at(0), 8.0f, 136.0f);
    const int slope = static_cast<int>(input(kFilterSlope)->at(0)[0] + 0.5f);
    low_cut_.configure(low_cutoff, slope, getSampleRate());
    high_cut_.configure(high_cutoff, slope, getSampleRate());
    poly_float note_gate = utils::clamp(input(kNotePressed)->at(0), 0.0f, 1.0f);
    poly_mask reset_mask = getResetMask(kReset);
    current_pan_amplitude = utils::maskLoad(current_pan_amplitude, pan_amplitude_, reset_mask);
    current_level = utils::maskLoad(current_level, level_, reset_mask);
    current_width = utils::maskLoad(current_width, width_, reset_mask);
    if (reset_mask.anyMask())
      reset(reset_mask);

    poly_float* raw_output = output(kRaw)->buffer;
    poly_float* levelled_output = output(kLevelled)->buffer;
    const SlotModulation::Block modulation = modulation_.resolve(*this, kDistortionType, kDistortionAmount,
                                                                 kSecondaryDistortionType,
                                                                 kSecondaryDistortionAmount, false);
    const bool rm_active = modulation.anyRm();
    for (int i = 0; i < num_samples; ++i) {
      current_pan_amplitude += delta_pan;
      // The modulation graph already smooths the base level. Decimating its
      // audio-rate output to one value per block aliases fast/keytracked LFOs.
      if (audio_rate_level)
        current_level = utils::clamp(level_source->buffer[i], 0.0f, kMaxAmplitude);
      else
        current_level += delta_level;
      current_width += delta_width;

      poly_float stereo_white = random_generator_.polyNext();
      poly_float mono_white = random_generator_.polyVoiceNext();
      poly_float white = utils::interpolate(mono_white, stereo_white, current_width);
      poly_float sample = shapeNoise(white, type);

      sample = high_cut_.tick(low_cut_.tick(sample));

      sample = utils::clamp(sample, -1.0f, 1.0f);
      if (!utils::isFinite(sample))
        sample = 0.0f;

      raw_output[i] = sample * note_gate;
      VITAL_ASSERT(utils::isContained(raw_output[i]));
      if (rm_active)
        raw_output[i] *= modulation.ringGain(i);
      levelled_output[i] = current_pan_amplitude * current_level * current_level * raw_output[i];
      if (!utils::isFinite(levelled_output[i]))
        levelled_output[i] = 0.0f;

      VITAL_ASSERT(utils::isFinite(levelled_output[i]));
    }

    if (reset_mask.anyMask()) {
      clearOutputBufferForReset(reset_mask, kReset, kRaw);
      clearOutputBufferForReset(reset_mask, kReset, kLevelled);
    }
  }
} // namespace vital
