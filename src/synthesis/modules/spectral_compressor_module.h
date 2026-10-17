// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "synth_module.h"
#include <array>
#include <memory>

namespace vital {
  // Atlas controls around the Spectral Compressor engine. Only the left and right
  // lanes reach the host, so those are processed and lanes 2-3 mirror them.
  class SpectralCompressorModule : public SynthModule {
    public:
      explicit SpectralCompressorModule(std::string prefix = "");
      ~SpectralCompressorModule() override;
      void init() override;
      void processWithInput(const poly_float* input, int samples) override;
      void hardReset() override;
      void enable(bool on) override;
      void setSampleRate(int rate) override;
      void setOversampleAmount(int amount) override;
      Processor* clone() const override { VITAL_ASSERT(false); return nullptr; }
      static constexpr int kLatencySamples = 1024;
      int getLatencySamples() const override { return kLatencySamples; }

    private:
      void prepare();
      struct Engines;
      std::unique_ptr<Engines> engines_;
      std::string prefix_;
      Value* curve_ = nullptr;
      std::array<Output*, 9> controls_ {};
  };
}
