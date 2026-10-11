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

#include "dimension_expander_module.h"

#include "poly_utils.h"

#include <algorithm>
#include <cmath>

namespace vital {
namespace {
  constexpr mono_float kTwoPi = 6.28318530717958647692f;

  mono_float clampDimensionScalar(mono_float value, mono_float min, mono_float max) {
    return std::min(std::max(value, min), max);
  }
} // namespace

  DimensionExpanderModule::DimensionExpanderModule(std::string prefix) :
      SynthModule(0, 1), amount_(nullptr), low_cutoff_(nullptr), high_cutoff_(nullptr),
      prefix_(prefix), write_index_(0), amount_smoothed_(0.0f), low_cut_state_(0.0f),
      high_cut_state_(0.0f) {
    delay_buffer_.fill(0.0f);
  }

  void DimensionExpanderModule::init() {
    mode_ = createBaseControl(prefix_ + "dimension_expander_mode");
    quorax_slope_ = createBaseControl(prefix_ + "dimension_expander_quorax_slope");
    const char* names[] = { "size", "mix", "output", "motion", "low_cut", "high_cut" };
    for (int i = 0; i < 6; ++i)
      quorax_controls_[i] = createMonoModControl(prefix_ + "dimension_expander_quorax_" + names[i]);
    quorax_.prepare(getSampleRate());
    amount_ = createMonoModControl(prefix_ + "dimension_expander_amount");
    delay_ = createMonoModControl(prefix_ + "dimension_expander_delay");
    low_cutoff_ = createMonoModControl(prefix_ + "dimension_expander_low_cutoff", true, true);
    high_cutoff_ = createMonoModControl(prefix_ + "dimension_expander_high_cutoff", true, true);

    SynthModule::init();
  }

  mono_float DimensionExpanderModule::onePoleCoefficient(mono_float midi_cutoff, int sample_rate) {
    mono_float frequency = utils::midiNoteToFrequency(midi_cutoff);
    frequency = clampDimensionScalar(frequency, 5.0f, sample_rate * 0.49f);
    return 1.0f - std::exp(-kTwoPi * frequency / sample_rate);
  }

  poly_float DimensionExpanderModule::readDelay(mono_float delay_samples) const {
    int delay_floor = static_cast<int>(delay_samples);
    mono_float fraction = delay_samples - delay_floor;
    int index_a = (write_index_ - delay_floor + kMaxDelaySamples) & (kMaxDelaySamples - 1);
    int index_b = (index_a - 1 + kMaxDelaySamples) & (kMaxDelaySamples - 1);
    return utils::interpolate(delay_buffer_[index_a], delay_buffer_[index_b], fraction);
  }

  void DimensionExpanderModule::processWithInput(const poly_float* audio_in, int num_samples) {
    if (num_samples <= 0) return;
    SynthModule::process(num_samples);
    const int mode = mode_->value() >= 0.5f ? 1 : 0;
    if (mode != last_mode_) {
      hardReset();
      last_mode_ = mode;
    }
    if (mode == 1) {
      auto control = [this](int index, double low, double high) {
        return std::clamp(static_cast<double>(quorax_controls_[index]->buffer[0][0]), low, high);
      };
      jsfx::Quorax::Parameters p;
      p.size = control(0, 0, 100); p.mix = control(1, 0, 100); p.output = control(2, -12, 12);
      p.motion = control(3, 0.02, 2); p.low_cut = control(4, 0, 2000); p.high_cut = control(5, 500, 20000);
      p.slope = std::clamp(static_cast<int>(quorax_slope_->value() + 0.5f), 0, 3);
      quorax_.setParameters(p);
      for (int i = 0; i < num_samples; ++i) {
        const auto frame = quorax_.process({ audio_in[i][0], audio_in[i][1], audio_in[i][2], audio_in[i][3] });
        output()->buffer[i] = poly_float(frame[0], frame[1], frame[2], frame[3]);
      }
      return;
    }

    const mono_float low_cutoff = low_cutoff_->buffer[0][0];
    const mono_float high_cutoff = high_cutoff_->buffer[0][0];
    const bool use_low_cut = low_cutoff > 8.5f;
    const bool use_high_cut = high_cutoff < 135.5f;
    const mono_float low_coefficient = use_low_cut ? onePoleCoefficient(low_cutoff, getSampleRate()) : 0.0f;
    const mono_float high_coefficient = use_high_cut ? onePoleCoefficient(high_cutoff, getSampleRate()) : 1.0f;
    const mono_float target_delay = clampDimensionScalar(delay_->buffer[0][0], 0.0f, 40.0f) *
                                   (0.001f * getSampleRate());
    if (!delay_initialized_) {
      delay_smoothed_ = target_delay;
      delay_initialized_ = true;
    }
    const mono_float delay_smoothing = 1.0f - std::exp(-1.0f / (0.005f * getSampleRate()));

    poly_float current_amount = amount_smoothed_;
    amount_smoothed_ = utils::clamp(amount_->buffer[0], 0.0f, 1.0f);
    poly_float amount_delta = (amount_smoothed_ - current_amount) * (1.0f / num_samples);
    poly_float* audio_out = output()->buffer;

    for (int i = 0; i < num_samples; ++i) {
      current_amount += amount_delta;

      poly_float band = audio_in[i];
      if (use_low_cut) {
        poly_float low = processLowPass(band, low_cut_state_, low_coefficient);
        band -= low;
      }
      if (use_high_cut)
        band = processLowPass(band, high_cut_state_, high_coefficient);

      poly_float swapped = utils::swapStereo(band);
      poly_float mono = (band + swapped) * 0.5f;
      delay_smoothed_ += (target_delay - delay_smoothed_) * delay_smoothing;
      const mono_float delay_samples = clampDimensionScalar(delay_smoothed_, 0.0f, kMaxDelaySamples - 2.0f);
      // Zero delay means the current sample, not a full lap around the ring.
      poly_float delayed = delay_samples < 1.0f ? utils::interpolate(mono, readDelay(1.0f), delay_samples)
                                                : readDelay(delay_samples);
      delay_buffer_[write_index_] = mono;
      write_index_ = (write_index_ + 1) & (kMaxDelaySamples - 1);

      mono_float amount_percent = current_amount[0] * 100.0f;
      mono_float fxamount = 0.5f + amount_percent / 115.0f;
      mono_float direct = (1.5f - fxamount) * fxamount;
      mono_float delayed_amount = (fxamount - 0.5f) * fxamount;

      poly_float expanded = (band + mono * direct) * 0.7f;
      expanded += poly_float(-delayed_amount, delayed_amount, -delayed_amount, delayed_amount) * delayed * 0.7f;

      audio_out[i] = audio_in[i] + (expanded - band) * current_amount;
    }
  }

  void DimensionExpanderModule::hardReset() {
    quorax_.reset();
    last_mode_ = -1;
    delay_buffer_.fill(0.0f);
    write_index_ = 0;
    amount_smoothed_ = 0.0f;
    delay_initialized_ = false;
    low_cut_state_ = 0.0f;
    high_cut_state_ = 0.0f;
  }

  void DimensionExpanderModule::enable(bool enable) {
    SynthModule::enable(enable);
    if (!enable)
      hardReset();
  }

  void DimensionExpanderModule::setSampleRate(int rate) {
    const int old = getSampleRate(); SynthModule::setSampleRate(rate);
    if (getSampleRate() != old) { quorax_.prepare(getSampleRate()); hardReset(); }
  }
  void DimensionExpanderModule::setOversampleAmount(int amount) {
    const int old = getSampleRate(); SynthModule::setOversampleAmount(amount);
    if (getSampleRate() != old) { quorax_.prepare(getSampleRate()); hardReset(); }
  }
} // namespace vital
