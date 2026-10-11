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

#include "waveshaper_module.h"

#include "futils.h"

#include <algorithm>
#include <cmath>

namespace vital {

  // Transfer curve math ported from Wolf Shaper by Patrick Desaulniers (GPL v3+).
  namespace {
    constexpr double kPiDouble = 3.14159265358979323846;
    constexpr double kTwoOverPi = 0.636619772367581343076;

    float powerScale(float input, float tension, float max_exponent,
                     float p1x, float p1y, float p2x, float p2y) {
      const float input_sign = input >= 0.0f ? 1.0f : -1.0f;
      const bool tension_positive = tension >= 0.0f;

      tension = std::abs(tension);
      input = std::abs(input);

      const float delta_x = p2x - p1x;
      const float delta_y = p2y - p1y;
      const float exponent = 1.0f + tension * (max_exponent - 1.0f);

      float result;
      if (tension_positive)
        result = delta_y * std::pow((input - p1x) / delta_x, exponent) + p1y;
      else
        result = 1.0f - (delta_y * std::pow(1.0f - (input - p1x) / delta_x, exponent) + p1y) + p2y - (1.0f - p1y);

      return input_sign * result;
    }

    float skewPlus(float x, float warp_amount) {
      return 1.0f - std::pow(1.0f - x, warp_amount * 2.0f + 1.0f);
    }

    float skewMinus(float x, float warp_amount) {
      return std::pow(x, warp_amount * 2.0f + 1.0f);
    }

    float bendPlus(float x, float warp_amount) {
      if (x < 0.5f)
        return powerScale(x, -warp_amount, 3.0f, 0.0f, 0.0f, 0.5f, 0.5f);
      if (x > 0.5f)
        return powerScale(x, warp_amount, 3.0f, 0.5f, 0.5f, 1.0f, 1.0f);
      return x;
    }

    float bendMinus(float x, float warp_amount) {
      if (x < 0.5f)
        return powerScale(x, warp_amount, 3.0f, 0.0f, 0.0f, 0.5f, 0.5f);
      if (x > 0.5f)
        return powerScale(x, -warp_amount, 3.0f, 0.5f, 0.5f, 1.0f, 1.0f);
      return x;
    }

    float warpCoordinate(float coordinate, float warp_amount, int warp_type) {
      switch (warp_type) {
        case WaveShaperModule::kWarpBendUp:
          return bendPlus(coordinate, warp_amount);
        case WaveShaperModule::kWarpBendDown:
          return bendMinus(coordinate, warp_amount);
        case WaveShaperModule::kWarpBendUpDown:
          if (warp_amount < 0.5f)
            return bendPlus(coordinate, (0.5f - warp_amount) * 2.0f);
          if (warp_amount > 0.5f)
            return bendMinus(coordinate, (warp_amount - 0.5f) * 2.0f);
          return coordinate;
        case WaveShaperModule::kWarpSkewUp:
          return skewPlus(coordinate, warp_amount);
        case WaveShaperModule::kWarpSkewDown:
          return skewMinus(coordinate, warp_amount);
        case WaveShaperModule::kWarpSkewUpDown:
          if (warp_amount < 0.5f)
            return skewPlus(coordinate, (0.5f - warp_amount) * 2.0f);
          if (warp_amount > 0.5f)
            return skewMinus(coordinate, (warp_amount - 0.5f) * 2.0f);
          return coordinate;
        default:
          return coordinate;
      }
    }

    float curveValue(float input, float tension, float p1x, float p1y,
                     float p2x, float p2y, int type) {
      const float input_sign = input >= 0.0f ? 1.0f : -1.0f;

      if (p1x == p2x)
        return input_sign * p2y;

      tension /= 100.0f;
      const bool tension_positive = tension >= 0.0f;

      if (tension_positive)
        tension = std::pow(tension, 1.2f);
      else
        tension = -std::pow(-tension, 1.2f);

      const float delta_x = p2x - p1x;
      const float delta_y = p2y - p1y;

      switch (type) {
        case WaveShaperModule::kCurveSingle:
          return powerScale(input, tension, 15.0f, p1x, p1y, p2x, p2y);
        case WaveShaperModule::kCurveDouble: {
          const float middle_x = p1x + delta_x / 2.0f;
          const float middle_y = p1y + delta_y / 2.0f;
          if (std::abs(input) > middle_x)
            return powerScale(input, -tension, 15.0f, middle_x, middle_y, p2x, p2y);
          return powerScale(input, tension, 15.0f, p1x, p1y, middle_x, middle_y);
        }
        case WaveShaperModule::kCurveStairs: {
          if (tension == 0.0f)
            return powerScale(input, tension, 15.0f, p1x, p1y, p2x, p2y);

          input = std::abs(input);
          const int num_steps = static_cast<int>(std::floor(2.0f / std::pow(tension, 2.0f)));
          const float step_x = delta_x / (tension_positive ? num_steps : num_steps - 1);
          const float step_y = delta_y / (tension_positive ? num_steps - 1 : num_steps);

          float result;
          if (tension_positive)
            result = std::floor((input - p1x) / step_x) * step_y + p1y;
          else
            result = std::floor((input - p1x) / step_x + 1.0f) * step_y + p1y;

          const float min_y = std::min(p1y, p2y);
          const float max_y = std::max(p1y, p2y);
          return input_sign * utils::clamp(result, min_y, max_y);
        }
        case WaveShaperModule::kCurveWave: {
          tension = std::floor(tension * 100.0f);
          input = std::abs(input);

          // Intermediate math in double, matching the reference implementation.
          const float frequency = (0.5f + tension) / delta_x;
          const float phase = static_cast<float>(p1x * frequency * 2.0 * kPiDouble);
          float wave = static_cast<float>(-std::cos(frequency * kPiDouble * 2.0 * input - phase) / 2.0 + 0.5);

          if (!tension_positive)
            wave = static_cast<float>(kTwoOverPi * std::asin(utils::clamp(wave, -1.0f, 1.0f)));

          return input_sign * (wave * delta_y + p1y);
        }
        default:
          return input;
      }
    }
  }

  WaveShaperModule::WaveShaperModule(std::string prefix) :
      SynthModule(0, 1), shape_(nullptr), warp_type_(nullptr), mode_(nullptr), remove_dc_(nullptr),
      drive_(nullptr), tension_(nullptr), warp_amount_(nullptr), mix_(nullptr), output_gain_(nullptr),
      prefix_(prefix), num_vertices_(0), current_shape_(-1), current_warp_type_(kWarpNone),
      current_bipolar_(false), current_warp_amount_(0.0f), old_num_vertices_(0),
      old_warp_type_(kWarpNone), old_bipolar_(false), crossfade_samples_left_(0),
      mix_amount_(1.0f), current_tension_(0.0f), current_output_gain_(1.0f),
      dc_input_previous_(0.0f), dc_output_previous_(0.0f) {
    rebuildGraph(kShapeCurve);
  }

  void WaveShaperModule::init() {
    shape_ = createBaseControl(prefix_ + "waveshaper_shape");
    warp_type_ = createBaseControl(prefix_ + "waveshaper_warp_type");
    mode_ = createBaseControl(prefix_ + "waveshaper_mode");
    remove_dc_ = createBaseControl(prefix_ + "waveshaper_remove_dc");

    drive_ = createMonoModControl(prefix_ + "waveshaper_drive", true, true);
    tension_ = createMonoModControl(prefix_ + "waveshaper_tension");
    warp_amount_ = createMonoModControl(prefix_ + "waveshaper_warp_amount");
    mix_ = createMonoModControl(prefix_ + "waveshaper_dry_wet");
    output_gain_ = createMonoModControl(prefix_ + "waveshaper_output_gain");

    SynthModule::init();
  }

  void WaveShaperModule::rebuildGraph(int shape) {
    auto set_vertex = [this](int index, float x, float y, int curve_type) {
      vertices_[index].x = x;
      vertices_[index].y = y;
      vertices_[index].curve_type = curve_type;
    };

    switch (shape) {
      case kShapeSCurve:
        set_vertex(0, 0.0f, 0.0f, kCurveDouble);
        set_vertex(1, 1.0f, 1.0f, kCurveDouble);
        num_vertices_ = 2;
        break;
      case kShapeStairs:
        set_vertex(0, 0.0f, 0.0f, kCurveStairs);
        set_vertex(1, 1.0f, 1.0f, kCurveStairs);
        num_vertices_ = 2;
        break;
      case kShapeSineWave:
        set_vertex(0, 0.0f, 0.0f, kCurveWave);
        set_vertex(1, 1.0f, 1.0f, kCurveWave);
        num_vertices_ = 2;
        break;
      case kShapeHardClip:
        set_vertex(0, 0.0f, 0.0f, kCurveSingle);
        set_vertex(1, 0.5f, 1.0f, kCurveSingle);
        set_vertex(2, 1.0f, 1.0f, kCurveSingle);
        num_vertices_ = 3;
        break;
      case kShapeFold:
        set_vertex(0, 0.0f, 0.0f, kCurveSingle);
        set_vertex(1, 0.5f, 1.0f, kCurveSingle);
        set_vertex(2, 1.0f, 0.0f, kCurveSingle);
        num_vertices_ = 3;
        break;
      case kShapeDoubleFold:
        set_vertex(0, 0.0f, 0.0f, kCurveSingle);
        set_vertex(1, 0.25f, 1.0f, kCurveSingle);
        set_vertex(2, 0.5f, 0.0f, kCurveSingle);
        set_vertex(3, 0.75f, 1.0f, kCurveSingle);
        set_vertex(4, 1.0f, 0.0f, kCurveSingle);
        num_vertices_ = 5;
        break;
      case kShapeCurve:
      default:
        set_vertex(0, 0.0f, 0.0f, kCurveSingle);
        set_vertex(1, 1.0f, 1.0f, kCurveSingle);
        num_vertices_ = 2;
        break;
    }

    current_shape_ = shape;
  }

  float WaveShaperModule::graphValueAt(const Vertex* vertices, int num_vertices, float x, float tension) {
    int left = 0;
    int right = num_vertices - 1;
    while (left <= right) {
      const int mid = left + (right - left) / 2;
      if (vertices[mid].x < x)
        left = mid + 1;
      else if (vertices[mid].x > x)
        right = mid - 1;
      else
        return vertices[mid].y;
    }

    const Vertex& p1 = vertices[left - 1];
    const Vertex& p2 = vertices[left];
    return curveValue(x, tension, p1.x, p1.y, p2.x, p2.y, p1.curve_type);
  }

  float WaveShaperModule::shapeSample(const Vertex* vertices, int num_vertices, float value,
                                      float tension, float warp_amount, int warp_type, bool bipolar) {
    value = utils::clamp(value, -1.0f, 1.0f);

    if (bipolar) {
      float x = (1.0f + value) * 0.5f;
      if (warp_type != kWarpNone)
        x = utils::clamp(warpCoordinate(x, warp_amount, warp_type), 0.0f, 1.0f);
      return -1.0f + graphValueAt(vertices, num_vertices, x, tension) * 2.0f;
    }

    float x = std::abs(value);
    if (warp_type != kWarpNone)
      x = utils::clamp(warpCoordinate(x, warp_amount, warp_type), 0.0f, 1.0f);
    const float shaped = graphValueAt(vertices, num_vertices, x, tension);
    return value >= 0.0f ? shaped : -shaped;
  }

  void WaveShaperModule::processWithInput(const poly_float* audio_in, int num_samples) {
    SynthModule::process(num_samples);

    const int shape = utils::iclamp(static_cast<int>(shape_->value() + 0.5f), 0, kNumShapes - 1);
    const int warp_type = utils::iclamp(static_cast<int>(warp_type_->value() + 0.5f), 0, kNumWarpTypes - 1);
    const bool bipolar = mode_->value() > 0.5f;
    if (shape != current_shape_ || warp_type != current_warp_type_ || bipolar != current_bipolar_) {
      std::copy(vertices_, vertices_ + num_vertices_, old_vertices_);
      old_num_vertices_ = num_vertices_;
      old_warp_type_ = current_warp_type_;
      old_bipolar_ = current_bipolar_;
      crossfade_samples_left_ = kCrossfadeSamples;

      rebuildGraph(shape);
      current_warp_type_ = warp_type;
      current_bipolar_ = bipolar;
    }

    const bool remove_dc = remove_dc_->value() > 0.5f;
    if (!remove_dc) {
      dc_input_previous_ = 0.0f;
      dc_output_previous_ = 0.0f;
    }

    // The knob is inverted from the reference math so that positive tension
    // saturates (audible on quiet signals) and negative tension expands/gates.
    const float target_tension = utils::clamp(-tension_->buffer[0][0], -100.0f, 100.0f);
    float tension = current_tension_;
    const float delta_tension = (target_tension - tension) * (1.0f / num_samples);

    const float target_warp = utils::clamp(warp_amount_->buffer[0][0], 0.0f, 1.0f);
    float warp_amount = current_warp_amount_;
    const float delta_warp = (target_warp - warp_amount) * (1.0f / num_samples);

    poly_float current_mix = mix_amount_;
    const poly_float target_mix = utils::clamp(mix_->buffer[0], 0.0f, 1.0f);
    const poly_float delta_mix = (target_mix - current_mix) * (1.0f / num_samples);

    poly_float current_gain = current_output_gain_;
    const poly_float target_gain = futils::dbToMagnitude(output_gain_->buffer[0]);
    const poly_float delta_gain = (target_gain - current_gain) * (1.0f / num_samples);

    const poly_float* drive = drive_->buffer;
    poly_float* audio_out = output()->buffer;

    for (int i = 0; i < num_samples; ++i) {
      tension += delta_tension;
      warp_amount += delta_warp;
      current_mix += delta_mix;
      current_gain += delta_gain;

      poly_float sample = audio_in[i] * futils::dbToMagnitude(drive[i]);

      // Snap inaudibly small values to zero so released voices can't excite
      // stepped shapes, and so tiny negatives don't oscillate the graph's DC.
      poly_mask snap_mask = poly_float::lessThan(poly_float::abs(sample), 0.00001f);
      sample = utils::maskLoad(sample, 0.0f, snap_mask);

      // Center the curve so silence always maps to zero. Any remaining
      // signal-dependent offset is left for the DC filter below.
      float silence_offset = 0.0f;
      if (current_bipolar_) {
        silence_offset = shapeSample(vertices_, num_vertices_, 0.0f, tension, warp_amount,
                                     current_warp_type_, true);
      }

      poly_float shaped = 0.0f;
      for (int lane = 0; lane < poly_float::kSize; ++lane) {
        shaped.set(lane, shapeSample(vertices_, num_vertices_, sample[lane], tension, warp_amount,
                                     current_warp_type_, current_bipolar_) - silence_offset);
      }

      // Shape, warp type, and mode switch discrete curves: crossfade from the
      // previous curve so the change doesn't click.
      if (crossfade_samples_left_ > 0) {
        float old_offset = 0.0f;
        if (old_bipolar_) {
          old_offset = shapeSample(old_vertices_, old_num_vertices_, 0.0f, tension, warp_amount,
                                   old_warp_type_, true);
        }

        poly_float old_shaped = 0.0f;
        for (int lane = 0; lane < poly_float::kSize; ++lane) {
          old_shaped.set(lane, shapeSample(old_vertices_, old_num_vertices_, sample[lane], tension,
                                           warp_amount, old_warp_type_, old_bipolar_) - old_offset);
        }

        const float fade = 1.0f - crossfade_samples_left_ * (1.0f / kCrossfadeSamples);
        shaped = utils::interpolate(old_shaped, shaped, fade);
        --crossfade_samples_left_;
      }

      // Boosting drive shouldn't just get louder: pull the wet path back down
      // by half the added drive. Full compensation would assume full-scale
      // input and makes typical signals quieter as drive goes up.
      poly_float compensation = futils::dbToMagnitude(poly_float::max(drive[i], 0.0f) * -0.5f);
      shaped *= compensation;

      poly_float mixed = utils::interpolate(audio_in[i], shaped, current_mix) * current_gain;

      if (remove_dc) {
        // One-pole DC blocker in the well-conditioned x - x1 + R*y1 form. The
        // reference plugin's accumulator form loses float precision once a
        // standing offset is present and turns quiet tails into steps.
        const poly_float dc_out = mixed - dc_input_previous_ + dc_output_previous_ * 0.999f;
        dc_input_previous_ = mixed;
        dc_output_previous_ = dc_out;
        mixed = dc_out;
      }

      audio_out[i] = mixed;
    }

    current_tension_ = target_tension;
    current_warp_amount_ = target_warp;
    mix_amount_ = target_mix;
    current_output_gain_ = target_gain;
  }

  void WaveShaperModule::hardReset() {
    mix_amount_ = 1.0f;
    current_tension_ = 0.0f;
    current_warp_amount_ = 0.0f;
    current_output_gain_ = 1.0f;
    dc_input_previous_ = 0.0f;
    dc_output_previous_ = 0.0f;
    crossfade_samples_left_ = 0;
  }

  void WaveShaperModule::enable(bool enable) {
    SynthModule::enable(enable);
    if (!enable)
      hardReset();
  }
} // namespace vital
