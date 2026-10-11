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

#include "modulation_connection_processor.h"
#include "futils.h"

namespace vital {

  namespace {
    constexpr mono_float kExpressionSmoothingCutoff = 20.0f;
  }

  ModulationConnectionProcessor::ModulationConnectionProcessor(int index) :
      SynthModule(kNumInputs, kNumOutputs), index_(index), polyphonic_(true), collapse_polyphonic_source_(false),
      process_when_idle_(false), refresh_source_when_idle_(false),
      source_smoothing_(std::make_shared<SourceSmoothingState>()), run_by_producers_(std::make_shared<bool>(false)),
      source_smoothing_revision_(0),
      source_smoothing_initialized_(false), current_value_(nullptr), bipolar_(nullptr), stereo_(nullptr),
      bypass_(nullptr), random_mode_(nullptr), smoothed_source_value_(0.0f), collapsed_output_value_(0.0f),
      collapsed_handoff_correction_(0.0f), collapsed_handoff_samples_left_(0),
      collapsed_output_initialized_(false) {
    setControlRate(true);

    modulation_amount_ = 0.0f;

    destination_scale_ = std::make_shared<mono_float>();
    *destination_scale_ = 0.0f;
    last_destination_scale_ = 0.0f;

    power_ = 0.0f;

    map_generator_ = std::make_shared<LineGenerator>();
    map_generator_->initLinear();
  }

  void ModulationConnectionProcessor::init() {
    std::string bipolar_name = "modulation_" + std::to_string(index_ + 1) + "_bipolar";
    bipolar_ = createBaseControl(bipolar_name);

    std::string stereo_name = "modulation_" + std::to_string(index_ + 1) + "_stereo";
    stereo_ = createBaseControl(stereo_name);

    std::string bypass_name = "modulation_" + std::to_string(index_ + 1) + "_bypass";
    bypass_ = createBaseControl(bypass_name);

    std::string random_mode_name = "modulation_" + std::to_string(index_ + 1) + "_random_mode";
    random_mode_ = createBaseControl(random_mode_name);

    SynthModule::init();
  }

  void ModulationConnectionProcessor::process(int num_samples) {
    if (!*run_by_producers_)
      processRoute(num_samples);
  }

  void ModulationConnectionProcessor::processRoute(int num_samples) {
    const unsigned smoothing_revision = source_smoothing_->revision.load(std::memory_order_acquire);
    if (source_smoothing_revision_ != smoothing_revision) {
      source_smoothing_revision_ = smoothing_revision;
      source_smoothing_initialized_ = false;
    }

    const Output* source = input(kModulationInput)->source;
    const Output* independent_source = input(kIndependentRandomInput)->source;
    if (random_mode_->value() >= 0.5f && independent_source->owner != nullptr)
      source = independent_source;
    poly_float modulation_input = source->trigger_value;
    output(kModulationSource)->buffer[0] = modulation_input;

    if (last_destination_scale_ != *destination_scale_)
      modulation_amount_ = 0.0f;
    last_destination_scale_ = *destination_scale_;

    const bool smooth_source = sourceSmoothing();
    if (smooth_source && !isControlRate() && source->isControlRate())
      processSmoothedControlSourceAudioRate(num_samples, source);
    else if (isControlRate() || source->isControlRate())
      processControlRate(num_samples, source, smooth_source);
    else 
      processAudioRate(num_samples, source);
  }

  void ModulationConnectionProcessor::collapseToVoice(poly_mask voice_mask, int num_samples,
                                                       int handoff_sample, const void* voice_identity) {
    if (!voice_mask.anyMask())
      return;

    const int samples = isControlRate() ? 1 : std::max(1, num_samples);
    const int handoff_samples = std::max(1, static_cast<int>(
        std::round(kModulationHandoffTime * getSampleRate())));
    poly_float* buffer = output(kModulationOutput)->buffer;

    // A voice-triggered LFO or envelope can begin at a very different value
    // from the voice that previously controlled a mono FX destination. Surge
    // avoids the corresponding coefficient step with a short interpolated
    // handoff. Apply that interpolation only when a new voice takes ownership;
    // ordinary audio-rate modulation remains sample-exact.
    int transition = handoff_sample;
    // A release can return ownership to an older voice without a note-on.
    // Compare voice identity, not SIMD lane: different voice groups reuse lanes.
    if (voice_identity != nullptr && collapsed_voice_identity_ != nullptr &&
        voice_identity != collapsed_voice_identity_ && transition < 0)
      transition = 0;
    collapsed_voice_identity_ = voice_identity;
    if (!collapsed_output_initialized_)
      transition = -1;
    else if (transition >= 0) {
      transition = utils::iclamp(transition, 0, samples - 1);
      for (int sample = 0; sample < transition; ++sample)
        buffer[sample] = collapsed_output_value_;

      const poly_float selected = buffer[transition] & voice_mask;
      const poly_float new_value = selected + utils::swapVoices(selected);
      collapsed_handoff_correction_ = collapsed_output_value_ - new_value;
      collapsed_handoff_samples_left_ = handoff_samples;
    }

    for (int sample = 0; sample < samples; ++sample) {
      if (transition >= 0 && sample < transition)
        continue;

      const poly_float selected = buffer[sample] & voice_mask;
      // SIMD lanes are [voice 0 L/R, voice 1 L/R]. Replicate the selected
      // voice pair rather than choosing the numerically largest lane, which
      // would discard stereo modulation and could select stale voice state.
      poly_float collapsed = selected + utils::swapVoices(selected);
      if (collapsed_handoff_samples_left_ > 0) {
        // Control-rate destinations interpolate to this block's endpoint;
        // audio-rate destinations retain the exact first-sample handoff.
        const int advance = isControlRate() ? std::max(1, num_samples) : 1;
        const int remaining = isControlRate() ? std::max(0, collapsed_handoff_samples_left_ - advance)
                                             : collapsed_handoff_samples_left_;
        const mono_float fraction = remaining /
                                    static_cast<mono_float>(handoff_samples);
        collapsed += collapsed_handoff_correction_ * fraction;
        collapsed_handoff_samples_left_ = std::max(0, collapsed_handoff_samples_left_ - advance);
        if (collapsed_handoff_samples_left_ == 0)
          collapsed_handoff_correction_ = 0.0f;
      }
      buffer[sample] = collapsed;
    }

    collapsed_output_value_ = buffer[samples - 1];
    collapsed_output_initialized_ = true;
    output(kModulationOutput)->trigger_value = buffer[0];
  }

  void ModulationConnectionProcessor::holdCollapsedOutput(int num_samples) {
    const int samples = isControlRate() ? 1 : std::max(1, num_samples);
    poly_float* buffer = output(kModulationOutput)->buffer;
    for (int sample = 0; sample < samples; ++sample)
      buffer[sample] = collapsed_output_value_;
    output(kModulationOutput)->trigger_value = collapsed_output_value_;
  }

  void ModulationConnectionProcessor::processAudioRate(int num_samples, const Output* source) {
    if (bypass_->value()) {
      output(kModulationOutput)->clearBuffer();
      output(kModulationOutput)->trigger_value = 0.0f;
      return;
    }

    poly_float power = -input(kModulationPower)->at(0);
    bool using_power = (poly_float::notEqual(0.0f, power) | poly_float::notEqual(0.0f, power_)).anyMask();
    bool using_map = !map_generator_->linear();

    if (using_power && using_map)
      processAudioRateRemappedAndMorphed(num_samples, source, power);
    else if (using_power)
      processAudioRateMorphed(num_samples, source, power);
    else if (using_map)
      processAudioRateRemapped(num_samples, source);
    else
      processAudioRateLinear(num_samples, source);

    power_ = power;
  }

  void ModulationConnectionProcessor::processAudioRateLinear(int num_samples, const Output* source) {
    poly_float* dest = output(kModulationOutput)->buffer;
    const poly_float* modulation_source = source->buffer;

    poly_float bipolar_offset = -bipolar_->value() * 0.5f;
    poly_float current_amount = modulation_amount_;
    poly_float stereo_scale = poly_float(1.0f) - (constants::kRightOne * 2.0f * stereo_->value());
    poly_float modulation_amount = utils::clamp(input(kModulationAmount)->at(0), -1.0f, 1.0f) * stereo_scale;
    modulation_amount_ = modulation_amount * (*destination_scale_);
    current_amount = utils::maskLoad(current_amount, modulation_amount_, getResetMask(kReset));
    poly_float delta_amount = (modulation_amount_ - current_amount) * (1.0f / num_samples);

    for (int i = 0; i < num_samples; ++i) {
      current_amount += delta_amount;
      poly_float modulation_value = modulation_source[i];
      dest[i] = (modulation_value + bipolar_offset) * current_amount;
    }

    output(kModulationPreScale)->buffer[0] = (modulation_source[0] + bipolar_offset) * modulation_amount;
    output(kModulationOutput)->trigger_value = dest[0];
  }

  void ModulationConnectionProcessor::processAudioRateMorphed(int num_samples, const Output* source,
                                                              poly_float power) {
    poly_float* dest = output(kModulationOutput)->buffer;
    const poly_float* modulation_source = source->buffer;
    poly_float bipolar_offset = -bipolar_->value() * 0.5f;

    poly_float bipolar = bipolar_->value();
    poly_float polarity_pre_scale = bipolar + 1.0f;
    poly_float polarity_post_scale = -bipolar * 0.5f + 1.0f;
    polarity_post_scale *= poly_float(1.0f) - (constants::kRightOne * 2.0f * stereo_->value());

    poly_float current_amount = modulation_amount_;
    poly_float current_power = power_;

    poly_float modulation_amount = utils::clamp(input(kModulationAmount)->at(0), -1.0f, 1.0f);
    modulation_amount_ = modulation_amount * (*destination_scale_);

    poly_mask reset_mask = getResetMask(kReset);
    current_amount = utils::maskLoad(current_amount, modulation_amount_, reset_mask);
    current_power = utils::maskLoad(current_power, power, reset_mask);

    float sample_inc = 1.0f / num_samples;
    poly_float delta_amount = (modulation_amount_ - current_amount) * sample_inc;
    poly_float delta_power = (power - current_power) * sample_inc;

    for (int i = 0; i < num_samples; ++i) {
      current_amount += delta_amount;
      current_power += delta_power;

      poly_float modulation_value = modulation_source[i];
      poly_float modulation_shift = modulation_value * polarity_pre_scale - bipolar;
      poly_float modulation_abs = poly_float::abs(modulation_shift);
      poly_mask sign_mask = poly_float::sign_mask(modulation_shift);

      poly_float shifted_modulation = futils::powerScale(modulation_abs, current_power);
      poly_float pre_modulation = current_amount * shifted_modulation;
      dest[i] = (pre_modulation ^ sign_mask) * polarity_post_scale;
    }

    output(kModulationPreScale)->buffer[0] = dest[0] * (1.0f / (*destination_scale_));
    output(kModulationOutput)->trigger_value = dest[0];
  }

  void ModulationConnectionProcessor::processAudioRateRemappedAndMorphed(int num_samples, const Output* source,
                                                                         poly_float power) {
    poly_float* dest = output(kModulationOutput)->buffer;
    const poly_float* modulation_source = source->buffer;
    poly_float bipolar_offset = -bipolar_->value() * 0.5f;

    poly_float bipolar = bipolar_->value();
    poly_float polarity_pre_scale = bipolar + 1.0f;
    poly_float polarity_post_scale = -bipolar * 0.5f + 1.0f;
    polarity_post_scale *= poly_float(1.0f) - (constants::kRightOne * 2.0f * stereo_->value());

    poly_float current_amount = modulation_amount_;
    poly_float current_power = power_;

    poly_float modulation_amount = utils::clamp(input(kModulationAmount)->at(0), -1.0f, 1.0f);
    modulation_amount_ = modulation_amount * (*destination_scale_);

    poly_mask reset_mask = getResetMask(kReset);
    current_amount = utils::maskLoad(current_amount, modulation_amount_, reset_mask);
    current_power = utils::maskLoad(current_power, power, reset_mask);

    float sample_inc = 1.0f / num_samples;
    poly_float delta_amount = (modulation_amount_ - current_amount) * sample_inc;
    poly_float delta_power = (power - current_power) * sample_inc;

    auto buffer_read = map_generator_->acquireBuffer();
    mono_float* buffer = buffer_read.get();
    for (int i = 0; i < num_samples; ++i) {
      current_amount += delta_amount;
      current_power += delta_power;
      poly_float modulation_value = modulation_source[i];

      mono_float resolution = map_generator_->resolution();
      poly_float boost = utils::clamp(modulation_value * resolution, 0.0f, resolution);
      poly_int indices = utils::clamp(utils::toInt(boost), 0, resolution - 1);
      poly_float t = boost - utils::toFloat(indices);

      matrix interpolation_matrix = utils::getCatmullInterpolationMatrix(t);
      matrix value_matrix = utils::getValueMatrix(buffer, indices);

      value_matrix.transpose();
      modulation_value = utils::clamp(interpolation_matrix.multiplyAndSumRows(value_matrix), -1.0f, 1.0f);

      poly_float modulation_shift = modulation_value * polarity_pre_scale - bipolar;
      poly_float modulation_abs = poly_float::abs(modulation_shift);
      poly_mask sign_mask = poly_float::sign_mask(modulation_shift);

      poly_float shifted_modulation = futils::powerScale(modulation_abs, current_power);
      poly_float pre_modulation = current_amount * shifted_modulation;
      dest[i] = (pre_modulation ^ sign_mask) * polarity_post_scale;
    }

    output(kModulationPreScale)->buffer[0] = dest[0] * (1.0f / (*destination_scale_));
    output(kModulationOutput)->trigger_value = dest[0];
  }

  void ModulationConnectionProcessor::processAudioRateRemapped(int num_samples, const Output* source) {
    poly_float* dest = output(kModulationOutput)->buffer;
    const poly_float* modulation_source = source->buffer;

    poly_float bipolar_offset = -bipolar_->value() * 0.5f;
    poly_float current_amount = modulation_amount_;
    poly_float stereo_scale = poly_float(1.0f) - (constants::kRightOne * 2.0f * stereo_->value());
    poly_float modulation_amount = utils::clamp(input(kModulationAmount)->at(0), -1.0f, 1.0f) * stereo_scale;
    modulation_amount_ = modulation_amount * (*destination_scale_);
    current_amount = utils::maskLoad(current_amount, modulation_amount_, getResetMask(kReset));
    poly_float delta_amount = (modulation_amount_ - current_amount) * (1.0f / num_samples);

    auto buffer_read = map_generator_->acquireBuffer();
    mono_float* buffer = buffer_read.get();
    for (int i = 0; i < num_samples; ++i) {
      current_amount += delta_amount;
      poly_float modulation_value = modulation_source[i];

      mono_float resolution = map_generator_->resolution();
      poly_float boost = utils::clamp(modulation_value * resolution, 0.0f, resolution);
      poly_int indices = utils::clamp(utils::toInt(boost), 0, resolution - 1);
      poly_float t = boost - utils::toFloat(indices);

      matrix interpolation_matrix = utils::getCatmullInterpolationMatrix(t);
      matrix value_matrix = utils::getValueMatrix(buffer, indices);

      value_matrix.transpose();

      modulation_value = utils::clamp(interpolation_matrix.multiplyAndSumRows(value_matrix), -1.0f, 1.0f);
      dest[i] = (modulation_value + bipolar_offset) * current_amount;
    }

    output(kModulationPreScale)->buffer[0] = (modulation_source[0] + bipolar_offset) * modulation_amount;
    output(kModulationOutput)->trigger_value = dest[0];
  }

  void ModulationConnectionProcessor::processSmoothedControlSourceAudioRate(int num_samples,
                                                                             const Output* source) {
    const poly_float target_source = utils::clamp(source->buffer[0], 0.0f, 1.0f);
    if (!source_smoothing_initialized_) {
      smoothed_source_value_ = target_source;
      source_smoothing_initialized_ = true;
    }

    const mono_float decay = futils::exp(-2.0f * kPi * kExpressionSmoothingCutoff / getSampleRate());
    if (bypass_->value()) {
      const mono_float block_decay = futils::exp(-2.0f * kPi * kExpressionSmoothingCutoff *
                                                  num_samples / getSampleRate());
      smoothed_source_value_ = utils::interpolate(target_source, smoothed_source_value_, block_decay);
      output(kModulationPreScale)->clearBuffer();
      output(kModulationOutput)->clearBuffer();
      output(kModulationPreScale)->trigger_value = 0.0f;
      output(kModulationOutput)->trigger_value = 0.0f;
      return;
    }

    const poly_float bipolar = bipolar_->value();
    const poly_float polarity_pre_scale = bipolar + 1.0f;
    poly_float polarity_post_scale = -bipolar * 0.5f + 1.0f;
    polarity_post_scale *= poly_float(1.0f) - (constants::kRightOne * 2.0f * stereo_->value());

    const poly_float target_power = -input(kModulationPower)->at(0);
    poly_float current_power = power_;
    const poly_float target_amount = utils::clamp(input(kModulationAmount)->at(0), -1.0f, 1.0f) *
                                     (*destination_scale_);
    poly_float current_amount = modulation_amount_;
    const poly_mask reset_mask = getResetMask(kReset);
    current_power = utils::maskLoad(current_power, target_power, reset_mask);
    current_amount = utils::maskLoad(current_amount, target_amount, reset_mask);

    const mono_float sample_increment = 1.0f / num_samples;
    const poly_float power_delta = (target_power - current_power) * sample_increment;
    const poly_float amount_delta = (target_amount - current_amount) * sample_increment;
    const bool using_map = !map_generator_->linear();
    LineGenerator::BufferRead map_buffer_read;
    if (using_map)
      map_buffer_read = map_generator_->acquireBuffer();
    mono_float* map_buffer = map_buffer_read.get();
    const mono_float map_resolution = using_map ? map_generator_->resolution() : 0.0f;
    poly_float current_source = smoothed_source_value_;
    poly_float* destination = output(kModulationOutput)->buffer;
    poly_float first_pre_scale = 0.0f;

    for (int i = 0; i < num_samples; ++i) {
      current_source = utils::interpolate(target_source, current_source, decay);
      current_power += power_delta;
      current_amount += amount_delta;

      poly_float modulation_value = current_source;
      if (using_map) {
        const poly_float boost = utils::clamp(modulation_value * map_resolution, 0.0f, map_resolution);
        const poly_int indices = utils::clamp(utils::toInt(boost), 0, map_resolution - 1);
        const poly_float t = boost - utils::toFloat(indices);
        matrix interpolation_matrix = utils::getCatmullInterpolationMatrix(t);
        matrix value_matrix = utils::getValueMatrix(map_buffer, indices);
        value_matrix.transpose();
        modulation_value = utils::clamp(interpolation_matrix.multiplyAndSumRows(value_matrix), -1.0f, 1.0f);
      }

      const poly_float modulation_shift = modulation_value * polarity_pre_scale - bipolar;
      const poly_float shifted_modulation = futils::powerScale(poly_float::abs(modulation_shift), current_power);
      const poly_float scaled_modulation = (current_amount * shifted_modulation ^
                                            poly_float::sign_mask(modulation_shift)) * polarity_post_scale;
      destination[i] = scaled_modulation;
      if (i == 0) {
        if (*destination_scale_ != 0.0f)
          first_pre_scale = scaled_modulation * (1.0f / *destination_scale_);
      }
    }

    smoothed_source_value_ = current_source;
    power_ = target_power;
    modulation_amount_ = target_amount;
    output(kModulationPreScale)->buffer[0] = first_pre_scale;
    output(kModulationPreScale)->trigger_value = first_pre_scale;
    output(kModulationOutput)->trigger_value = destination[0];
    VITAL_ASSERT(utils::isFinite(destination[0]));
  }

  void ModulationConnectionProcessor::processControlRate(int num_samples, const Output* source,
                                                        bool smooth_source) {
    poly_float modulation_input = utils::clamp(source->buffer[0], 0.0f, 1.0f);
    if (smooth_source) {
      if (!source_smoothing_initialized_) {
        smoothed_source_value_ = modulation_input;
        source_smoothing_initialized_ = true;
      }
      else {
        const int smoothing_samples = num_samples > 0 ? num_samples : 1;
        const mono_float decay = futils::exp(-2.0f * kPi * kExpressionSmoothingCutoff *
                                             smoothing_samples / getSampleRate());
        smoothed_source_value_ = utils::interpolate(modulation_input, smoothed_source_value_, decay);
      }
      modulation_input = smoothed_source_value_;
    }

    if (bypass_->value()) {
      output(kModulationOutput)->buffer[0] = 0.0f;
      output(kModulationOutput)->trigger_value = 0.0f;
      return;
    }

    if (!map_generator_->linear()) {
      auto buffer_read = map_generator_->acquireBuffer();
      mono_float* buffer = buffer_read.get();
      mono_float resolution = map_generator_->resolution();
      poly_float boost = utils::clamp(modulation_input * resolution, 0.0f, resolution);
      poly_int indices = utils::clamp(utils::toInt(boost), 0, resolution - 1);
      poly_float t = boost - utils::toFloat(indices);

      matrix interpolation_matrix = utils::getCatmullInterpolationMatrix(t);
      matrix value_matrix = utils::getValueMatrix(buffer, indices);

      value_matrix.transpose();

      modulation_input = utils::clamp(interpolation_matrix.multiplyAndSumRows(value_matrix), -1.0f, 1.0f);
    }

    poly_float bipolar = bipolar_->value();
    poly_float polarity_pre_scale = bipolar + 1.0f;
    poly_float polarity_post_scale = -bipolar * 0.5f + 1.0f;
    polarity_post_scale *= poly_float(1.0f) - (constants::kRightOne * 2.0f * stereo_->value());

    poly_float modulation_shift = modulation_input * polarity_pre_scale - bipolar;
    poly_float modulation_abs = poly_float::abs(modulation_shift);
    poly_mask sign_mask = poly_float::sign_mask(modulation_shift);

    poly_float power = -input(kModulationPower)->at(0);
    poly_float shifted_modulation = futils::powerScale(modulation_abs, power);
    poly_float modulation_amount = utils::clamp(input(kModulationAmount)->at(0), -1.0f, 1.0f);
    poly_float pre_modulation = modulation_amount * shifted_modulation;
    poly_float raw_modulation = (pre_modulation ^ sign_mask) * polarity_post_scale;
    output(kModulationPreScale)->buffer[0] = raw_modulation;
    output(kModulationOutput)->buffer[0] = raw_modulation * (*destination_scale_);
    output(kModulationPreScale)->trigger_value = output(kModulationPreScale)->buffer[0];
    output(kModulationOutput)->trigger_value = output(kModulationOutput)->buffer[0];
    VITAL_ASSERT(utils::isFinite(output()->buffer[0]));
  }
} // namespace vital
