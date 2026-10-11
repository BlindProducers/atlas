// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "synth_module.h"
#include <array>
#include <memory>

namespace atlas::analogue::rotary { class Engine; }

namespace vital {
  // Atlas controls around the Analogue Rotary engine (Organ 122 and Guitar 16
  // rotating speakers). Speed is a modulatable control, so the mod wheel or a
  // macro can flip Slow and Fast. Only the left and right lanes reach the
  // host, so those are processed and lanes 2-3 mirror them.
  class AnalogueRotaryModule : public SynthModule {
    public:
      explicit AnalogueRotaryModule(std::string prefix = "");
      ~AnalogueRotaryModule() override;
      void init() override;
      void processWithInput(const poly_float* input, int samples) override;
      void hardReset() override;
      void enable(bool on) override;
      void setSampleRate(int rate) override;
      void setOversampleAmount(int amount) override;
      Processor* clone() const override { VITAL_ASSERT(false); return nullptr; }

      static constexpr int kNumModControls = 6;

    private:
      std::unique_ptr<atlas::analogue::rotary::Engine> engine_;
      std::string prefix_;
      std::array<Output*, kNumModControls> controls_ {};
      Value* model_ = nullptr;
      bool reset_pending_ = true;
  };
}
