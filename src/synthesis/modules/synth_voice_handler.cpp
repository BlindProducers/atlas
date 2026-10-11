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

#include "synth_voice_handler.h"

#include "envelope.h"
#include "envelope_module.h"
#include "filters_module.h"
#include "line_map.h"
#include "operators.h"
#include "release_velocity_glide.h"
#include "portamento_slope.h"
#include "random_lfo_module.h"
#include "synth_constants.h"
#include "lfo_module.h"
#include "trigger_random.h"
#include "value_switch.h"
#include "modulation_connection_processor.h"

#include <cstdlib>

namespace vital {
  namespace {
    int noteOnOffsetForVoice(const Output* trigger, poly_mask voice_mask, int num_samples) {
      const poly_mask note_on = trigger->trigger_mask & voice_mask &
                                poly_float::equal(trigger->trigger_value, kVoiceOn);
      for (int lane = 0; lane < poly_float::kSize; ++lane) {
        if (note_on[lane])
          return utils::iclamp(trigger->trigger_offset[lane], 0, std::max(0, num_samples - 1));
      }
      return -1;
    }
  }

  SynthVoiceHandler::SynthVoiceHandler(Output* beats_per_second) :
      VoiceHandler(0, kMaxPolyphony), producers_(nullptr), beats_per_second_(beats_per_second),
      note_from_reference_(nullptr), midi_offset_output_(nullptr),
      bent_midi_(nullptr), current_midi_note_(nullptr), amplitude_envelope_(nullptr), amplitude_(nullptr),
      pitch_wheel_(nullptr), filters_module_(nullptr), lfos_(), envelopes_(), lfo_sources_(), random_(nullptr),
      random_lfos_(), note_mapping_(nullptr), velocity_mapping_(nullptr), aftertouch_mapping_(nullptr),
      slide_mapping_(nullptr), lift_mapping_(nullptr), mod_wheel_mapping_(nullptr),
      pitch_wheel_mapping_(nullptr), stereo_(nullptr), note_percentage_(nullptr), last_active_voice_mask_(0) {
    output_ = new Add();
    registerOutput(output_->output());

    direct_output_ = new Add();
    registerOutput(direct_output_->output());
    for (int i = 0; i < kNumBuses; ++i) {
      bus_outputs_[i] = new Add();
      registerOutput(bus_outputs_[i]->output());
    }

    note_from_reference_ = new cr::Add();
    midi_offset_output_ = registerControlRateOutput(note_from_reference_->output(), true);

    enabled_modulation_processors_.ensureCapacity(kMaxModulationConnections);
  }

  void SynthVoiceHandler::init() {
    createNoteArticulation();
    createProducers();
    createModulators();
    createFilters(note_from_reference_->output());
    createVoiceOutput();
    filters_module_->plug(amplitude_, FiltersModule::kAmplitude);
    filters_module_->plug(voice_event(), FiltersModule::kVoiceEvent);

    Multiply* voice_sum = new Multiply();
    voice_sum->plug(producers_->output(ProducersModule::kRawOut), 0);
    voice_sum->plug(amplitude_, 1);
    output_->plug(voice_sum, 0);
    output_->plug(filters_module_->output(FiltersModule::kMainOut), 1);

    Multiply* direct_sum = new Multiply();
    direct_sum->plug(producers_->output(ProducersModule::kDirectOut), 0);
    direct_sum->plug(amplitude_, 1);
    direct_output_->plug(direct_sum, 0);
    direct_output_->plug(filters_module_->output(FiltersModule::kDirectOut), 1);

    addProcessor(voice_sum);
    addProcessor(direct_sum);
    addProcessor(output_);
    addProcessor(direct_output_);

    for (int i = 0; i < kNumBuses; ++i) {
      Multiply* bus_sum = new Multiply();
      bus_sum->plug(producers_->output(ProducersModule::kBus1Out + i), 0);
      bus_sum->plug(amplitude_, 1);
      bus_outputs_[i]->plug(bus_sum, 0);
      bus_outputs_[i]->plug(filters_module_->output(FiltersModule::kBus1Out + i), 1);
      addProcessor(bus_sum);
      addProcessor(bus_outputs_[i]);
    }

    Output* macros[kNumMacros];
    for (int i = 0; i < kNumMacros; ++i)
      macros[i] = createMonoModControl("macro_control_" + std::to_string(i + 1));

    setVoiceKiller(filters_module_->output(FiltersModule::kVoiceAlive));

    for (int i = 0; i < vital::kMaxModulationConnections; ++i) {
      ModulationConnectionProcessor* processor = modulation_bank_.atIndex(i)->modulation_processor.get();

      processor->plug(reset(), ModulationConnectionProcessor::kReset);

      std::string number = std::to_string(i + 1);
      std::string amount_name = "modulation_" + number + "_amount";
      Output* modulation_amount = createPolyModControl(amount_name);
      processor->plug(modulation_amount, ModulationConnectionProcessor::kModulationAmount);
      processor->initializeBaseValue(data_->controls[amount_name]);

      Output* modulation_power = createPolyModControl("modulation_" + number + "_power");
      processor->plug(modulation_power, ModulationConnectionProcessor::kModulationPower);

      addProcessor(processor);
      addSubmodule(processor);
      processor->enable(false);
    }

    VoiceHandler::init();
    producers_->setFilter1On(filters_module_->getFilter1OnValue());
    producers_->setFilter2On(filters_module_->getFilter2OnValue());
    producers_->setFilterInputs(filters_module_->getFilter1InputValue(), filters_module_->getFilter2InputValue());
    setupPolyModulationReadouts();

    for (int i = 0; i < kNumMacros; ++i) {
      std::string name = "macro_control_" + std::to_string(i + 1);
      data_->mod_sources[name] = macros[i];
      createStatusOutput(name, macros[i]);
    }

    for (int i = 0; i < kNumRandomLfos; ++i) {
      std::string name = "random_" + std::to_string(i + 1);
      data_->mod_sources[name] = random_lfos_[i]->output();
      createStatusOutput(name, random_lfos_[i]->output());
    }

    data_->mod_sources["random"] = random_->output();
    data_->mod_sources["stereo"] = stereo_->output();

    createStatusOutput("random", random_->output());
    createStatusOutput("stereo", stereo_->output());
    for (int i = 0; i < kNumOscillators; ++i) {
      // Each slot's playhead (sample or granular position) is a modulation source.
      const std::string name = source_types::slotPrefix(i) + "playhead";
      data_->mod_sources[name] = producers_->slotPhaseOutput(i);
      createStatusOutput(name, producers_->slotPhaseOutput(i));
      // So is each slot's audio, at audio rate, for audio-rate controls.
      data_->mod_sources[source_types::outputSourceName(i)] = producers_->slotModulationOutput(i);
    }
    createStatusOutput("num_voices", &num_voices_);

    std::string modulation_source_prefix = "modulation_source_";
    std::string modulation_amount_prefix = "modulation_amount_";
    for (int i = 0; i < vital::kMaxModulationConnections; ++i) {
      ModulationConnectionProcessor* processor = modulation_bank_.atIndex(i)->modulation_processor.get();
      std::string number = std::to_string(i + 1);
      Output* source_output = processor->output(ModulationConnectionProcessor::kModulationSource);
      Output* pre_scale_output = processor->output(ModulationConnectionProcessor::kModulationPreScale);
      createStatusOutput(modulation_source_prefix + number, source_output);
      createStatusOutput(modulation_amount_prefix + number, pre_scale_output);
    }
  }

  void SynthVoiceHandler::prepareDestroy() {
    for (int i = 0; i < vital::kMaxModulationConnections; ++i) {
      ModulationConnectionProcessor* processor = modulation_bank_.atIndex(i)->modulation_processor.get();
      removeProcessor(processor);
    }
  }

  Output* SynthVoiceHandler::getIndependentRandomModulationSource(const std::string& source,
                                                                  int connection_index) {
    if (connection_index < 0 || connection_index >= kMaxModulationConnections)
      return nullptr;
    if (source == "random")
      return random_->independentOutput(connection_index);
    if (source.rfind("random_", 0) != 0)
      return nullptr;

    const int random_index = std::atoi(source.c_str() + 7) - 1;
    if (random_index < 0 || random_index >= kNumRandomLfos)
      return nullptr;
    return random_lfos_[random_index]->independentOutput(connection_index);
  }

  void SynthVoiceHandler::createProducers() {
    producers_ = new ProducersModule(beats_per_second_);
    producers_->setVoiceHandler(this);
    producers_->plug(reset(), ProducersModule::kReset);
    producers_->plug(retrigger(), ProducersModule::kRetrigger);
    producers_->plug(voice_event(), ProducersModule::kVoiceEvent);
    producers_->plug(bent_midi_, ProducersModule::kMidi);
    producers_->plug(note(), ProducersModule::kZoneMidi);
    producers_->plug(current_midi_note_->output(), ProducersModule::kZoneGlideMidi);
    producers_->plug(velocity(), ProducersModule::kVelocity);
    producers_->plug(note_count(), ProducersModule::kNoteCount);
    producers_->plug(note_pressed(), ProducersModule::kNotePressed);
    producers_->plug(channel(), ProducersModule::kChannel);
    producers_->plug(active_mask(), ProducersModule::kActiveVoices);
    addSubmodule(producers_);
    addProcessor(producers_);
  }

  void SynthVoiceHandler::createModulators() {
    for (int i = 0; i < kNumLfos; ++i) {
      lfo_sources_[i].setLoop(false);
      lfo_sources_[i].initTriangle();
      std::string prefix = std::string("lfo_") + std::to_string(i + 1);
      LfoModule* lfo = new LfoModule(prefix, &lfo_sources_[i], beats_per_second_);
      addSubmodule(lfo);
      addProcessor(lfo);
      lfos_[i] = lfo;
      lfo->plug(retrigger(), LfoModule::kNoteTrigger);
      lfo->plug(note_count(), LfoModule::kNoteCount);
      lfo->plug(bent_midi_, LfoModule::kMidi);

      data_->mod_sources[prefix] = lfo->output(LfoModule::kValue);
      createStatusOutput(prefix, lfo->output(LfoModule::kValue));
      createStatusOutput(prefix + "_phase", lfo->output(LfoModule::kOscPhase));
      createStatusOutput(prefix + "_frequency", lfo->output(LfoModule::kOscFrequency));
    }

    for (int i = 0; i < kNumEnvelopes; ++i) {
      std::string prefix = std::string("env_") + std::to_string(i + 1);
      EnvelopeModule* envelope = new EnvelopeModule(prefix, beats_per_second_, i == 0);
      envelope->plug(retrigger(), EnvelopeModule::kTrigger);
      envelope->plug(reset(), EnvelopeModule::kReset);
      addSubmodule(envelope);
      addProcessor(envelope);
      envelopes_[i] = envelope;

      data_->mod_sources[prefix] = envelope->output();
      createStatusOutput(prefix, envelope->output(EnvelopeModule::kValue));
      createStatusOutput(prefix + "_phase", envelope->output(EnvelopeModule::kPhase));
    }

    random_ = new TriggerRandom();
    // Note-on random follows the same real retrigger event as the envelopes
    // and LFOs. A pitch change inside a mono-legato phrase must not jump every
    // destination fed by this source.
    random_->plug(retrigger());
    addProcessor(random_);

    for (int i = 0; i < kNumRandomLfos; ++i) {
      std::string name = "random_" + std::to_string(i + 1);
      random_lfos_[i] = new RandomLfoModule(name, beats_per_second_);
      random_lfos_[i]->plug(retrigger(), RandomLfoModule::kNoteTrigger);
      random_lfos_[i]->plug(bent_midi_, RandomLfoModule::kMidi);
      addSubmodule(random_lfos_[i]);
      addProcessor(random_lfos_[i]);
    }

    stereo_ = new cr::Value(constants::kLeftOne);
    addIdleMonoProcessor(stereo_);

    data_->mod_sources["note"] = note_percentage_->output();
    data_->mod_sources["note_in_octave"] = note_in_octave();
    data_->mod_sources["aftertouch"] = aftertouch();
    data_->mod_sources["velocity"] = velocity();
    data_->mod_sources["slide"] = slide();
    auto* release_glide = new ReleaseVelocityGlide();
    release_glide->plug(lift(), ReleaseVelocityGlide::kLift);
    release_glide->plug(retrigger(), ReleaseVelocityGlide::kRetrigger);
    release_glide->plug(createBaseControl("release_velocity_mode"), ReleaseVelocityGlide::kMode);
    release_glide->plug(createBaseControl("release_velocity_target"), ReleaseVelocityGlide::kTarget);
    release_glide->plug(createBaseControl("release_velocity_glide_time"), ReleaseVelocityGlide::kTime);
    release_glide->plug(createBaseControl("release_velocity_glide_curve"), ReleaseVelocityGlide::kCurve);
    addProcessor(release_glide);
    data_->mod_sources["lift"] = release_glide->output();
    SmoothControlSource* smooth_mod_wheel = new SmoothControlSource();
    smooth_mod_wheel->plug(mod_wheel(), SmoothControlSource::kTarget);
    smooth_mod_wheel->plug(reset(), SmoothControlSource::kReset);
    addProcessor(smooth_mod_wheel);

    data_->mod_sources["mod_wheel"] = smooth_mod_wheel->output();
    data_->mod_sources["pitch_wheel"] = pitch_wheel_percent();

    createStatusOutput("note", note_percentage_->output());
    createStatusOutput("note_in_octave", note_in_octave());
    createStatusOutput("aftertouch" ,aftertouch());
    createStatusOutput("velocity", velocity());
    createStatusOutput("slide", slide());
    createStatusOutput("lift", release_glide->output());
    createStatusOutput("mod_wheel", smooth_mod_wheel->output());
    createStatusOutput("pitch_wheel", pitch_wheel_percent());
  }

  void SynthVoiceHandler::createFilters(Output* keytrack) {
    filters_module_ = new FiltersModule();
    addSubmodule(filters_module_);
    addProcessor(filters_module_);

    filters_module_->plug(producers_->output(ProducersModule::kToFilter1), FiltersModule::kFilter1Input);
    filters_module_->plug(producers_->output(ProducersModule::kToFilter2), FiltersModule::kFilter2Input);
    filters_module_->plug(reset(), FiltersModule::kReset);
    filters_module_->plug(keytrack, FiltersModule::kKeytrack);
    filters_module_->plug(bent_midi_, FiltersModule::kMidi);
  }

  void SynthVoiceHandler::createNoteArticulation() {
    Output* portamento = createPolyModControl("portamento_time");
    Output* portamento_slope = createPolyModControl("portamento_slope");
    Value* portamento_force = createBaseControl("portamento_force");
    Value* portamento_scale = createBaseControl("portamento_scale");

    current_midi_note_ = new PortamentoSlope();
    current_midi_note_->plug(last_note(), PortamentoSlope::kSource);
    current_midi_note_->plug(note(), PortamentoSlope::kTarget);
    current_midi_note_->plug(portamento_force, PortamentoSlope::kPortamentoForce);
    current_midi_note_->plug(portamento_scale, PortamentoSlope::kPortamentoScale);
    current_midi_note_->plug(portamento, PortamentoSlope::kRunSeconds);
    current_midi_note_->plug(portamento_slope, PortamentoSlope::kSlopePower);
    current_midi_note_->plug(voice_event(), PortamentoSlope::kReset);
    current_midi_note_->plug(note_pressed(), PortamentoSlope::kNumNotesPressed);
    setVoiceMidi(current_midi_note_->output());
    addProcessor(current_midi_note_);

    Output* pitch_bend_range = createPolyModControl("pitch_bend_range");
    Output* voice_tune = createPolyModControl("voice_tune");
    Output* voice_transpose = createPolyModControl("voice_transpose");
    cr::Multiply* pitch_bend = new cr::Multiply();
    pitch_bend->plug(pitch_wheel(), 0);
    pitch_bend->plug(pitch_bend_range, 1);
    bent_midi_ = new cr::VariableAdd();
    bent_midi_->plugNext(current_midi_note_);
    bent_midi_->plugNext(pitch_bend);
    bent_midi_->plugNext(local_pitch_bend());
    bent_midi_->plugNext(voice_tune);
    bent_midi_->plugNext(voice_transpose);

    static const cr::Value max_midi_invert(1.0f / (kMidiSize - 1));
    note_percentage_ = new cr::Multiply();
    note_percentage_->plug(&max_midi_invert, 0);
    note_percentage_->plug(bent_midi_, 1);
    addProcessor(note_percentage_);

    static const cr::Value reference_adjust(-kMidiTrackCenter);
    note_from_reference_->plug(&reference_adjust, 0);
    note_from_reference_->plug(bent_midi_, 1);
    addProcessor(note_from_reference_);

    addProcessor(pitch_bend);
    addProcessor(bent_midi_);
  }

  void SynthVoiceHandler::createVoiceOutput() {
    Output* velocity_track_amount = createPolyModControl("velocity_track");
    cr::Interpolate* velocity_track_mult = new cr::Interpolate();
    velocity_track_mult->plug(&constants::kValueOne, Interpolate::kFrom);
    velocity_track_mult->plug(velocity(), Interpolate::kTo);
    velocity_track_mult->plug(velocity_track_amount, Interpolate::kFractional);
    addProcessor(velocity_track_mult);

    Output* voice_amplitude = createPolyModControl("voice_amplitude");
    cr::Multiply* amplitude = new cr::Multiply();
    amplitude->plug(velocity_track_mult, 0);
    amplitude->plug(voice_amplitude, 1);
    addProcessor(amplitude);

    amplitude_envelope_ = envelopes_[0];
    amplitude_envelope_->setControlRate(false);

    Processor* control_amplitude = new SmoothMultiply();
    control_amplitude->plug(amplitude_envelope_->output(Envelope::kValue), SmoothMultiply::kAudioRate);
    control_amplitude->plug(amplitude, SmoothMultiply::kControlRate);
    control_amplitude->plug(reset(), SmoothMultiply::kReset);

    amplitude_ = new Square();
    amplitude_->plug(control_amplitude);

    addProcessor(control_amplitude);
    addProcessor(amplitude_);
  }

  void SynthVoiceHandler::process(int num_samples) {
    poly_mask reset_mask = reset()->trigger_mask;
    if (reset_mask.anyMask())
      resetFeedbacks(reset_mask);

    // VoiceHandler may retire the final release voice while rendering this
    // block. Keep the voice pair that actually produced the block so global
    // FX modulation can be collapsed before that identity disappears.
    const poly_mask rendered_voice_mask = getCurrentVoiceMask();
    const Voice* rendered_voice = getCurrentVoice();
    VoiceHandler::process(num_samples);
    int num_voices = getNumActiveVoices();
    num_voices_.buffer[0] = num_voices;
    note_retriggered_.clearTrigger();

    if (rendered_voice_mask.anyMask()) {
      last_active_voice_mask_ = rendered_voice_mask;
      const int modulation_handoff_sample =
          noteOnOffsetForVoice(retrigger(), rendered_voice_mask, num_samples);
      for (ModulationConnectionProcessor* processor : enabled_modulation_processors_) {
        poly_float* buffer = processor->output()->buffer;
        if (processor->collapsesPolyphonicSource())
          processor->collapseToVoice(rendered_voice_mask, num_samples, modulation_handoff_sample, rendered_voice);
        else if (processor->isControlRate() || processor->isPolyphonicModulation()) {
          poly_float masked_value = buffer[0] & rendered_voice_mask;
          buffer[0] = masked_value + utils::swapVoices(masked_value);
        }
        else {
          for (int i = 0; i < num_samples; ++i) {
            poly_float masked_value = buffer[i] & rendered_voice_mask;
            buffer[i] = masked_value + utils::swapVoices(masked_value);
          }
        }
      }
    }

    if (num_voices == 0) {
      for (auto& status_source : data_->status_outputs)
        status_source.second->clear();
    }
    else {
      last_active_voice_mask_ = getCurrentVoiceMask();
      for (auto& status_source : data_->status_outputs)
        status_source.second->update(last_active_voice_mask_);
    }
  }

  void SynthVoiceHandler::noteOn(int note, mono_float velocity, int sample, int channel) {
    const bool internal_arp_channel = channel >= 16 && channel < 24;
    if (!internal_arp_channel && (getNumPressedNotes() < polyphony() || !legato()))
      note_retriggered_.trigger(constants::kFullMask, note, sample);
    VoiceHandler::noteOn(note, velocity, sample, channel);
  }

  void SynthVoiceHandler::noteOff(int note, mono_float lift, int sample, int channel) {
    const bool internal_arp_channel = channel >= 16 && channel < 24;
    if (!internal_arp_channel && getNumPressedNotes() > polyphony() && isNotePlaying(note) && !legato())
      note_retriggered_.trigger(constants::kFullMask, note, sample);

    VoiceHandler::noteOff(note, lift, sample, channel);
  }

  bool SynthVoiceHandler::shouldAccumulate(Output* output) {
    if (output->owner == amplitude_envelope_)
      return false;

    return VoiceHandler::shouldAccumulate(output);
  }

  void SynthVoiceHandler::correctToTime(double seconds) {
    for (int i = 0; i < kNumLfos; ++i)
      lfos_[i]->correctToTime(seconds);

    for (int i = 0; i < kNumRandomLfos; ++i)
      random_lfos_[i]->correctToTime(seconds);

    // Granular grid timing reads the shared host clock.
    producers_->correctToTime(seconds);
  }

  void SynthVoiceHandler::disableUnnecessaryModSources() {
    for (int i = 0; i < kNumLfos; ++i)
      lfos_[i]->enable(false);

    for (int i = 1; i < kNumEnvelopes; ++i)
      envelopes_[i]->enable(false);

    for (int i = 0; i < kNumRandomLfos; ++i)
      random_lfos_[i]->enable(false);

    random_->enable(false);

    for (int i = 0; i < kNumOscillators; ++i)
      producers_->slotModulationOutput(i)->owner->enable(false);
  }

  void SynthVoiceHandler::disableModSource(const std::string& source) {
    if (source == "env_1" || source == "lift")
      return;
    // Slot playheads are plain outputs with no processor of their own.
    Output* output = getModulationSource(source);
    if (output != nullptr && output->owner != nullptr)
      output->owner->enable(false);
  }

  void SynthVoiceHandler::enableModulationConnection(ModulationConnectionProcessor* processor) {
    enabled_modulation_processors_.push_back(processor);
  }

  void SynthVoiceHandler::disableModulationConnection(ModulationConnectionProcessor* processor) {
    enabled_modulation_processors_.remove(processor);
  }

  void SynthVoiceHandler::addOscillatorRoute(ModulationConnectionProcessor* processor, int source, int destination) {
    producers_->addOscillatorRoute(processor, source, destination);
  }

  void SynthVoiceHandler::removeOscillatorRoute(ModulationConnectionProcessor* processor) {
    producers_->removeOscillatorRoute(processor);
  }

  void SynthVoiceHandler::setupPolyModulationReadouts() {
    output_map& poly_mods = SynthModule::getPolyModulations();

    for (auto& mod : poly_mods)
      poly_readouts_[mod.first] = registerControlRateOutput(mod.second, mod.second->owner->enabled());
  }

  output_map& SynthVoiceHandler::getPolyModulations() {
    return poly_readouts_;
  }
  void SynthVoiceHandler::setFilterTable(int index, Wavetable* table) {
    if (filters_module_ != nullptr && index >= 0 && index < 2)
      filters_module_->getFilterModule(index)->setTableWavetable(table);
  }
} // namespace vital
