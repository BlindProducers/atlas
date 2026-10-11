// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "braids_source.h"
#include "synth_module.h"

#include <string>

namespace vital {
  // The Braids oscillator type (Mutable Instruments' macro-oscillator).
  // `prefix` names its controls ("osc_3_braids_"); level, pan and the FM/RM
  // distortion pairs are the slot's shared controls.
  class BraidsModule : public SynthModule, public SlotModulationTarget {
    public:
      enum {
        kReset,
        kVoiceEvent,
        kMidi,
        kNumInputs
      };

      enum {
        kRaw,
        kLevelled,
        kNumOutputs
      };

      BraidsModule(std::string prefix, std::string slot_prefix);
      virtual ~BraidsModule() { }

      void init() override;
      void setModulatorOutput(int position, Output* output) override {
        source_->modulation().setModulatorOutput(position, output);
      }
      Processor* clone() const override { return new BraidsModule(*this); }

    protected:
      std::string prefix_;
      std::string slot_prefix_;
      BraidsSource* source_;

      JUCE_LEAK_DETECTOR(BraidsModule)
  };
} // namespace vital
