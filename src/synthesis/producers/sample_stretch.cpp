// SPDX-License-Identifier: GPL-3.0-or-later
#include "sample_stretch.h"

#if ATLAS_USE_PFFFT
#define SIGNALSMITH_USE_PFFFT
#endif
#include "signalsmith-stretch/signalsmith-stretch.h"

#include <algorithm>
#include <cmath>

namespace atlas {

namespace {
  // A 50 ms block analysed every 12.5 ms: short enough to keep drum
  // transients, long enough to resolve bass notes. The work is spread over
  // the interval's samples (split computation), so no host block carries a
  // whole frame. Latency is one block plus one interval.
  constexpr double kBlockSeconds = 0.05;
  constexpr int kIntervalsPerBlock = 4;
}

struct SampleStretcher::Impl {
  // A fixed seed: the same settings render the same samples every time.
  signalsmith::stretch::SignalsmithStretch<float> stretch { 1 };
  float transpose = 0.0f;
  bool formants = false;

  void apply() {
    stretch.setTransposeSemitones(transpose);
    // With the pitch moved, compensating the formants keeps them where the
    // file has them.
    stretch.setFormantFactor(1.0f, formants);
  }
};

SampleStretcher::SampleStretcher() : impl_(std::make_unique<Impl>()) { }
SampleStretcher::~SampleStretcher() = default;

void SampleStretcher::configure(int sample_rate) {
  const int block = std::max(64, static_cast<int>(std::lround(sample_rate * kBlockSeconds)));
  impl_->stretch.configure(kChannels, block, block / kIntervalsPerBlock, true);
  impl_->apply();
}

void SampleStretcher::setTranspose(float semitones) {
  if (semitones == impl_->transpose)
    return;
  impl_->transpose = semitones;
  impl_->apply();
}

void SampleStretcher::setPreserveFormants(bool preserve) {
  if (preserve == impl_->formants)
    return;
  impl_->formants = preserve;
  impl_->apply();
}

int SampleStretcher::startLength(float speed) const {
  return impl_->stretch.outputSeekLength(speed);
}

void SampleStretcher::start(const float* const* input, float speed) {
  impl_->stretch.outputSeek(input, startLength(speed));
}

void SampleStretcher::process(const float* const* input, int input_samples, float* const* output,
                              int output_samples) {
  impl_->stretch.process(input, input_samples, output, output_samples);
}

int SampleStretcher::latency() const {
  return impl_->stretch.inputLatency() + impl_->stretch.outputLatency();
}

} // namespace atlas
