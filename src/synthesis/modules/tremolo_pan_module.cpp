// SPDX-License-Identifier: GPL-3.0-or-later

#include "tremolo_pan_module.h"

#include <algorithm>
#include <cmath>

namespace vital {

  TremoloPanModule::TremoloPanModule(const Output* beats_per_second, std::string prefix) :
      SynthModule(0, 1), beats_per_second_(beats_per_second), prefix_(std::move(prefix)) { }

  void TremoloPanModule::init() {
    const std::string name = prefix_ + "tremolo_pan";
    Output* free_rate = createMonoModControl(name + "_frequency");
    // A linear mod control comes from ValueSwitch's second output. Feed the
    // actual rate through a bounded processor with a primary output, as the
    // shared tempo chooser accepts a Processor rather than an Output.
    auto* bounded_rate = new cr::Clamp(0.01f, 20.0f);
    bounded_rate->plug(free_rate);
    addMonoProcessor(bounded_rate);
    frequency_ = createTempoSyncSwitch(name, bounded_rate, beats_per_second_, false);
    sync_ = getControls().at(name + "_sync");
    shape_ = createBaseControl(name + "_shape");
    stereo_phase_ = createMonoModControl(name + "_stereo_phase");
    offset_ = createMonoModControl(name + "_offset");
    depth_ = createMonoModControl(name + "_depth");
    smoothing_ = createMonoModControl(name + "_smoothing");
    mix_ = createMonoModControl(name + "_mix");
    SynthModule::init();
  }

  double TremoloPanModule::waveform(int shape, double phase) {
    phase -= std::floor(phase);
    switch (shape) {
      case kTriangle: return std::abs(2.0 * phase - 1.0);
      case kSquare: return phase < 0.5 ? 1.0 : 0.0;
      case kRampUp: return phase;
      case kRampDown: return 1.0 - phase;
      default: return 0.5 + 0.5 * std::cos(2.0 * 3.14159265358979323846 * phase);
    }
  }

  void TremoloPanModule::correctToTime(double seconds) {
    transport_time_ = seconds;
    has_transport_time_ = true;
  }

  void TremoloPanModule::hardReset() {
    lanes_ = {};
    shape_weights_ = {};
    initialized_ = false;
    timeline_valid_ = false;
    previous_sync_ = 0;
  }

  void TremoloPanModule::enable(bool enable) {
    if (enable != enabled())
      hardReset();
    SynthModule::enable(enable);
  }

  void TremoloPanModule::processWithInput(const poly_float* audio_in, int num_samples) {
    if (num_samples <= 0)
      return;
    SynthModule::process(num_samples);

    const double sample_rate = std::max(1, getSampleRate());
    const double tick = 1.0 / sample_rate;
    // Fixed time constants keep automation smoothing independent of block size
    // and oversampling. Smoothing is separate from the requested LFO edge slew.
    const double control_step = -std::expm1(-tick / 0.005);
    const int shape = std::clamp(static_cast<int>(shape_->value()), 0, kNumShapes - 1);
    const int sync = static_cast<int>(sync_->value());
    const bool seek = has_transport_time_ &&
        (!timeline_valid_ || std::abs(transport_time_ - expected_transport_time_) > 2.0 * tick);
    const bool align = sync != 0 && has_transport_time_ && (seek || previous_sync_ == 0);
    std::array<Lane, poly_float::kSize> targets;
    std::array<double, poly_float::kSize> wave_steps;
    for (int lane = 0; lane < poly_float::kSize; ++lane) {
      auto& target = targets[lane];
      target.frequency = sync == 0 ? std::clamp<double>(frequency_->buffer[0][lane], 0.01, 20.0)
                                   : std::clamp<double>(frequency_->buffer[0][lane], 0.0, sample_rate * 0.49);
      target.stereo_phase = std::clamp<double>(stereo_phase_->buffer[0][lane], 0.0, 1.0);
      target.offset = std::clamp<double>(offset_->buffer[0][lane], 0.0, 1.0);
      target.depth = std::clamp<double>(depth_->buffer[0][lane], 0.0, 1.0);
      target.mix = std::clamp<double>(mix_->buffer[0][lane], 0.0, 1.0);
      const double milliseconds = std::clamp<double>(smoothing_->buffer[0][lane], 0.0, 20.0);
      wave_steps[lane] = milliseconds <= 0.0 ? 1.0 : -std::expm1(-tick / (0.001 * milliseconds));
      if (!initialized_)
        lanes_[lane] = target;
      auto& state = lanes_[lane];
      if (align) {
        state.phase = transport_time_ * target.frequency;
        state.phase -= std::floor(state.phase);
      }
      if (!initialized_)
        state.wave = waveform(shape, state.phase + state.offset + (lane % 2) * state.stereo_phase);
    }
    if (!initialized_)
      shape_weights_[shape] = 1.0;

    poly_float* audio_out = output()->buffer;
    for (int sample = 0; sample < num_samples; ++sample) {
      for (int index = 0; index < kNumShapes; ++index)
        shape_weights_[index] += ((index == shape ? 1.0 : 0.0) - shape_weights_[index]) * control_step;
      // The voice handler sums both voices into both SIMD pairs, so lanes 2-3
      // repeat lanes 0-1: only lanes 0-1 are computed, and lanes 2-3 copy them.
      poly_float gains;
      for (int lane = 0; lane < 2; ++lane) {
        auto& state = lanes_[lane];
        const auto& target = targets[lane];
        // Sync adopts live tempo immediately; free-rate edits slew without
        // resetting the cycle. Transport corrections only reanchor on seeks.
        state.frequency += (target.frequency - state.frequency) * (sync != 0 ? 1.0 : control_step);
        state.stereo_phase += (target.stereo_phase - state.stereo_phase) * control_step;
        state.offset += (target.offset - state.offset) * control_step;
        state.depth += (target.depth - state.depth) * control_step;
        state.mix += (target.mix - state.mix) * control_step;
        const double phase = state.phase + state.offset + (lane % 2) * state.stereo_phase;
        double wave = 0.0;
        for (int index = 0; index < kNumShapes; ++index) {
          if (shape_weights_[index] > 1.0e-12)
            wave += shape_weights_[index] * waveform(index, phase);
        }
        state.wave += (wave - state.wave) * wave_steps[lane];
        const float gain = static_cast<float>(1.0 - state.mix * state.depth * (1.0 - state.wave));
        gains.set(lane, gain);
        gains.set(lane + 2, gain);
        state.phase += state.frequency * tick;
        state.phase -= std::floor(state.phase);
      }
      audio_out[sample] = audio_in[sample] * gains;
    }
    if (has_transport_time_) {
      expected_transport_time_ = transport_time_ + num_samples * tick;
      timeline_valid_ = true;
    }
    has_transport_time_ = false;
    previous_sync_ = sync;
    initialized_ = true;
  }
}
