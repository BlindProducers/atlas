// SPDX-License-Identifier: GPL-3.0-or-later
#include "scanner_vibrato_module.h"

#include "openb3/scanner_vibrato.h"

#include <algorithm>
#include <cmath>

namespace vital {
  namespace openb3 = atlas::openb3;

  ScannerVibratoModule::ScannerVibratoModule(std::string prefix) :
      SynthModule(0, 1), scanner_(std::make_unique<openb3::ScannerVibrato>()), prefix_(std::move(prefix)) {
    scanner_->prepare(getSampleRate());
  }

  ScannerVibratoModule::~ScannerVibratoModule() = default;

  void ScannerVibratoModule::init() {
    const std::string name = prefix_ + "scanner_vibrato_";
    mode_ = createMonoModControl(name + "mode");
    rate_ = createMonoModControl(name + "rate");
    SynthModule::init();
  }

  void ScannerVibratoModule::processWithInput(const poly_float* input, int samples) {
    if (samples <= 0)
      return;
    SynthModule::process(samples);

    const double mode = mode_->buffer[0][0];
    const double rate = rate_->buffer[0][0];
    scanner_->setMode(std::isfinite(mode) ? static_cast<int>(std::lround(mode)) : openb3::kChorus3);
    scanner_->setFrequency(std::isfinite(rate) ? rate : openb3::ScannerVibrato::kDefaultHz);

    poly_float* output_buffer = output()->buffer;
    for (int offset = 0; offset < samples;) {
      const int count = std::min(kMaxBufferSize, samples - offset);
      float in_left[kMaxBufferSize], in_right[kMaxBufferSize];
      float out_left[kMaxBufferSize], out_right[kMaxBufferSize];
      for (int i = 0; i < count; ++i) {
        in_left[i] = input[offset + i][0];
        in_right[i] = input[offset + i][1];
      }
      const float* in[] = { in_left, in_right };
      float* out[] = { out_left, out_right };
      scanner_->process(in, out, 2, count);
      for (int i = 0; i < count; ++i)
        output_buffer[offset + i] = poly_float(out_left[i], out_right[i], out_left[i], out_right[i]);
      offset += count;
    }
  }

  void ScannerVibratoModule::hardReset() {
    scanner_->reset();
  }

  void ScannerVibratoModule::enable(bool on) {
    if (enabled() != on) hardReset();
    SynthModule::enable(on);
  }

  void ScannerVibratoModule::setSampleRate(int rate) {
    const int previous = getSampleRate();
    SynthModule::setSampleRate(rate);
    if (getSampleRate() != previous)
      scanner_->prepare(getSampleRate());
  }

  void ScannerVibratoModule::setOversampleAmount(int amount) {
    const int previous = getSampleRate();
    SynthModule::setOversampleAmount(amount);
    if (getSampleRate() != previous)
      scanner_->prepare(getSampleRate());
  }
} // namespace vital
