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

#include "granular_module.h"

namespace vital {

  GranularModule::GranularModule(std::string prefix, std::shared_ptr<Sample> sample,
                                 const Output* beats_per_second, std::string slot_prefix) :
      SynthModule(kNumInputs, kNumOutputs), prefix_(std::move(prefix)), slot_prefix_(std::move(slot_prefix)),
      beats_per_second_(beats_per_second) {
    granular_ = new GranularSource(std::move(sample));
  }

  void GranularModule::init() {
    Value* keytrack = createBaseControl(prefix_ + "keytrack");
    Value* root_key = createBaseControl(prefix_ + "root_key");
    Value* transpose_quantize = createBaseControl(prefix_ + "transpose_quantize");
    Value* transpose_quantize_key = createBaseControl(prefix_ + "transpose_quantize_key");
    Value* transpose_quantize_scale = createBaseControl(prefix_ + "transpose_quantize_scale");
    Value* transpose_quantize_mode = createBaseControl(prefix_ + "transpose_quantize_mode");
    Value* mode = createBaseControl(prefix_ + "mode");
    Value* midi_density = createBaseControl(prefix_ + "midi_density");
    Value* grain_count = createBaseControl(prefix_ + "grain_count");
    Value* direction = createBaseControl(prefix_ + "direction");
    Output* start = createPolyModControl(prefix_ + "start");
    Output* end = createPolyModControl(prefix_ + "end");
    Output* density = createPolyModControl(prefix_ + "density");
    Output* grain_size = createPolyModControl(prefix_ + "grain_size");
    Output* speed = createPolyModControl(prefix_ + "speed");
    Output* position = createPolyModControl(prefix_ + "position", true, true);
    Output* position_mod = createPolyModControl(prefix_ + "position_mod", true, true);
    Output* position_mod_rate = createPolyModControl(prefix_ + "position_mod_rate", true, true);
    Output* random_position = createPolyModControl(prefix_ + "random_position");
    Output* random_volume = createPolyModControl(prefix_ + "random_volume");
    Output* random_pan = createPolyModControl(prefix_ + "random_pan");
    Output* random_pitch = createPolyModControl(prefix_ + "random_pitch");
    Output* interval = createPolyModControl(prefix_ + "interval");
    Output* interval_chance = createPolyModControl(prefix_ + "interval_chance");
    Output* low_cutoff = createPolyModControl(prefix_ + "low_cutoff", true, true);
    Output* high_cutoff = createPolyModControl(prefix_ + "high_cutoff", true, true);
    Output* transpose = createPolyModControl(prefix_ + "transpose");
    Output* tune = createPolyModControl(prefix_ + "tune");
    const std::string mix_prefix = slot_prefix_.empty() ? prefix_ : slot_prefix_;
    Output* level = createPolyModControl(mix_prefix + "level", true, true);
    Output* pan = createPolyModControl(mix_prefix + "pan");
    Value* window_shape = createBaseControl(prefix_ + "window_shape");
    Output* timing_jitter = createPolyModControl(prefix_ + "timing_jitter");
    Value* timing_mode = createBaseControl(prefix_ + "timing_mode");
    Value* grid_rate = createBaseControl(prefix_ + "grid_rate");
    Value* size_mode = createBaseControl(prefix_ + "size_mode");
    Output* grid_length = createPolyModControl(prefix_ + "grid_length");
    Value* position_grid = createBaseControl(prefix_ + "position_grid");

    granular_->useInput(input(kReset), GranularSource::kReset);
    granular_->useInput(input(kVoiceEvent), GranularSource::kVoiceEvent);
    granular_->useInput(input(kMidi), GranularSource::kMidi);
    granular_->useInput(input(kNoteCount), GranularSource::kNoteCount);
    granular_->plug(keytrack, GranularSource::kKeytrack);
    granular_->plug(root_key, GranularSource::kRootKey);
    granular_->plug(transpose, GranularSource::kTranspose);
    granular_->plug(transpose_quantize, GranularSource::kTransposeQuantize);
    granular_->plug(transpose_quantize_key, GranularSource::kTransposeQuantizeKey);
    granular_->plug(transpose_quantize_scale, GranularSource::kTransposeQuantizeScale);
    granular_->plug(transpose_quantize_mode, GranularSource::kTransposeQuantizeMode);
    granular_->plug(tune, GranularSource::kTune);
    granular_->plug(start, GranularSource::kStart);
    granular_->plug(end, GranularSource::kEnd);
    granular_->plug(mode, GranularSource::kMode);
    granular_->plug(midi_density, GranularSource::kMidiDensity);
    granular_->plug(grain_count, GranularSource::kGrainCount);
    granular_->plug(density, GranularSource::kDensity);
    granular_->plug(grain_size, GranularSource::kGrainSize);
    granular_->plug(speed, GranularSource::kSpeed);
    granular_->plug(position, GranularSource::kPosition);
    granular_->plug(position_mod, GranularSource::kPositionMod);
    granular_->plug(position_mod_rate, GranularSource::kPositionModRate);
    granular_->plug(random_position, GranularSource::kRandomPosition);
    granular_->plug(random_volume, GranularSource::kRandomVolume);
    granular_->plug(random_pan, GranularSource::kRandomPan);
    granular_->plug(random_pitch, GranularSource::kRandomPitch);
    granular_->plug(interval, GranularSource::kInterval);
    granular_->plug(interval_chance, GranularSource::kIntervalChance);
    granular_->plug(direction, GranularSource::kDirection);
    granular_->plug(low_cutoff, GranularSource::kLowCutoff);
    granular_->plug(high_cutoff, GranularSource::kHighCutoff);
    granular_->plug(level, GranularSource::kLevel);
    granular_->plug(pan, GranularSource::kPan);
    granular_->plug(window_shape, GranularSource::kWindowShape);
    granular_->plug(timing_jitter, GranularSource::kTimingJitter);
    granular_->plug(timing_mode, GranularSource::kTimingMode);
    granular_->plug(grid_rate, GranularSource::kGridRate);
    granular_->plug(size_mode, GranularSource::kSizeMode);
    granular_->plug(grid_length, GranularSource::kGridLength);
    granular_->plug(position_grid, GranularSource::kPositionGrid);
    if (beats_per_second_)
      granular_->plug(beats_per_second_, GranularSource::kBeatsPerSecond);
    else {
      // Standalone use (tests): 120 BPM.
      Value* default_tempo = new cr::Value(2.0f);
      addIdleProcessor(default_tempo);
      granular_->plug(default_tempo, GranularSource::kBeatsPerSecond);
    }
    if (!slot_prefix_.empty()) {
      // Slot-level FM/RM reads the slot's shared distortion controls.
      Value* distortion_type = createBaseControl(slot_prefix_ + "distortion_type");
      Output* distortion_amount = createPolyModControl(slot_prefix_ + "distortion_amount");
      Value* secondary_distortion_type = createBaseControl(slot_prefix_ + "secondary_distortion_type");
      Output* secondary_distortion_amount = createPolyModControl(slot_prefix_ + "secondary_distortion_amount");
      granular_->plug(distortion_type, GranularSource::kDistortionType);
      granular_->plug(distortion_amount, GranularSource::kDistortionAmount);
      granular_->plug(secondary_distortion_type, GranularSource::kSecondaryDistortionType);
      granular_->plug(secondary_distortion_amount, GranularSource::kSecondaryDistortionAmount);
    }
    granular_->useOutput(output(kRaw), GranularSource::kRaw);
    granular_->useOutput(output(kLevelled), GranularSource::kLevelled);

    addProcessor(granular_);
    SynthModule::init();
  }

} // namespace vital
