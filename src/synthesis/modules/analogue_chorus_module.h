// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "synth_module.h"
#include <array>
#include <memory>

namespace atlas::analogue::chorus { class Engine; }

namespace vital {
  // Atlas controls around the Analogue Chorus engine (Juno, Dimension, CE and
  // Ensemble BBD choruses). Only the left and right lanes reach the host, so
  // those are processed and lanes 2-3 mirror them.
  class AnalogueChorusModule : public SynthModule {
    public:
      explicit AnalogueChorusModule(std::string prefix = "");
      ~AnalogueChorusModule() override;
      void init() override;
      void processWithInput(const poly_float* input, int samples) override;
      void hardReset() override;
      void enable(bool on) override;
      void setSampleRate(int rate) override;
      void setOversampleAmount(int amount) override;
      Processor* clone() const override { VITAL_ASSERT(false); return nullptr; }

      static constexpr int kNumModControls = 5;

    private:
      std::unique_ptr<atlas::analogue::chorus::Engine> engine_;
      std::string prefix_;
      std::array<Output*, kNumModControls> controls_ {};
      Value* model_ = nullptr;
      Value* juno_mode_ = nullptr;
      Value* dimension_mode_ = nullptr;
      bool reset_pending_ = true;
  };
}
