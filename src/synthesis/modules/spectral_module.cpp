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

#include "spectral_module.h"

namespace vital {

  SpectralModule::SpectralModule(std::string prefix, std::shared_ptr<Sample> sample, std::string slot_prefix) :
      SynthModule(kNumInputs, kNumOutputs), prefix_(std::move(prefix)), slot_prefix_(std::move(slot_prefix)),
      spectral_(nullptr) {
    spectral_ = new SpectralSource(std::move(sample));
  }

  void SpectralModule::init() {
    Value* keytrack = createBaseControl(prefix_ + "keytrack");
    Value* root_key = createBaseControl(prefix_ + "root_key");
    Value* transpose_quantize = createBaseControl(prefix_ + "transpose_quantize");
    Value* transpose_quantize_key = createBaseControl(prefix_ + "transpose_quantize_key");
    Value* transpose_quantize_scale = createBaseControl(prefix_ + "transpose_quantize_scale");
    Value* transpose_quantize_mode = createBaseControl(prefix_ + "transpose_quantize_mode");
    Value* play_mode = createBaseControl(prefix_ + "play_mode");
    Value* freeze = createBaseControl(prefix_ + "freeze");
    Value* keep_fundamental = createBaseControl(prefix_ + "keep_fundamental");
    Value* phase_mode = createBaseControl(prefix_ + "phase_mode");
    Value* preserve_transients = createBaseControl(prefix_ + "preserve_transients");
    Value* spectral_effect = createBaseControl(prefix_ + "effect");
    // Level and pan are slot controls shared by every type; the slot smooths the
    // level and keeps pan at control rate like the wavetable oscillator.
    const std::string mix_prefix = slot_prefix_.empty() ? prefix_ : slot_prefix_;
    Output* level = createPolyModControl(mix_prefix + "level", true, true);
    Output* transpose = createPolyModControl(prefix_ + "transpose", true);
    Output* tune = createPolyModControl(prefix_ + "tune", true);
    Output* position = createPolyModControl(prefix_ + "position", true, true);
    Output* speed = createPolyModControl(prefix_ + "speed", true, true);
    Output* loop_start = createPolyModControl(prefix_ + "loop_start", true, true);
    Output* loop_end = createPolyModControl(prefix_ + "loop_end", true, true);
    Output* bin_low = createPolyModControl(prefix_ + "bin_low", true, true);
    Output* bin_high = createPolyModControl(prefix_ + "bin_high", true, true);
    Output* spectral_shift = createPolyModControl(prefix_ + "shift", true, true);
    Output* spectral_stretch = createPolyModControl(prefix_ + "stretch", true, true);
    Output* spectral_blur = createPolyModControl(prefix_ + "blur", true, true);
    Output* spectral_tilt = createPolyModControl(prefix_ + "tilt", true, true);
    Output* phase_randomize = createPolyModControl(prefix_ + "phase_randomize", true, true);
    Output* effect_amount = createPolyModControl(prefix_ + "effect_amount", true, true);
    Output* pan = slot_prefix_.empty() ? createPolyModControl(prefix_ + "pan", true, true)
                                       : createPolyModControl(mix_prefix + "pan");

    spectral_->useInput(input(kReset), SpectralSource::kReset);
    spectral_->useInput(input(kRetrigger), SpectralSource::kRetrigger);
    spectral_->useInput(input(kMidi), SpectralSource::kMidi);
    spectral_->useInput(input(kNotePressed), SpectralSource::kNotePressed);
    spectral_->useInput(input(kActiveVoices), SpectralSource::kActiveVoices);
    spectral_->plug(keytrack, SpectralSource::kKeytrack);
    spectral_->plug(root_key, SpectralSource::kRootKey);
    spectral_->plug(level, SpectralSource::kLevel);
    spectral_->plug(transpose, SpectralSource::kTranspose);
    spectral_->plug(transpose_quantize, SpectralSource::kTransposeQuantize);
    spectral_->plug(transpose_quantize_key, SpectralSource::kTransposeQuantizeKey);
    spectral_->plug(transpose_quantize_scale, SpectralSource::kTransposeQuantizeScale);
    spectral_->plug(transpose_quantize_mode, SpectralSource::kTransposeQuantizeMode);
    spectral_->plug(tune, SpectralSource::kTune);
    spectral_->plug(play_mode, SpectralSource::kPlayMode);
    spectral_->plug(position, SpectralSource::kPosition);
    spectral_->plug(speed, SpectralSource::kSpeed);
    spectral_->plug(loop_start, SpectralSource::kLoopStart);
    spectral_->plug(loop_end, SpectralSource::kLoopEnd);
    spectral_->plug(bin_low, SpectralSource::kBinLow);
    spectral_->plug(bin_high, SpectralSource::kBinHigh);
    spectral_->plug(spectral_shift, SpectralSource::kSpectralShift);
    spectral_->plug(spectral_stretch, SpectralSource::kSpectralStretch);
    spectral_->plug(spectral_blur, SpectralSource::kSpectralBlur);
    spectral_->plug(spectral_tilt, SpectralSource::kSpectralTilt);
    spectral_->plug(phase_randomize, SpectralSource::kPhaseRandomize);
    spectral_->plug(freeze, SpectralSource::kFreeze);
    spectral_->plug(keep_fundamental, SpectralSource::kKeepFundamental);
    spectral_->plug(phase_mode, SpectralSource::kPhaseMode);
    spectral_->plug(preserve_transients, SpectralSource::kPreserveTransients);
    spectral_->plug(spectral_effect, SpectralSource::kSpectralEffect);
    spectral_->plug(effect_amount, SpectralSource::kEffectAmount);
    spectral_->plug(pan, SpectralSource::kPan);
    if (!slot_prefix_.empty()) {
      // Slot-level FM/RM reads the slot's shared distortion controls.
      Value* distortion_type = createBaseControl(slot_prefix_ + "distortion_type");
      Output* distortion_amount = createPolyModControl(slot_prefix_ + "distortion_amount");
      Value* secondary_distortion_type = createBaseControl(slot_prefix_ + "secondary_distortion_type");
      Output* secondary_distortion_amount = createPolyModControl(slot_prefix_ + "secondary_distortion_amount");
      spectral_->plug(distortion_type, SpectralSource::kDistortionType);
      spectral_->plug(distortion_amount, SpectralSource::kDistortionAmount);
      spectral_->plug(secondary_distortion_type, SpectralSource::kSecondaryDistortionType);
      spectral_->plug(secondary_distortion_amount, SpectralSource::kSecondaryDistortionAmount);
    }
    spectral_->useOutput(output(kRaw), SpectralSource::kRaw);
    spectral_->useOutput(output(kLevelled), SpectralSource::kLevelled);

    addProcessor(spectral_);
    SynthModule::init();
  }

} // namespace vital
