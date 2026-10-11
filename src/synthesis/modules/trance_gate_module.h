// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "synth_module.h"
#include "trance_gate_sequence.h"
#include "trance_gate_clock.h"

namespace vital {
  class TranceGateModule : public SynthModule {
    public:
      TranceGateModule(const Output* bps, const TranceGateClock* clock, std::string prefix = "");
      void init() override;
      void processWithInput(const poly_float* audio, int samples) override;
      void correctToTime(double seconds) override { transport_seconds_ = seconds; }
      void hardReset() override { gain_.fill(0.0); mix_initialized_ = false; }
      void enable(bool on) override;
      Processor* clone() const override { VITAL_ASSERT(false); return nullptr; }
    private:
      const Output* bps_;
      const TranceGateClock* clock_;
      std::string prefix_;
      Value *trigger_ = nullptr, *grid_ = nullptr, *start_ = nullptr, *stop_ = nullptr;
      Output *mix_ = nullptr, *attack_ = nullptr, *decay_ = nullptr, *sustain_ = nullptr;
      std::array<Value*, trance_gate::kSteps> values_ {}, modes_ {}, curves_ {};
      std::array<double, poly_float::kSize> gain_ {};
      std::array<double, poly_float::kSize> smoothed_mix_ {};
      bool mix_initialized_ = false;
      double transport_seconds_ = 0.0;
  };
}
