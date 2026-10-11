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

#include "noise_module.h"

namespace vital {

  NoiseModule::NoiseModule(std::string prefix, std::string slot_prefix) :
      SynthModule(kNumInputs, kNumOutputs), prefix_(std::move(prefix)), slot_prefix_(std::move(slot_prefix)),
      noise_(nullptr) {
    noise_ = new NoiseSource();
  }

  void NoiseModule::init() {
    Value* type = createBaseControl(prefix_ + "type");
    Value* slope = createBaseControl(prefix_ + "filter_slope");
    const std::string mix_prefix = slot_prefix_.empty() ? prefix_ : slot_prefix_;
    Output* level = createPolyModControl(mix_prefix + "level", true, true);
    Output* pan = createPolyModControl(mix_prefix + "pan");
    Output* width = createPolyModControl(prefix_ + "width");
    Output* low_cutoff = createPolyModControl(prefix_ + "low_cutoff", true, true);
    Output* high_cutoff = createPolyModControl(prefix_ + "high_cutoff", true, true);

    noise_->useInput(input(kReset), NoiseSource::kReset);
    noise_->useInput(input(kNotePressed), NoiseSource::kNotePressed);
    noise_->plug(type, NoiseSource::kType);
    noise_->plug(level, NoiseSource::kLevel);
    noise_->plug(pan, NoiseSource::kPan);
    noise_->plug(width, NoiseSource::kWidth);
    noise_->plug(low_cutoff, NoiseSource::kLowCutoff);
    noise_->plug(high_cutoff, NoiseSource::kHighCutoff);
    noise_->plug(slope, NoiseSource::kFilterSlope);
    if (!slot_prefix_.empty()) {
      // Slot-level FM/RM reads the slot's shared distortion controls.
      Value* distortion_type = createBaseControl(slot_prefix_ + "distortion_type");
      Output* distortion_amount = createPolyModControl(slot_prefix_ + "distortion_amount");
      Value* secondary_distortion_type = createBaseControl(slot_prefix_ + "secondary_distortion_type");
      Output* secondary_distortion_amount = createPolyModControl(slot_prefix_ + "secondary_distortion_amount");
      noise_->plug(distortion_type, NoiseSource::kDistortionType);
      noise_->plug(distortion_amount, NoiseSource::kDistortionAmount);
      noise_->plug(secondary_distortion_type, NoiseSource::kSecondaryDistortionType);
      noise_->plug(secondary_distortion_amount, NoiseSource::kSecondaryDistortionAmount);
    }
    noise_->useOutput(output(kRaw), NoiseSource::kRaw);
    noise_->useOutput(output(kLevelled), NoiseSource::kLevelled);

    addProcessor(noise_);
    SynthModule::init();
  }

} // namespace vital
