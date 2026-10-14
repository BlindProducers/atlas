// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "synth_module.h"

#include <memory>
#include <string>

namespace openwurli { class AmpChain; }

namespace vital {
  // The OpenWurli Amp effect: the Wurlitzer 200A's shared amplifier from
  // OpenWurli (preamp with the tremolo inside its feedback loop, volume pot,
  // power amp and speaker), for the reeds of the OpenWurli oscillator type.
  // The instrument is mono: the effect plays the middle of its input through
  // one amplifier and sends the result to both channels.
  class OpenWurliAmpModule : public SynthModule {
    public:
      // The input level that is the pickup's real voltage: an OpenWurli
      // oscillator at its default level (70.7%, so 0.5 after the level curve)
      // and centred.
      static constexpr double kInputPerVolt = 50.0;
      // OpenWurli presents the amplifier's rail 14 dB up (OUTPUT_ALIGNMENT_DB),
      // where playing at the defaults peaks around -6 to -10 dBFS.
      static constexpr double kOutputGain = 5.0118723362727229;
      // Far above any reed (a fortissimo chord is tens of millivolts), and
      // below where an absurd input leaves the preamp's solver latched.
      static constexpr double kMaxInputVolts = 10.0;

      explicit OpenWurliAmpModule(std::string prefix = "");
      ~OpenWurliAmpModule() override;

      void init() override;
      void processWithInput(const poly_float* input, int samples) override;
      void hardReset() override;
      void enable(bool on) override;
      void setSampleRate(int rate) override;
      void setOversampleAmount(int amount) override;
      Processor* clone() const override { VITAL_ASSERT(false); return nullptr; }

    private:
      void prepareChain();
      // The controls' current values, NaN read as the default.
      void setChainControls();

      std::unique_ptr<openwurli::AmpChain> chain_;
      std::string prefix_;
      Output* volume_ = nullptr;
      Output* tremolo_depth_ = nullptr;
      Output* speaker_ = nullptr;
  };
} // namespace vital
