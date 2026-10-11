// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "synth_module.h"
#include <array>
#include <memory>

namespace atlas::analogue::echo { class Engine; }

namespace vital {
  // Atlas controls and tempo sync around the Analogue Echo engine (tape loop
  // or BBD). Only the left and right lanes reach the host, so those are
  // processed and lanes 2-3 mirror them.
  class AnalogueEchoModule : public SynthModule {
    public:
      AnalogueEchoModule(const Output* beats_per_second, std::string prefix = "");
      ~AnalogueEchoModule() override;
      void init() override;
      void processWithInput(const poly_float* input, int samples) override;
      void hardReset() override;
      void enable(bool on) override;
      void setSampleRate(int rate) override;
      void setOversampleAmount(int amount) override;
      Processor* clone() const override { VITAL_ASSERT(false); return nullptr; }

      static constexpr int kNumModControls = 14;

    private:
      std::unique_ptr<atlas::analogue::echo::Engine> engine_;
      const Output* beats_per_second_;
      std::string prefix_;
      std::array<Output*, kNumModControls> controls_ {};
      std::array<Value*, 3> heads_ {};
      Output* frequency_ = nullptr;
      Value* medium_ = nullptr;
      Value* sync_ = nullptr;
      bool reset_pending_ = true;
  };
}
