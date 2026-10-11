// SPDX-License-Identifier: GPL-3.0-or-later
#include "flanger_engine.h"
#include "tape_saturation.h"

#include <algorithm>
#include <cmath>

namespace atlas::analogue::flanger {
  namespace {
    constexpr double kPi = 3.14159265358979323846;
    constexpr double kFadeSeconds = 0.03;
    constexpr double kSmoothingSeconds = 0.02;

    // BBD voice after the Electric Mistress (Ralf Metzger's measurements):
    // a SAD1024 with its 512-stage halves in parallel, clocked by a VCO whose
    // period, and so the delay, is roughly linear in the control voltage over
    // about 1.2-11 ms. SAD1024 S/N is over 70 dB. Its filters are
    // undocumented; 4th-order sections at 10 kHz keep the bucket clock's
    // images down without dulling the comb.
    constexpr int kBbdStages = 512;
    constexpr double kBbdMinDelay = 0.0012, kBbdMaxDelay = 0.011;
    constexpr double kBbdFilterHz = 10000.0;
    constexpr double kBbdSignalToNoise = 70.0;

    // Two-machine tape flanging: the moving head travels up to 5 ms either
    // side of a reference head 6 ms behind the record head.
    constexpr double kTapeReference = 0.006;
    constexpr double kTapeRange = 0.005;
    constexpr double kTapeMinDelay = 0.0002;
    constexpr double kTapeDrive = 1.5;

    BbdChip bbdChip(const AnalogFilter& output_filter) {
      BbdChip chip;
      chip.stages = kBbdStages;
      chip.headroom = 2.5;
      chip.even_distortion = 0.05;
      chip.noise = noiseForSignalToNoise(kBbdSignalToNoise, output_filter, 100000.0);
      chip.clock_leakage = 1.0e-3;
      return chip;
    }

    double triangle(double phase) {
      phase -= std::floor(phase);
      return 4.0 * std::abs(phase - 0.5) - 1.0;
    }
  }

  Range bbdRange() { return { kBbdMinDelay, kBbdMaxDelay }; }
  Range tapeRange() { return { -kTapeRange, kTapeRange }; }
  double tapeReferenceDelay() { return kTapeReference; }

  Engine::Engine() { prepare(sample_rate_); }

  void Engine::prepare(double sample_rate) {
    sample_rate_ = sample_rate;
    smoothing_ = -std::expm1(-1.0 / (kSmoothingSeconds * sample_rate));
    dc_coefficient_ = -std::expm1(-2.0 * kPi * 10.0 / sample_rate);
    fade_length_ = std::max(1, static_cast<int>(kFadeSeconds * sample_rate));
    // The incoming medium fills its longest delay before it fades in.
    prime_length_ = static_cast<int>(std::ceil((std::max(kBbdMaxDelay, kTapeReference + kTapeRange) + 0.001) * sample_rate));
    const auto filter = toPartialFractions(butterworthLowPass(4, kBbdFilterHz));
    for (auto& line : lines_)
      line.configure(bbdChip(filter), filter, filter, sample_rate);
    const double longest = kTapeReference + kTapeRange + 0.002;
    tape_.assign(static_cast<size_t>(std::ceil(longest * sample_rate)) + 8, 0.0f);
    reset();
  }

  void Engine::reset() {
    for (auto& line : lines_) line.reset();
    bbd_feedback_ = {};
    dc_block_ = {};
    std::fill(tape_.begin(), tape_.end(), 0.0f);
    tape_write_ = 0;
    playback_lowpass_ = {};
    tape_feedback_ = 0.0;
    flutter_phase_ = wow_phase_ = 0.0;
    current_medium_ = parameters_.medium;
    fading_medium_ = -1;
    fade_remaining_ = fade_wait_ = 0;
    depth_ = parameters_.depth;
    manual_ = parameters_.manual;
    feedback_ = parameters_.feedback;
    stereo_phase_ = parameters_.stereo_phase;
    noise_ = parameters_.noise;
    age_ = parameters_.age;
    mix_ = parameters_.mix;
    random_ = 0x9e3779b9u;
  }

  void Engine::alignLfo(double cycles) { lfo_phase_ = cycles - std::floor(cycles); }

  void Engine::setParameters(const Parameters& parameters) {
    parameters_ = parameters;
    parameters_.medium = std::clamp(parameters.medium, 0, kNumMedia - 1);
    if (parameters_.medium != current_medium_) {
      if (fading_medium_ < 0) {
        fading_medium_ = current_medium_;
        fade_remaining_ = fade_length_;
        fade_wait_ = prime_length_;
      }
      current_medium_ = parameters_.medium;
      // The incoming medium starts empty so stale audio never replays.
      if (current_medium_ == kBbd) {
        for (auto& line : lines_) line.reset();
        bbd_feedback_ = {};
      }
      else {
        std::fill(tape_.begin(), tape_.end(), 0.0f);
        playback_lowpass_ = {};
        tape_feedback_ = 0.0;
      }
      if (fading_medium_ == current_medium_) {
        fading_medium_ = -1;
        fade_remaining_ = fade_wait_ = 0;
      }
    }
  }

  double Engine::noiseSample() {
    double sum = 0.0;
    for (int i = 0; i < 4; ++i) {
      random_ ^= random_ << 13; random_ ^= random_ >> 17; random_ ^= random_ << 5;
      sum += random_ * (1.0 / 4294967296.0);
    }
    return (sum - 2.0) * 1.7320508075688772;
  }

  double Engine::readTape(double delay_samples) const {
    const int size = static_cast<int>(tape_.size());
    double position = tape_write_ - std::clamp(delay_samples, 1.0, size - 3.0);
    if (position < 0) position += size;
    const int i = static_cast<int>(position);
    const double t = position - i;
    const auto at = [&](int index) { return double(tape_[(index + size) % size]); };
    const double y0 = at(i - 1), y1 = at(i), y2 = at(i + 1), y3 = at(i + 2);
    return ((( -0.5 * y0 + 1.5 * y1 - 1.5 * y2 + 0.5 * y3) * t +
             (y0 - 2.5 * y1 + 2.0 * y2 - 0.5 * y3)) * t + (-0.5 * y0 + 0.5 * y2)) * t + y1;
  }

  void Engine::renderBbd(const float* mono, float* wet_left, float* wet_right, int count) {
    const double noise_scale = (noise_ / 0.5) * (noise_ / 0.5) * (1.0 + 3.0 * age_);
    for (auto& line : lines_) line.setNoiseScale(noise_scale, noise_scale * (1.0 + 4.0 * age_));
    const double increment = parameters_.lfo_frequency / sample_rate_;
    double phase = lfo_phase_;
    for (int n = 0; n < count; ++n) {
      // The control voltage sets the clock period: delay is linear in it.
      const double center = kBbdMinDelay + manual_ * (kBbdMaxDelay - kBbdMinDelay);
      const double swing = depth_ * 0.5 * (kBbdMaxDelay - kBbdMinDelay);
      double out[2];
      for (int side = 0; side < 2; ++side) {
        const double lfo = triangle(phase + side * 0.5 * stereo_phase_);
        const double delay = std::clamp(center + swing * lfo, kBbdMinDelay, kBbdMaxDelay);
        lines_[side].setDelay(delay);
        const double input = mono[n] + feedback_ * bbd_feedback_[side];
        out[side] = lines_[side].process(input);
        // Coupling capacitors keep the regeneration free of DC.
        dc_block_[side] += (out[side] - dc_block_[side]) * dc_coefficient_;
        bbd_feedback_[side] = out[side] - dc_block_[side];
      }
      wet_left[n] = static_cast<float>(out[0]);
      wet_right[n] = static_cast<float>(out[1]);
      phase += increment;
      phase -= std::floor(phase);
    }
  }

  void Engine::renderTape(const float* mono, float* wet_left, float* wet_right, int count) {
    const double increment = parameters_.lfo_frequency / sample_rate_;
    const double polarity = feedback_ < 0.0 ? -1.0 : 1.0;
    const double regeneration = std::abs(feedback_);
    const double noise_level = 3.0e-4 * (noise_ / 0.5) * (noise_ / 0.5) * (1.0 + age_);
    // Age dulls the playback heads and adds transport wobble.
    const double cutoff = 15000.0 * (1.0 - 0.7 * age_);
    const double lowpass = -std::expm1(-2.0 * kPi * std::min(cutoff, 0.45 * sample_rate_) / sample_rate_);
    const double wow_increment = 0.6 / sample_rate_, flutter_increment = 7.3 / sample_rate_;
    double phase = lfo_phase_;
    for (int n = 0; n < count; ++n) {
      const double wobble = age_ * (0.00002 * std::sin(2.0 * kPi * wow_phase_) +
                                    0.000005 * std::sin(2.0 * kPi * flutter_phase_));
      const double write = mono[n] + regeneration * tape_feedback_;
      tape_[tape_write_] = static_cast<float>(tape_saturation::shape(write * kTapeDrive) / kTapeDrive);

      const double reference_delay = (kTapeReference + wobble) * sample_rate_;
      double heads[3];
      heads[0] = readTape(reference_delay);
      for (int side = 0; side < 2; ++side) {
        const double lfo = std::sin(2.0 * kPi * (phase + side * 0.5 * stereo_phase_));
        const double offset = kTapeRange * std::clamp(2.0 * manual_ - 1.0 + depth_ * lfo, -1.0, 1.0);
        const double delay = std::max(kTapeMinDelay, kTapeReference + offset - wobble);
        heads[side + 1] = readTape(delay * sample_rate_);
      }
      for (int head = 0; head < 3; ++head) {
        playback_lowpass_[head] += (heads[head] - playback_lowpass_[head]) * lowpass;
        heads[head] = playback_lowpass_[head];
      }
      const double hiss = noise_level * noiseSample();
      const double left = 0.5 * (heads[0] + polarity * heads[1]) + hiss;
      const double right = 0.5 * (heads[0] + polarity * heads[2]) + hiss;
      tape_feedback_ = 0.5 * (left + right);
      wet_left[n] = static_cast<float>(left);
      wet_right[n] = static_cast<float>(right);

      if (++tape_write_ == static_cast<int>(tape_.size())) tape_write_ = 0;
      phase += increment;
      phase -= std::floor(phase);
      wow_phase_ += wow_increment; wow_phase_ -= std::floor(wow_phase_);
      flutter_phase_ += flutter_increment; flutter_phase_ -= std::floor(flutter_phase_);
    }
  }

  void Engine::process(const float* in_left, const float* in_right, float* out_left, float* out_right, int count) {
    constexpr int kBlock = 128;
    for (int offset = 0; offset < count;) {
      const int block = std::min(kBlock, count - offset);
      float mono[kBlock], wet_left[kBlock], wet_right[kBlock];
      for (int n = 0; n < block; ++n)
        mono[n] = 0.5f * (in_left[offset + n] + in_right[offset + n]);

      const double block_smoothing = 1.0 - std::pow(1.0 - smoothing_, block);
      depth_ += (parameters_.depth - depth_) * block_smoothing;
      manual_ += (parameters_.manual - manual_) * block_smoothing;
      feedback_ += (parameters_.feedback - feedback_) * block_smoothing;
      stereo_phase_ += (parameters_.stereo_phase - stereo_phase_) * block_smoothing;
      noise_ += (parameters_.noise - noise_) * block_smoothing;
      age_ += (parameters_.age - age_) * block_smoothing;

      const auto render = [&](int medium, float* left, float* right) {
        if (medium == kBbd) renderBbd(mono, left, right, block);
        else renderTape(mono, left, right, block);
      };
      render(current_medium_, wet_left, wet_right);
      if (fading_medium_ >= 0) {
        float old_left[kBlock], old_right[kBlock];
        render(fading_medium_, old_left, old_right);
        for (int n = 0; n < block; ++n) {
          double progress = 0.0;
          if (fade_wait_ > 0) --fade_wait_;
          else {
            progress = fade_remaining_ > 0 ? 1.0 - double(fade_remaining_) / fade_length_ : 1.0;
            if (fade_remaining_ > 0) --fade_remaining_;
          }
          const double in_gain = std::sin(0.5 * kPi * progress), out_gain = std::cos(0.5 * kPi * progress);
          wet_left[n] = static_cast<float>(wet_left[n] * in_gain + old_left[n] * out_gain);
          wet_right[n] = static_cast<float>(wet_right[n] * in_gain + old_right[n] * out_gain);
        }
        if (fade_remaining_ == 0) fading_medium_ = -1;
      }
      lfo_phase_ += parameters_.lfo_frequency * block / sample_rate_;
      lfo_phase_ -= std::floor(lfo_phase_);

      for (int n = 0; n < block; ++n) {
        mix_ += (parameters_.mix - mix_) * smoothing_;
        const double dry_gain = std::cos(0.5 * kPi * mix_), wet_gain = std::sin(0.5 * kPi * mix_);
        out_left[offset + n] = static_cast<float>(dry_gain * in_left[offset + n] + wet_gain * wet_left[n]);
        out_right[offset + n] = static_cast<float>(dry_gain * in_right[offset + n] + wet_gain * wet_right[n]);
      }
      offset += block;
    }
  }
}
