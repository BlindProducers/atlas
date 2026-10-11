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

#include "filter_module.h"

#include "comb_module.h"
#include "digital_svf.h"
#include "diode_filter.h"
#include "dirty_filter.h"
#include "formant_module.h"
#include "ladder_filter.h"
#include "phaser_filter.h"
#include "sallen_key_filter.h"
#include "reverb_filter.h"
#include "ring_mod_filter.h"
#include "sample_hold_filter.h"
#include "scream_filter.h"
#include "synth_constants.h"
#include "table_filter.h"

namespace vital {

  FilterModule::FilterModule(std::string prefix) :
      SynthModule(kNumInputs, 1), last_model_(-1), was_on_(false), 
      prefix_(std::move(prefix)), create_on_value_(true), mono_(false),
      on_(nullptr), filter_model_(nullptr), mix_(0.0f), filter_mix_(nullptr), keytrack_smoother_(nullptr),
      comb_filter_(nullptr), digital_svf_(nullptr), dirty_filter_(nullptr),
      formant_filter_(nullptr), ladder_filter_(nullptr), phaser_filter_(nullptr),
      sallen_key_filter_(nullptr), ring_mod_filter_(nullptr), sample_hold_filter_(nullptr), table_filter_(nullptr) {
    reverb_filter_ = new ReverbFilter();
    scream_filter_ = new ScreamFilter();
    ring_mod_filter_ = new RingModFilter();
    sample_hold_filter_ = new SampleHoldFilter();
    comb_filter_ = new CombModule();
    digital_svf_ = new DigitalSvf();
    diode_filter_ = new DiodeFilter();
    dirty_filter_ = new DirtyFilter();
    ladder_filter_ = new LadderFilter();
    phaser_filter_ = new PhaserFilter(false);
    sallen_key_filter_ = new SallenKeyFilter();
    formant_filter_ = new FormantModule(prefix_);
    table_filter_ = new TableFilter();

    addSubmodule(comb_filter_);
    addSubmodule(formant_filter_);

    addProcessor(reverb_filter_);
    addProcessor(scream_filter_);
    addProcessor(ring_mod_filter_);
    addProcessor(sample_hold_filter_);
    addProcessor(comb_filter_);
    addProcessor(digital_svf_);
    addProcessor(diode_filter_);
    addProcessor(dirty_filter_);
    addProcessor(formant_filter_);
    addProcessor(ladder_filter_);
    addProcessor(phaser_filter_);
    addProcessor(sallen_key_filter_);
    addProcessor(table_filter_);
  }

  void FilterModule::setTableWavetable(Wavetable* table) {
    table_filter_->setWavetable(table);
  }

  void FilterModule::prepareTableFilter() {
    table_filter_->prepare();
  }

  int FilterModule::getLatencySamples() const {
    if (filter_model_ == nullptr || static_cast<int>(roundf(filter_model_->value())) != constants::kFilterTable)
      return 0;
    return table_filter_->getLatencySamples();
  }

  void FilterModule::init() {
    Output* keytrack_amount = createModControl(prefix_ + "_keytrack");
    cr::Multiply* current_keytrack = new cr::Multiply();
    current_keytrack->useInput(input(kKeytrack), 0);
    current_keytrack->plug(keytrack_amount, 1);
    addProcessor(current_keytrack);

    // MIDI events can leave only one sample in a processing block. A block-long
    // ramp then jumps the comb delay tap almost instantly. Smooth only the
    // keytracking contribution with a fixed time (about 15 ms to settle at 99%),
    // preserving audio-rate envelope/LFO cutoff modulation and fresh-note resets.
    keytrack_smoother_ = new SmoothControlSource(50.0f);
    keytrack_smoother_->plug(current_keytrack, SmoothControlSource::kTarget);
    keytrack_smoother_->useInput(input(kReset), SmoothControlSource::kReset);
    addProcessor(keytrack_smoother_);

    Output* midi_cutoff = createModControl(prefix_ + "_cutoff", true, true, keytrack_smoother_->output());
    Output* resonance = createModControl(prefix_ + "_resonance");
    Output* drive = createModControl(prefix_ + "_drive");
    Output* blend = createModControl(prefix_ + "_blend");
    Output* blend_transpose = createModControl(prefix_ + "_blend_transpose");
    if (create_on_value_)
      on_ = createBaseControl(prefix_ + "_on");
    Value* filter_style = createBaseControl(prefix_ + "_style");
    filter_model_ = createBaseControl(prefix_ + "_model");

    filter_mix_ = createModControl(prefix_ + "_mix");
    Output* reverb_cutoff = createModControl(prefix_ + "_reverb_cutoff", true, true);
    Output* reverb_damp = createModControl(prefix_ + "_reverb_damp");
    Output* reverb_bass = createModControl(prefix_ + "_reverb_bass_preserve");
    reverb_output_ = createModControl(prefix_ + "_reverb_output");
    reverb_filter_->useInput(input(kAudio), ReverbFilter::kAudio);
    reverb_filter_->useInput(input(kReset), ReverbFilter::kReset);
    reverb_filter_->plug(reverb_cutoff, ReverbFilter::kCutoffLog);
    reverb_filter_->plug(keytrack_smoother_, ReverbFilter::kKeytrack);
    reverb_filter_->plug(resonance, ReverbFilter::kResonance);
    reverb_filter_->plug(reverb_damp, ReverbFilter::kDamp);
    reverb_filter_->plug(reverb_bass, ReverbFilter::kBassPreserve);
    reverb_filter_->useOutput(output());

    scream_filter_->useInput(input(kAudio), ScreamFilter::kAudio);
    scream_filter_->useInput(input(kReset), ScreamFilter::kReset);
    scream_filter_->plug(midi_cutoff, ScreamFilter::kMidiCutoff);
    scream_filter_->plug(resonance, ScreamFilter::kResonance);
    scream_filter_->plug(createModControl(prefix_ + "_scream_amount"), ScreamFilter::kScream);
    scream_filter_->plug(createModControl(prefix_ + "_scream_input_gain"), ScreamFilter::kInputGain);
    scream_filter_->plug(filter_mix_, ScreamFilter::kMix);
    scream_filter_->plug(createModControl(prefix_ + "_scream_output_gain"), ScreamFilter::kOutputGain);
    scream_filter_->plug(createBaseControl(prefix_ + "_scream_autogain"), ScreamFilter::kAutogain);
    scream_filter_->useOutput(output());

    ring_mod_filter_->useInput(input(kAudio), RingModFilter::kAudio);
    ring_mod_filter_->useInput(input(kReset), RingModFilter::kReset);
    ring_mod_filter_->plug(midi_cutoff, RingModFilter::kMidiCutoff);
    ring_mod_filter_->plug(filter_style, RingModFilter::kStyle);
    ring_mod_filter_->useOutput(output());

    sample_hold_filter_->useInput(input(kAudio), SampleHoldFilter::kAudio);
    sample_hold_filter_->useInput(input(kReset), SampleHoldFilter::kReset);
    sample_hold_filter_->plug(midi_cutoff, SampleHoldFilter::kMidiCutoff);
    sample_hold_filter_->plug(filter_style, SampleHoldFilter::kStyle);
    sample_hold_filter_->plug(blend, SampleHoldFilter::kCrush);
    sample_hold_filter_->useOutput(output());

    table_filter_->useInput(input(kAudio), TableFilter::kAudio);
    table_filter_->useInput(input(kReset), TableFilter::kReset);
    table_filter_->plug(midi_cutoff, TableFilter::kMidiCutoff);
    table_filter_->plug(resonance, TableFilter::kResonance);
    table_filter_->plug(filter_style, TableFilter::kStyle);
    table_filter_->plug(createModControl(prefix_ + "_table_frame"), TableFilter::kFrame);
    table_filter_->useOutput(output());

    comb_filter_->useInput(input(kAudio), CombModule::kAudio);
    comb_filter_->plug(filter_style, CombModule::kStyle);
    comb_filter_->useInput(input(kReset), CombModule::kReset);
    comb_filter_->useInput(input(kMidi), CombModule::kMidi);
    comb_filter_->plug(midi_cutoff, CombModule::kMidiCutoff);
    comb_filter_->plug(blend_transpose, CombModule::kMidiBlendTranspose);
    comb_filter_->plug(blend, CombModule::kFilterCutoffBlend);
    comb_filter_->plug(resonance, CombModule::kResonance);
    comb_filter_->useOutput(output());

    digital_svf_->useInput(input(kAudio), DigitalSvf::kAudio);
    digital_svf_->plug(filter_style, DigitalSvf::kStyle);
    digital_svf_->plug(blend, DigitalSvf::kPassBlend);
    digital_svf_->useInput(input(kReset), DigitalSvf::kReset);
    digital_svf_->plug(midi_cutoff, DigitalSvf::kMidiCutoff);
    digital_svf_->plug(resonance, DigitalSvf::kResonance);
    digital_svf_->plug(drive, DigitalSvf::kDriveGain);
    digital_svf_->useOutput(output());

    diode_filter_->useInput(input(kAudio), DiodeFilter::kAudio);
    diode_filter_->useInput(input(kReset), DiodeFilter::kReset);
    diode_filter_->plug(resonance, DiodeFilter::kResonance);
    diode_filter_->plug(filter_style, DiodeFilter::kStyle);
    diode_filter_->plug(blend, DiodeFilter::kPassBlend);
    diode_filter_->plug(midi_cutoff, DiodeFilter::kMidiCutoff);
    diode_filter_->plug(drive, DiodeFilter::kDriveGain);
    diode_filter_->useOutput(output());

    dirty_filter_->useInput(input(kAudio), DirtyFilter::kAudio);
    dirty_filter_->useInput(input(kReset), DirtyFilter::kReset);
    dirty_filter_->plug(resonance, DirtyFilter::kResonance);
    dirty_filter_->plug(filter_style, DirtyFilter::kStyle);
    dirty_filter_->plug(blend, DirtyFilter::kPassBlend);
    dirty_filter_->plug(midi_cutoff, DirtyFilter::kMidiCutoff);
    dirty_filter_->plug(drive, DirtyFilter::kDriveGain);
    dirty_filter_->useOutput(output());

    formant_filter_->useInput(input(kAudio), FormantModule::kAudio);
    formant_filter_->useInput(input(kReset), FormantModule::kReset);
    formant_filter_->plug(blend, FormantModule::kBlend);
    formant_filter_->plug(filter_style, FormantModule::kStyle);
    formant_filter_->useOutput(output());

    ladder_filter_->useInput(input(kAudio), LadderFilter::kAudio);
    ladder_filter_->useInput(input(kReset), LadderFilter::kReset);
    ladder_filter_->plug(resonance, LadderFilter::kResonance);
    ladder_filter_->plug(filter_style, LadderFilter::kStyle);
    ladder_filter_->plug(blend, LadderFilter::kPassBlend);
    ladder_filter_->plug(midi_cutoff, LadderFilter::kMidiCutoff);
    ladder_filter_->plug(drive, LadderFilter::kDriveGain);
    ladder_filter_->useOutput(output());

    phaser_filter_->useInput(input(kAudio), PhaserFilter::kAudio);
    phaser_filter_->useInput(input(kReset), PhaserFilter::kReset);
    phaser_filter_->plug(resonance, PhaserFilter::kResonance);
    phaser_filter_->plug(filter_style, PhaserFilter::kStyle);
    phaser_filter_->plug(blend_transpose, PhaserFilter::kTranspose);
    phaser_filter_->plug(blend, PhaserFilter::kPassBlend);
    phaser_filter_->plug(midi_cutoff, PhaserFilter::kMidiCutoff);
    phaser_filter_->plug(drive, PhaserFilter::kDriveGain);
    phaser_filter_->useOutput(output());

    sallen_key_filter_->plug(filter_style, SallenKeyFilter::kStyle);
    sallen_key_filter_->useInput(input(kAudio), SallenKeyFilter::kAudio);
    sallen_key_filter_->plug(blend, SallenKeyFilter::kPassBlend);
    sallen_key_filter_->useInput(input(kReset), SallenKeyFilter::kReset);
    sallen_key_filter_->plug(midi_cutoff, SallenKeyFilter::kMidiCutoff);
    sallen_key_filter_->plug(resonance, SallenKeyFilter::kResonance);
    sallen_key_filter_->plug(drive, SallenKeyFilter::kDriveGain);
    sallen_key_filter_->useOutput(output());

    reverb_filter_->enable(false);
    scream_filter_->enable(false);
    ring_mod_filter_->enable(false);
    sample_hold_filter_->enable(false);
    comb_filter_->enable(false);
    digital_svf_->enable(false);
    diode_filter_->enable(false);
    dirty_filter_->enable(false);
    formant_filter_->enable(false);
    ladder_filter_->enable(false);
    sallen_key_filter_->enable(false);

    SynthModule::init();
  }

  void FilterModule::hardReset() {
    getLocalProcessor(reverb_filter_)->hardReset();
    getLocalProcessor(scream_filter_)->hardReset();
    getLocalProcessor(ring_mod_filter_)->hardReset();
    getLocalProcessor(sample_hold_filter_)->hardReset();
    getLocalProcessor(table_filter_)->hardReset();
    reverb_gain_ = 1.0f;
    if (keytrack_smoother_)
      getLocalProcessor(keytrack_smoother_)->hardReset();
    comb_filter_->hardReset();
    digital_svf_->hardReset();
    diode_filter_->hardReset();
    dirty_filter_->hardReset();
    formant_filter_->hardReset();
    ladder_filter_->hardReset();
    phaser_filter_->hardReset();
    sallen_key_filter_->hardReset();
  }

  Output* FilterModule::createModControl(std::string name, bool audio_rate, bool smooth_value,
                                         Output* internal_modulation) {
    if (mono_)
      return createMonoModControl(name, audio_rate, smooth_value, internal_modulation);
    return createPolyModControl(name, audio_rate, smooth_value, internal_modulation, input(kReset));
  }

  force_inline void FilterModule::setModel(int new_model) {
    reverb_filter_->enable(new_model == constants::kReverbFilter);
    scream_filter_->enable(new_model == constants::kScreamFilter);
    ring_mod_filter_->enable(new_model == constants::kRingModFilter);
    sample_hold_filter_->enable(new_model == constants::kSampleHoldFilter);
    table_filter_->enable(new_model == constants::kFilterTable);
    comb_filter_->enable(new_model == constants::kComb);
    digital_svf_->enable(new_model == constants::kDigital);
    diode_filter_->enable(new_model == constants::kDiode);
    dirty_filter_->enable(new_model == constants::kDirty);
    formant_filter_->enable(new_model == constants::kFormant);
    ladder_filter_->enable(new_model == constants::kLadder);
    phaser_filter_->enable(new_model == constants::kPhase);
    sallen_key_filter_->enable(new_model == constants::kAnalog);

    if (new_model == last_model_)
      return;

    Processor* to_reset = nullptr;
    if (new_model == constants::kReverbFilter)
      to_reset = reverb_filter_;
    else if (new_model == constants::kScreamFilter)
      to_reset = scream_filter_;
    else if (new_model == constants::kRingModFilter)
      to_reset = ring_mod_filter_;
    else if (new_model == constants::kSampleHoldFilter)
      to_reset = sample_hold_filter_;
    else if (new_model == constants::kFilterTable)
      to_reset = table_filter_;
    else if (new_model == constants::kAnalog)
      to_reset = sallen_key_filter_;
    else if (new_model == constants::kComb)
      to_reset = comb_filter_;
    else if (new_model == constants::kDigital)
      to_reset = digital_svf_;
    else if (new_model == constants::kDiode)
      to_reset = diode_filter_;
    else if (new_model == constants::kDirty)
      to_reset = dirty_filter_;
    else if (new_model == constants::kFormant)
      to_reset = formant_filter_;
    else if (new_model == constants::kLadder)
      to_reset = ladder_filter_;
    else if (new_model == constants::kPhase)
      to_reset = phaser_filter_;

    if (to_reset)
      getLocalProcessor(to_reset)->hardReset();

    last_model_ = new_model;
  }

  void FilterModule::process(int num_samples) {
    bool on = on_ == nullptr || on_->value() > 0.5f;
    setModel(static_cast<int>(roundf(filter_model_->value())));
    if (on && !was_on_ && last_model_ == constants::kScreamFilter)
      getLocalProcessor(scream_filter_)->hardReset();
    was_on_ = on;

    if (on) {
      SynthModule::process(num_samples);

      // Scream applies its mix before autogain/output, as in the original DSP.
      if (last_model_ == constants::kScreamFilter) return;

      poly_float current_mix = mix_;
      mix_ = utils::clamp(filter_mix_->buffer[0], 0.0f, 1.0f);
      current_mix = utils::maskLoad(current_mix, mix_, getResetMask(kReset));
      poly_float delta_mix = (mix_ - current_mix) * (1.0f / num_samples);

      const bool reverb = last_model_ == constants::kReverbFilter;
      poly_float current_gain = reverb_gain_;
      reverb_gain_ = reverb ? futils::dbToMagnitude(utils::clamp(reverb_output_->buffer[0], -24.0f, 12.0f)) : poly_float(1.0f);
      current_gain = utils::maskLoad(current_gain, reverb_gain_, getResetMask(kReset));
      const poly_float gain_step = (reverb_gain_ - current_gain) * (1.0f / num_samples);
      poly_float* audio_out = output()->buffer;
      const poly_float* audio_in = input(kAudio)->source->buffer;
      for (int i = 0; i < num_samples; ++i) {
        current_mix += delta_mix;
        current_gain += gain_step;
        audio_out[i] = utils::interpolate(audio_in[i], audio_out[i], current_mix);
        if (reverb) audio_out[i] *= current_gain;
      }
    }
    else
      utils::zeroBuffer(output()->buffer, num_samples);
  }

  void FilterModule::setMono(bool mono) {
    mono_ = mono;
    scream_filter_->setFxMode(mono);
    formant_filter_->setMono(mono);
    table_filter_->setMono(mono);
  }
} // namespace vital
