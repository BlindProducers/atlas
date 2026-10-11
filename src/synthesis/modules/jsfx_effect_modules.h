#pragma once

#include "synth_module.h"
#include "jsfx_ports.h"

namespace vital {
  class ErosionModule : public SynthModule {
    public:
      explicit ErosionModule(std::string prefix = "") : SynthModule(0, 1), prefix_(prefix) {}
      void init() override;
      void processWithInput(const poly_float* input, int samples) override;
      void hardReset() override { core_.reset(); rate_adapter_.reset(); }
      void enable(bool on) override;
      void setSampleRate(int rate) override;
      void setOversampleAmount(int amount) override;
      Processor* clone() const override { VITAL_ASSERT(false); return nullptr; }
    private:
      std::string prefix_;
      jsfx::Erosion core_;
      jsfx::HostRate rate_adapter_;
      Value* mode_ = nullptr;
      std::array<Output*, 4> controls_ {};
  };

  class FloatyDelayModule : public SynthModule {
    public:
      FloatyDelayModule(const Output* beats_per_second, std::string prefix = "");
      void init() override;
      void processWithInput(const poly_float* input, int samples) override;
      void hardReset() override { core_.reset(); rate_adapter_.reset(); }
      void enable(bool on) override;
      void setSampleRate(int rate) override;
      void setOversampleAmount(int amount) override;
      Processor* clone() const override { VITAL_ASSERT(false); return nullptr; }
    private:
      std::string prefix_;
      const Output* beats_per_second_;
      jsfx::Floaty core_;
      jsfx::HostRate rate_adapter_;
      Value* sync_ = nullptr;
      std::array<Output*, 10> controls_ {};
  };
}
