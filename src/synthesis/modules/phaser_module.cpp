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

#include "phaser_module.h"

#include "phaser.h"

namespace vital {

  PhaserModule::PhaserModule(const Output* beats_per_second, std::string prefix) :
      SynthModule(0, kNumOutputs), beats_per_second_(beats_per_second), prefix_(prefix), phaser_(nullptr) { }

  PhaserModule::~PhaserModule() {
  }

  void PhaserModule::init() {
    phaser_ = new Phaser();
    phaser_->useOutput(output(kAudioOutput), Phaser::kAudioOutput);
    phaser_->useOutput(output(kCutoffOutput), Phaser::kCutoffOutput);
    addIdleProcessor(phaser_);

    Output* phaser_free_frequency = createMonoModControl(prefix_ + "phaser_frequency");
    Output* phaser_frequency = createTempoSyncSwitch(prefix_ + "phaser", phaser_free_frequency->owner,
                                                     beats_per_second_, false);
    Output* phaser_feedback = createMonoModControl(prefix_ + "phaser_feedback");
    Output* phaser_wet = createMonoModControl(prefix_ + "phaser_dry_wet");
    Output* phaser_center = createMonoModControl(prefix_ + "phaser_center", true, true);
    Output* phaser_mod_depth = createMonoModControl(prefix_ + "phaser_mod_depth");
    Output* phaser_phase_offset = createMonoModControl(prefix_ + "phaser_phase_offset");
    Output* phaser_blend = createMonoModControl(prefix_ + "phaser_blend");

    phaser_->plug(phaser_frequency, Phaser::kRate);
    phaser_->plug(phaser_wet, Phaser::kMix);
    phaser_->plug(phaser_feedback, Phaser::kFeedbackGain);
    phaser_->plug(phaser_center, Phaser::kCenter);
    phaser_->plug(phaser_mod_depth, Phaser::kModDepth);
    phaser_->plug(phaser_phase_offset, Phaser::kPhaseOffset);
    phaser_->plug(phaser_blend, Phaser::kBlend);
    phaser_->init();

    SynthModule::init();
  }

  void PhaserModule::hardReset() {
    phaser_->hardReset();
  }

  void PhaserModule::enable(bool enable) {
    SynthModule::enable(enable);
    process(1);
    if (enable)
      phaser_->hardReset();
  }

  void PhaserModule::correctToTime(double seconds) {
    SynthModule::correctToTime(seconds);
    phaser_->correctToTime(seconds);
  }

  void PhaserModule::setSampleRate(int sample_rate) {
    SynthModule::setSampleRate(sample_rate);
    phaser_->setSampleRate(sample_rate);
  }

  void PhaserModule::processWithInput(const poly_float* audio_in, int num_samples) {
    SynthModule::process(num_samples);
    phaser_->processWithInput(audio_in, num_samples);
  }
} // namespace vital
