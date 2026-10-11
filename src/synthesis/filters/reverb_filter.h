// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "processor.h"
#include "reverb_filter_core.h"
#include "reverb_filter_bass.h"
#include <array>

namespace vital {
class ReverbFilter : public Processor {
public:
  enum { kAudio, kReset, kCutoffLog, kResonance, kDamp, kKeytrack, kBassPreserve, kNumInputs };
  ReverbFilter();
  ReverbFilter(const ReverbFilter& other);
  Processor* clone() const override { return new ReverbFilter(*this); }
  void setSampleRate(int sample_rate) override;
  void process(int num_samples) override;
  void reset(poly_mask mask) override;
private:
  void prepare(int sample_rate);
  std::array<ReverbFilterCore, poly_float::kSize> lanes_;
  std::array<ReverbFilterBass, poly_float::kSize> bass_;
  int prepared_rate_ = kDefaultSampleRate;
};
}
