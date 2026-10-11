// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "filter_module.h"
#include "synth_module.h"

namespace vital {
  // The Filter FX effect: a mono filter module in an effect slot.
  class FilterFxModule : public SynthModule {
    public:
      enum {
        kAudio,
        kKeytrack,
        kNumInputs
      };

      FilterFxModule(const Output* keytrack, const std::string& prefix = "") : SynthModule(kNumInputs, 1) {
        filter_ = new FilterModule(prefix + "filter_fx");
        filter_->setCreateOnValue(false);
        filter_->setMono(true);
        addSubmodule(filter_);
        addProcessor(filter_);
        filter_->useOutput(output());
        filter_->plug(&input_, FilterModule::kAudio);
        filter_->plug(keytrack, FilterModule::kKeytrack);
      }

      void processWithInput(const poly_float* audio_in, int num_samples) override {
        utils::copyBuffer(input_.buffer, audio_in, num_samples);
        filter_->process(num_samples);
      }

      void setOversampleAmount(int oversampling) override {
        input_.ensureBufferSize(kMaxBufferSize * oversampling);
        SynthModule::setOversampleAmount(oversampling);
      }

      int getLatencySamples() const override { return filter_->getLatencySamples(); }
      FilterModule* getFilter() { return filter_; }
      // Filter Table: the wavetable this slot's filter reads (owned by the synth).
      void setTableWavetable(Wavetable* table) { filter_->setTableWavetable(table); }
      void prepareTable() { filter_->prepareTableFilter(); }

    private:
      FilterModule* filter_;
      Output input_;
  };
} // namespace vital
