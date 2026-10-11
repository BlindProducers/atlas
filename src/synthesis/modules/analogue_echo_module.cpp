// SPDX-License-Identifier: GPL-3.0-or-later
#include "analogue_echo_module.h"
#include "synth_parameters.h"
#include "../effects/analogue/echo_engine.h"
#include <algorithm>
#include <cmath>

namespace vital {
  namespace {
    namespace echo = atlas::analogue::echo;
    enum {
      kHeadOnePan, kHeadTwoPan, kHeadThreePan, kWidth, kFeedback, kInertia, kDrive, kBass, kTreble,
      kWow, kFlutter, kNoise, kAge, kMix, kNumControls
    };
    const char* const kControlNames[kNumControls] = {
      "head_1_pan", "head_2_pan", "head_3_pan", "width", "feedback", "inertia", "drive", "bass", "treble",
      "wow", "flutter", "noise", "age", "mix"
    };
    static_assert(kNumControls == AnalogueEchoModule::kNumModControls);

    int choice(const Value* value, int maximum) {
      return std::clamp(static_cast<int>(std::lround(value->value())), 0, maximum);
    }
  }

  AnalogueEchoModule::AnalogueEchoModule(const Output* beats_per_second, std::string prefix) :
      SynthModule(0, 1), engine_(std::make_unique<echo::Engine>()),
      beats_per_second_(beats_per_second), prefix_(std::move(prefix)) {
    engine_->prepare(getSampleRate());
  }

  AnalogueEchoModule::~AnalogueEchoModule() = default;

  void AnalogueEchoModule::init() {
    const std::string name = prefix_ + "analogueecho";
    for (int i = 0; i < kNumControls; ++i)
      controls_[i] = createMonoModControl(name + "_" + kControlNames[i]);
    // Like Vital's Delay, the free time is stored as a rate so the shared
    // tempo chooser resolves free and synced times to one frequency.
    Output* free_rate = createMonoModControl(name + "_frequency");
    frequency_ = createTempoSyncSwitch(name, free_rate->owner, beats_per_second_, false);
    sync_ = getControls().at(name + "_sync");
    sync_->set(Parameters::getDetails(name + "_sync").default_value);
    medium_ = createBaseControl(name + "_medium");
    for (int head = 0; head < echo::kNumHeads; ++head)
      heads_[head] = createBaseControl(name + "_head_" + std::to_string(head + 1));
    SynthModule::init();
  }

  void AnalogueEchoModule::processWithInput(const poly_float* input, int samples) {
    if (samples <= 0) return;
    SynthModule::process(samples);
    const auto value = [this](int index) { return static_cast<double>(controls_[index]->buffer[0][0]); };

    echo::Parameters p;
    p.medium = choice(medium_, echo::kNumMedia - 1);
    p.synced = choice(sync_, 3) != 0;
    // Freeze (0 Hz) holds the longest time.
    const double frequency = static_cast<double>(frequency_->buffer[0][0]);
    p.time = frequency > 0.0 ? 1.0 / frequency : 10.0;
    for (int head = 0; head < echo::kNumHeads; ++head) {
      p.heads[head] = heads_[head]->value() >= 0.5f;
      p.pans[head] = std::clamp(value(kHeadOnePan + head), -1.0, 1.0);
    }
    p.width = std::clamp(value(kWidth), 0.0, 1.0);
    p.feedback = std::clamp(value(kFeedback), 0.0, 1.2);
    p.inertia = std::clamp(value(kInertia), 0.0, 1.0);
    p.drive = std::clamp(value(kDrive), 0.0, 1.0);
    p.bass_db = std::clamp(value(kBass), -10.0, 10.0);
    p.treble_db = std::clamp(value(kTreble), -10.0, 10.0);
    p.wow = std::clamp(value(kWow), 0.0, 1.0);
    p.flutter = std::clamp(value(kFlutter), 0.0, 1.0);
    p.noise = std::clamp(value(kNoise), 0.0, 1.0);
    p.age = std::clamp(value(kAge), 0.0, 1.0);
    p.mix = std::clamp(value(kMix), 0.0, 1.0);
    engine_->setParameters(p);
    if (reset_pending_) {
      // A fresh instance starts at its settings, without gliding from stale ones.
      engine_->reset();
      reset_pending_ = false;
    }

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

  void AnalogueEchoModule::hardReset() {
    engine_->reset();
    reset_pending_ = true;
  }

  void AnalogueEchoModule::enable(bool on) {
    if (enabled() != on) hardReset();
    SynthModule::enable(on);
  }

  void AnalogueEchoModule::setSampleRate(int rate) {
    const int previous = getSampleRate();
    SynthModule::setSampleRate(rate);
    if (getSampleRate() != previous) {
      engine_->prepare(getSampleRate());
      hardReset();
    }
  }

  void AnalogueEchoModule::setOversampleAmount(int amount) {
    const int previous = getSampleRate();
    SynthModule::setOversampleAmount(amount);
    if (getSampleRate() != previous) {
      engine_->prepare(getSampleRate());
      hardReset();
    }
  }
}
