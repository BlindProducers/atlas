// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "slot_modulation.h"
#include "synth_module.h"
#include "flute_instrument.h"

#include <memory>
#include <string>

namespace vital {
  class FluteSource;

  // Renders the slot's flute once per block in the voice handler's mono
  // router, ahead of the voices, from the slot's controls: the voice handler
  // has given the instrument the block's keys, controllers and sample rate by
  // then (SynthVoiceHandler::prepareFlutes).
  class FluteRenderer : public Processor {
    public:
      enum {
        kAir,
        kBrightness,
        kAttack,
        kVibratoDepth,
        kVibratoRate,
        kVibratoDelay,
        kHumanize,
        kDynamicsSource,
        kDynamicsController,
        kDynamicRange,
        kRelease,
        kTuning,
        kOverlap,
        kTranspose,
        kNumInputs
      };

      explicit FluteRenderer(std::shared_ptr<atlas::flute::Instrument> instrument) :
          Processor(kNumInputs, 1), instrument_(std::move(instrument)) { }

      void process(int num_samples) override;
      Processor* clone() const override { return new FluteRenderer(*this); }

    private:
      std::shared_ptr<atlas::flute::Instrument> instrument_;
  };

  // The Flute oscillator type: Modeled Flute's concert flute, one per slot,
  // monophonic as the instrument is. `prefix` names its controls
  // ("osc_3_flute_"); level, pan and the RM distortion pairs are the slot's
  // shared controls. The flute itself (atlas::flute::Instrument) is shared by
  // every voice of the slot and rendered once per block by the renderer; the
  // voices' lanes copy its audio out (FluteSource).
  class FluteModule : public SynthModule, public SlotModulationTarget {
    public:
      enum {
        kReset,
        kVoiceEvent,
        kNoteCount,
        kActiveVoices,
        kNumInputs
      };

      enum {
        kRaw,
        kLevelled,
        kHold,
        kNumOutputs
      };

      FluteModule(std::string prefix, std::string slot_prefix);
      virtual ~FluteModule() { }

      void init() override;
      void setModulatorOutput(int position, Output* output) override;
      Processor* clone() const override { return new FluteModule(*this); }

      const std::shared_ptr<atlas::flute::Instrument>& instrument() const { return instrument_; }

    protected:
      std::string prefix_;
      std::string slot_prefix_;
      std::shared_ptr<atlas::flute::Instrument> instrument_;
      FluteSource* source_;

      JUCE_LEAK_DETECTOR(FluteModule)
  };
} // namespace vital
