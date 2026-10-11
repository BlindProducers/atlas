#pragma once

#include "synth_module.h"
#include <array>
#include <memory>

namespace vital {
  // Atlas routing/controls around the supplied BP Scream DSP. The source DSP
  // is compiled separately, with its original floating-point settings.
  class ScreamModule : public SynthModule {
    public:
      explicit ScreamModule(std::string prefix = "", const Output* midi_note = nullptr);
      ~ScreamModule() override;
      void init() override;
      void processWithInput(const poly_float* input, int samples) override;
      void hardReset() override;
      void enable(bool on) override;
      void setSampleRate(int rate) override;
      void setOversampleAmount(int amount) override;
      Processor* clone() const override { VITAL_ASSERT(false); return nullptr; }

    private:
      void prepare();
      struct Engines;
      std::unique_ptr<Engines> engines_;
      std::string prefix_;
      const Output* midi_note_;
      std::array<Output*, 7> controls_ {};
      Value* autogain_ = nullptr;
      Value* keytracking_ = nullptr;
  };
}
