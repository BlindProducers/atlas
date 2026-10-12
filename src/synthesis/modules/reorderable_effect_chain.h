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

#include "synth_module.h"

#include "synth_constants.h"

#include <memory>
#include <array>
#include <utility>
#include <vector>

namespace vital {
  class Wavetable;
  class PreparedEffectModule;

  class TranceGateClock;
  class NoteMonitor;
  class StereoMemory;
  class ConvolutionReverbModule;
  class ConvolutionIrWorker;
  class EffectPreparationWorker;

  class ReorderableEffectChain : public SynthModule {
    public:
      enum {
        kAudio,
        kOrder,
        kNumInputs
      };

      ReorderableEffectChain(const Output* beats_per_second, const Output* keytrack,
                             std::string prefix = "",
                             std::shared_ptr<ConvolutionIrWorker> convolution_worker = nullptr,
                             const Output* scream_midi_note = nullptr,
                             const TranceGateClock* trance_gate_clock = nullptr,
                             std::shared_ptr<EffectPreparationWorker> preparation_worker = nullptr,
                             const NoteMonitor* note_monitor = nullptr);

      virtual void process(int num_samples) override;
      void init() override;
      virtual void hardReset() override;
      void enable(bool on) override;
      virtual void processWithInput(const poly_float* audio_in, int num_samples) override;
      virtual Processor* clone() const override { return new ReorderableEffectChain(*this); }

      virtual void correctToTime(double seconds) override;
      int getLatencySamples() const override;
      // Largest supported spectral FFT, in each of the twenty serial slots.
      static constexpr int kMaxLatencySamples = constants::kMaxEffectSlots * 8192;

      SynthModule* getEffect(constants::Effect effect) { return effects_[effect]; }
      // Filter Table: the wavetable the Filter FX in `slot` reads (-1: the
      // fixed-order Filter FX), and its buffer allocation (message thread, before
      // the model is first used).
      void setFilterTable(int slot, Wavetable* table);
      void prepareFilterTable(int slot);
      PreparedEffectModule* filterFx(int slot);
      const StereoMemory* getEqualizerMemory() { return equalizer_memory_; }

      void collectConvolutionModules(std::vector<std::pair<std::string, ConvolutionReverbModule*>>& modules);

      ~ReorderableEffectChain() override;
      void stopPreparation();
      void prepareSelectedEffects();
      // Reserves the parameter controls of every slot's selected effect, enabled
      // or not. Allocates: processing stopped or the callback lock held.
      void reserveSelectedControls();
      void setSampleRate(int rate) override;
      void setOversampleAmount(int amount) override;

    protected:
      void configureEffect(int slot, int index);
      SynthModule* createEffectDsp(int index, const std::string& module_prefix);
      SynthModule* createEffectModule(int index, const std::string& module_prefix);

      const StereoMemory* equalizer_memory_;
      const Output* beats_per_second_;
      const Output* keytrack_;
      const Output* scream_midi_note_;
      const TranceGateClock* trance_gate_clock_;
      const NoteMonitor* note_monitor_;
      std::string prefix_;
      std::shared_ptr<ConvolutionIrWorker> convolution_worker_;
      std::shared_ptr<EffectPreparationWorker> preparation_worker_;
      SynthModule* effects_[constants::kNumEffects];
      Value* effects_on_[constants::kNumEffects];
      Value* effect_order_slots_[constants::kNumEffects];
      SynthModule* slot_effects_[constants::kMaxEffectSlots][constants::kNumEffects];
      Value* slot_types_[constants::kMaxEffectSlots];
      Value* slot_enabled_[constants::kMaxEffectSlots];
      std::array<std::shared_ptr<Output>, constants::kMaxEffectSlots> slot_outputs_;
      std::shared_ptr<Output> unused_output_;
      // -2 requires a full reconciliation (initial use or recursive enable).
      std::array<int, constants::kMaxEffectSlots> selected_effects_;
      std::array<std::array<int, constants::kNumEffects>, constants::kMaxEffectSlots> slot_rates_ {}, slot_oversamples_ {};
      bool buffers_prepared_ = false;
      Value* post_effect_order_;
      int effect_order_[constants::kNumEffects];
      float last_order_;

      JUCE_LEAK_DETECTOR(ReorderableEffectChain)
  };
} // namespace vital
