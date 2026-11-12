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

#include "distortion_module.h"

#include "distortion.h"
#include "digital_svf.h"

#include <algorithm>
#include <cmath>

namespace vital {

  namespace {
    namespace modes = atlas::distortion_modes;
    // The script's Mode for each Type from Tube on.
    constexpr int kJsfxModes[Distortion::kNumEffectTypes - Distortion::kNumTypes] = {
      modes::kTube, modes::kDiode1, modes::kDiode2, modes::kZeroSquare, modes::kAsym,
      modes::kStompBox, modes::kTapeSat, modes::kSoftSat };
  }

  DistortionModule::DistortionModule(std::string prefix) :
      SynthModule(0, 1), distortion_(nullptr), prefix_(prefix), distortion_type_(nullptr), filter_order_(nullptr),
      filter_(nullptr), distortion_mix_(nullptr), jsfx_drive_(nullptr), jsfx_level_(nullptr), mix_(0.0f),
      jsfx_active_(false) { }

  DistortionModule::~DistortionModule() {
  }

  void DistortionModule::init() {
    distortion_ = new Distortion();
    distortion_->useOutput(output());
    addIdleProcessor(distortion_);

    distortion_type_ = createBaseControl(prefix_ + "distortion_type");
    Output* distortion_drive = createMonoModControl(prefix_ + "distortion_drive", true, true);
    distortion_mix_ = createMonoModControl(prefix_ + "distortion_mix");
    jsfx_drive_ = createMonoModControl(prefix_ + "distortion_jsfx_drive");
    jsfx_level_ = createMonoModControl(prefix_ + "distortion_jsfx_level");

    distortion_->plug(distortion_type_, Distortion::kType);
    distortion_->plug(distortion_drive, Distortion::kDrive);

    filter_order_ = createBaseControl(prefix_ + "distortion_filter_order");
    Output* midi_cutoff = createMonoModControl(prefix_ + "distortion_filter_cutoff", true, true);
    Output* resonance = createMonoModControl(prefix_ + "distortion_filter_resonance");
    Output* blend = createMonoModControl(prefix_ + "distortion_filter_blend");

    filter_ = new DigitalSvf();
    filter_->useOutput(output());
    filter_->plug(midi_cutoff, DigitalSvf::kMidiCutoff);
    filter_->plug(resonance, DigitalSvf::kResonance);
    filter_->plug(blend, DigitalSvf::kPassBlend);
    filter_->setDriveCompensation(false);
    filter_->setBasic(true);
    addIdleProcessor(filter_);

    SynthModule::init();
  }

  void DistortionModule::setSampleRate(int sample_rate) {
    SynthModule::setSampleRate(sample_rate);
    distortion_->setSampleRate(sample_rate);
    filter_->setSampleRate(sample_rate);
    jsfx_active_ = false;
  }

  void DistortionModule::setOversampleAmount(int oversample) {
    SynthModule::setOversampleAmount(oversample);
    jsfx_active_ = false;
  }

  void DistortionModule::hardReset() {
    SynthModule::hardReset();
    jsfx_active_ = false;
  }

  void DistortionModule::processJsfxMode(int type, const poly_float* audio_in, int num_samples) {
    // Controls are read once per block, when the script's @slider would run.
    modes::Engine::Parameters parameters;
    parameters.mode = kJsfxModes[type - Distortion::kNumTypes];
    parameters.drive = std::clamp(static_cast<double>(jsfx_drive_->buffer[0][0]), 0.0, 100.0);
    parameters.mix = 100.0 * std::clamp(static_cast<double>(distortion_mix_->buffer[0][0]), 0.0, 1.0);
    parameters.level = std::clamp(static_cast<double>(jsfx_level_->buffer[0][0]), 0.0, 1.0);
    // Coming from a Type before Tube, or after a reset, the script starts
    // over in its new mode; between its own modes it crossfades.
    if (!jsfx_active_) {
      jsfx_.setParameters(parameters);
      jsfx_.prepare(getSampleRate());
      jsfx_active_ = true;
    }
    else if (parameters != jsfx_.parameters())
      jsfx_.setParameters(parameters);
    jsfx_.block(num_samples);

    // Lanes 2-3 repeat lanes 0-1, so one stereo pass serves both. Atlas's
    // filter takes the script's filter's place: Pre shapes the filtered
    // input, Post filters the wet signal before the mix.
    poly_float* audio_out = output()->buffer;
    const float order = filter_order_->output()->buffer[0][0];
    if (order > 1.0f) {
      for (int i = 0; i < num_samples; ++i) {
        double wl, wr;
        jsfx_.wet(audio_in[i][0], audio_in[i][1], audio_in[i][0], audio_in[i][1], wl, wr);
        const mono_float wet_left = static_cast<mono_float>(wl), wet_right = static_cast<mono_float>(wr);
        audio_out[i] = poly_float(wet_left, wet_right, wet_left, wet_right);
      }
      filter_->processWithInput(audio_out, num_samples);
    }
    else if (order >= 1.0f)
      filter_->processWithInput(audio_in, num_samples);

    for (int i = 0; i < num_samples; ++i) {
      const double spl0 = audio_in[i][0], spl1 = audio_in[i][1];
      double wl, wr;
      if (order > 1.0f) {
        wl = audio_out[i][0]; wr = audio_out[i][1];
      }
      else if (order >= 1.0f)
        jsfx_.wet(spl0, spl1, audio_out[i][0], audio_out[i][1], wl, wr);
      else
        jsfx_.wet(spl0, spl1, spl0, spl1, wl, wr);
      double left, right;
      jsfx_.output(spl0, spl1, wl, wr, left, right);
      const mono_float out_left = static_cast<mono_float>(left), out_right = static_cast<mono_float>(right);
      audio_out[i] = poly_float(out_left, out_right, out_left, out_right);
    }
  }

  void DistortionModule::processWithInput(const poly_float* audio_in, int num_samples) {
    SynthModule::process(num_samples);

    const int type = std::clamp(static_cast<int>(std::lround(distortion_type_->value())), 0,
                                Distortion::kNumEffectTypes - 1);
    if (type >= Distortion::kNumTypes) {
      processJsfxMode(type, audio_in, num_samples);
      // A later Type before Tube mixes from the current Mix.
      mix_ = utils::clamp(distortion_mix_->buffer[0], 0.0f, 1.0f);
      return;
    }
    jsfx_active_ = false;

    if (filter_order_->output()->buffer[0][0] < 1.0f)
      distortion_->processWithInput(audio_in, num_samples);
    else if (filter_order_->output()->buffer[0][0] > 1.0f) {
      distortion_->processWithInput(audio_in, num_samples);
      filter_->processWithInput(output()->buffer, num_samples);
    }
    else {
      filter_->processWithInput(audio_in, num_samples);
      distortion_->processWithInput(output()->buffer, num_samples);
    }
    
    poly_float current_mix = mix_;
    mix_ = utils::clamp(distortion_mix_->buffer[0], 0.0f, 1.0f);
    poly_float delta_mix = (mix_ - current_mix) * (1.0f / num_samples);
    poly_float* audio_out = output()->buffer;
    
    for (int i = 0; i < num_samples; ++i) {
      current_mix += delta_mix;
      audio_out[i] = utils::interpolate(audio_in[i], audio_out[i], current_mix);
    }
  }
} // namespace vital
