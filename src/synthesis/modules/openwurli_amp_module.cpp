// SPDX-License-Identifier: GPL-3.0-or-later
#include "openwurli_amp_module.h"

#include "wurli_amp.h"

#include <algorithm>
#include <cmath>

namespace vital {
  OpenWurliAmpModule::OpenWurliAmpModule(std::string prefix) :
      SynthModule(0, 1), chain_(std::make_unique<openwurli::AmpChain>()), prefix_(std::move(prefix)) { }

  OpenWurliAmpModule::~OpenWurliAmpModule() = default;

  void OpenWurliAmpModule::init() {
    const std::string name = prefix_ + "openwurli_amp_";
    volume_ = createMonoModControl(name + "volume");
    tremolo_depth_ = createMonoModControl(name + "tremolo_depth");
    speaker_ = createMonoModControl(name + "speaker");
    SynthModule::init();
  }

  void OpenWurliAmpModule::setChainControls() {
    const auto control = [](const Output* output, double default_value) {
      const double value = output->buffer[0][0];
      return std::isfinite(value) ? std::clamp(value, 0.0, 1.0) : default_value;
    };
    chain_->setVolume(control(volume_, openwurli::AmpChain::kDefaultVolume));
    chain_->setTremoloDepth(control(tremolo_depth_, openwurli::AmpChain::kDefaultTremoloDepth));
    chain_->setSpeakerCharacter(control(speaker_, openwurli::AmpChain::kDefaultSpeakerCharacter));
  }

  void OpenWurliAmpModule::prepareChain() {
    // The settings go in first: the warm-up settles the tremolo at its depth.
    if (volume_ != nullptr)
      setChainControls();
    chain_->prepare(getSampleRate());
  }

  void OpenWurliAmpModule::processWithInput(const poly_float* input, int samples) {
    if (samples <= 0)
      return;
    SynthModule::process(samples);
    // Normally prepared on the preparation worker, from setSampleRate.
    if (chain_->sampleRate() != getSampleRate())
      prepareChain();

    setChainControls();

    poly_float* output_buffer = output()->buffer;
    constexpr double kInputScale = 0.5 / kInputPerVolt;
    for (int offset = 0; offset < samples;) {
      const int count = std::min(kMaxBufferSize, samples - offset);
      double volts[kMaxBufferSize];
      double out[kMaxBufferSize];
      // The chain reads a non-finite sample as silence.
      for (int i = 0; i < count; ++i) {
        const double sample = (static_cast<double>(input[offset + i][0]) + input[offset + i][1]) * kInputScale;
        volts[i] = std::clamp(sample, -kMaxInputVolts, kMaxInputVolts);
      }
      chain_->process(volts, out, count);
      for (int i = 0; i < count; ++i)
        output_buffer[offset + i] = poly_float(static_cast<float>(out[i] * kOutputGain));
      offset += count;
    }
  }

  void OpenWurliAmpModule::hardReset() {
    // Back to the settled state the warm-up left: no half-second settle here.
    if (chain_->sampleRate() == getSampleRate())
      chain_->reset();
  }

  void OpenWurliAmpModule::enable(bool on) {
    if (enabled() != on)
      hardReset();
    SynthModule::enable(on);
  }

  void OpenWurliAmpModule::setSampleRate(int rate) {
    SynthModule::setSampleRate(rate);
    if (chain_->sampleRate() != getSampleRate())
      prepareChain();
  }

  void OpenWurliAmpModule::setOversampleAmount(int amount) {
    SynthModule::setOversampleAmount(amount);
    if (chain_->sampleRate() != getSampleRate())
      prepareChain();
  }
} // namespace vital
