// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "synth_module.h"
#include "slot_modulation.h"

#include <memory>
#include <string>

namespace vital {
  class MultiSampleSlot;
  class MultiSampleSource;

  // The Multi-sample oscillator type: plays an SFZ or SF2 instrument held by
  // the slot. `prefix` names its controls ("osc_3_multisample_"), and level
  // and pan are the slot's shared controls.
  class MultiSampleModule : public SynthModule, public SlotModulationTarget {
    public:
      enum {
        kReset,
        kRetrigger,
        kVoiceEvent,
        kMidi,
        kNote,
        kMidiKey,
        kVelocity,
        kNumInputs
      };

      enum {
        kRaw,
        kLevelled,
        kHold,
        kNumOutputs
      };

      MultiSampleModule(std::string prefix, std::shared_ptr<MultiSampleSlot> slot, std::string slot_prefix);
      virtual ~MultiSampleModule() { }

      void init() override;
      // Slot FM and RM do not apply to this type.
      void setModulatorOutput(int, Output*) override { }
      Processor* clone() const override { return new MultiSampleModule(*this); }

    protected:
      std::string prefix_;
      std::string slot_prefix_;
      MultiSampleSource* source_;

      JUCE_LEAK_DETECTOR(MultiSampleModule)
  };
} // namespace vital
