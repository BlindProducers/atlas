// SPDX-License-Identifier: GPL-3.0-or-later
#include "maim_module.h"
#include "maim/maim_engine.h"
#include "prepared_effect_module.h"
#include <algorithm>
#include <cmath>

namespace vital {
  namespace {
    namespace maim = atlas::maim;
    const char* const kModSuffixes[MaimModule::kNumModControls] = {
      "turbo", "tilt", "error", "mix", "shift", "level_shift", "feedback", "butterfly_straight",
      "butterfly_crossed", "loss_period", "loss_width", "loss_jitter", "drive", "makeup", "low_cut", "high_cut"
    };
    const char* const kBaseSuffixes[MaimModule::kNumBaseControls] = {
      "encoder", "bitrate", "lpc_rate", "band_step", "band_invert", "window_step", "band_shift", "band_reverse",
      "loss_mode"
    };

    int roundedChoice(float value, int low, int high) {
      if (!std::isfinite(value)) return low;
      return std::clamp(static_cast<int>(std::lround(value)), low, high);
    }
  }

  const char* MaimModule::modControlSuffix(int control) { return kModSuffixes[control]; }
  const char* MaimModule::baseControlSuffix(int control) { return kBaseSuffixes[control]; }

  MaimModule::MaimModule(std::string prefix) :
      SynthModule(0, 1), engine_(std::make_unique<maim::Engine>()), prefix_(std::move(prefix)) { }

  MaimModule::~MaimModule() = default;

  void MaimModule::init() {
    const std::string name = prefix_ + "maim_";
    for (int i = 0; i < kNumModControls; ++i)
      mod_controls_[static_cast<size_t>(i)] = createMonoModControl(name + kModSuffixes[i]);
    for (int i = 0; i < kNumBaseControls; ++i)
      base_controls_[static_cast<size_t>(i)] = createBaseControl(name + kBaseSuffixes[i]);
    SynthModule::init();
  }

  int MaimModule::latencyFor(float encoder, float lpc_rate, int sample_rate) {
    return std::min(kMaxLatencySamples,
                    maim::Engine::latencyFor(roundedChoice(encoder, 0, maim::kNumEncoders - 1),
                                             roundedChoice(lpc_rate, 0, maim::kNumLpcRates - 1), sample_rate));
  }

  int MaimModule::getLatencySamples() const {
    const Value* encoder = base_controls_[kEncoder];
    const Value* lpc_rate = base_controls_[kLpcRate];
    return latencyFor(encoder ? encoder->value() : maim::kLame, lpc_rate ? lpc_rate->value() : maim::kDefaultLpcRateIndex,
                      getSampleRate());
  }

  void MaimModule::prepareEngine() {
    prepared_rate_ = getSampleRate();
    engine_->prepare(prepared_rate_);
    reset_pending_ = false;
  }

  void MaimModule::processWithInput(const poly_float* input, int samples) {
    if (samples <= 0) return;
    SynthModule::process(samples);
    const bool realtime = RealtimeEffectScope::active();
    if (!engine_->prepared() || prepared_rate_ != getSampleRate()) {
      if (realtime) {
        // Built by the preparation worker before any realtime block; until
        // then the audio passes through.
        utils::copyBuffer(output()->buffer, input, samples);
        return;
      }
      prepareEngine();
    }

    const auto mod = [this](int index) { return mod_controls_[static_cast<size_t>(index)]->buffer[0][0]; };
    const auto base = [this](int index) { return base_controls_[static_cast<size_t>(index)]->value(); };
    maim::Parameters p;
    p.encoder = roundedChoice(base(kEncoder), 0, maim::kNumEncoders - 1);
    p.bitrate_index = roundedChoice(base(kBitrate), 0, maim::kNumBitrates - 1);
    p.lpc_rate_index = roundedChoice(base(kLpcRate), 0, maim::kNumLpcRates - 1);
    p.band_step = roundedChoice(base(kBandStep), 8, 18);
    p.band_invert = base(kBandInvert) >= 0.5f;
    p.window_step = roundedChoice(base(kWindowStep), -64, 64);
    p.band_shift = roundedChoice(base(kBandShift), -31, 31);
    p.band_reverse = base(kBandReverse) >= 0.5f;
    p.loss_mode = roundedChoice(base(kLossMode), 0, maim::kNumLossModes - 1);
    p.turbo = mod(kTurbo);
    p.tilt = mod(kTilt);
    p.error = mod(kError);
    p.mix = mod(kMix);
    p.shift = roundedChoice(mod(kShift), -100, 100);
    p.level_shift = mod(kLevelShift);
    p.feedback = mod(kFeedback);
    p.butterfly_straight = mod(kButterflyStraight);
    p.butterfly_crossed = mod(kButterflyCrossed);
    // The mod controls apply each parameter's scale: the period arrives in
    // seconds and the cutoffs in hertz.
    p.loss_period = mod(kLossPeriod);
    p.loss_width = mod(kLossWidth);
    p.loss_jitter = mod(kLossJitter);
    p.drive_db = mod(kDrive);
    p.makeup_db = mod(kMakeup);
    p.low_cut_hz = mod(kLowCut);
    p.high_cut_hz = mod(kHighCut);
    engine_->setParameters(p, realtime);
    if (reset_pending_) {
      engine_->reset();
      reset_pending_ = false;
    }

    poly_float* output_buffer = output()->buffer;
    for (int offset = 0; offset < samples;) {
      const int count = std::min(kMaxBufferSize, samples - offset);
      float left[kMaxBufferSize], right[kMaxBufferSize];
      for (int i = 0; i < count; ++i) {
        left[i] = input[offset + i][0];
        right[i] = input[offset + i][1];
      }
      engine_->process(left, right, left, right, count);
      for (int i = 0; i < count; ++i)
        output_buffer[offset + i] = poly_float(left[i], right[i], left[i], right[i]);
      offset += count;
    }
  }

  void MaimModule::hardReset() {
    reset_pending_ = true;
  }

  void MaimModule::enable(bool on) {
    if (enabled() != on) hardReset();
    SynthModule::enable(on);
  }

  void MaimModule::setSampleRate(int rate) {
    SynthModule::setSampleRate(rate);
    if (getSampleRate() != prepared_rate_) prepareEngine();
  }

  void MaimModule::setOversampleAmount(int amount) {
    SynthModule::setOversampleAmount(amount);
    if (getSampleRate() != prepared_rate_) prepareEngine();
  }
}
