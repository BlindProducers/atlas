// SPDX-License-Identifier: GPL-3.0-or-later
#include "analogue_flanger_module.h"
#include "synth_parameters.h"
#include "../effects/analogue/flanger_engine.h"
#include <algorithm>
#include <cmath>

namespace vital {
  namespace {
    namespace flanger = atlas::analogue::flanger;
    enum { kDepth, kManual, kFeedback, kStereoPhase, kNoise, kAge, kMix, kNumControls };
    const char* const kControlNames[kNumControls] = {
      "depth", "manual", "feedback", "stereo_phase", "noise", "age", "mix"
    };
    static_assert(kNumControls == AnalogueFlangerModule::kNumModControls);

    int choice(const Value* value, int maximum) {
      return std::clamp(static_cast<int>(std::lround(value->value())), 0, maximum);
    }
  }

  AnalogueFlangerModule::AnalogueFlangerModule(const Output* beats_per_second, std::string prefix) :
      SynthModule(0, 1), engine_(std::make_unique<flanger::Engine>()),
      beats_per_second_(beats_per_second), prefix_(std::move(prefix)) {
    engine_->prepare(getSampleRate());
  }

  AnalogueFlangerModule::~AnalogueFlangerModule() = default;

  void AnalogueFlangerModule::init() {
    const std::string name = prefix_ + "analogueflanger";
    for (int i = 0; i < kNumControls; ++i)
      controls_[i] = createMonoModControl(name + "_" + kControlNames[i]);
    // An exponential control's scaled output is its owner's primary output,
    // which the shared tempo chooser takes as the free-running rate.
    Output* free_rate = createMonoModControl(name + "_frequency");
    frequency_ = createTempoSyncSwitch(name, free_rate->owner, beats_per_second_, false);
    sync_ = getControls().at(name + "_sync");
    sync_->set(Parameters::getDetails(name + "_sync").default_value);
    medium_ = createBaseControl(name + "_medium");
    SynthModule::init();
  }

  void AnalogueFlangerModule::correctToTime(double seconds) {
    transport_time_ = seconds;
    has_transport_time_ = true;
  }

  void AnalogueFlangerModule::processWithInput(const poly_float* input, int samples) {
    if (samples <= 0) return;
    SynthModule::process(samples);
    const auto value = [this](int index) { return static_cast<double>(controls_[index]->buffer[0][0]); };

    flanger::Parameters p;
    p.medium = choice(medium_, flanger::kNumMedia - 1);
    p.lfo_frequency = std::max(0.0, static_cast<double>(frequency_->buffer[0][0]));
    p.depth = std::clamp(value(kDepth), 0.0, 1.0);
    p.manual = std::clamp(value(kManual), 0.0, 1.0);
    p.feedback = std::clamp(value(kFeedback), -1.0, 1.0);
    p.stereo_phase = std::clamp(value(kStereoPhase), 0.0, 1.0);
    p.noise = std::clamp(value(kNoise), 0.0, 1.0);
    p.age = std::clamp(value(kAge), 0.0, 1.0);
    p.mix = std::clamp(value(kMix), 0.0, 1.0);
    engine_->setParameters(p);
    if (reset_pending_) {
      // A fresh instance starts at its settings, without smoothing from stale ones.
      engine_->reset();
      reset_pending_ = false;
    }

    // Tempo sync anchors the sweep to the host timeline when it starts and
    // after seeks, then runs freely so live tempo changes stay continuous.
    const int sync = choice(sync_, 3);
    const double tick = 1.0 / std::max(1, getSampleRate());
    const bool seek = has_transport_time_ &&
        (!timeline_valid_ || std::abs(transport_time_ - expected_transport_time_) > 2.0 * tick);
    if (sync != 0 && has_transport_time_ && (seek || previous_sync_ == 0))
      engine_->alignLfo(transport_time_ * p.lfo_frequency);
    if (has_transport_time_) {
      expected_transport_time_ = transport_time_ + samples * tick;
      timeline_valid_ = true;
    }
    has_transport_time_ = false;
    previous_sync_ = sync;

    poly_float* output_buffer = output()->buffer;
    for (int offset = 0; offset < samples;) {
      const int count = std::min(kMaxBufferSize, samples - offset);
      float in_left[kMaxBufferSize], in_right[kMaxBufferSize];
      float out_left[kMaxBufferSize], out_right[kMaxBufferSize];
      for (int i = 0; i < count; ++i) {
        in_left[i] = input[offset + i][0];
        in_right[i] = input[offset + i][1];
      }
      engine_->process(in_left, in_right, out_left, out_right, count);
      for (int i = 0; i < count; ++i)
        output_buffer[offset + i] = poly_float(out_left[i], out_right[i], out_left[i], out_right[i]);
      offset += count;
    }
  }

  void AnalogueFlangerModule::hardReset() {
    engine_->reset();
    reset_pending_ = true;
    timeline_valid_ = false;
    previous_sync_ = 0;
  }

  void AnalogueFlangerModule::enable(bool on) {
    if (enabled() != on) hardReset();
    SynthModule::enable(on);
  }

  void AnalogueFlangerModule::setSampleRate(int rate) {
    const int previous = getSampleRate();
    SynthModule::setSampleRate(rate);
    if (getSampleRate() != previous) {
      engine_->prepare(getSampleRate());
      hardReset();
    }
  }

  void AnalogueFlangerModule::setOversampleAmount(int amount) {
    const int previous = getSampleRate();
    SynthModule::setOversampleAmount(amount);
    if (getSampleRate() != previous) {
      engine_->prepare(getSampleRate());
      hardReset();
    }
  }
}
