// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "synth_module.h"
#include <array>
#include <memory>

namespace atlas::spectral_balance { class Engine; }

namespace vital {
  // Atlas controls around the Spectral Balance engine. Only the left and right
  // lanes reach the host, so those are processed and lanes 2-3 mirror them.
  class SpectralBalanceModule : public SynthModule {
    public:
      explicit SpectralBalanceModule(std::string prefix = "");
      ~SpectralBalanceModule() override;
      void init() override;
      void processWithInput(const poly_float* input, int samples) override;
      void hardReset() override;
      void enable(bool on) override;
      void setSampleRate(int rate) override;
      void setOversampleAmount(int amount) override;
      Processor* clone() const override { VITAL_ASSERT(false); return nullptr; }
      static constexpr int kLatencySamples = 2048;
      int getLatencySamples() const override { return kLatencySamples; }

      static constexpr int kNumModControls = 7;
      const atlas::spectral_balance::Engine& engine() const { return *engine_; }

    private:
      std::unique_ptr<atlas::spectral_balance::Engine> engine_;
      std::string prefix_;
      std::array<Output*, kNumModControls> controls_ {};
      Value* target_ = nullptr;
  };
}
