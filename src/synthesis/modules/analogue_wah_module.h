// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "synth_module.h"
#include <array>
#include <memory>

namespace atlas::analogue::wah { class Engine; }

namespace vital {
  // Atlas controls around the Analogue Wah engine (Cry Baby and V846 pedals,
  // Mu-Tron III envelope filter). Only the left and right lanes reach the
  // host, so those are processed and lanes 2-3 mirror them.
  class AnalogueWahModule : public SynthModule {
    public:
      explicit AnalogueWahModule(std::string prefix = "");
      ~AnalogueWahModule() override;
      void init() override;
      void processWithInput(const poly_float* input, int samples) override;
      void hardReset() override;
      void enable(bool on) override;
      void setSampleRate(int rate) override;
      void setOversampleAmount(int amount) override;
      Processor* clone() const override { VITAL_ASSERT(false); return nullptr; }

      static constexpr int kNumModControls = 5;

    private:
      std::unique_ptr<atlas::analogue::wah::Engine> engine_;
      std::string prefix_;
      std::array<Output*, kNumModControls> controls_ {};
      Value* model_ = nullptr;
      Value* mode_ = nullptr;
      Value* range_ = nullptr;
      bool reset_pending_ = true;
  };
}
