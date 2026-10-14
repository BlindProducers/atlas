// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "slot_modulation.h"
#include "synth_module.h"

#include <string>

namespace vital {
  class OpenB3Source;

  // The OpenB3 oscillator type: OpenB3's tonewheel organ, one key per voice.
  // `prefix` names its controls ("osc_3_b3_"); level, pan and the RM
  // distortion pairs are the slot's shared controls. The scanner vibrato and
  // the rotary speaker are effects (Scanner Vibrato, and Analogue Rotary's
  // OpenB3 Whirl model).
  class OpenB3Module : public SynthModule, public SlotModulationTarget {
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
        kHold,
        kNumOutputs
      };

      // The drawbar control suffixes, 16' to 1'.
      static const char* const kDrawbarSuffixes[];

      OpenB3Module(std::string prefix, std::string slot_prefix);
      virtual ~OpenB3Module() { }

      void init() override;
      void setModulatorOutput(int position, Output* output) override;
      Processor* clone() const override { return new OpenB3Module(*this); }

    protected:
      std::string prefix_;
      std::string slot_prefix_;
      OpenB3Source* source_;

      JUCE_LEAK_DETECTOR(OpenB3Module)
  };
} // namespace vital
