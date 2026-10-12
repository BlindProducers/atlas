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

#include "equalizer_module.h"

#include "digital_svf.h"
#include "value.h"

namespace vital {

  EqualizerModule::EqualizerModule(std::string prefix, bool create_audio_memory) :
      SynthModule(0, 1),
      low_mode_(nullptr), band_modes_ { nullptr, nullptr, nullptr, nullptr }, high_mode_(nullptr), prefix_(prefix),
      high_pass_(nullptr), low_shelf_(nullptr),
      notches_ { nullptr, nullptr, nullptr, nullptr }, band_shelves_ { nullptr, nullptr, nullptr, nullptr },
      low_pass_(nullptr), high_shelf_(nullptr) {
    if (create_audio_memory)
      audio_memory_ = std::make_shared<vital::StereoMemory>(vital::kAudioMemorySamples);
  }

  void EqualizerModule::init() {
    static const cr::Value kPass(DigitalSvf::k12Db);
    static const cr::Value kNotch(DigitalSvf::kNotchPassSwap);
    static const cr::Value kShelving(DigitalSvf::kShelving);

    high_pass_ = new DigitalSvf();
    low_shelf_ = new DigitalSvf();
    for (int i = 0; i < static_cast<int>(band_shelves_.size()); ++i) {
      band_shelves_[i] = new DigitalSvf();
      notches_[i] = new DigitalSvf();
    }
    low_pass_ = new DigitalSvf();
    high_shelf_ = new DigitalSvf();

    high_pass_->setDriveCompensation(false);
    high_pass_->setBasic(true);
    for (auto* notch : notches_) {
      notch->setDriveCompensation(false);
      notch->setBasic(true);
    }
    low_pass_->setDriveCompensation(false);
    low_pass_->setBasic(true);

    addIdleProcessor(high_pass_);
    addIdleProcessor(low_shelf_);
    for (int i = 0; i < static_cast<int>(band_shelves_.size()); ++i) {
      addIdleProcessor(notches_[i]);
      addIdleProcessor(band_shelves_[i]);
    }
    addIdleProcessor(low_pass_);
    addIdleProcessor(high_shelf_);
    low_pass_->useOutput(output());
    high_shelf_->useOutput(output());

    low_mode_ = createBaseControl(prefix_ + "eq_low_mode");
    band_modes_[0] = createBaseControl(prefix_ + "eq_band_mode");
    for (int i = 1; i < static_cast<int>(band_modes_.size()); ++i)
      band_modes_[i] = createBaseControl(prefix_ + "eq_band_" + std::to_string(i + 1) + "_mode");
    high_mode_ = createBaseControl(prefix_ + "eq_high_mode");

    Output* low_cutoff_midi = createMonoModControl(prefix_ + "eq_low_cutoff", true, true);
    std::array<Output*, 4> band_cutoff_midi;
    band_cutoff_midi[0] = createMonoModControl(prefix_ + "eq_band_cutoff", true, true);
    for (int i = 1; i < static_cast<int>(band_cutoff_midi.size()); ++i)
      band_cutoff_midi[i] = createMonoModControl(prefix_ + "eq_band_" + std::to_string(i + 1) + "_cutoff", true, true);
    Output* high_cutoff_midi = createMonoModControl(prefix_ + "eq_high_cutoff", true, true);

    Output* low_resonance = createMonoModControl(prefix_ + "eq_low_resonance");
    std::array<Output*, 4> band_resonance;
    band_resonance[0] = createMonoModControl(prefix_ + "eq_band_resonance");
    for (int i = 1; i < static_cast<int>(band_resonance.size()); ++i)
      band_resonance[i] = createMonoModControl(prefix_ + "eq_band_" + std::to_string(i + 1) + "_resonance");
    Output* high_resonance = createMonoModControl(prefix_ + "eq_high_resonance");

    Output* low_decibels = createMonoModControl(prefix_ + "eq_low_gain");
    std::array<Output*, 4> band_decibels;
    band_decibels[0] = createMonoModControl(prefix_ + "eq_band_gain");
    for (int i = 1; i < static_cast<int>(band_decibels.size()); ++i)
      band_decibels[i] = createMonoModControl(prefix_ + "eq_band_" + std::to_string(i + 1) + "_gain");
    Output* high_decibels = createMonoModControl(prefix_ + "eq_high_gain");

    high_pass_->plug(&kPass, DigitalSvf::kStyle);
    high_pass_->plug(&constants::kValueTwo, DigitalSvf::kPassBlend);
    high_pass_->plug(low_cutoff_midi, DigitalSvf::kMidiCutoff);
    high_pass_->plug(low_resonance, DigitalSvf::kResonance);

    low_shelf_->plug(&kShelving, DigitalSvf::kStyle);
    low_shelf_->plug(&constants::kValueZero, DigitalSvf::kPassBlend);
    low_shelf_->plug(low_cutoff_midi, DigitalSvf::kMidiCutoff);
    low_shelf_->plug(low_resonance, DigitalSvf::kResonance);
    low_shelf_->plug(low_decibels, DigitalSvf::kGain);

    for (int i = 0; i < static_cast<int>(band_shelves_.size()); ++i) {
      band_shelves_[i]->plug(&kShelving, DigitalSvf::kStyle);
      band_shelves_[i]->plug(&constants::kValueOne, DigitalSvf::kPassBlend);
      band_shelves_[i]->plug(band_cutoff_midi[i], DigitalSvf::kMidiCutoff);
      band_shelves_[i]->plug(band_resonance[i], DigitalSvf::kResonance);
      band_shelves_[i]->plug(band_decibels[i], DigitalSvf::kGain);

      notches_[i]->plug(&kNotch, DigitalSvf::kStyle);
      notches_[i]->plug(&constants::kValueOne, DigitalSvf::kPassBlend);
      notches_[i]->plug(band_cutoff_midi[i], DigitalSvf::kMidiCutoff);
      notches_[i]->plug(band_resonance[i], DigitalSvf::kResonance);
    }

    low_pass_->plug(&kPass, DigitalSvf::kStyle);
    low_pass_->plug(&constants::kValueZero, DigitalSvf::kPassBlend);
    low_pass_->plug(high_cutoff_midi, DigitalSvf::kMidiCutoff);
    low_pass_->plug(high_resonance, DigitalSvf::kResonance);

    high_shelf_->plug(&kShelving, DigitalSvf::kStyle);
    high_shelf_->plug(&constants::kValueTwo, DigitalSvf::kPassBlend);
    high_shelf_->plug(high_cutoff_midi, DigitalSvf::kMidiCutoff);
    high_shelf_->plug(high_resonance, DigitalSvf::kResonance);
    high_shelf_->plug(high_decibels, DigitalSvf::kGain);
    
    SynthModule::init();
  }

  void EqualizerModule::hardReset() {
    high_pass_->reset(constants::kFullMask);
    low_shelf_->reset(constants::kFullMask);
    for (int i = 0; i < static_cast<int>(band_shelves_.size()); ++i) {
      band_shelves_[i]->reset(constants::kFullMask);
      notches_[i]->reset(constants::kFullMask);
    }
    low_pass_->reset(constants::kFullMask);
    high_shelf_->reset(constants::kFullMask);
  }

  void EqualizerModule::enable(bool enable) {
    SynthModule::enable(enable);
    if (enable) {
      high_pass_->reset(constants::kFullMask);
      low_shelf_->reset(constants::kFullMask);
      for (int i = 0; i < static_cast<int>(band_shelves_.size()); ++i) {
        band_shelves_[i]->reset(constants::kFullMask);
        notches_[i]->reset(constants::kFullMask);
      }
      low_pass_->reset(constants::kFullMask);
      high_shelf_->reset(constants::kFullMask);
    }
  }

  void EqualizerModule::setSampleRate(int sample_rate) {
    // The base call also gives the smoothed controls the real rate, as in
    // every other module (a slot's controls get it from their prepared shell).
    SynthModule::setSampleRate(sample_rate);
    high_pass_->setSampleRate(sample_rate);
    low_shelf_->setSampleRate(sample_rate);
    for (int i = 0; i < static_cast<int>(band_shelves_.size()); ++i) {
      band_shelves_[i]->setSampleRate(sample_rate);
      notches_[i]->setSampleRate(sample_rate);
    }
    low_pass_->setSampleRate(sample_rate);
    high_shelf_->setSampleRate(sample_rate);
  }

  void EqualizerModule::processWithInput(const poly_float* audio_in, int num_samples) {
    SynthModule::process(num_samples);

    Processor* low_processor = low_mode_->value() ? high_pass_ : low_shelf_;
    Processor* high_processor = high_mode_->value() ? low_pass_ : high_shelf_;

    low_processor->processWithInput(audio_in, num_samples);
    const poly_float* current_output = low_processor->output()->buffer;
    for (int i = 0; i < static_cast<int>(band_shelves_.size()); ++i) {
      Processor* band_processor = band_modes_[i]->value() ? notches_[i] : band_shelves_[i];
      band_processor->processWithInput(current_output, num_samples);
      current_output = band_processor->output()->buffer;
    }
    high_processor->processWithInput(current_output, num_samples);

    if (audio_memory_) {
      const poly_float* output_buffer = high_processor->output()->buffer;
      for (int i = 0; i < num_samples; ++i)
        audio_memory_->push(output_buffer[i]);
    }
  }
} // namespace vital
