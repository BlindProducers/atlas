// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "synth_module.h"
#include <array>
#include <memory>

namespace atlas::analogue::flanger { class Engine; }

namespace vital {
  // Atlas controls and tempo sync around the Analogue Flanger engine (BBD or
  // through-zero tape). Only the left and right lanes reach the host, so those
  // are processed and lanes 2-3 mirror them.
  class AnalogueFlangerModule : public SynthModule {
    public:
      AnalogueFlangerModule(const Output* beats_per_second, std::string prefix = "");
      ~AnalogueFlangerModule() override;
      void init() override;
      void processWithInput(const poly_float* input, int samples) override;
      void correctToTime(double seconds) override;
      void hardReset() override;
      void enable(bool on) override;
      void setSampleRate(int rate) override;
      void setOversampleAmount(int amount) override;
      Processor* clone() const override { VITAL_ASSERT(false); return nullptr; }

      static constexpr int kNumModControls = 7;

    private:
      std::unique_ptr<atlas::analogue::flanger::Engine> engine_;
      const Output* beats_per_second_;
      std::string prefix_;
      std::array<Output*, kNumModControls> controls_ {};
      Output* frequency_ = nullptr;
      Value* medium_ = nullptr;
      Value* sync_ = nullptr;
      bool reset_pending_ = true;
      bool has_transport_time_ = false;
      bool timeline_valid_ = false;
      int previous_sync_ = 0;
      double transport_time_ = 0.0;
      double expected_transport_time_ = 0.0;
  };
}
