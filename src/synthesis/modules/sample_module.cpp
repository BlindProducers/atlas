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

#include "sample_module.h"

#include "synth_constants.h"

namespace vital {

  SampleModule::SampleModule(std::string prefix, std::shared_ptr<Sample> sample, std::string slot_prefix,
                             const Output* beats_per_second) :
      SynthModule(kNumInputs, kNumOutputs), prefix_(std::move(prefix)), slot_prefix_(std::move(slot_prefix)),
      beats_per_second_(beats_per_second) {
    sampler_ = new SampleSource(std::move(sample));
  }

  void SampleModule::updateStretchMemory() {
    if (mode_ == nullptr || stretch_mode_ == nullptr || slice_stretch_mode_ == nullptr)
      return;
    const bool slicer = static_cast<int>(mode_->value() + 0.5f) == SampleSource::kSlicer;
    const Value* stretch_mode = slicer ? slice_stretch_mode_ : stretch_mode_;
    sampler_->setStretchMemory(static_cast<int>(stretch_mode->value() + 0.5f) != 0);
  }

  void SampleModule::init() {
    Value* random_phase = createBaseControl(prefix_ + "random_phase");
    Value* loop = createBaseControl(prefix_ + "loop");
    Value* bounce = createBaseControl(prefix_ + "bounce");
    Value* reverse = createBaseControl(prefix_ + "reverse");
    Value* normalize = createBaseControl(prefix_ + "normalize");
    stretch_mode_ = createBaseControl(prefix_ + "stretch_mode");
    Value* formants = createBaseControl(prefix_ + "formants");
    mode_ = createBaseControl(prefix_ + "mode");
    Value* slice_loop = createBaseControl(prefix_ + "slice_loop");
    Value* play_through = createBaseControl(prefix_ + "play_through");
    slice_stretch_mode_ = createBaseControl(prefix_ + "slice_stretch_mode");
    // How the slices are made (SynthBase::resliceSample reads these); the
    // engine plays whatever slices the sample holds.
    createBaseControl(prefix_ + "slice_by");
    createBaseControl(prefix_ + "slice_divisions");
    createBaseControl(prefix_ + "slice_sensitivity");
    Value* playback_mode = createBaseControl(prefix_ + "playback_mode");
    Value* keytrack = createBaseControl(prefix_ + "keytrack");
    Value* filter_slope = createBaseControl(prefix_ + "filter_slope");
    Value* root_key = createBaseControl(prefix_ + "root_key");
    Value* transpose_quantize = createBaseControl(prefix_ + "transpose_quantize");
    Value* transpose_quantize_key = createBaseControl(prefix_ + "transpose_quantize_key");
    Value* transpose_quantize_scale = createBaseControl(prefix_ + "transpose_quantize_scale");
    Value* transpose_quantize_mode = createBaseControl(prefix_ + "transpose_quantize_mode");
    Output* start = createPolyModControl(prefix_ + "start");
    Output* end = createPolyModControl(prefix_ + "end");
    Output* loop_start = createPolyModControl(prefix_ + "loop_start");
    Output* loop_end = createPolyModControl(prefix_ + "loop_end");
    Output* loop_shift = createPolyModControl(prefix_ + "loop_shift");
    Output* loop_crossfade = createPolyModControl(prefix_ + "loop_crossfade");
    Output* low_cutoff = createPolyModControl(prefix_ + "low_cutoff", true, true);
    Output* high_cutoff = createPolyModControl(prefix_ + "high_cutoff", true, true);
    Output* transpose = createPolyModControl(prefix_ + "transpose");
    Output* tune = createPolyModControl(prefix_ + "tune");
    Output* bpm = createPolyModControl(prefix_ + "bpm");
    Output* stretch_length = createPolyModControl(prefix_ + "stretch_length");
    // Level and pan are slot controls shared by every type (osc_N_level, osc_N_pan).
    const std::string mix_prefix = slot_prefix_.empty() ? prefix_ : slot_prefix_;
    Output* level = createPolyModControl(mix_prefix + "level", true, true);
    Output* pan = createPolyModControl(mix_prefix + "pan");

    sampler_->useInput(input(kReset), SampleSource::kReset);
    sampler_->useInput(input(kRetrigger), SampleSource::kRetrigger);
    sampler_->useInput(input(kVoiceEvent), SampleSource::kVoiceEvent);
    sampler_->useInput(input(kMidi), SampleSource::kMidi);
    sampler_->useInput(input(kNoteCount), SampleSource::kNoteCount);
    sampler_->useInput(input(kNotePressed), SampleSource::kNotePressed);
    sampler_->useInput(input(kActiveVoices), SampleSource::kActiveVoices);
    if (beats_per_second_)
      sampler_->plug(beats_per_second_, SampleSource::kBeatsPerSecond);
    sampler_->plug(stretch_mode_, SampleSource::kStretchMode);
    sampler_->plug(bpm, SampleSource::kBpm);
    sampler_->plug(stretch_length, SampleSource::kStretchLength);
    sampler_->plug(formants, SampleSource::kFormants);
    sampler_->plug(mode_, SampleSource::kMode);
    sampler_->plug(slice_loop, SampleSource::kSliceLoop);
    sampler_->plug(play_through, SampleSource::kPlayThrough);
    sampler_->plug(slice_stretch_mode_, SampleSource::kSliceStretchMode);
    sampler_->plug(random_phase, SampleSource::kRandomPhase);
    sampler_->plug(keytrack, SampleSource::kKeytrack);
    sampler_->plug(root_key, SampleSource::kRootKey);
    sampler_->plug(loop, SampleSource::kLoop);
    sampler_->plug(bounce, SampleSource::kBounce);
    sampler_->plug(reverse, SampleSource::kReverse);
    sampler_->plug(normalize, SampleSource::kNormalize);
    sampler_->plug(playback_mode, SampleSource::kPlaybackMode);
    sampler_->plug(start, SampleSource::kStart);
    sampler_->plug(end, SampleSource::kEnd);
    sampler_->plug(loop_start, SampleSource::kLoopStart);
    sampler_->plug(loop_end, SampleSource::kLoopEnd);
    sampler_->plug(loop_shift, SampleSource::kLoopShift);
    sampler_->plug(loop_crossfade, SampleSource::kLoopCrossfade);
    sampler_->plug(low_cutoff, SampleSource::kLowCutoff);
    sampler_->plug(high_cutoff, SampleSource::kHighCutoff);
    sampler_->plug(filter_slope, SampleSource::kFilterSlope);
    sampler_->plug(transpose, SampleSource::kTranspose);
    sampler_->plug(transpose_quantize, SampleSource::kTransposeQuantize);
    sampler_->plug(transpose_quantize_key, SampleSource::kTransposeQuantizeKey);
    sampler_->plug(transpose_quantize_scale, SampleSource::kTransposeQuantizeScale);
    sampler_->plug(transpose_quantize_mode, SampleSource::kTransposeQuantizeMode);
    sampler_->plug(tune, SampleSource::kTune);
    sampler_->plug(level, SampleSource::kLevel);
    sampler_->plug(pan, SampleSource::kPan);
    if (!slot_prefix_.empty()) {
      // Slot-level FM/RM reads the slot's shared distortion controls.
      Value* distortion_type = createBaseControl(slot_prefix_ + "distortion_type");
      Output* distortion_amount = createPolyModControl(slot_prefix_ + "distortion_amount");
      Value* secondary_distortion_type = createBaseControl(slot_prefix_ + "secondary_distortion_type");
      Output* secondary_distortion_amount = createPolyModControl(slot_prefix_ + "secondary_distortion_amount");
      sampler_->plug(distortion_type, SampleSource::kDistortionType);
      sampler_->plug(distortion_amount, SampleSource::kDistortionAmount);
      sampler_->plug(secondary_distortion_type, SampleSource::kSecondaryDistortionType);
      sampler_->plug(secondary_distortion_amount, SampleSource::kSecondaryDistortionAmount);
    }
    sampler_->useOutput(output(kRaw), SampleSource::kRaw);
    sampler_->useOutput(output(kLevelled), SampleSource::kLevelled);

    addProcessor(sampler_);
    SynthModule::init();
  }

} // namespace vital
