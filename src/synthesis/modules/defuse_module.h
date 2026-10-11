// SPDX-License-Identifier: GPL-3.0-or-later
// Diopser DSP copyright (C) 2021-2024 Robbert van der Helm.
// C++/Atlas adaptation: see DEFUSE.md for source and fidelity checks.
#pragma once

#include "synth_module.h"
#include <array>

namespace vital {
  class DefuseModule : public SynthModule {
    public:
      explicit DefuseModule(std::string prefix = "");
      static constexpr float kMinimumFrequency = 20.0f;
      void init() override;
      void processWithInput(const poly_float* input, int samples) override;
      void hardReset() override;
      void enable(bool on) override;
      void setSampleRate(int rate) override;
      void setOversampleAmount(int amount) override;
      Processor* clone() const override { VITAL_ASSERT(false); return nullptr; }

    private:
      struct Smoother {
        float current = 0, target = 0, step = 0;
        int remaining = 0;
        void reset(float value);
        void setTarget(float value, float rate, bool logarithmic);
        float next(int steps, bool logarithmic);
      };
      struct Biquad {
        poly_float b0 { 1 }, b1 { 0 }, b2 { 0 }, a1 { 0 }, a2 { 0 };
        poly_float s1 { 0 }, s2 { 0 };
        void coefficients(float rate, float frequency, float q);
        poly_float process(poly_float input);
      };
      void updateFilters(int interval, bool reset);
      std::string prefix_;
      Output* frequency_ = nullptr;
      Output* resonance_ = nullptr;
      Output* spread_ = nullptr;
      Output* precision_ = nullptr;
      Value* stages_ = nullptr;
      Value* style_ = nullptr;
      std::array<Biquad, 512> filters_;
      Smoother frequency_smoother_, resonance_smoother_, spread_smoother_;
      int stage_count_ = 0, spread_style_ = 0, next_filter_smoothing_in_ = 1;
      bool reset_pending_ = true;
  };
}
