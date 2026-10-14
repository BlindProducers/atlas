// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "slot_modulation.h"
#include "synth_module.h"

#include <string>

namespace vital {
  class OpenWurliSource;

  // The OpenWurli oscillator type: a Wurlitzer 200A reed struck on every note.
  // `prefix` names its controls ("osc_3_wurli_"); level, pan and the RM
  // distortion pairs are the slot's shared controls.
  class OpenWurliModule : public SynthModule, public SlotModulationTarget {
    public:
      enum {
        kReset,
        kVoiceEvent,
        kMidi,
        kVelocity,
        kNumInputs
      };

      enum {
        kRaw,
        kLevelled,
        kHold,
        kNumOutputs
      };

      OpenWurliModule(std::string prefix, std::string slot_prefix);
      virtual ~OpenWurliModule() { }

      void init() override;
      void setModulatorOutput(int position, Output* output) override;
      Processor* clone() const override { return new OpenWurliModule(*this); }

    protected:
      std::string prefix_;
      std::string slot_prefix_;
      OpenWurliSource* source_;

      JUCE_LEAK_DETECTOR(OpenWurliModule)
  };
} // namespace vital
