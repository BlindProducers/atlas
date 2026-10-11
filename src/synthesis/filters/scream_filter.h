// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "processor.h"
#include <memory>

namespace vital {
class ScreamFilter : public Processor {
public:
  enum { kAudio, kReset, kMidiCutoff, kResonance, kScream, kInputGain, kMix,
         kOutputGain, kAutogain, kNumInputs };
  ScreamFilter();
  ScreamFilter(const ScreamFilter& other);
  ~ScreamFilter() override;
  Processor* clone() const override { return new ScreamFilter(*this); }
  void process(int num_samples) override;
  void reset(poly_mask mask) override;
  void setFxMode(bool fx) { fx_mode_ = fx; }
private:
  struct State;
  std::unique_ptr<State> state_;
  int prepared_rate_ = 0;
  bool fx_mode_ = false;
};
}
