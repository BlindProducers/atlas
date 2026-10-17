// SPDX-License-Identifier: GPL-3.0-or-later
#include "trance_gate_module.h"

namespace vital {
  TranceGateModule::TranceGateModule(const Output* bps, const TranceGateClock* clock, std::string prefix) :
      SynthModule(0, 1), bps_(bps), clock_(clock), prefix_(std::move(prefix)) { }

  void TranceGateModule::init() {
    const auto name = prefix_ + "trance_gate_";
    mix_ = createMonoModControl(name + "mix");
    trigger_ = createBaseControl(name + "trigger");
    grid_ = createBaseControl(name + "grid");
    start_ = createBaseControl(name + "start");
    stop_ = createBaseControl(name + "stop");
    attack_ = createMonoModControl(name + "attack");
    decay_ = createMonoModControl(name + "decay");
    sustain_ = createMonoModControl(name + "sustain");
    for (int i = 0; i < trance_gate::kSteps; ++i) {
      const auto step = name + "step_" + std::to_string(i + 1) + "_";
      values_[i] = createBaseControl(step + "value");
      modes_[i] = createBaseControl(step + "mode");
      curves_[i] = createBaseControl(step + "curve");
    }
    SynthModule::init();
  }

  void TranceGateModule::enable(bool on) {
    if (on != enabled()) hardReset();
    SynthModule::enable(on);
  }

  void TranceGateModule::processWithInput(const poly_float* audio, int samples) {
    if (samples <= 0) return;
    SynthModule::process(samples);
    const bool midi = trigger_->value() >= 0.5f;
    if (clock_ == nullptr || (!midi && !clock_->playing())) {
      utils::copyBuffer(output()->buffer, audio, samples);
      hardReset();
      return;
    }
    const double rate = std::max(1, getSampleRate());
    const double bps = std::max(0.001, static_cast<double>(bps_->buffer[0][0]));
    const double beat_div = trance_gate::beatsPerStep(static_cast<int>(grid_->value()));
    const double duration = beat_div / bps;
    const double smoothing = 1.0 - std::exp(-1.0 / (0.002 * rate));
    const double mix_smoothing = -std::expm1(-1.0 / (0.005 * rate));
    const int start = std::clamp(static_cast<int>(start_->value()), 1, trance_gate::kSteps);
    const int stop = std::clamp(static_cast<int>(stop_->value()), 1, trance_gate::kSteps);
    const int oversampling = getOversampleAmount();
    std::array<trance_gate::Step, trance_gate::kSteps> steps;
    std::array<trance_gate::Envelope, poly_float::kSize> envelopes;
    std::array<double, poly_float::kSize> mix_targets;
    for (int i = 0; i < trance_gate::kSteps; ++i)
      steps[i] = { std::clamp<double>(values_[i]->value(), -1.0, 1.0),
                   std::clamp(static_cast<int>(modes_[i]->value()), 0, trance_gate::kModes - 1),
                   std::clamp<double>(curves_[i]->value(), -1.0, 1.0) };
    for (int lane = 0; lane < poly_float::kSize; ++lane) {
      envelopes[lane] = { std::clamp<double>(attack_->buffer[0][lane], 0.0, 500.0) / 1000.0,
                          std::clamp<double>(decay_->buffer[0][lane], 0.0, 500.0) / 1000.0,
                          std::clamp<double>(sustain_->buffer[0][lane], 0.0, 1.0) };
      mix_targets[lane] = std::clamp<double>(mix_->buffer[0][lane], 0.0, 1.0);
      if (!mix_initialized_) smoothed_mix_[lane] = mix_targets[lane];
    }
    mix_initialized_ = true;
    for (int sample = 0; sample < samples; ++sample) {
      double beat = (transport_seconds_ + sample / rate) * bps;
      if (midi) {
        beat = clock_->midiBeat(sample / oversampling);
        if (beat < 0.0) {
          output()->buffer[sample] = audio[sample];
          gain_.fill(0.0);
          continue;
        }
        beat += (sample % oversampling) * bps / rate;
      }
      double position = beat / beat_div;
      // Host sub-block arithmetic can put a mathematically exact boundary a
      // few ulps on either side. Stabilize step and half/quarter gate edges so
      // MIDI event splits and host buffer sizes cannot move an edge by a sample.
      const double boundary = std::round(position * 4.0) * 0.25;
      if (std::abs(position - boundary) < 1.0e-9) position = boundary;
      const double whole_steps = std::floor(position);
      const auto& step = steps[trance_gate::stepIndex(whole_steps, start, stop)];
      // The voice handler sums both voices into both SIMD pairs, so lanes 2-3
      // repeat lanes 0-1: only lanes 0-1 are computed, and lanes 2-3 copy them.
      poly_float gain;
      for (int lane = 0; lane < 2; ++lane) {
        const double target = trance_gate::targetGain(step, envelopes[lane], position - whole_steps, duration);
        gain_[lane] += (target - gain_[lane]) * smoothing;
        // Linear dry/wet blending preserves unity. Smooth only Mix edits;
        // the original StepGate gain smoothing remains unchanged at 100% wet.
        smoothed_mix_[lane] += (mix_targets[lane] - smoothed_mix_[lane]) * mix_smoothing;
        const float value = static_cast<float>((1.0 - smoothed_mix_[lane]) + smoothed_mix_[lane] * gain_[lane]);
        gain.set(lane, value);
        gain.set(lane + 2, value);
      }
      output()->buffer[sample] = audio[sample] * gain;
    }
    transport_seconds_ += samples / rate;
  }
}
