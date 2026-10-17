// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "slot_modulation.h"
#include "synth_module.h"

#include <memory>
#include <string>

namespace vital {
  class SpeechSlot;
  class SpeechSource;

  // The Speech oscillator type: sings the slot's text one syllable per note
  // with espeak-ng's voice. `prefix` names its controls ("osc_3_speech_");
  // level and pan are the slot's shared controls. The language and variant
  // only choose how the text is rendered, off the audio thread; the amp
  // envelope mode chooses between the syllable's own shape and Env 1.
  class SpeechModule : public SynthModule, public SlotModulationTarget {
    public:
      enum {
        kReset,
        // Note-ons except mono legato transitions, and note-offs.
        kRetrigger,
        kMidi,
        kNumInputs
      };

      enum {
        kRaw,
        kLevelled,
        kHold,
        kNumOutputs
      };

      SpeechModule(std::string prefix, std::shared_ptr<SpeechSlot> slot, std::string slot_prefix);
      virtual ~SpeechModule() { }

      void init() override;
      // Slot FM and RM do not apply to this type.
      void setModulatorOutput(int, Output*) override { }
      Processor* clone() const override { return new SpeechModule(*this); }

    protected:
      std::string prefix_;
      std::string slot_prefix_;
      SpeechSource* source_;

      JUCE_LEAK_DETECTOR(SpeechModule)
  };
} // namespace vital
