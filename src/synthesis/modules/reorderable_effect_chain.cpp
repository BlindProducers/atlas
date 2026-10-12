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

#include "reorderable_effect_chain.h"
#include "prepared_effect_module.h"

#include "analogue_chorus_module.h"
#include "analogue_flanger_module.h"
#include "analogue_echo_module.h"
#include "analogue_rotary_module.h"
#include "analogue_spring_module.h"
#include "analogue_wah_module.h"
#include "filter_fx_module.h"
#include "spectral_balance_module.h"
#include "glitch_delay_module.h"
#include "chorus_module.h"
#include "compressor_module.h"
#include "convolution_reverb_module.h"
#include "delay_module.h"
#include "dimension_expander_module.h"
#include "distortion_module.h"
#include "equalizer_module.h"
#include "frequency_shifter_module.h"
#include "flanger_module.h"
#include "filter_module.h"
#include "limiter_module.h"
#include "mid_side_eq_module.h"
#include "phase_shift_module.h"
#include "phaser_module.h"
#include "reverb_module.h"
#include "spectral_gate_module.h"
#include "spectral_compressor_module.h"
#include "spectral_lock_module.h"
#include "resonator_module.h"
#include "scream_module.h"
#include "tremolo_pan_module.h"
#include "trance_gate_module.h"
#include "tape_module.h"
#include "defuse_module.h"
#include "suppressor_module.h"
#include "jsfx_effect_modules.h"
#include "synth_strings.h"
#include "utility_module.h"
#include "waveshaper_module.h"

#include <algorithm>

namespace vital {

  ReorderableEffectChain::ReorderableEffectChain(const Output* beats_per_second, const Output* keytrack,
                                                 std::string prefix,
                                                 std::shared_ptr<ConvolutionIrWorker> convolution_worker,
                                                 const Output* scream_midi_note,
                                                 const TranceGateClock* trance_gate_clock,
                                                 std::shared_ptr<EffectPreparationWorker> preparation_worker,
                                                 const NoteMonitor* note_monitor) :
      vital::SynthModule(kNumInputs, 1), equalizer_memory_(nullptr),
      beats_per_second_(beats_per_second), keytrack_(keytrack), scream_midi_note_(scream_midi_note),
      trance_gate_clock_(trance_gate_clock), note_monitor_(note_monitor), prefix_(prefix),
      convolution_worker_(convolution_worker ? std::move(convolution_worker) :
                                              std::make_shared<ConvolutionIrWorker>()),
      preparation_worker_(preparation_worker ? std::move(preparation_worker) : std::make_shared<EffectPreparationWorker>()),
      unused_output_(std::make_shared<Output>()), post_effect_order_(nullptr), last_order_(0.0f) {
    unused_output_->owner = this;
    selected_effects_.fill(-2);
    for (int i = 0; i < constants::kNumEffects; ++i) {
      SynthModule* effect_module = createEffectModule(i, prefix_);
      VITAL_ASSERT(effect_module);

      effect_module->shareDependencyScratch(*this);
      addSubmodule(effect_module);
      addProcessor(effect_module);
      effects_on_[i] = createBaseControl(prefix_ + strings::kEffectOrder[i] + "_on");
      effect_order_slots_[i] = createBaseControl(prefix_ + "effect_chain_slot_" + std::to_string(i + 1));
      effects_[i] = effect_module;
      effect_order_[i] = i;
    }

    for (int slot = 0; slot < constants::kMaxEffectSlots; ++slot) {
      const std::string slot_prefix = prefix_ + "fx_slot_" + std::to_string(slot + 1) + "_";
      slot_types_[slot] = createBaseControl(slot_prefix + "type");
      slot_enabled_[slot] = createBaseControl(slot_prefix + "enabled");
      slot_outputs_[slot] = std::make_shared<Output>();
      slot_outputs_[slot]->owner = this;

      for (int effect = 0; effect < constants::kNumEffects; ++effect) {
        SynthModule* effect_module = createEffectModule(effect, slot_prefix);
        VITAL_ASSERT(effect_module);

        // A slot processes only its selected effect, so all of its effects
        // write one audio buffer. Their other outputs are never read.
        effect_module->useOutput(slot_outputs_[slot].get(), 0);
        for (int output = 1; output < effect_module->numOutputs(); ++output)
          effect_module->useOutput(unused_output_.get(), output);
        effect_module->releaseReplacedOutputs();
        effect_module->shareDependencyScratch(*this);
        addSubmodule(effect_module);
        addProcessor(effect_module);
        slot_effects_[slot][effect] = effect_module;
      }
    }

    post_effect_order_ = createBaseControl(prefix_ + "post_effect_order");
    last_order_ = utils::encodeOrderToFloat(effect_order_, constants::kNumReorderableEffects);
  }

  SynthModule* ReorderableEffectChain::createEffectModule(int index, const std::string& module_prefix) {
    // Every slot can hold any effect, so building each one's DSP up front
    // cost hundreds of megabytes per instance. Each slot and effect keeps only
    // its parameters; the selected effect's DSP is built on the worker.
#if !VITAL_ACCESSIBLE_UI
    if (index == constants::kEq && module_prefix == prefix_)
      return createEffectDsp(index, module_prefix);
#endif
    return new PreparedEffectModule(index, module_prefix, [this, index, module_prefix] {
      return std::unique_ptr<SynthModule>(createEffectDsp(index, module_prefix));
    }, preparation_worker_, convolution_worker_);
  }

  SynthModule* ReorderableEffectChain::createEffectDsp(int index, const std::string& module_prefix) {
    switch(index) {
      case constants::kChorus:
        return new ChorusModule(beats_per_second_, module_prefix);
      case constants::kCompressor:
        return new CompressorModule(module_prefix);
      case constants::kDelay:
        return new DelayModule(beats_per_second_, module_prefix);
      case constants::kDistortion:
        return new DistortionModule(module_prefix);
      case constants::kEq: {
#if VITAL_ACCESSIBLE_UI
        EqualizerModule* eq = new EqualizerModule(module_prefix, false);
#else
        EqualizerModule* eq = new EqualizerModule(module_prefix);
#endif
        if (module_prefix == prefix_ && !VITAL_ACCESSIBLE_UI)
          equalizer_memory_ = eq->getAudioMemory();
        return eq;
      }
      case constants::kFlanger:
        return new FlangerModule(beats_per_second_, module_prefix);
      case constants::kFilterFx:
        return new FilterFxModule(keytrack_, module_prefix);
      case constants::kPhaser:
        return new PhaserModule(beats_per_second_, module_prefix);
      case constants::kReverb:
        return new ReverbModule(module_prefix);
      case constants::kUtility:
        return new UtilityModule(module_prefix);
      case constants::kPhaseShift:
        return new PhaseShiftModule(module_prefix);
      case constants::kLimiter:
        return new LimiterModule(module_prefix);
      case constants::kFrequencyShifter:
        return new FrequencyShifterModule(module_prefix);
      case constants::kDimensionExpander:
        return new DimensionExpanderModule(module_prefix);
      case constants::kWaveShaper:
        return new WaveShaperModule(module_prefix);
      case constants::kConvolutionReverb:
        return new ConvolutionReverbModule(module_prefix, convolution_worker_, false, beats_per_second_);
      case constants::kMidSideEq:
        return new MidSideEqModule(module_prefix);
      case constants::kSpectralGate:
        return new SpectralGateModule(module_prefix);
      case constants::kSuppressor:
        return new SuppressorModule(module_prefix);
      case constants::kDefuse:
        return new DefuseModule(module_prefix);
      case constants::kTape:
        return new TapeModule(module_prefix);
      case constants::kTranceGate:
        return new TranceGateModule(beats_per_second_, trance_gate_clock_, module_prefix);
      case constants::kTremoloPan:
        return new TremoloPanModule(beats_per_second_, module_prefix);
      case constants::kScream:
        return new ScreamModule(module_prefix, scream_midi_note_);
      case constants::kSpectralLock:
        return new SpectralLockModule(module_prefix);
      case constants::kResonator:
        return new ResonatorModule(beats_per_second_, note_monitor_, module_prefix);
      case constants::kSpectralCompressor:
        return new SpectralCompressorModule(module_prefix);
      case constants::kErosion:
        return new ErosionModule(module_prefix);
      case constants::kFloatyDelay:
        return new FloatyDelayModule(beats_per_second_, module_prefix);
      case constants::kAnalogueChorus:
        return new AnalogueChorusModule(module_prefix);
      case constants::kAnalogueFlanger:
        return new AnalogueFlangerModule(beats_per_second_, module_prefix);
      case constants::kAnalogueEcho:
        return new AnalogueEchoModule(beats_per_second_, module_prefix);
      case constants::kAnalogueRotary:
        return new AnalogueRotaryModule(module_prefix);
      case constants::kAnalogueSpring:
        return new AnalogueSpringModule(module_prefix);
      case constants::kAnalogueWah:
        return new AnalogueWahModule(module_prefix);
      case constants::kSpectralBalance:
        return new SpectralBalanceModule(module_prefix);
      case constants::kGlitchDelay:
        return new GlitchDelayModule(beats_per_second_, trance_gate_clock_, module_prefix);
      default:
        return nullptr;
    }
  }

  void ReorderableEffectChain::init() {
    SynthModule::init();
    // Start unused effects disabled before playback. Their initial resets
    // belong to construction, rather than the first host audio callback.
    for (auto* module : effects_) module->enable(false);
    for (auto& slot : slot_effects_)
      for (auto* module : slot) module->enable(false);
    selected_effects_.fill(-1);
  }

  void ReorderableEffectChain::setSampleRate(int rate) {
    // Hosts call this with processing stopped. All lightweight controls can
    // update here; expensive DSP receives its new rate on the worker.
    SynthModule::setSampleRate(rate);
    for (auto& slot : slot_rates_) slot.fill(rate);
  }

  void ReorderableEffectChain::configureEffect(int slot, int index) {
    auto* module = slot_effects_[slot][index];
    const int rate = getSampleRate() / getOversampleAmount();
    if (slot_rates_[slot][index] != rate) {
      module->setSampleRate(rate);
      slot_rates_[slot][index] = rate;
    }
    if (slot_oversamples_[slot][index] != getOversampleAmount()) {
      module->setOversampleAmount(getOversampleAmount());
      slot_oversamples_[slot][index] = getOversampleAmount();
    }
  }

  void ReorderableEffectChain::setOversampleAmount(int amount) {
    if (!buffers_prepared_) {
      // Reserve the chain's and slots' audio buffers once during graph setup.
      // Each effect's parameter controls reserve theirs when a slot first
      // selects it (reserveSelectedControls).
      SynthModule::setOversampleAmount(amount);
      for (auto& output : slot_outputs_) output->ensureBufferSize(kMaxBufferSize * amount);
      unused_output_->ensureBufferSize(kMaxBufferSize * amount);
      for (auto& slot : slot_oversamples_) slot.fill(amount);
      for (auto& slot : slot_rates_) slot.fill(getSampleRate() / amount);
      buffers_prepared_ = true;
      return;
    }
    Processor::setOversampleAmount(amount);
    // Inactive effect graphs need no rate changes on the audio thread.
    for (int slot = 0; slot < constants::kMaxEffectSlots; ++slot) {
      const int index = utils::iclamp(static_cast<int>(slot_types_[slot]->value() + .5f) - 1,
                                      -1, constants::kNumEffects - 1);
      if (index >= 0 && slot_enabled_[slot]->value()) configureEffect(slot, index);
    }
  }

  void ReorderableEffectChain::process(int num_samples) {
    const poly_float* audio_in = input(kAudio)->source->buffer;
    processWithInput(audio_in, num_samples);
  }

  namespace {
    bool isValidEffectOrder(const int* order) {
      bool seen[constants::kNumEffects] = {};
      for (int i = 0; i < constants::kNumEffects; ++i) {
        if (order[i] < 0 || order[i] >= constants::kNumEffects || seen[order[i]])
          return false;
        seen[order[i]] = true;
      }
      return true;
    }
  }

  PreparedEffectModule* ReorderableEffectChain::filterFx(int slot) {
    SynthModule* module = slot < 0 ? effects_[constants::kFilterFx] : slot_effects_[slot][constants::kFilterFx];
    return static_cast<PreparedEffectModule*>(module);
  }

  void ReorderableEffectChain::setFilterTable(int slot, Wavetable* table) {
    filterFx(slot)->setFilterTable(table);
  }

  void ReorderableEffectChain::prepareFilterTable(int slot) {
    filterFx(slot)->prepareFilterTable();
  }

  int ReorderableEffectChain::getLatencySamples() const {
    static_assert(kMaxLatencySamples >= constants::kMaxEffectSlots * SpectralGateModule::kMaxFftSize);
    static_assert(kMaxLatencySamples >= constants::kMaxEffectSlots * SpectralCompressorModule::kLatencySamples);
    static_assert(kMaxLatencySamples >= constants::kMaxEffectSlots * SuppressorModule::kLatencySamples);
    // Spectral Lock's Spectral mode grows with the processing rate (up to
    // 32768 samples at 176.4 kHz and above), so the budget holds two of them
    // there; LatencyCompensatedSum clamps rather than overruns beyond it.
    static_assert(kMaxLatencySamples >= 2 * SpectralLockModule::kMaxSpectralLatencySamples);
    int latency = 0;
    for (int slot = 0; slot < constants::kMaxEffectSlots; ++slot) {
      const int index = utils::iclamp(static_cast<int>(slot_types_[slot]->value() + 0.5f) - 1,
                                      -1, constants::kNumEffects - 1);
      if (index >= 0 && slot_enabled_[slot]->value())
        latency += slot_effects_[slot][index]->getLatencySamples();
    }
    return latency;
  }

  void ReorderableEffectChain::processWithInput(const poly_float* audio_in, int num_samples) {
    for (int slot = 0; slot < constants::kMaxEffectSlots; ++slot) {
      VITAL_ASSERT(utils::isFinite(audio_in, num_samples));

      const int index = utils::iclamp(static_cast<int>(slot_types_[slot]->value() + 0.5f) - 1,
                                      -1, constants::kNumEffects - 1);
      const bool on = index >= 0 && slot_enabled_[slot]->value();
      const int previous = selected_effects_[slot];
      if (previous == -2) {
        for (int effect = 0; effect < constants::kNumEffects; ++effect) {
          if (effect != index && slot_effects_[slot][effect]->enabled())
            slot_effects_[slot][effect]->enable(false);
        }
      }
      else if (previous >= 0 && previous != index && slot_effects_[slot][previous]->enabled())
        slot_effects_[slot][previous]->enable(false);
      selected_effects_[slot] = index;
      if (!on) {
        if (index >= 0 && slot_effects_[slot][index]->enabled())
          slot_effects_[slot][index]->enable(false);
        continue;
      }

      configureEffect(slot, index);
      bool enabled = slot_effects_[slot][index]->enabled();
      if (on != enabled)
        slot_effects_[slot][index]->enable(on);

      slot_effects_[slot][index]->processWithInput(audio_in, num_samples);
      audio_in = slot_effects_[slot][index]->output(0)->buffer;
    }

    VITAL_ASSERT(utils::isFinite(audio_in, num_samples));
    utils::copyBuffer(output()->buffer, audio_in, num_samples);
  }

  void ReorderableEffectChain::collectConvolutionModules(
      std::vector<std::pair<std::string, ConvolutionReverbModule*>>& modules) {
    auto* legacy = static_cast<PreparedEffectModule*>(effects_[constants::kConvolutionReverb])->convolutionState();
    if (legacy)
      modules.emplace_back(prefix_, legacy);

    for (int slot = 0; slot < constants::kMaxEffectSlots; ++slot) {
      auto* module = static_cast<PreparedEffectModule*>(slot_effects_[slot][constants::kConvolutionReverb])->convolutionState();
      if (module)
        modules.emplace_back(prefix_ + "fx_slot_" + std::to_string(slot + 1) + "_", module);
    }
  }

  void ReorderableEffectChain::stopPreparation() {
    for (auto* module : effects_)
      if (auto* prepared = dynamic_cast<PreparedEffectModule*>(module)) prepared->stopPreparation();
    for (auto& slot : slot_effects_)
      for (auto* module : slot)
        if (auto* prepared = dynamic_cast<PreparedEffectModule*>(module)) prepared->stopPreparation();
  }

  ReorderableEffectChain::~ReorderableEffectChain() { stopPreparation(); }

  void ReorderableEffectChain::reserveSelectedControls() {
    for (int slot = 0; slot < constants::kMaxEffectSlots; ++slot) {
      const int index = utils::iclamp(static_cast<int>(slot_types_[slot]->value() + 0.5f) - 1,
                                      -1, constants::kNumEffects - 1);
      if (index < 0) continue;
      if (auto* prepared = dynamic_cast<PreparedEffectModule*>(slot_effects_[slot][index]))
        prepared->reserveControls();
    }
  }

  void ReorderableEffectChain::prepareSelectedEffects() {
    reserveSelectedControls();
    for (int slot = 0; slot < constants::kMaxEffectSlots; ++slot) {
      const int index = utils::iclamp(static_cast<int>(slot_types_[slot]->value() + 0.5f) - 1,
                                      -1, constants::kNumEffects - 1);
      if (index < 0 || !slot_enabled_[slot]->value()) continue;
      configureEffect(slot, index);
      auto* module = slot_effects_[slot][index];
      if (!module->enabled()) module->enable(true);
      if (auto* prepared = dynamic_cast<PreparedEffectModule*>(module))
        prepared->prepareForPlayback();
    }
  }

  void ReorderableEffectChain::hardReset() {
    for (int i = 0; i < constants::kNumEffects; ++i)
      effects_[i]->hardReset();
    for (int slot = 0; slot < constants::kMaxEffectSlots; ++slot)
      for (int effect = 0; effect < constants::kNumEffects; ++effect)
        slot_effects_[slot][effect]->hardReset();
  }

  void ReorderableEffectChain::enable(bool on) {
    if (on != enabled()) {
      SynthModule::enable(on);
      selected_effects_.fill(-2);
    }
  }

  void ReorderableEffectChain::correctToTime(double seconds) {
    // Read the pending selection, not the last processed one: this runs before
    // processWithInput and must synchronize an effect on its first active block.
    for (int slot = 0; slot < constants::kMaxEffectSlots; ++slot) {
      const int index = utils::iclamp(static_cast<int>(slot_types_[slot]->value() + 0.5f) - 1,
                                      -1, constants::kNumEffects - 1);
      if (index >= 0 && slot_enabled_[slot]->value()) {
        configureEffect(slot, index);
        slot_effects_[slot][index]->correctToTime(seconds);
      }
    }
  }
} // namespace vital
