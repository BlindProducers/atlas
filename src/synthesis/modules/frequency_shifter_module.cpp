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

#include "frequency_shifter_module.h"

#include "futils.h"
#include "poly_utils.h"

#include <algorithm>
#include <cmath>

namespace vital {
  namespace {
    constexpr double kHilbertMinimumFrequency = 15.0;
    constexpr double kCoefficientConvergence = 1.0e-100;
    constexpr float kDownshiftTransitionRatio = 1.1f;

    double computeAccumulatedTerm(double q, int order, int coefficient, bool denominator) {
      int index = denominator ? 1 : 0;
      int sign = denominator ? -1 : 1;
      double sum = 0.0;
      double term = 0.0;
      do {
        int paired_index = index + 1 - (denominator ? 1 : 0);
        term = std::pow(q, index * paired_index);
        term *= std::sin((index + paired_index) * coefficient * juce::MathConstants<double>::pi / order +
                         (denominator ? juce::MathConstants<double>::halfPi : 0.0));
        term *= sign;
        sum += term;
        sign = -sign;
        ++index;
      } while (std::abs(term) > kCoefficientConvergence);
      return sum;
    }

    double computeHilbertCoefficient(int coefficient, int num_coefficients, double k, double q) {
      const int order = num_coefficients * 2 + 1;
      double numerator = computeAccumulatedTerm(q, order, coefficient, false) * std::pow(q, 0.25);
      double denominator = computeAccumulatedTerm(q, order, coefficient, true) + 0.5;
      double warped_frequency = numerator / denominator;
      warped_frequency *= warped_frequency;

      double root = std::sqrt((1.0 - warped_frequency * k) *
                              (1.0 - warped_frequency / k)) / (1.0 + warped_frequency);
      return (1.0 - root) / (1.0 + root);
    }
  } // namespace

  FrequencyShifterModule::FrequencyShifterModule(std::string prefix) :
      SynthModule(0, 1), shift_(nullptr), feedback_(nullptr), delay_(nullptr), mix_(nullptr), prefix_(prefix),
      wet_delay_write_index_(0), quadrature_delay_(0.0f), phase_(0.0f), feedback_sample_(0.0f),
      feedback_amount_(0.0f), mix_amount_(1.0f), delay_milliseconds_(0.0f), downshift_cutoff_(0.0f) {
    updateHilbertCoefficients();
    resizeDelayBuffer();
  }

  void FrequencyShifterModule::init() {
    shift_ = createMonoModControl(prefix_ + "frequency_shifter_shift", true, true);
    feedback_ = createMonoModControl(prefix_ + "frequency_shifter_feedback");
    delay_ = createMonoModControl(prefix_ + "frequency_shifter_delay");
    mix_ = createMonoModControl(prefix_ + "frequency_shifter_dry_wet");
    mode_ = createBaseControl(prefix_ + "frequency_shifter_mode");
    mode_2_mono_input_ = createBaseControl(prefix_ + "frequency_shifter_bode_mono_input");
    // Range is stored as its base-2 logarithm. The module raises 2 to it
    // exactly, where the shared exponential scale approximates.
    const char* names[] = { "shift", "range", "direction", "width", "delay", "feedback", "blur", "mix" };
    for (size_t i = 0; i < mode_2_controls_.size(); ++i) {
      const std::string name = prefix_ + "frequency_shifter_bode_" + names[i];
      mode_2_controls_[i] = i == 1 ? createBaseModControl(name) : createMonoModControl(name);
    }

    SynthModule::init();
  }

  void FrequencyShifterModule::prepareMode2() {
    if (!mode_2_)
      mode_2_ = std::make_unique<atlas::bode::Shifter>();
    mode_2_->prepare(getSampleRate());
  }

  void FrequencyShifterModule::processMode2(const poly_float* audio_in, int num_samples) {
    poly_float* audio_out = output()->buffer;
    if (!mode_2_) {
      utils::copyBuffer(audio_out, audio_in, num_samples);
      return;
    }
    // Controls are read once per block, when the script's @slider would run.
    auto control = [this](int index, double low, double high) {
      return std::clamp(static_cast<double>(mode_2_controls_[index]->buffer[0][0]), low, high);
    };
    atlas::bode::Shifter::Parameters parameters;
    parameters.shift = control(0, -100, 100);
    parameters.range = std::clamp(std::exp2(static_cast<double>(mode_2_controls_[1]->buffer[0][0])), 0.1, 5000.0);
    parameters.direction = control(2, -100, 100);
    parameters.width = control(3, 0, 100);
    parameters.delay = control(4, 0, 5);
    parameters.feedback = control(5, 0, 100);
    parameters.blur = control(6, 0, 100);
    parameters.mix = control(7, 0, 100);
    parameters.mono_input = mode_2_mono_input_->value() >= 0.5f ? 1 : 0;
    if (parameters != mode_2_->parameters())
      mode_2_->setParameters(parameters);

    // Lanes 2-3 repeat lanes 0-1, so one stereo pass serves both.
    for (int i = 0; i < num_samples; ++i) {
      double left = audio_in[i][0], right = audio_in[i][1];
      mode_2_->process(left, right);
      const mono_float out_left = static_cast<mono_float>(left), out_right = static_cast<mono_float>(right);
      audio_out[i] = poly_float(out_left, out_right, out_left, out_right);
    }
  }

  void FrequencyShifterModule::updateHilbertCoefficients() {
    // Elliptic polyphase all-pass design: the even and odd coefficient
    // cascades form a broadband 90-degree pair without a long FIR delay.
    const double sample_rate = std::max(1, getSampleRate());
    const double transition = std::min(0.45, 2.0 * kHilbertMinimumFrequency / sample_rate);
    double k = std::tan((1.0 - transition * 2.0) * juce::MathConstants<double>::pi / 4.0);
    k *= k;
    const double fourth_root = std::pow(1.0 - k * k, 0.25);
    const double elliptic_term = 0.5 * (1.0 - fourth_root) / (1.0 + fourth_root);
    const double elliptic_term_4 = std::pow(elliptic_term, 4.0);
    const double q = elliptic_term *
                     (1.0 + elliptic_term_4 *
                              (2.0 + elliptic_term_4 * (15.0 + 150.0 * elliptic_term_4)));

    for (int i = 0; i < kNumHilbertCoefficients; ++i) {
      hilbert_coefficients_[i] = static_cast<float>(
          computeHilbertCoefficient(i + 1, kNumHilbertCoefficients, k, q));
    }
  }

  void FrequencyShifterModule::resetHilbertState() {
    for (AllpassSection& section : real_sections_)
      section.reset();
    for (AllpassSection& section : quadrature_sections_)
      section.reset();
    for (AllpassSection& section : dry_sections_)
      section.reset();
    quadrature_delay_ = 0.0f;
  }

  void FrequencyShifterModule::updateDownshiftFilterCoefficients(mono_float cutoff) {
    // Eighth-order Butterworth high-pass. Moving the corner slightly above
    // the shift magnitude creates a smooth guard band around 0 Hz instead of
    // letting the lower sideband reflect back into the audible spectrum.
    static constexpr mono_float kButterworthQ[kNumDownshiftFilterSections] = {
      0.50979558f, 0.60134489f, 0.89997622f, 2.56291545f
    };
    const mono_float warped = std::tan(kPi * cutoff / getSampleRate());
    const mono_float warped_squared = warped * warped;
    for (int section = 0; section < kNumDownshiftFilterSections; ++section) {
      const mono_float normalizer =
          1.0f / (1.0f + warped / kButterworthQ[section] + warped_squared);
      BiquadCoefficients& coefficients = downshift_coefficients_[section];
      coefficients.b0 = normalizer;
      coefficients.b1 = -2.0f * normalizer;
      coefficients.b2 = normalizer;
      coefficients.a1 = 2.0f * (warped_squared - 1.0f) * normalizer;
      coefficients.a2 =
          (1.0f - warped / kButterworthQ[section] + warped_squared) * normalizer;
    }
    downshift_cutoff_ = cutoff;
  }

  void FrequencyShifterModule::resetDownshiftFilters() {
    for (BiquadState& filter : downshift_real_filters_)
      filter.reset();
    for (BiquadState& filter : downshift_quadrature_filters_)
      filter.reset();
    downshift_cutoff_ = 0.0f;
  }

  void FrequencyShifterModule::resizeDelayBuffer() {
    const int maximum_delay_samples =
        std::max(2, static_cast<int>(std::ceil(kMaxDelayMilliseconds * 0.001f * getSampleRate())) + 2);
    // The maximum host rate supported by Atlas is 192 kHz with up to 8x
    // internal quality. Reserve the delay once during construction.
    if (wet_delay_buffer_.capacity() == 0)
      wet_delay_buffer_.reserve(static_cast<size_t>(std::ceil(kMaxDelayMilliseconds * .001f * kMaxSampleRate * 8)) + 2);
    wet_delay_buffer_.assign(static_cast<size_t>(maximum_delay_samples), poly_float(0.0f));
    wet_delay_write_index_ = 0;
  }

  FrequencyShifterModule::QuadraturePair FrequencyShifterModule::processHilbert(poly_float audio) {
    poly_float real = audio;
    poly_float quadrature = audio;
    for (int i = 0; i < kNumHilbertSections; ++i) {
      real = real_sections_[i].process(real, hilbert_coefficients_[i * 2]);
      quadrature = quadrature_sections_[i].process(quadrature, hilbert_coefficients_[i * 2 + 1]);
    }

    poly_float delayed_quadrature = quadrature_delay_;
    quadrature_delay_ = quadrature;
    return { real, delayed_quadrature };
  }

  FrequencyShifterModule::QuadraturePair FrequencyShifterModule::protectDownshiftFromDcFold(
      QuadraturePair pair) {
    for (int section = 0; section < kNumDownshiftFilterSections; ++section) {
      pair.real = downshift_real_filters_[section].process(pair.real,
                                                            downshift_coefficients_[section]);
      pair.quadrature = downshift_quadrature_filters_[section].process(pair.quadrature,
                                                                       downshift_coefficients_[section]);
    }
    return pair;
  }

  poly_float FrequencyShifterModule::processAlignedDry(poly_float audio) {
    for (int i = 0; i < kNumHilbertSections; ++i)
      audio = dry_sections_[i].process(audio, hilbert_coefficients_[i * 2]);
    return audio;
  }

  poly_float FrequencyShifterModule::processWetDelay(poly_float wet, mono_float delay_samples) {
    VITAL_ASSERT(!wet_delay_buffer_.empty());
    wet_delay_buffer_[wet_delay_write_index_] = wet;

    delay_samples = std::min(std::max(delay_samples, 0.0f),
                             static_cast<mono_float>(wet_delay_buffer_.size() - 2));
    const int whole_samples = static_cast<int>(delay_samples);
    const mono_float fraction = delay_samples - whole_samples;
    const int size = static_cast<int>(wet_delay_buffer_.size());
    const int current_index = (wet_delay_write_index_ - whole_samples + size) % size;
    const int previous_index = (current_index - 1 + size) % size;
    const poly_float delayed = utils::interpolate(wet_delay_buffer_[current_index],
                                                  wet_delay_buffer_[previous_index], fraction);
    wet_delay_write_index_ = (wet_delay_write_index_ + 1) % size;
    return delayed;
  }

  void FrequencyShifterModule::processWithInput(const poly_float* audio_in, int num_samples) {
    static constexpr float kParameterSmoothingSeconds = 0.01f;

    SynthModule::process(num_samples);
    // Each mode starts from silence, with its own settings.
    const int mode = mode_->value() >= 0.5f ? 1 : 0;
    if (mode != last_mode_) {
      hardReset();
      last_mode_ = mode;
    }
    if (mode == 1) {
      processMode2(audio_in, num_samples);
      return;
    }

    poly_float target_feedback = utils::clamp(feedback_->buffer[0], -0.95f, 0.95f);
    poly_float target_delay = utils::clamp(delay_->buffer[0], 0.0f, kMaxDelayMilliseconds);
    poly_float target_mix = utils::clamp(mix_->buffer[0], 0.0f, 1.0f);
    poly_float parameter_smoothing = poly_float(1.0f) -
                                     futils::exp(poly_float(-1.0f) /
                                                 (kParameterSmoothingSeconds * getSampleRate()));
    poly_float* audio_out = output()->buffer;

    for (int i = 0; i < num_samples; ++i) {
      feedback_amount_ += (target_feedback - feedback_amount_) * parameter_smoothing;
      mix_amount_ += (target_mix - mix_amount_) * parameter_smoothing;
      delay_milliseconds_ += (target_delay[0] - delay_milliseconds_) * parameter_smoothing[0];
      phase_ = utils::mod(phase_ + shift_->buffer[i] * (1.0f / getSampleRate()));

      poly_float input = audio_in[i] + feedback_sample_ * feedback_amount_;
      QuadraturePair pair = processHilbert(input);
      const poly_float unfiltered_real = pair.real;
      poly_float aligned_dry = processAlignedDry(audio_in[i]);

      // Reuse the exact same all-pass result when the paths are mathematically
      // identical. This avoids microscopic recursive-filter drift turning into
      // audible dry/wet combing at zero feedback or zero shift.
      poly_mask zero_feedback = poly_float::equal(feedback_amount_, 0.0f);
      aligned_dry = utils::maskLoad(aligned_dry, unfiltered_real, zero_feedback);

      // Follow the existing audio-rate Shift smoothing sample by sample. The
      // old block-rate cutoff lagged large downward moves and briefly exposed
      // the reflected lower sideband as zippery, inharmonic material.
      const mono_float downshift = std::max(0.0f, -shift_->buffer[i][0]);
      if (downshift >= 5.0f) {
        const mono_float cutoff = std::min(downshift * kDownshiftTransitionRatio,
                                            getSampleRate() * 0.45f);
        if (cutoff != downshift_cutoff_)
          updateDownshiftFilterCoefficients(cutoff);
        pair = protectDownshiftFromDcFold(pair);
      }
      else if (downshift_cutoff_ != 0.0f) {
        resetDownshiftFilters();
      }
      poly_float shifted = pair.real * utils::cos(phase_ * (2.0f * kPi)) -
                           pair.quadrature * utils::sin(phase_ * (2.0f * kPi));
      poly_mask zero_shift = poly_float::equal(shift_->buffer[i], 0.0f);
      shifted = utils::maskLoad(shifted, unfiltered_real, zero_shift);

      const mono_float delay_samples = delay_milliseconds_ * 0.001f * getSampleRate();
      const poly_float delayed_wet = processWetDelay(shifted, delay_samples);
      feedback_sample_ = delayed_wet;
      audio_out[i] = utils::interpolate(aligned_dry, delayed_wet, mix_amount_);
    }
  }

  void FrequencyShifterModule::hardReset() {
    resetHilbertState();
    resetDownshiftFilters();
    std::fill(wet_delay_buffer_.begin(), wet_delay_buffer_.end(), poly_float(0.0f));
    wet_delay_write_index_ = 0;
    phase_ = 0.0f;
    feedback_sample_ = 0.0f;
    feedback_amount_ = 0.0f;
    mix_amount_ = 1.0f;
    delay_milliseconds_ = 0.0f;
    if (mode_2_)
      mode_2_->reset();
    last_mode_ = -1;
  }

  void FrequencyShifterModule::enable(bool enable) {
    SynthModule::enable(enable);
    if (!enable)
      hardReset();
  }

  void FrequencyShifterModule::setSampleRate(int sample_rate) {
    SynthModule::setSampleRate(sample_rate);
    updateHilbertCoefficients();
    resetHilbertState();
    resetDownshiftFilters();
    resizeDelayBuffer();
    feedback_sample_ = 0.0f;
    if (mode_2_)
      mode_2_->prepare(getSampleRate());
  }

  void FrequencyShifterModule::setOversampleAmount(int oversample) {
    SynthModule::setOversampleAmount(oversample);
    updateHilbertCoefficients();
    resetHilbertState();
    resetDownshiftFilters();
    resizeDelayBuffer();
    feedback_sample_ = 0.0f;
    if (mode_2_)
      mode_2_->prepare(getSampleRate());
  }
} // namespace vital
