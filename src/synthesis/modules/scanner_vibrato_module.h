// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "synth_module.h"

#include <memory>
#include <string>

namespace atlas::openb3 { class ScannerVibrato; }

namespace vital {
  // The Scanner Vibrato effect: OpenB3's tonewheel organ vibrato and chorus
  // (setBfree's vibrato.c), V1-V3 and C1-C3. Each channel runs its own delay
  // line under the one scanner. Mode is a modulatable switch, as the organ's
  // vibrato knob is often turned while playing.
  class ScannerVibratoModule : public SynthModule {
    public:
      explicit ScannerVibratoModule(std::string prefix = "");
      ~ScannerVibratoModule() override;

      void init() override;
      void processWithInput(const poly_float* input, int samples) override;
      void hardReset() override;
      void enable(bool on) override;
      void setSampleRate(int rate) override;
      void setOversampleAmount(int amount) override;
      Processor* clone() const override { VITAL_ASSERT(false); return nullptr; }

    private:
      std::unique_ptr<atlas::openb3::ScannerVibrato> scanner_;
      std::string prefix_;
      Output* mode_ = nullptr;
      Output* rate_ = nullptr;
  };
} // namespace vital
