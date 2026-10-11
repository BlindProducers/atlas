/* Copyright 2013-2019 Matt Tytel
 *
 * vital is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "synth_lfo.h"

#include "common.h"
#include "line_generator.h"
#include "synth_constants.h"
#include "utils.h"

#include "futils.h"

#include <algorithm>
#include <cmath>

namespace vital {
  namespace {
    force_inline poly_float lfoRateMultiplier(const poly_float& rate_x10) {
      return utils::maskLoad(poly_float(1.0f), poly_float(10.0f),
                             poly_float::greaterThan(rate_x10, 0.5f));
    }

    force_inline float finiteOr(float value, float fallback) {
      return futils::isFinite(value) ? value : fallback;
    }
  } // namespace

  SynthLfo::SynthLfo(LineGenerator* source) : Processor(kNumInputs, kNumOutputs), source_(source),
      sync_seconds_(std::make_shared<double>(0.0)) { }

  poly_float SynthLfo::applySwingToPhase(poly_float phase, bool preserve_end) const {
    if (preserve_end)
      return utils::clamp(phase, 0.0f, 1.0f);

    const float amount = (source_->swing() - LineGenerator::defaultSwing()) * 2.0f;
    if (fabsf(amount) < 0.0001f)
      return utils::mod(phase);

    static constexpr float kCycleStep = 1.0f;
    static constexpr float kPairWidth = 2.0f;
    const float swing_point = utils::clamp(kCycleStep + amount * 0.5f, 0.25f, 1.75f);
    poly_float source_phase = utils::mod(phase * 0.5f) * 2.0f;
    poly_float pair_start = utils::floor(source_phase * (1.0f / kPairWidth)) * kPairWidth;
    poly_float local = source_phase - pair_start;
    poly_float first_half = local * (1.0f / swing_point);
    poly_float second_half = poly_float(kCycleStep) +
                             (local - swing_point) * (1.0f / (kPairWidth - swing_point));
    poly_float swung = pair_start + utils::maskLoad(second_half, first_half,
                                                    poly_float::lessThan(local, swing_point));
    return utils::mod(swung);
  }

  void SynthLfo::updateLineGeneratorControls() {
    source_->setDeform(utils::clamp(input(kDeform)->at(0)[0], 0.0f, 1.0f));
    source_->setSwing(utils::clamp(input(kSwing)->at(0)[0], 0.0f, 1.0f));
  }

  poly_float SynthLfo::inputAt(int input_index, int sample) const {
    const Output* source = input(input_index)->source;
    if (source->buffer_size > 1)
      return source->buffer[std::min(sample, source->buffer_size - 1)];
    return source->buffer[0];
  }

  void SynthLfo::applyEventAtSample(int sample, int sync_type, const poly_float& frequency,
                                    const poly_mask& trigger_mask, const poly_float& trigger_value,
                                    const poly_int& trigger_offset) {
    for (int lane = 0; lane < poly_float::kSize; ++lane) {
      if (!trigger_mask[lane] || trigger_offset[lane] != static_cast<uint32_t>(sample))
        continue;

      const int event = static_cast<int>(std::round(trigger_value[lane]));
      if (event == kVoiceOff || event == kVoiceKill) {
        held_mask_.set(lane, 0);
        continue;
      }
      if (event != kVoiceOn)
        continue;

      held_mask_.set(lane, poly_int::kFullMask);
      state_.delay_time_passed.set(lane, 0.0f);
      state_.fade_amplitude.set(lane, 0.0f);
      state_.smooth_value.set(lane, 0.0f);
      if (sync_type == kSync) {
        const double event_seconds = *sync_seconds_ + static_cast<double>(sample) / getSampleRate();
        phase_clock_[lane] = event_seconds * static_cast<double>(frequency[lane]);
      }
      else
        phase_clock_[lane] = 0.0;
    }
  }

  void SynthLfo::advanceClock(int lane, int sync_type, double delta, double loop_point, bool held) {
    double& clock = phase_clock_[lane];
    clock += std::max(0.0, delta);

    if (sync_type == kTrigger || sync_type == kSync) {
      clock = std::fmod(clock, 2.0);
      if (clock < 0.0)
        clock += 2.0;
    }
    else if (sync_type == kEnvelope)
      clock = std::min(clock, 1.0);
    else if (sync_type == kSustainEnvelope) {
      const double maximum = held ? utils::clamp(loop_point, 0.0, 1.0) : 1.0;
      clock = std::min(clock, maximum);
    }
    else if (sync_type == kLoopPoint) {
      const double start = utils::clamp(loop_point, 0.0, 1.0);
      const double width = 1.0 - start;
      if (clock >= 1.0)
        clock = width > 1.0e-12 ? start + std::fmod(clock - 1.0, width) : 1.0;
    }
    else if (sync_type == kLoopHold) {
      const double end = utils::clamp(loop_point, 0.0, 1.0);
      if (held && clock >= end)
        clock = end > 1.0e-12 ? std::fmod(clock, end) : 0.0;
      else if (!held)
        clock = std::min(clock, 1.0);
    }
  }

  void SynthLfo::process(int num_samples) {
    if (num_samples <= 0)
      return;

    updateLineGeneratorControls();

    const int sync_type = utils::iclamp(static_cast<int>(std::round(input(kSyncType)->at(0)[0])),
                                        0, kNumSyncTypes - 1);
    const poly_mask trigger_mask = input(kNoteTrigger)->source->trigger_mask;
    const poly_float trigger_value = input(kNoteTrigger)->source->trigger_value;
    const poly_int trigger_offset = input(kNoteTrigger)->source->trigger_offset;
    const float tick_time = 1.0f / getSampleRate();
    const bool smooth_mode = input(kSmoothMode)->at(0)[0] >= 0.5f;
    const bool preserve_end = sync_type != kTrigger && sync_type != kSync;
    poly_float last_result = state_.smooth_value;
    poly_float last_phase = 0.0f;
    poly_float last_frequency = 0.0f;
    auto source_buffer = source_->acquireBuffer();
    const int source_resolution = source_->resolution();

    for (int sample = 0; sample < num_samples; ++sample) {
      poly_float frequency = inputAt(kFrequency, sample) * lfoRateMultiplier(inputAt(kRateX10, sample));
      for (int lane = 0; lane < poly_float::kSize; ++lane) {
        const float safe = utils::clamp(finiteOr(frequency[lane], 0.0f),
                                        0.0f, getSampleRate() * 0.5f);
        frequency.set(lane, safe);
      }
      applyEventAtSample(sample, sync_type, frequency, trigger_mask, trigger_value, trigger_offset);

      const poly_float user_phase = inputAt(kPhase, sample);
      const poly_float stereo_phase = inputAt(kStereoPhase, sample);
      const poly_float phase_control = user_phase + stereo_phase * poly_float(0.5f, -0.5f);
      const poly_float delay_time = utils::max(inputAt(kDelay, sample), 0.0f);
      const poly_float fade_time = utils::max(inputAt(kFade, sample), 0.0f);
      const poly_float smooth_time = utils::max(inputAt(kSmoothTime, sample), 0.0f);

      poly_float raw_phase = 0.0f;
      poly_mask past_delay_mask = 0;
      for (int lane = 0; lane < poly_float::kSize; ++lane) {
        const bool past_delay = state_.delay_time_passed[lane] + 1.0e-12f >= delay_time[lane];
        past_delay_mask.set(lane, past_delay ? poly_int::kFullMask : 0);

        if (sync_type == kSync) {
          const double seconds = *sync_seconds_ + static_cast<double>(sample) / getSampleRate();
          phase_clock_[lane] = seconds * static_cast<double>(frequency[lane]);
        }

        double phase = phase_clock_[lane];
        if (sync_type == kTrigger || sync_type == kSync)
          phase += finiteOr(phase_control[lane], 0.0f);
        else if (sync_type == kEnvelope)
          phase = std::min(phase + static_cast<double>(finiteOr(phase_control[lane], 0.0f)), 1.0);
        raw_phase.set(lane, static_cast<float>(phase));
      }

      poly_float sample_phase = applySwingToPhase(raw_phase, preserve_end);
      poly_float waveform_value = getValueAtPhase(source_buffer.get(), source_resolution,
                                                   source_resolution - 1, sample_phase);
      poly_float result = 0.0f;
      for (int lane = 0; lane < poly_float::kSize; ++lane) {
        float current = 0.0f;
        if (past_delay_mask[lane]) {
          float processed_value = waveform_value[lane];
          if (smooth_mode) {
            const float half_life = smooth_time[lane] * kHalfLifeRatio;
            const float ratio = half_life > kMinHalfLife ? std::exp2(-tick_time / half_life) : 0.0f;
            processed_value = utils::interpolate(waveform_value[lane], state_.smooth_value[lane], ratio);
            state_.smooth_value.set(lane, processed_value);
          }
          else
            state_.smooth_value.set(lane, waveform_value[lane]);

          const float amplitude = fade_time[lane] <= tick_time ? 1.0f :
              std::min(1.0f, state_.fade_amplitude[lane] + tick_time / fade_time[lane]);
          state_.fade_amplitude.set(lane, amplitude);
          current = processed_value * amplitude;
        }
        result.set(lane, utils::clamp(finiteOr(current, 0.0f), -1.0f, 1.0f));

        state_.delay_time_passed.set(lane, state_.delay_time_passed[lane] + tick_time);
        if (past_delay_mask[lane] && sync_type != kSync) {
          const bool held = held_mask_[lane] != 0;
          advanceClock(lane, sync_type, frequency[lane] * tick_time,
                       finiteOr(phase_control[lane], 0.0f), held);
        }
        state_.offset.set(lane, static_cast<float>(phase_clock_[lane]));
      }

      if (!isControlRate())
        output(kValue)->buffer[sample] = result;
      last_result = result;
      last_phase = sample_phase;
      last_frequency = frequency;
      state_.phase = phase_control;
    }

    if (isControlRate())
      output(kValue)->buffer[0] = last_result;
    output(kValue)->trigger_value = last_result;
    output(kOscPhase)->buffer[0] = utils::encodePhaseAndVoice(last_phase, input(kNoteCount)->at(0));
    output(kOscFrequency)->buffer[0] = last_frequency;
  }

  void SynthLfo::correctToTime(double seconds) {
    *sync_seconds_ = futils::isFinite(seconds) ? seconds : 0.0;
  }
} // namespace vital
