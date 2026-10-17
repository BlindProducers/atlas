// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "slot_modulation.h"
#include "synth_module.h"

#include <memory>
#include <string>

namespace vital {
  class SpeechSlot;
  class SpeechSource;

  // The Speech oscillator type: OpenEVV's text-to-speech, a piece of the text
  // per key. `prefix` names its controls ("osc_3_speech_"); level, pan and the
  // RM distortion pairs are the slot's shared controls. The voice controls
  // (language, gender, head size ...) are read by SynthBase's SpeechManager,
  // which renders the text when they change.
  class SpeechModule : public SynthModule, public SlotModulationTarget {
    public:
      enum {
        kReset,
        kVoiceEvent,
        kMidi,
        kRetrigger,
        kNumInputs
      };

      enum {
        kRaw,
        kLevelled,
        kHold,
        kNumOutputs
      };

      // The controls SpeechManager reads to render the text, by suffix.
      static const char* const kRenderSuffixes[];
      static const int kNumRenderSuffixes;

      SpeechModule(std::string prefix, std::shared_ptr<SpeechSlot> slot, std::string slot_prefix);
      virtual ~SpeechModule() { }

      void init() override;
      void setModulatorOutput(int position, Output* output) override;
      Processor* clone() const override { return new SpeechModule(*this); }

    protected:
      std::string prefix_;
      std::string slot_prefix_;
      std::shared_ptr<SpeechSlot> slot_;
      SpeechSource* source_;

      JUCE_LEAK_DETECTOR(SpeechModule)
  };
} // namespace vital
