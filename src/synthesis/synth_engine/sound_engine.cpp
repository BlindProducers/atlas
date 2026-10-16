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

#include "sound_engine.h"
#include "prepared_effect_module.h"

#include "compressor_module.h"
#include "convolution_reverb_module.h"
#include "flanger_module.h"
#include "phaser_module.h"
#include "decimator.h"
#include "modulation_connection_processor.h"
#include "synth_constants.h"
#include "synth_voice_handler.h"
#include "multisample_midi_state.h"
#include "multisample_slot.h"
#include "source_slot_module.h"
#include "peak_meter.h"
#include "operators.h"
#include "latency_compensated_sum.h"
#include "reorderable_effect_chain.h"
#include "random_lfo.h"
#include "trigger_random.h"
#include "value_switch.h"

#include <cmath>

namespace vital {
  namespace {
    void setIndependentRandomOutputActive(Output* source, int connection_index, bool active) {
      if (source == nullptr || source->owner == nullptr)
        return;
      if (auto* random_lfo = dynamic_cast<RandomLfo*>(source->owner))
        random_lfo->setIndependentOutputActive(connection_index, active);
      else if (auto* note_random = dynamic_cast<TriggerRandom*>(source->owner))
        note_random->setIndependentOutputActive(connection_index, active);
    }
  }

  class BusRouter : public Processor {
    public:
      enum {
        kAudio,
        kDestination,
        kNumInputs
      };

      enum {
        kMainOut,
        kDirectOut,
        kBus1Out,
        kBus2Out,
        kBus3Out,
        kNumOutputs
      };

      explicit BusRouter(int bus_index) : Processor(kNumInputs, kNumOutputs), bus_index_(bus_index) { }

      Processor* clone() const override { return new BusRouter(*this); }

      void process(int num_samples) override {
        for (int output_index = 0; output_index < kNumOutputs; ++output_index)
          utils::zeroBuffer(output(output_index)->buffer, num_samples);

        const poly_float* source = input(kAudio)->source->buffer;
        poly_float* destination = output(outputIndex())->buffer;
        for (int i = 0; i < num_samples; ++i)
          destination[i] = source[i];
      }

      int outputIndex() const {
        const int destination = static_cast<int>(std::round(input(kDestination)->at(0)[0]));
        if (destination == constants::kBusDestinationDirectOut)
          return kDirectOut;

        if (destination >= constants::kBusDestinationBus1 &&
            destination <= constants::kBusDestinationBus3) {
          const int target_bus = destination - constants::kBusDestinationBus1;
          if (target_bus > bus_index_)
            return kBus1Out + target_bus;
        }

        return kMainOut;
      }

    private:
      int bus_index_;
  };


  SoundEngine::SoundEngine() : SynthModule(0, 1),
                               convolution_worker_(std::make_shared<ConvolutionIrWorker>()),
                               preparation_worker_(std::make_shared<EffectPreparationWorker>()),
                               multisample_midi_state_(std::make_unique<MultiSampleMidiState>()),
                               voice_handler_(nullptr), effect_chain_(nullptr),
                               main_chain_input_(nullptr), output_total_(nullptr),
                               last_oversampling_amount_(-1), last_sample_rate_(-1),
                               oversampling_(nullptr), legato_(nullptr), decimator_(nullptr), peak_meter_(nullptr) {
    for (int i = 0; i < kNumBuses; ++i) {
      bus_effect_chains_[i] = nullptr;
      bus_routers_[i] = nullptr;
      bus_chain_inputs_[i] = nullptr;
    }
    SoundEngine::init();
    bps_ = data_->controls["beats_per_minute"];
    modulation_processors_.reserve(kMaxModulationConnections);
  }

  SoundEngine::~SoundEngine() {
    for (auto* chain : bus_effect_chains_) chain->stopPreparation();
    effect_chain_->stopPreparation();
    voice_handler_->prepareDestroy();
  }

  void SoundEngine::prepareSelectedEffects() {
    voice_handler_->updateReverbMemory();
    for (auto* chain : bus_effect_chains_) chain->prepareSelectedEffects();
    effect_chain_->prepareSelectedEffects();
  }

  bool SoundEngine::takeEffectControlReservationRequest() {
    const bool reverb = voice_handler_->takeReverbMemoryRequest();
    const bool latency = latency_history_requested_.exchange(false, std::memory_order_acq_rel);
    return preparation_worker_->takeControlReservationRequest() || reverb || latency;
  }

  void SoundEngine::reserveSelectedEffectControls() {
    voice_handler_->updateReverbMemory();
    for (auto* chain : bus_effect_chains_) chain->reserveSelectedControls();
    effect_chain_->reserveSelectedControls();
    reserveLatencyHistory();
  }

  void SoundEngine::setVoiceReverbMemory(int filter, bool reserve) {
    voice_handler_->setReverbMemory(filter, reserve);
  }

  void SoundEngine::parkUnselectedEffects() {
    for (auto* chain : bus_effect_chains_) chain->parkUnselectedEffects();
    effect_chain_->parkUnselectedEffects();
  }

  bool SoundEngine::wireModulationDestination(const std::string& name) {
    for (auto* chain : bus_effect_chains_)
      if (chain->wireModulationDestination(name)) return true;
    return effect_chain_->wireModulationDestination(name);
  }

  void SoundEngine::init() {
    createBaseControl("bypass");
    createBaseControl("mpe_enabled");
    createBaseControl("view_spectrogram");
    oversampling_ = createBaseControl("oversampling");
    legato_ = createBaseControl("legato");

    Output* stereo_routing = createMonoModControl("stereo_routing");
    Value* stereo_mode = createBaseControl("stereo_mode");
    Output* beats_per_second = createMonoModControl("beats_per_minute");
    cr::LowerBound* beats_per_second_clamped = new cr::LowerBound(0.0f);
    beats_per_second_clamped->plug(beats_per_second);
    addProcessor(beats_per_second_clamped);

    Output* polyphony = createMonoModControl("polyphony");
    Value* voice_priority = createBaseControl("voice_priority");
    Value* voice_override = createBaseControl("voice_override");

    voice_handler_ = new SynthVoiceHandler(beats_per_second_clamped->output());
    addSubmodule(voice_handler_);
    voice_handler_->setPolyphony(vital::kMaxPolyphony);
    voice_handler_->plug(polyphony, VoiceHandler::kPolyphony);
    voice_handler_->plug(voice_priority, VoiceHandler::kVoicePriority);
    voice_handler_->plug(voice_override, VoiceHandler::kVoiceOverride);

    addProcessor(voice_handler_);

    createBaseControl("pitch_wheel");
    createBaseControl("mod_wheel");

    Value* effect_chain_order = createBaseControl("effect_chain_order");

    for (int i = 0; i < kNumBuses; ++i) {
      const std::string bus_prefix = "bus_" + std::to_string(i + 1) + "_";
      Value* bus_destination = createBaseControl(bus_prefix + "destination");
      Value* bus_effect_chain_order = createBaseControl(bus_prefix + "effect_chain_order");

      bus_chain_inputs_[i] = new LatencyCompensatedSum(i + 1,
          i * ReorderableEffectChain::kMaxLatencySamples, false);
      bus_chain_inputs_[i]->plug(voice_handler_->getBusOutput(i), 0);
      for (int previous_bus = 0; previous_bus < i; ++previous_bus)
        bus_chain_inputs_[i]->plug(bus_routers_[previous_bus]->output(BusRouter::kBus1Out + i), previous_bus + 1);
      addProcessor(bus_chain_inputs_[i]);

      bus_effect_chains_[i] = new ReorderableEffectChain(beats_per_second, voice_handler_->midi_offset_output(),
                                                         bus_prefix, convolution_worker_, scream_midi_note_.output(), &trance_gate_clock_, preparation_worker_,
                                               &note_monitor_);
      addSubmodule(bus_effect_chains_[i]);
      addProcessor(bus_effect_chains_[i]);
      bus_effect_chains_[i]->plug(bus_chain_inputs_[i], ReorderableEffectChain::kAudio);
      bus_effect_chains_[i]->plug(bus_effect_chain_order, ReorderableEffectChain::kOrder);

      bus_routers_[i] = new BusRouter(i);
      bus_routers_[i]->plug(bus_effect_chains_[i], BusRouter::kAudio);
      bus_routers_[i]->plug(bus_destination, BusRouter::kDestination);
      addProcessor(bus_routers_[i]);
    }

    main_chain_input_ = new LatencyCompensatedSum(kNumBuses + 1,
        kNumBuses * ReorderableEffectChain::kMaxLatencySamples, false);
    main_chain_input_->plug(voice_handler_, 0);
    for (int i = 0; i < kNumBuses; ++i)
      main_chain_input_->plug(bus_routers_[i]->output(BusRouter::kMainOut), i + 1);
    addProcessor(main_chain_input_);

    effect_chain_ = new ReorderableEffectChain(beats_per_second, voice_handler_->midi_offset_output(),
                                               "", convolution_worker_, scream_midi_note_.output(), &trance_gate_clock_, preparation_worker_,
                                               &note_monitor_);
    addSubmodule(effect_chain_);
    addProcessor(effect_chain_);
    effect_chain_->plug(main_chain_input_, ReorderableEffectChain::kAudio);
    effect_chain_->plug(effect_chain_order, ReorderableEffectChain::kOrder);

    SynthModule* compressor = effect_chain_->getEffect(constants::kCompressor);
    createStatusOutput("compressor_low_input", compressor->output(CompressorModule::kLowInputMeanSquared));
    createStatusOutput("compressor_band_input", compressor->output(CompressorModule::kBandInputMeanSquared));
    createStatusOutput("compressor_high_input", compressor->output(CompressorModule::kHighInputMeanSquared));
    createStatusOutput("compressor_low_output", compressor->output(CompressorModule::kLowOutputMeanSquared));
    createStatusOutput("compressor_band_output", compressor->output(CompressorModule::kBandOutputMeanSquared));
    createStatusOutput("compressor_high_output", compressor->output(CompressorModule::kHighOutputMeanSquared));

    SynthModule* chorus = effect_chain_->getEffect(constants::kChorus);
    for (int i = 0; i < ChorusModule::kMaxDelayPairs; ++i)
      createStatusOutput("chorus_delays" + std::to_string(i + 1), chorus->output(i + 1));
    
    SynthModule* phaser = effect_chain_->getEffect(constants::kPhaser);
    createStatusOutput("phaser_cutoff", phaser->output(PhaserModule::kCutoffOutput));

    SynthModule* flanger = effect_chain_->getEffect(constants::kFlanger);
    createStatusOutput("flanger_delay_frequency", flanger->output(FlangerModule::kFrequencyOutput));

    output_total_ = new LatencyCompensatedSum(kNumBuses + 2,
        (kNumBuses + 1) * ReorderableEffectChain::kMaxLatencySamples, false);
    output_total_->plug(effect_chain_, 0);
    output_total_->plug(voice_handler_->getDirectOutput(), 1);
    for (int i = 0; i < kNumBuses; ++i)
      output_total_->plug(bus_routers_[i]->output(BusRouter::kDirectOut), i + 2);
    addProcessor(output_total_);

    decimator_ = new Decimator(3);
    decimator_->plug(output_total_);
    addProcessor(decimator_);

    StereoEncoder* decoder = new StereoEncoder(true);
    decoder->plug(decimator_, StereoEncoder::kAudio);
    decoder->plug(stereo_routing, StereoEncoder::kEncodingValue);
    decoder->plug(stereo_mode, StereoEncoder::kMode);
    addProcessor(decoder);

    Output* volume = createMonoModControl("volume");
    SmoothVolume* scaled_audio = new SmoothVolume();
    scaled_audio->plug(decoder, SmoothVolume::kAudioRate);
    scaled_audio->plug(volume, SmoothVolume::kDb);

    peak_meter_ = new PeakMeter();
    peak_meter_->plug(scaled_audio, 0);
    createStatusOutput("peak_meter", peak_meter_->output(PeakMeter::kLevel));
    createStatusOutput("peak_meter_memory", peak_meter_->output(PeakMeter::kMemoryPeak));

    Clamp* clamp = new Clamp(-2.1f, 2.1f);
    clamp->plug(scaled_audio);
    
    addProcessor(peak_meter_);
    addProcessor(scaled_audio);

    addProcessor(clamp);
    clamp->useOutput(output());

    SynthModule::init();
    // Controls hold their defaults now; build each oscillator slot's default type.
    voice_handler_->prepareSelectedSources();
    for (int i = 0; i < kNumOscillators; ++i)
      getMultiSample(i)->setMidiState(multisample_midi_state_.get());
    // Resonator's MIDI pitch follows the voices' bend range, transpose and tune.
    static_assert(NoteMonitor::kLocalBendRange == VoiceHandler::kLocalPitchBendRange);
    const control_map voice_controls = voice_handler_->getControls();
    const auto control = [&voice_controls](const char* name) -> const Value* {
      const auto found = voice_controls.find(name);
      return found != voice_controls.end() ? found->second : nullptr;
    };
    note_monitor_.setPitchControls(control("pitch_bend_range"), control("voice_transpose"), control("voice_tune"));
    disableUnnecessaryModSources();
    setOversamplingAmount(kDefaultOversamplingAmount, kDefaultSampleRate);
  }

  void SoundEngine::connectModulation(const modulation_change& change) {
    // Audio-thread backstop: a destination that isn't a registered modulation target (e.g. the
    // envelope power sliders) leaves mono_destination null. Bail rather than dereference it.
    if (change.mono_destination == nullptr || change.source == nullptr ||
        change.modulation_processor == nullptr || change.mono_modulation_switch == nullptr)
      return;

    change.modulation_processor->plug(change.source, ModulationConnectionProcessor::kModulationInput);
    if (change.independent_random_source) {
      change.modulation_processor->plug(change.independent_random_source,
                                        ModulationConnectionProcessor::kIndependentRandomInput);
      setIndependentRandomOutputActive(change.independent_random_source,
                                       change.modulation_processor->index(), true);
    }
    else
      change.modulation_processor->unplugIndex(ModulationConnectionProcessor::kIndependentRandomInput);
    change.modulation_processor->setDestinationScale(change.destination_scale);
    VITAL_ASSERT(vital::utils::isFinite(change.destination_scale));

    Processor* destination = change.mono_destination;
    const bool source_polyphonic = change.source->owner->isPolyphonic();
    bool polyphonic = source_polyphonic && change.poly_destination;
    change.modulation_processor->setPolyphonicModulation(polyphonic);
    change.modulation_processor->setCollapsePolyphonicSource(source_polyphonic && !polyphonic);
    voice_handler_->enableModulationConnection(change.modulation_processor);
    if (polyphonic) {
      destination = change.poly_destination;
      voice_handler_->setActiveNonaccumulatedOutput(change.poly_destination->output());
    }

    if (!destination->isControlRate() &&
        (!change.source->isControlRate() || change.modulation_processor->sourceSmoothing())) {
      if (!change.source->isControlRate())
        change.source->owner->setControlRate(false);
      change.modulation_processor->setControlRate(false);
    }
    change.source->owner->enable(true);
    change.modulation_processor->enable(true);
    destination->plugNext(change.modulation_processor);
    change.modulation_processor->process(1);
    destination->process(1);

    change.mono_modulation_switch->set(1);
    if (change.poly_modulation_switch)
      change.poly_modulation_switch->set(1);

    // A route from an oscillator's audio runs inside the producers, between
    // slots, so it reads this block's audio rather than the last one's.
    if (change.source_slot >= 0) {
      voice_handler_->addOscillatorRoute(change.modulation_processor, change.source_slot, change.destination_slot);
      change.modulation_processor->setRunByProducers(true);
    }

    modulation_processors_.push_back(change.modulation_processor);
  }

  int SoundEngine::getNumPressedNotes() {
    return voice_handler_->getNumPressedNotes();
  }

  void SoundEngine::disconnectModulation(const modulation_change& change) {
    voice_handler_->removeOscillatorRoute(change.modulation_processor);
    change.modulation_processor->setRunByProducers(false);
    setIndependentRandomOutputActive(change.independent_random_source,
                                     change.modulation_processor->index(), false);
    change.modulation_processor->unplugIndex(ModulationConnectionProcessor::kIndependentRandomInput);
    change.modulation_processor->setDestinationScale(0.0f);
    change.modulation_processor->setCollapsePolyphonicSource(false);
    change.modulation_processor->setRefreshSourceWhenIdle(false);

    // A route kept while its slot plays a type without that destination was
    // never plugged in; there is nothing to unplug.
    if (change.mono_destination != nullptr && change.source != nullptr) {
      Processor* destination = change.mono_destination;
      if (change.source->owner->isPolyphonic() && change.poly_destination)
        destination = change.poly_destination;

      destination->unplug(change.modulation_processor);
    }
    voice_handler_->disableModulationConnection(change.modulation_processor);

    if (change.mono_destination != nullptr && change.mono_modulation_switch != nullptr &&
        change.mono_destination->connectedInputs() == 1 &&
        (change.poly_destination == nullptr || change.poly_destination->connectedInputs() == 0)) {
      change.mono_modulation_switch->set(0);

      if (change.poly_modulation_switch) {
        change.poly_modulation_switch->set(0);
        voice_handler_->setInactiveNonaccumulatedOutput(change.poly_destination->output());
      }
    }

    change.modulation_processor->enable(false);
    change.modulation_processor->setControlRate(true);
    if (change.num_audio_rate == 0 && change.source != nullptr)
      change.source->owner->setControlRate(true);

    modulation_processors_.remove(change.modulation_processor);
  }

  int SoundEngine::getNumActiveVoices() {
    return voice_handler_->getNumActiveVoices();
  }

  ModulationConnectionBank& SoundEngine::getModulationBank() {
    return voice_handler_->getModulationBank();
  }

  Output* SoundEngine::getIndependentRandomModulationSource(const std::string& source, int connection_index) {
    return voice_handler_->getIndependentRandomModulationSource(source, connection_index);
  }

  mono_float SoundEngine::getLastActiveNote() const {
    return voice_handler_->getLastActiveNote();
  }

  void SoundEngine::setTuning(const Tuning* tuning) {
    voice_handler_->setTuning(tuning);
    note_monitor_.setTuning(tuning);
  }

  void SoundEngine::syncVoiceGraphs() {
    // Setting the rate walks every voice copy and makes it match the voice
    // router's processors, which is also how prepareToPlay syncs them.
    const int oversample = std::max(1, voice_handler_->getOversampleAmount());
    voice_handler_->setSampleRate(voice_handler_->getSampleRate() / oversample);
  }

  void SoundEngine::checkOversampling() {
    const mono_float value = oversampling_->value();
    const int oversampling = value >= 0.0f && value <= 3.0f ? static_cast<int>(value) : 1;
    int oversampling_amount = 1 << oversampling;
    int sample_rate = getSampleRate();
    if (last_oversampling_amount_ != oversampling_amount || last_sample_rate_ != sample_rate)
      setOversamplingAmount(oversampling_amount, sample_rate);
  }

  void SoundEngine::setOversamplingAmount(int oversampling_amount, int sample_rate) {
    static constexpr int kBaseSampleRate = 44100;
    
    int oversample = oversampling_amount;
    int sample_rate_mult = sample_rate / kBaseSampleRate;
    while (sample_rate_mult > 1 && oversample > 1) {
      sample_rate_mult >>= 1;
      oversample >>= 1;
    }
    voice_handler_->setOversampleAmount(oversample);
    for (int i = 0; i < kNumBuses; ++i) {
      bus_chain_inputs_[i]->setOversampleAmount(oversample);
      bus_effect_chains_[i]->setOversampleAmount(oversample);
      bus_routers_[i]->setOversampleAmount(oversample);
    }
    main_chain_input_->setOversampleAmount(oversample);
    effect_chain_->setOversampleAmount(oversample);
    output_total_->setOversampleAmount(oversample);
    last_oversampling_amount_ = oversampling_amount;
    last_sample_rate_ = sample_rate;
    for (auto* input : bus_chain_inputs_)
      input->hardReset();
    main_chain_input_->hardReset();
    output_total_->hardReset();
  }

  void SoundEngine::updateEffectLatency() {
    applyEffectLatency();
    if (!latencyHistoryNeeded())
      return;
    // A compensation line meets its first delay. In realtime it passes audio
    // through undelayed until the message thread gives it history.
    if (RealtimeEffectScope::active())
      latency_history_requested_.store(true, std::memory_order_release);
    else
      reserveLatencyHistory();
  }

  bool SoundEngine::latencyHistoryNeeded() const {
    for (auto* input : bus_chain_inputs_)
      if (input->needsHistory()) return true;
    return main_chain_input_->needsHistory() || output_total_->needsHistory();
  }

  void SoundEngine::reserveLatencyHistory() {
    applyEffectLatency();
    if (!latencyHistoryNeeded())
      return;
    for (auto* input : bus_chain_inputs_)
      input->reserveNeededHistory();
    main_chain_input_->reserveNeededHistory();
    output_total_->reserveNeededHistory();
    applyEffectLatency();
  }

  void SoundEngine::applyEffectLatency() {
    int bus_latencies[kNumBuses] = {};
    int routes[kNumBuses] = {};
    int main_inputs[kNumBuses + 1] = {};
    int final_inputs[kNumBuses + 2] = {};
    for (int bus = 0; bus < kNumBuses; ++bus) {
      routes[bus] = bus_routers_[bus]->outputIndex();
      int inputs[kNumBuses] = {};
      for (int previous = 0; previous < bus; ++previous)
        inputs[previous + 1] = routes[previous] == BusRouter::kBus1Out + bus ?
            bus_latencies[previous] : -1;
      bus_latencies[bus] = bus_chain_inputs_[bus]->setInputLatencies(inputs) +
          bus_effect_chains_[bus]->getLatencySamples();
      main_inputs[bus + 1] = routes[bus] == BusRouter::kMainOut ? bus_latencies[bus] : -1;
      final_inputs[bus + 2] = routes[bus] == BusRouter::kDirectOut ? bus_latencies[bus] : -1;
    }
    final_inputs[0] = main_chain_input_->setInputLatencies(main_inputs) + effect_chain_->getLatencySamples();
    const int latency = output_total_->setInputLatencies(final_inputs);
    effect_latency_samples_ = latency / effect_chain_->getOversampleAmount();
  }

  void SoundEngine::process(int num_samples) {
    VITAL_ASSERT(num_samples <= output()->buffer_size);

    FloatVectorOperations::disableDenormalisedNumberSupport();
    updateEffectLatency();
    trance_gate_clock_.prepare(num_samples, getSampleRate(), bps_->value());
    voice_handler_->setLegato(legato_->value());
    ProcessorRouter::process(num_samples);
    multisample_midi_state_->endBlock();

    if (getNumActiveVoices() == 0) {
      CircularQueue<ModulationConnectionProcessor*>& connections = voice_handler_->enabledModulationConnection();
      refreshed_idle_sources_.clear();
      for (ModulationConnectionProcessor* modulation : connections) {
        // ProcessorRouter normally expands a host block to each processor's
        // oversampled length. Idle modulation is refreshed manually here, so
        // it must apply the same expansion. Processing only the host-sized
        // prefix leaves the second half (or more) of an audio-rate modulation
        // buffer stale; the next note then reads that stale data as oscillator
        // level, pitch, or another per-sample destination.
        const int modulation_samples = num_samples * modulation->getOversampleAmount();
        if (modulation->collapsesPolyphonicSource()) {
          // The final active block has already latched the actual sounding
          // voice pair. Keep that value constant through an FX-only tail;
          // reprocessing stale SIMD lanes here causes note-to-note alternation.
          modulation->holdCollapsedOutput(modulation_samples);
          continue;
        }
        if (!modulation->isInputSourcePolyphonic() || modulation->processWhenIdle()) {
          if (modulation->refreshSourceWhenIdle()) {
            Processor* source_owner = modulation->input(ModulationConnectionProcessor::kModulationInput)->source->owner;
            if (source_owner != nullptr && refreshed_idle_sources_.insert(source_owner))
              source_owner->process(num_samples * source_owner->getOversampleAmount());
          }
          modulation->process(modulation_samples);
        }
      }
    }

    for (auto& status_source : data_->status_outputs)
      status_source.second->update();
  }

  void SoundEngine::correctToTime(double seconds) {
    voice_handler_->correctToTime(seconds - voice_clock_origin_);
    for (int i = 0; i < kNumBuses; ++i)
      bus_effect_chains_[i]->correctToTime(seconds);
    effect_chain_->correctToTime(seconds);
  }

  void SoundEngine::allSoundsOff() {
    trance_gate_clock_.reset();
    note_monitor_.reset();
    voice_handler_->allSoundsOff();
    for (int i = 0; i < kNumOscillators; ++i)
      voice_handler_->getSourceSlot(i)->getMultiSample()->resetVoicesState();
    for (int i = 0; i < kNumBuses; ++i)
      bus_effect_chains_[i]->hardReset();
    effect_chain_->hardReset();
    for (auto* input : bus_chain_inputs_)
      input->hardReset();
    main_chain_input_->hardReset();
    output_total_->hardReset();
    decimator_->hardReset();
  }

  void SoundEngine::allNotesOff(int sample) {
    trance_gate_clock_.allNotesOff();
    note_monitor_.allNotesOff();
    voice_handler_->allNotesOff(sample);
  }

  void SoundEngine::allNotesOff(int sample, int channel) {
    trance_gate_clock_.allNotesOff(channel, channel);
    note_monitor_.allNotesOff(channel, channel);
    voice_handler_->allNotesOff(sample, channel);
  }

  void SoundEngine::allNotesOffRange(int sample, int from_channel, int to_channel) {
    trance_gate_clock_.allNotesOff(from_channel, to_channel);
    note_monitor_.allNotesOff(from_channel, to_channel);
    voice_handler_->allNotesOffRange(sample, from_channel, to_channel);
  }

  void SoundEngine::noteOn(int note, mono_float velocity, int sample, int channel) {
    if (velocity > 0) {
      scream_midi_note_.set(static_cast<float>(note));
      trance_gate_clock_.noteOn(note, channel, sample);
      note_monitor_.noteOn(note, channel);
    }
    else {
      trance_gate_clock_.noteOff(note, channel);
      note_monitor_.noteOff(note, channel);
    }
    voice_handler_->noteOn(note, velocity, sample, channel);
  }

  void SoundEngine::noteOff(int note, mono_float lift, int sample, int channel) {
    trance_gate_clock_.noteOff(note, channel);
    note_monitor_.noteOff(note, channel);
    voice_handler_->noteOff(note, lift, sample, channel);
  }

  void SoundEngine::setModWheel(mono_float value, int channel) {
    voice_handler_->setModWheel(value, channel);
  }

  void SoundEngine::setModWheelAllChannels(mono_float value) {
    voice_handler_->setModWheelAllChannels(value);
  }
  
  void SoundEngine::setPitchWheel(mono_float value, int channel) {
    note_monitor_.setLocalPitchWheel(value, channel);
    voice_handler_->setPitchWheel(value, channel);
  }

  void SoundEngine::setZonedPitchWheel(mono_float value, int from_channel, int to_channel) {
    note_monitor_.setZonedPitchWheel(value, from_channel, to_channel);
    voice_handler_->setZonedPitchWheel(value, from_channel, to_channel);
  }

  void SoundEngine::disableUnnecessaryModSources() {
    voice_handler_->disableUnnecessaryModSources();
  }

  void SoundEngine::enableModSource(const std::string& source) {
    getModulationSource(source)->owner->enable(true);
  }

  void SoundEngine::disableModSource(const std::string& source) {
    voice_handler_->disableModSource(source);
  }

  bool SoundEngine::isModSourceEnabled(const std::string& source) {
    return getModulationSource(source)->owner->enabled();
  }

  const StereoMemory* SoundEngine::getEqualizerMemory() {
    return effect_chain_->getEqualizerMemory();
  }

  std::vector<std::pair<std::string, ConvolutionReverbModule*>> SoundEngine::getConvolutionReverbModules() {
    std::vector<std::pair<std::string, ConvolutionReverbModule*>> modules;
    if (effect_chain_)
      effect_chain_->collectConvolutionModules(modules);
    for (int i = 0; i < kNumBuses; ++i) {
      if (bus_effect_chains_[i])
        bus_effect_chains_[i]->collectConvolutionModules(modules);
    }
    return modules;
  }

  namespace {
    // Resolves "filter_fx", "fx_slot_N_filter_fx" and "bus_B_..." prefixes.
    bool resolveFilterFx(const std::string& prefix, ReorderableEffectChain* main,
                         ReorderableEffectChain* const* buses, ReorderableEffectChain*& chain, int& slot) {
      chain = main;
      std::string rest = prefix;
      if (rest.rfind("bus_", 0) == 0 && rest.size() > 6) {
        const int bus = rest[4] - '1';
        if (bus < 0 || bus >= kNumBuses) return false;
        chain = buses[bus];
        rest = rest.substr(6);
      }
      if (chain == nullptr) return false;
      if (rest == "filter_fx") { slot = -1; return true; }
      if (rest.rfind("fx_slot_", 0) != 0) return false;
      const size_t separator = rest.find('_', 8);
      if (separator == std::string::npos || rest.substr(separator + 1) != "filter_fx") return false;
      slot = std::atoi(rest.substr(8, separator - 8).c_str()) - 1;
      return slot >= 0 && slot < constants::kMaxEffectSlots;
    }
  }

  void SoundEngine::prepareFilterTable(const std::string& prefix) {
    ReorderableEffectChain* chain = nullptr;
    int slot = -1;
    if (resolveFilterFx(prefix, effect_chain_, bus_effect_chains_, chain, slot))
      chain->prepareFilterTable(slot);
  }

  void SoundEngine::setFilterTable(const std::string& prefix, Wavetable* table) {
    if (prefix == "filter_1" || prefix == "filter_2") {
      voice_handler_->setFilterTable(prefix == "filter_2" ? 1 : 0, table);
      return;
    }
    ReorderableEffectChain* chain = effect_chain_;
    std::string rest = prefix;
    if (rest.rfind("bus_", 0) == 0 && rest.size() > 6) {
      const int bus = rest[4] - '1';
      if (bus < 0 || bus >= kNumBuses) return;
      chain = bus_effect_chains_[bus];
      rest = rest.substr(6);
    }
    if (chain == nullptr) return;
    if (rest == "filter_fx") {
      chain->setFilterTable(-1, table);
      return;
    }
    if (rest.rfind("fx_slot_", 0) == 0) {
      const size_t separator = rest.find('_', 8);
      if (separator == std::string::npos || rest.substr(separator + 1) != "filter_fx") return;
      const int slot = std::atoi(rest.substr(8, separator - 8).c_str()) - 1;
      if (slot >= 0 && slot < constants::kMaxEffectSlots) chain->setFilterTable(slot, table);
    }
  }

  void SoundEngine::setAftertouch(mono_float note, mono_float value, int sample, int channel) {
    voice_handler_->setAftertouch(note, value, sample, channel);
  }

  void SoundEngine::setChannelAftertouch(int channel, mono_float value, int sample) {
    voice_handler_->setChannelAftertouch(channel, value, sample);
  }

  void SoundEngine::setChannelRangeAftertouch(int from_channel, int to_channel, mono_float value, int sample) {
    voice_handler_->setChannelRangeAftertouch(from_channel, to_channel, value, sample);
  }

  void SoundEngine::setChannelSlide(int channel, mono_float value, int sample) {
    voice_handler_->setChannelSlide(channel, value, sample);
  }

  void SoundEngine::setChannelRangeSlide(int from_channel, int to_channel, mono_float value, int sample) {
    voice_handler_->setChannelRangeSlide(from_channel, to_channel, value, sample);
  }

  void SoundEngine::setBpm(mono_float bpm) {
    mono_float bps = bpm / 60.0f;
    if (bps_->value() != bps)
      bps_->set(bps);
  }

  Wavetable* SoundEngine::getWavetable(int index) {
    return voice_handler_->getWavetable(index);
  }

  Sample* SoundEngine::getSample(int index) {
    return voice_handler_->getSample(index);
  }

  SourceSlotModule* SoundEngine::getSourceSlot(int index) {
    return voice_handler_->getSourceSlot(index);
  }

  MultiSampleSlot* SoundEngine::getMultiSample(int index) {
    return voice_handler_->getSourceSlot(index)->getMultiSample();
  }

  void SoundEngine::controlChange(int controller, int value, int sample) {
    // All Controllers Off returns every CC to the instrument defaults.
    if (controller == 121)
      multisample_midi_state_->resetControllers();
    else
      multisample_midi_state_->controlChange(controller, static_cast<float>(value), sample);
  }

  bool SoundEngine::prepareSelectedSources() {
    return voice_handler_->prepareSelectedSources();
  }

  LineGenerator* SoundEngine::getLfoSource(int index) {
    return voice_handler_->getLfoSource(index);
  }

  void SoundEngine::sustainOn(int channel) {
    note_monitor_.sustainOn(channel, channel);
    voice_handler_->sustainOn(channel);
  }

  void SoundEngine::sustainOff(int sample, int channel) {
    note_monitor_.sustainOff(channel, channel);
    voice_handler_->sustainOff(sample, channel);
  }

  void SoundEngine::sostenutoOn(int channel) {
    voice_handler_->sostenutoOn(channel);
  }

  void SoundEngine::sostenutoOff(int sample, int channel) {
    voice_handler_->sostenutoOff(sample, channel);
  }

  void SoundEngine::sustainOnRange(int from_channel, int to_channel) {
    note_monitor_.sustainOn(from_channel, to_channel);
    voice_handler_->sustainOnRange(from_channel, to_channel);
  }

  void SoundEngine::sustainOffRange(int sample, int from_channel, int to_channel) {
    note_monitor_.sustainOff(from_channel, to_channel);
    voice_handler_->sustainOffRange(sample, from_channel, to_channel);
  }

  void SoundEngine::sostenutoOnRange(int from_channel, int to_channel) {
    voice_handler_->sostenutoOnRange(from_channel, to_channel);
  }

  void SoundEngine::sostenutoOffRange(int sample, int from_channel, int to_channel) {
    voice_handler_->sostenutoOffRange(sample, from_channel, to_channel);
  }
} // namespace vital
