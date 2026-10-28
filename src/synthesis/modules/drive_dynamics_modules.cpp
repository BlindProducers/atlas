// SPDX-License-Identifier: GPL-3.0-or-later
#include "drive_dynamics_modules.h"

#include <algorithm>
#include <cmath>

namespace vital {
  namespace dd = atlas::drive_dynamics;
  using namespace drive_dynamics;

  template<class Core, size_t kNumControls>
  DriveDynamicsModule<Core, kNumControls>::DriveDynamicsModule(std::string prefix,
                                                               std::array<Control, kNumControls> controls,
                                                               Parameters (*parameters)(const Values&)) :
      SynthModule(0, 1), prefix_(std::move(prefix)), controls_(std::move(controls)), parameters_(parameters) { }

  template<class Core, size_t kNumControls>
  void DriveDynamicsModule<Core, kNumControls>::init() {
    for (size_t i = 0; i < kNumControls; ++i) {
      const std::string name = prefix_ + controls_[i].suffix;
      if (controls_[i].modulated) mod_controls_[i] = createMonoModControl(name);
      else base_controls_[i] = createBaseControl(name);
    }
    SynthModule::init();
  }

  template<class Core, size_t kNumControls>
  typename DriveDynamicsModule<Core, kNumControls>::Values DriveDynamicsModule<Core, kNumControls>::readValues() const {
    Values values {};
    for (size_t i = 0; i < kNumControls; ++i) {
      const float value = controls_[i].modulated ? mod_controls_[i]->buffer[0][0] : base_controls_[i]->value();
      values[i] = std::isfinite(value) ? std::clamp(value, controls_[i].minimum, controls_[i].maximum)
                                       : controls_[i].minimum;
    }
    return values;
  }

  template<class Core, size_t kNumControls>
  void DriveDynamicsModule<Core, kNumControls>::processWithInput(const poly_float* input, int samples) {
    if (samples <= 0) return;
    SynthModule::process(samples);
    const Values values = readValues();
    // @init on a reset or a new rate, then @slider whenever a control moves.
    if (reset_pending_ || prepared_rate_ != getSampleRate()) {
      prepared_rate_ = getSampleRate();
      core_.prepare(prepared_rate_);
      core_.setParameters(parameters_(values));
      applied_ = values;
      reset_pending_ = false;
    }
    else if (values != applied_) {
      core_.setParameters(parameters_(values));
      applied_ = values;
    }

    poly_float* output_buffer = output()->buffer;
    for (int i = 0; i < samples; ++i) {
      double left = input[i][0], right = input[i][1];
      core_.process(left, right);
      const float l = static_cast<float>(left), r = static_cast<float>(right);
      output_buffer[i] = poly_float(l, r, l, r);
    }
  }

  template<class Core, size_t kNumControls>
  void DriveDynamicsModule<Core, kNumControls>::enable(bool on) {
    if (enabled() != on) hardReset();
    SynthModule::enable(on);
  }

  template<class Core, size_t kNumControls>
  void DriveDynamicsModule<Core, kNumControls>::setSampleRate(int rate) {
    SynthModule::setSampleRate(rate);
    if (getSampleRate() != prepared_rate_) hardReset();
  }

  template<class Core, size_t kNumControls>
  void DriveDynamicsModule<Core, kNumControls>::setOversampleAmount(int amount) {
    SynthModule::setOversampleAmount(amount);
    if (getSampleRate() != prepared_rate_) hardReset();
  }

  template<class Core, size_t kNumControls>
  int DriveDynamicsModule<Core, kNumControls>::getLatencySamples() const {
    // Read from the controls, so a change to Hi-Quality reports its latency before the next block.
    return Core::latencyFor(parameters_(readValues()), getSampleRate());
  }

  namespace {
    dd::Saturator::Parameters saturatorParameters(const std::array<double, kNumSaturatorControls>& v) {
      dd::Saturator::Parameters p;
      p.drive = v[kSaturatorDrive]; p.type = v[kSaturatorType]; p.color = v[kSaturatorColor];
      p.amt_lo = v[kSaturatorColorLow]; p.frequency = v[kSaturatorColorFrequency];
      p.width = v[kSaturatorColorWidth]; p.amt_hi = v[kSaturatorColorHigh]; p.post_clip = v[kSaturatorPostClip];
      p.output = v[kSaturatorOutput]; p.dry_wet = v[kSaturatorMix]; p.bass_threshold = v[kSaturatorBassThreshold];
      p.ws_drive = v[kSaturatorWsDrive]; p.ws_linear = v[kSaturatorWsLinear]; p.ws_curve = v[kSaturatorWsCurve];
      p.ws_damp = v[kSaturatorWsDamp]; p.ws_period = v[kSaturatorWsPeriod]; p.ws_depth = v[kSaturatorWsDepth];
      p.dc_filter = v[kSaturatorDcFilter]; p.hi_quality = v[kSaturatorHiQuality];
      return p;
    }

    dd::DynamicTube::Parameters dynamicTubeParameters(const std::array<double, kNumDynamicTubeControls>& v) {
      dd::DynamicTube::Parameters p;
      p.drive = v[kDynamicTubeDrive]; p.tube_type = v[kDynamicTubeType]; p.bias = v[kDynamicTubeBias];
      p.envelope = v[kDynamicTubeEnvelope]; p.attack = v[kDynamicTubeAttack]; p.release = v[kDynamicTubeRelease];
      p.tone = v[kDynamicTubeTone]; p.output = v[kDynamicTubeOutput]; p.dry_wet = v[kDynamicTubeMix];
      p.hi_quality = v[kDynamicTubeHiQuality];
      return p;
    }

    dd::Overdrive::Parameters overdriveParameters(const std::array<double, kNumOverdriveControls>& v) {
      dd::Overdrive::Parameters p;
      p.freq = v[kOverdriveFrequency]; p.width = v[kOverdriveWidth]; p.drive = v[kOverdriveDrive];
      p.tone = v[kOverdriveTone]; p.dynamics = v[kOverdriveDynamics]; p.dry_wet = v[kOverdriveMix];
      return p;
    }

    dd::MultibandDynamics::Parameters multibandParameters(const std::array<double, kNumMultibandControls>& v) {
      dd::MultibandDynamics::Parameters p;
      p.split_low_mid = v[kMultibandLowSplit]; p.split_mid_high = v[kMultibandHighSplit];
      p.low_band = v[kMultibandLowBand]; p.high_band = v[kMultibandHighBand];
      for (int b = 0; b < kNumMultibandBands; ++b) {
        auto& band = p.bands[static_cast<size_t>(b)];
        const auto at = [&](int control) { return v[static_cast<size_t>(multibandControl(b, control))]; };
        band.active = at(kBandActive); band.solo = at(kBandSolo);
        band.above_threshold = at(kBandAboveThreshold); band.above_ratio = at(kBandAboveRatio);
        band.below_threshold = at(kBandBelowThreshold); band.below_ratio = at(kBandBelowRatio);
        band.attack = at(kBandAttack); band.release = at(kBandRelease);
        band.input = at(kBandInput); band.output = at(kBandOutput);
      }
      p.amount = v[kMultibandAmount]; p.time = v[kMultibandTime]; p.output = v[kMultibandOutput];
      p.peak = v[kMultibandDetection]; p.knee = v[kMultibandSoftKnee];
      return p;
    }

    std::array<DriveDynamicsModule<dd::MultibandDynamics, kNumMultibandControls>::Control, kNumMultibandControls>
    multibandControls() {
      std::array<DriveDynamicsModule<dd::MultibandDynamics, kNumMultibandControls>::Control, kNumMultibandControls> c;
      for (int i = 0; i < kNumMultibandControls; ++i) {
        const drive_dynamics::ControlSpec& spec = multibandSpec(i);
        c[static_cast<size_t>(i)] = { "multiband_dynamics_" + multibandSuffix(i), spec.minimum, spec.maximum,
                                      spec.modulated };
      }
      return c;
    }

    template<class Core, size_t N>
    std::array<typename DriveDynamicsModule<Core, N>::Control, N> controlsFor(const std::string& effect,
                                                                             const drive_dynamics::ControlSpec (&specs)[N]) {
      std::array<typename DriveDynamicsModule<Core, N>::Control, N> controls;
      for (size_t i = 0; i < N; ++i)
        controls[i] = { effect + specs[i].suffix, specs[i].minimum, specs[i].maximum, specs[i].modulated };
      return controls;
    }
  }

  template class DriveDynamicsModule<dd::Saturator, kNumSaturatorControls>;
  template class DriveDynamicsModule<dd::DynamicTube, kNumDynamicTubeControls>;
  template class DriveDynamicsModule<dd::Overdrive, kNumOverdriveControls>;
  template class DriveDynamicsModule<dd::MultibandDynamics, kNumMultibandControls>;

  SaturatorModule::SaturatorModule(std::string prefix) :
      DriveDynamicsModule(std::move(prefix), controlsFor<dd::Saturator>("saturator_", kSaturatorControls),
                          saturatorParameters) { }
  DynamicTubeModule::DynamicTubeModule(std::string prefix) :
      DriveDynamicsModule(std::move(prefix), controlsFor<dd::DynamicTube>("dynamic_tube_", kDynamicTubeControls),
                          dynamicTubeParameters) { }
  OverdriveModule::OverdriveModule(std::string prefix) :
      DriveDynamicsModule(std::move(prefix), controlsFor<dd::Overdrive>("overdrive_", kOverdriveControls),
                          overdriveParameters) { }
  MultibandDynamicsModule::MultibandDynamicsModule(std::string prefix) :
      DriveDynamicsModule(std::move(prefix), multibandControls(), multibandParameters) { }
}
