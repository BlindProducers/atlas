/* Copyright 2013-2019 Matt Tytel
 *
 * vital is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * vital is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with vital.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <atomic>

#include "synth_module.h"
#include "synth_constants.h"
#include "line_generator.h"

namespace vital {
  class ModulationConnectionProcessor : public SynthModule {
    public:
      enum {
        kModulationInput,
        kIndependentRandomInput,
        kModulationAmount,
        kModulationPower,
        kReset,
        kNumInputs
      };

      enum {
        kModulationOutput,
        kModulationPreScale,
        kModulationSource,
        kNumOutputs
      };

      ModulationConnectionProcessor(int index);
      virtual ~ModulationConnectionProcessor() { }

      void init() override;
      void process(int num_samples) override;
      void processAudioRate(int num_samples, const Output* source);
      void processAudioRateLinear(int num_samples, const Output* source);
      void processAudioRateRemapped(int num_samples, const Output* source);
      void processAudioRateMorphed(int num_samples, const Output* source, poly_float power);
      void processAudioRateRemappedAndMorphed(int num_samples, const Output* source, poly_float power);
      void processSmoothedControlSourceAudioRate(int num_samples, const Output* source);
      void processControlRate(int num_samples, const Output* source, bool smooth_source);

      virtual Processor* clone() const override { return new ModulationConnectionProcessor(*this); }

      void initializeBaseValue(Value* base_value) { current_value_ = base_value; }
      void initializeMapping() { map_generator_->initLinear(); }

      mono_float currentBaseValue() const { return current_value_->value(); }
      void setBaseValue(mono_float value) { current_value_->set(value); }

      bool isPolyphonicModulation() const { return polyphonic_; }
      void setPolyphonicModulation(bool polyphonic) { polyphonic_ = polyphonic; }
      bool collapsesPolyphonicSource() const { return collapse_polyphonic_source_; }
      void setCollapsePolyphonicSource(bool collapse) {
        collapse_polyphonic_source_ = collapse;
        collapsed_output_value_ = 0.0f;
        collapsed_handoff_correction_ = 0.0f;
        collapsed_handoff_samples_left_ = 0;
        collapsed_output_initialized_ = false;
        collapsed_voice_identity_ = nullptr;
      }
      void collapseToVoice(poly_mask voice_mask, int num_samples, int handoff_sample = -1,
                           const void* voice_identity = nullptr);
      void holdCollapsedOutput(int num_samples);
      bool processWhenIdle() const { return process_when_idle_; }
      void setProcessWhenIdle(bool process_when_idle) { process_when_idle_ = process_when_idle; }
      bool refreshSourceWhenIdle() const { return refresh_source_when_idle_; }
      void setRefreshSourceWhenIdle(bool refresh_source) { refresh_source_when_idle_ = refresh_source; }
      void setSourceSmoothing(bool smooth) {
        source_smoothing_->enabled.store(smooth, std::memory_order_relaxed);
        source_smoothing_->revision.fetch_add(1, std::memory_order_release);
      }
      bool sourceSmoothing() const { return source_smoothing_->enabled.load(std::memory_order_relaxed); }
      bool isBipolar() const { return bipolar_->value() != 0.0f; }
      void setBipolar(bool bipolar) { bipolar_->set(bipolar ? 1.0f : 0.0f); }
      bool isStereo() const { return stereo_->value() != 0.0f; }
      void setStereo(bool stereo) { stereo_->set(stereo ? 1.0f : 0.0f); }
      bool isBypassed() const { return bypass_->value() != 0.0f; }
      bool usesIndependentRandom() const { return random_mode_->value() >= 0.5f; }
      force_inline void setDestinationScale(mono_float scale) { *destination_scale_ = scale; }
      force_inline int index() const { return index_; }

      LineGenerator* lineMapGenerator() { return map_generator_.get(); }

    protected:
      int index_;
      bool polyphonic_;
      bool collapse_polyphonic_source_;
      bool process_when_idle_;
      bool refresh_source_when_idle_;
      // Routing configuration belongs to the connection, while the smoothing
      // history belongs to each voice. Existing voice clones must observe a
      // new route (including reuse of the same slot) without being recreated.
      struct SourceSmoothingState {
        std::atomic<bool> enabled { false };
        std::atomic<unsigned> revision { 0 };
      };
      std::shared_ptr<SourceSmoothingState> source_smoothing_;
      unsigned source_smoothing_revision_;
      bool source_smoothing_initialized_;
      Value* current_value_;
      Value* bipolar_;
      Value* stereo_;
      Value* bypass_;
      Value* random_mode_;

      poly_float power_;
      poly_float modulation_amount_;
      poly_float smoothed_source_value_;
      poly_float collapsed_output_value_;
      poly_float collapsed_handoff_correction_;
      int collapsed_handoff_samples_left_;
      bool collapsed_output_initialized_;
      const void* collapsed_voice_identity_ = nullptr;

      std::shared_ptr<mono_float> destination_scale_;
      mono_float last_destination_scale_;
      std::shared_ptr<LineGenerator> map_generator_;

      JUCE_LEAK_DETECTOR(ModulationConnectionProcessor)
  };
} // namespace vital
