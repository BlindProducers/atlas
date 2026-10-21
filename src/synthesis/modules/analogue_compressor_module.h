// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "synth_module.h"
#include <array>
#include <memory>
#include <string>

namespace atlas::zl { class Engine; }

namespace vital {
  // Atlas controls around the Analogue Compressor, ZL Compressor's engine
  // (src/synthesis/effects/zl_compressor). Mode picks ZL's Clean, Classic,
  // Optical or Vocal style; the rest are ZL's basic controls. Only the left
  // and right lanes reach the host, so those are processed and lanes 2-3
  // mirror them.
  class AnalogueCompressorModule : public SynthModule {
    public:
      explicit AnalogueCompressorModule(std::string prefix = "");
      ~AnalogueCompressorModule() override;
      void init() override;
      void processWithInput(const poly_float* input, int samples) override;
      void hardReset() override;
      void enable(bool on) override;
      void setSampleRate(int rate) override;
      void setOversampleAmount(int amount) override;
      Processor* clone() const override { VITAL_ASSERT(false); return nullptr; }

      static constexpr int kNumModControls = 7;

    private:
      std::unique_ptr<atlas::zl::Engine> engine_;
      std::string prefix_;
      std::array<Output*, kNumModControls> controls_ {};
      Value* mode_ = nullptr;
      bool reset_pending_ = true;
  };
}
