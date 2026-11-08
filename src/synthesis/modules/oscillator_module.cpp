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

#include "oscillator_module.h"

#include "synth_oscillator.h"
#include "wavetable.h"

namespace vital {

  OscillatorModule::OscillatorModule(std::string prefix, std::shared_ptr<Wavetable> wavetable) :
      SynthModule(kNumInputs, kNumOutputs), prefix_(std::move(prefix)), distortion_type_(nullptr) {
    wavetable_ = wavetable ? std::move(wavetable) : std::make_shared<Wavetable>(kNumOscillatorWaveFrames);
  }

  void OscillatorModule::init() {
    oscillator_ = new SynthOscillator(wavetable_.get());

    createBaseControl(prefix_ + "_view_2d");
    Value* midi_track = createBaseControl(prefix_ + "_midi_track");
    Value* smooth_interpolation = createBaseControl(prefix_ + "_smooth_interpolation");
    Value* spectral_unison = createBaseControl(prefix_ + "_spectral_unison");
    Value* stack_style = createBaseControl(prefix_ + "_stack_style");
    Value* transpose_quantize = createBaseControl(prefix_ + "_transpose_quantize");
    Value* transpose_quantize_key = createBaseControl(prefix_ + "_transpose_quantize_key");
    Value* transpose_quantize_scale = createBaseControl(prefix_ + "_transpose_quantize_scale");
    Value* transpose_quantize_mode = createBaseControl(prefix_ + "_transpose_quantize_mode");
    Value* fixed_frequency_mode = createBaseControl(prefix_ + "_fixed_frequency_mode");

    Input* reset = input(kReset);

    Output* wave_frame = createPolyModControl(prefix_ + "_wave_frame");
    Output* transpose = createPolyModControl(prefix_ + "_transpose", true, false, nullptr, reset);
    Output* tune = createPolyModControl(prefix_ + "_tune", true, false, nullptr, reset);
    Output* fixed_frequency = createPolyModControl(prefix_ + "_fixed_frequency", true, false, nullptr, reset);
    Output* unison_voices = createPolyModControl(prefix_ + "_unison_voices");
    Output* unison_detune = createPolyModControl(prefix_ + "_unison_detune");
    Output* detune_power = createPolyModControl(prefix_ + "_detune_power");
    Output* detune_range = createPolyModControl(prefix_ + "_detune_range");
    Output* amplitude = createPolyModControl(prefix_ + "_level", true, true, nullptr, reset);
    Output* pan = createPolyModControl(prefix_ + "_pan");
    Output* phase = createPolyModControl(prefix_ + "_phase", true, true, nullptr, reset);
    Output* distortion_phase = createPolyModControl(prefix_ + "_distortion_phase");
    Output* rand_phase = createPolyModControl(prefix_ + "_random_phase");
    Output* blend = createPolyModControl(prefix_ + "_unison_blend");
    Output* stereo_spread = createPolyModControl(prefix_ + "_stereo_spread");
    Output* frame_spread = createPolyModControl(prefix_ + "_frame_spread");
    Output* distortion_spread = createPolyModControl(prefix_ + "_distortion_spread");
    distortion_type_ = createBaseControl(prefix_ + "_distortion_type");
    Output* distortion_amount = createPolyModControl(prefix_ + "_distortion_amount");
    Output* spectral_morph_spread = createPolyModControl(prefix_ + "_spectral_morph_spread");
    Value* spectral_morph_type = createBaseControl(prefix_ + "_spectral_morph_type");
    Value* spectral_morph_seed = createBaseControl(prefix_ + "_spectral_morph_seed");
    Output* spectral_morph_amount = createPolyModControl(prefix_ + "_spectral_morph_amount");

    secondary_distortion_type_ = createBaseControl(prefix_ + "_secondary_distortion_type");
    Output* secondary_distortion_amount = createPolyModControl(prefix_ + "_secondary_distortion_amount");
    Value* secondary_spectral_type = createBaseControl(prefix_ + "_secondary_spectral_morph_type");
    Value* secondary_spectral_seed = createBaseControl(prefix_ + "_secondary_spectral_morph_seed");
    Output* secondary_spectral_amount = createPolyModControl(prefix_ + "_secondary_spectral_morph_amount");
    Output* secondary_distortion_spread = createPolyModControl(prefix_ + "_secondary_distortion_spread");
    Output* secondary_spectral_spread = createPolyModControl(prefix_ + "_secondary_spectral_morph_spread");
    oscillator_->plug(secondary_distortion_spread, SynthOscillator::kSecondaryDistortionSpread);
    oscillator_->plug(secondary_spectral_spread, SynthOscillator::kSecondarySpectralMorphSpread);
    oscillator_->plug(secondary_distortion_type_, SynthOscillator::kSecondaryDistortionType);
    oscillator_->plug(secondary_distortion_amount, SynthOscillator::kSecondaryDistortionAmount);
    oscillator_->plug(secondary_spectral_type, SynthOscillator::kSecondarySpectralMorphType);
    oscillator_->plug(secondary_spectral_amount, SynthOscillator::kSecondarySpectralMorphAmount);
    oscillator_->plug(secondary_spectral_seed, SynthOscillator::kSecondarySpectralMorphSeed);

    Value* oscillator_type = createBaseControl(prefix_ + "_type");
    Value* analog_waveform = createBaseControl(prefix_ + "_analog_waveform");
    Output* analog_shape = createPolyModControl(prefix_ + "_analog_shape", true, true, nullptr, reset);
    Output* sub_level = createPolyModControl(prefix_ + "_sub_level", true, true, nullptr, reset);
    Value* sub_octave = createBaseControl(prefix_ + "_sub_octave");
    Value* sub_waveform = createBaseControl(prefix_ + "_sub_waveform");
    oscillator_->plug(oscillator_type, SynthOscillator::kOscillatorType);
    oscillator_->plug(analog_waveform, SynthOscillator::kAnalogWaveform);
    oscillator_->plug(analog_shape, SynthOscillator::kAnalogShape);
    oscillator_->plug(sub_level, SynthOscillator::kSubLevel);
    oscillator_->plug(sub_octave, SynthOscillator::kSubOctave);
    oscillator_->plug(sub_waveform, SynthOscillator::kSubWaveform);
    oscillator_->plug(createPolyModControl(prefix_ + "_analog_drift"), SynthOscillator::kAnalogDrift);
    oscillator_->plug(createPolyModControl(prefix_ + "_analog_voice_spread"), SynthOscillator::kAnalogVoiceSpread);
    oscillator_->plug(createPolyModControl(prefix_ + "_analog_wear"), SynthOscillator::kAnalogWear);
    oscillator_->plug(createPolyModControl(prefix_ + "_analog_noise"), SynthOscillator::kAnalogNoise);
    oscillator_->plug(createPolyModControl(prefix_ + "_analog_hum"), SynthOscillator::kAnalogHum);
    oscillator_->plug(createBaseControl(prefix_ + "_analog_hum_frequency"), SynthOscillator::kAnalogHumFrequency);
    oscillator_->plug(createPolyModControl(prefix_ + "_analog_sag"), SynthOscillator::kAnalogSag);
    oscillator_->plug(createBaseControl(prefix_ + "_random_phase_mode"), SynthOscillator::kRandomPhaseMode);
    oscillator_->plug(createBaseControl(prefix_ + "_random_phase_seed"), SynthOscillator::kRandomPhaseSeed);
    oscillator_->plug(createBaseControl(prefix_ + "_unison_remove_fundamental"),
                      SynthOscillator::kUnisonRemoveFundamental);
    oscillator_->setVoiceCardSalt(prefix_);

    oscillator_->useInput(input(kReset), SynthOscillator::kReset);
    oscillator_->useInput(input(kRetrigger), SynthOscillator::kRetrigger);
    oscillator_->useInput(input(kActiveVoices), SynthOscillator::kActiveVoices);
    oscillator_->useInput(input(kNotePressed), SynthOscillator::kNotePressed);
    oscillator_->useInput(input(kSoundingVoices), SynthOscillator::kSoundingVoices);
    oscillator_->useInput(input(kMidi), SynthOscillator::kMidiNote);
    oscillator_->plug(wave_frame, SynthOscillator::kWaveFrame);
    oscillator_->plug(midi_track, SynthOscillator::kMidiTrack);
    oscillator_->plug(smooth_interpolation, SynthOscillator::kSmoothlyInterpolate);
    oscillator_->plug(spectral_unison, SynthOscillator::kSpectralUnison);
    oscillator_->plug(transpose_quantize, SynthOscillator::kTransposeQuantize);
    oscillator_->plug(transpose_quantize_key, SynthOscillator::kTransposeQuantizeKey);
    oscillator_->plug(transpose_quantize_scale, SynthOscillator::kTransposeQuantizeScale);
    oscillator_->plug(transpose_quantize_mode, SynthOscillator::kTransposeQuantizeMode);
    oscillator_->plug(transpose, SynthOscillator::kTranspose);
    oscillator_->plug(tune, SynthOscillator::kTune);
    oscillator_->plug(fixed_frequency_mode, SynthOscillator::kFixedFrequencyMode);
    oscillator_->plug(fixed_frequency, SynthOscillator::kFixedFrequency);
    oscillator_->plug(stack_style, SynthOscillator::kStackStyle);
    oscillator_->plug(unison_detune, SynthOscillator::kUnisonDetune);
    oscillator_->plug(unison_voices, SynthOscillator::kUnisonVoices);
    oscillator_->plug(phase, SynthOscillator::kPhase);
    oscillator_->plug(distortion_phase, SynthOscillator::kDistortionPhase);
    oscillator_->plug(rand_phase, SynthOscillator::kRandomPhase);
    oscillator_->plug(blend, SynthOscillator::kBlend);
    oscillator_->plug(amplitude, SynthOscillator::kAmplitude);
    oscillator_->plug(pan, SynthOscillator::kPan);
    oscillator_->plug(detune_power, SynthOscillator::kDetunePower);
    oscillator_->plug(detune_range, SynthOscillator::kDetuneRange);
    oscillator_->plug(stereo_spread, SynthOscillator::kStereoSpread);
    oscillator_->plug(frame_spread, SynthOscillator::kUnisonFrameSpread);
    oscillator_->plug(distortion_spread, SynthOscillator::kUnisonDistortionSpread);
    oscillator_->plug(distortion_type_, SynthOscillator::kDistortionType);
    oscillator_->plug(distortion_amount, SynthOscillator::kDistortionAmount);
    oscillator_->plug(spectral_morph_spread, SynthOscillator::kUnisonSpectralMorphSpread);
    oscillator_->plug(spectral_morph_type, SynthOscillator::kSpectralMorphType);
    oscillator_->plug(spectral_morph_seed, SynthOscillator::kSpectralMorphSeed);
    oscillator_->plug(spectral_morph_amount, SynthOscillator::kSpectralMorphAmount);
    oscillator_->useOutput(output(kRaw), SynthOscillator::kRaw);
    oscillator_->useOutput(output(kLevelled), SynthOscillator::kLevelled);

    addProcessor(oscillator_);
    SynthModule::init();
  }

} // namespace vital
