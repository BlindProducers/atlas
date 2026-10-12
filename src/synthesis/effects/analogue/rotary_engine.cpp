// SPDX-License-Identifier: GPL-3.0-or-later
#include "rotary_engine.h"

#include "openb3/whirl.h"

#include <algorithm>
#include <cmath>

namespace atlas::analogue::rotary {
  namespace {
    constexpr double kPi = 3.14159265358979323846;
    constexpr double kSpeedOfSound = 343.0;
    constexpr double kFadeSeconds = 0.03;
    constexpr double kSmoothingSeconds = 0.02;
    // Leslie 122/147 passive crossover: 800 Hz, 12 dB/octave (Henricksen).
    constexpr double kCrossoverHz = 800.0;
    // Below about 200 Hz the drum hardly modulates the woofer (Henricksen).
    constexpr double kDrumStillHz = 200.0;
    // Horn voicing: its response rolls off above ~4.5 kHz with a resonance,
    // and an angle-independent part leaks out at about 15% (Smith et al.'s
    // measurements as used by setBfree).
    constexpr double kHornVoiceHz = 4500.0, kHornVoiceQ = 2.7456, kHornLeak = 0.15;
    // Leslie 16: the rotor's feed has a low cut near 100 Hz.
    constexpr double kGuitarLowCutHz = 100.0;
    // A reflection off the cabinet's back wall: the opposite side of the
    // rotation, a little later and weaker.
    constexpr double kReflectionGain = 0.35, kReflectionSeconds = 0.0008;

    // Speeds: John Fisher's measurements of a 147 (48/400 rpm horn, 40/342
    // rpm drum); Leslie 16 about 40/340 rpm. Speed changes are first order
    // with Herrera, Hanson & Abel's measured time constants (via setBfree):
    // horn 0.161 s up, 0.321 s down; drum 4.127 s up, 1.371 s down. The 16's
    // light foam rotor is undocumented; its constants are estimates. Horn
    // radius 19.2 cm; drum about 20 cm. Facing away the horn is about 12 dB
    // down and darker above ~1.7 kHz (Smith et al.).
    const RotorSpec kHornSpec { 48.0 / 60.0, 400.0 / 60.0, 0.161, 0.321, 0.192, 0.75, 2000.0 };
    const RotorSpec kOrganDrumSpec { 40.0 / 60.0, 342.0 / 60.0, 4.127, 1.371, 0.20, 0.5, 600.0 };
    const RotorSpec kGuitarDrumSpec { 40.0 / 60.0, 340.0 / 60.0, 1.0, 0.8, 0.15, 0.6, 1800.0 };

    double coefficientFor(double seconds, double rate) {
      return seconds > 0.0 ? -std::expm1(-1.0 / (seconds * rate)) : 1.0;
    }

    // RBJ Butterworth sections; two in cascade make a Linkwitz-Riley crossover.
    void butterworth(double* c, double hz, double rate, bool high) {
      const double w = 2.0 * kPi * hz / rate, cosw = std::cos(w), alpha = std::sin(w) / std::sqrt(2.0);
      const double a0 = 1.0 + alpha;
      if (high) { c[0] = (1.0 + cosw) / 2.0; c[1] = -(1.0 + cosw); c[2] = (1.0 + cosw) / 2.0; }
      else { c[0] = (1.0 - cosw) / 2.0; c[1] = 1.0 - cosw; c[2] = (1.0 - cosw) / 2.0; }
      c[3] = -2.0 * cosw; c[4] = 1.0 - alpha;
      for (int i = 0; i < 5; ++i) c[i] /= a0;
    }
  }

  const RotorSpec& hornSpec() { return kHornSpec; }
  const RotorSpec& drumSpec(int model) { return model == kGuitar ? kGuitarDrumSpec : kOrganDrumSpec; }

  Engine::Engine() : whirl_(std::make_unique<openb3::Whirl>()) { prepare(sample_rate_); }

  Engine::~Engine() = default;

  double Engine::hornHz() const {
    return parameters_.model == kWhirl ? whirl_->hornHz() : rotors_[kHorn].hz;
  }

  double Engine::drumHz() const {
    if (parameters_.model == kWhirl)
      return whirl_->drumHz();
    return rotors_[parameters_.model == kGuitar ? kGuitarDrum : kOrganDrum].hz;
  }

  void Engine::prepare(double sample_rate) {
    sample_rate_ = sample_rate;
    whirl_->prepare(sample_rate);
    smoothing_ = coefficientFor(kSmoothingSeconds, sample_rate);
    fade_length_ = std::max(1, static_cast<int>(kFadeSeconds * sample_rate));
    // Longest delay: the full rotation diameter plus the reflection.
    const double longest = 2.0 * 0.25 / kSpeedOfSound + kReflectionSeconds + 0.002;
    for (auto& rotor : rotors_)
      rotor.line.assign(static_cast<size_t>(std::ceil(longest * sample_rate)) + 8, 0.0f);
    configureCrossover();
    reset();
  }

  void Engine::configureCrossover() {
    const auto set = [](Biquad& filter, const double* c) {
      filter.b0 = c[0]; filter.b1 = c[1]; filter.b2 = c[2]; filter.a1 = c[3]; filter.a2 = c[4];
    };
    double c[5];
    butterworth(c, kCrossoverHz, sample_rate_, false); set(lowpass_[0], c);
    butterworth(c, kCrossoverHz, sample_rate_, true); set(highpass_[0], c);
    butterworth(c, kDrumStillHz, sample_rate_, false); set(lowpass_[1], c);
    butterworth(c, kGuitarLowCutHz, sample_rate_, true); set(highpass_[1], c);
    // Resonant low pass for the horn's voicing.
    const double w = 2.0 * kPi * std::min(kHornVoiceHz, 0.45 * sample_rate_) / sample_rate_;
    const double cosw = std::cos(w), alpha = std::sin(w) / (2.0 * kHornVoiceQ), a0 = 1.0 + alpha;
    horn_voice_.b0 = (1.0 - cosw) / 2.0 / a0; horn_voice_.b1 = (1.0 - cosw) / a0; horn_voice_.b2 = horn_voice_.b0;
    horn_voice_.a1 = -2.0 * cosw / a0; horn_voice_.a2 = (1.0 - alpha) / a0;
  }

  void Engine::reset() {
    const auto& p = parameters_;
    for (int i = 0; i < kNumRotors; ++i) {
      auto& rotor = rotors_[i];
      std::fill(rotor.line.begin(), rotor.line.end(), 0.0f);
      rotor.write = 0;
      rotor.rear_state = {};
      const RotorSpec& spec = i == kHorn ? kHornSpec : i == kOrganDrum ? kOrganDrumSpec : kGuitarDrumSpec;
      rotor.hz = p.speed == kFast ? spec.fast_hz : p.speed == kSlow ? spec.slow_hz : 0.0;
      // Rotors start at different angles, as they would on a real cabinet.
      rotor.angle = 0.37 * i;
    }
    for (auto& filter : lowpass_) filter.z1 = filter.z2 = 0.0;
    for (auto& filter : highpass_) filter.z1 = filter.z2 = 0.0;
    horn_voice_.z1 = horn_voice_.z2 = 0.0;
    whirl_->setSpeed(p.speed);
    whirl_->setInertia(p.inertia / 0.5);
    whirl_->reset(true);
    drive_ = p.drive; balance_ = p.balance; spread_ = p.spread; mix_ = p.mix;
    current_model_ = p.model;
    fading_model_ = -1;
    fade_remaining_ = 0;
  }

  void Engine::setParameters(const Parameters& parameters) {
    parameters_ = parameters;
    parameters_.model = std::clamp(parameters.model, 0, kNumModels - 1);
    parameters_.speed = std::clamp(parameters.speed, 0, kNumSpeeds - 1);
    if (parameters_.model != current_model_) {
      if (fading_model_ < 0) {
        fading_model_ = current_model_;
        fade_remaining_ = fade_length_;
      }
      current_model_ = parameters_.model;
      if (fading_model_ == current_model_) {
        fading_model_ = -1;
        fade_remaining_ = 0;
      }
    }
  }

  void Engine::updateSpeeds(int samples) {
    // Each rotor approaches its target speed with its own time constant,
    // speeding up and slowing down at different rates.
    const double scale = parameters_.inertia / 0.5;
    for (int i = 0; i < kNumRotors; ++i) {
      auto& rotor = rotors_[i];
      const RotorSpec& spec = i == kHorn ? kHornSpec : i == kOrganDrum ? kOrganDrumSpec : kGuitarDrumSpec;
      const double target = parameters_.speed == kFast ? spec.fast_hz
                          : parameters_.speed == kSlow ? spec.slow_hz : 0.0;
      const double seconds = (target > rotor.hz ? spec.speed_up_seconds : spec.slow_down_seconds) * scale;
      const double coefficient = 1.0 - std::pow(1.0 - coefficientFor(seconds, sample_rate_), samples);
      rotor.hz += (target - rotor.hz) * coefficient;
      if (std::abs(target - rotor.hz) < 1.0e-6) rotor.hz = target;
    }
  }

  void Engine::write(Rotor& rotor, double input) {
    rotor.line[rotor.write] = static_cast<float>(input);
    if (++rotor.write == static_cast<int>(rotor.line.size())) rotor.write = 0;
  }

  double Engine::readLine(const Rotor& rotor, double delay_samples) const {
    const int size = static_cast<int>(rotor.line.size());
    double position = rotor.write - 1 - std::clamp(delay_samples, 1.0, size - 4.0);
    if (position < 0) position += size;
    const int i = static_cast<int>(position);
    const double t = position - i;
    const auto at = [&](int index) { return static_cast<double>(rotor.line[(index + size) % size]); };
    const double y0 = at(i - 1), y1 = at(i), y2 = at(i + 1), y3 = at(i + 2);
    return ((( -0.5 * y0 + 1.5 * y1 - 1.5 * y2 + 0.5 * y3) * t +
             (y0 - 2.5 * y1 + 2.0 * y2 - 0.5 * y3)) * t + (-0.5 * y0 + 0.5 * y2)) * t + y1;
  }

  double Engine::amplifier(double x) const {
    // Tube amplifier: a slightly biased soft clip. Drive raises the level into
    // it; the output is scaled back so quiet passages keep their level.
    constexpr double kBias = 0.1;
    const double gain = std::pow(10.0, drive_ * 24.0 / 20.0);
    return (std::tanh(x * gain + kBias) - std::tanh(kBias)) / (gain * (1.0 - std::tanh(kBias) * std::tanh(kBias)));
  }

  // Resynchronises the per-sample phasor with the exact angle and sets the
  // per-sample rotation for this block's speed.
  void Engine::beginBlock(Rotor& rotor) {
    rotor.cosine = std::cos(2.0 * kPi * rotor.angle);
    rotor.sine = std::sin(2.0 * kPi * rotor.angle);
    const double step = 2.0 * kPi * rotor.hz / sample_rate_;
    rotor.step_cosine = std::cos(step);
    rotor.step_sine = std::sin(step);
  }

  // What one microphone hears of one rotor: the source on the rotor at its
  // radius, delayed by its distance (Doppler) and dimmed and darkened as it
  // turns away, plus its reflection from the back of the cabinet (the opposite
  // side of the rotation). facing_cosine: cos of the rotor's angle to the mic.
  double Engine::hear(Rotor& rotor, const RotorSpec& spec, int microphone, double facing_cosine) {
    const double radius_samples = spec.radius_metres / kSpeedOfSound * sample_rate_;
    const double reflection_samples = kReflectionSeconds * sample_rate_;
    const double rear = coefficientFor(1.0 / (2.0 * kPi * spec.rear_lowpass_hz), sample_rate_);
    double out = 0.0;
    for (int source = 0; source < 2; ++source) {
      const double cosine = source ? -facing_cosine : facing_cosine;
      const double facing = 0.5 + 0.5 * cosine;
      const double delay = radius_samples * (1.0 - cosine) + (source ? reflection_samples : 0.0) + 1.0;
      const double sound = readLine(rotor, delay);
      // The rear-facing part reaches the mic dimmed and through a low pass.
      auto& state = rotor.rear_state[microphone];
      if (source == 0) state += (sound - state) * rear;
      const double heard = facing * sound + (1.0 - facing) * (1.0 - spec.directivity) * (source == 0 ? state : sound);
      out += (source ? kReflectionGain : 1.0) * heard;
    }
    return out;
  }

  void Engine::renderModel(int model, const float* mono, float* left, float* right, int count) {
    if (model == kWhirl) {
      // OpenB3's output is horn plus drum at each microphone.
      float horn_left[128], horn_right[128], drum_left[128], drum_right[128];
      whirl_->setMicAngle(180.0 * spread_);
      const double horn_gain = std::min(1.0, 1.0 + balance_), drum_gain = std::min(1.0, 1.0 - balance_);
      for (int offset = 0; offset < count; offset += 128) {
        const int block = std::min(128, count - offset);
        whirl_->process(mono + offset, horn_left, horn_right, drum_left, drum_right, block);
        for (int n = 0; n < block; ++n) {
          left[offset + n] = static_cast<float>(horn_gain * horn_left[n] + drum_gain * drum_left[n]);
          right[offset + n] = static_cast<float>(horn_gain * horn_right[n] + drum_gain * drum_right[n]);
        }
      }
      return;
    }

    // Microphones at +/- the spread angle from the front of the cabinet.
    const double mic_angle = 0.5 * kPi * spread_;
    const double mic_cosine = std::cos(mic_angle), mic_sine = std::sin(mic_angle);
    const auto facing = [&](const Rotor& rotor, int microphone) {
      // cos(rotor angle - mic angle), mics at -angle (left) and +angle (right).
      return rotor.cosine * mic_cosine + rotor.sine * mic_sine * (microphone ? 1.0 : -1.0);
    };
    const auto advance = [](Rotor& rotor) {
      const double c = rotor.cosine * rotor.step_cosine - rotor.sine * rotor.step_sine;
      rotor.sine = rotor.sine * rotor.step_cosine + rotor.cosine * rotor.step_sine;
      rotor.cosine = c;
    };
    if (model == kOrgan) { beginBlock(rotors_[kHorn]); beginBlock(rotors_[kOrganDrum]); }
    else beginBlock(rotors_[kGuitarDrum]);

    for (int n = 0; n < count; ++n) {
      const double driven = amplifier(mono[n]);
      double l = 0.0, r = 0.0;
      if (model == kOrgan) {
        // 12 dB/octave crossover: treble to the horn, bass to the drum, whose
        // lowest octaves pass the rotating drum almost unchanged.
        const double treble = horn_voice_.tick(highpass_[0].tick(driven));
        const double bass = lowpass_[0].tick(driven);
        const double still = lowpass_[1].tick(bass);
        Rotor& horn = rotors_[kHorn];
        Rotor& drum = rotors_[kOrganDrum];
        write(horn, treble);
        write(drum, bass - still);
        const double horn_gain = std::min(1.0, 1.0 + balance_), drum_gain = std::min(1.0, 1.0 - balance_);
        const double leak = horn_gain * kHornLeak * treble + drum_gain * still;
        l = leak + horn_gain * hear(horn, kHornSpec, 0, facing(horn, 0)) + drum_gain * hear(drum, kOrganDrumSpec, 0, facing(drum, 0));
        r = leak + horn_gain * hear(horn, kHornSpec, 1, facing(horn, 1)) + drum_gain * hear(drum, kOrganDrumSpec, 1, facing(drum, 1));
        advance(horn);
        advance(drum);
      }
      else {
        Rotor& drum = rotors_[kGuitarDrum];
        write(drum, highpass_[1].tick(driven));
        l = hear(drum, kGuitarDrumSpec, 0, facing(drum, 0));
        r = hear(drum, kGuitarDrumSpec, 1, facing(drum, 1));
        advance(drum);
      }
      left[n] = static_cast<float>(l);
      right[n] = static_cast<float>(r);
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
      drive_ += (parameters_.drive - drive_) * block_smoothing;
      balance_ += (parameters_.balance - balance_) * block_smoothing;
      spread_ += (parameters_.spread - spread_) * block_smoothing;
      updateSpeeds(block);
      whirl_->setSpeed(parameters_.speed);
      whirl_->setInertia(parameters_.inertia / 0.5);

      renderModel(current_model_, mono, wet_left, wet_right, block);
      if (fading_model_ >= 0) {
        float old_left[kBlock], old_right[kBlock];
        renderModel(fading_model_, mono, old_left, old_right, block);
        for (int n = 0; n < block; ++n) {
          const double t = std::clamp(1.0 - double(fade_remaining_ - n) / fade_length_, 0.0, 1.0);
          const double in_gain = std::sin(0.5 * kPi * t), out_gain = std::cos(0.5 * kPi * t);
          wet_left[n] = static_cast<float>(wet_left[n] * in_gain + old_left[n] * out_gain);
          wet_right[n] = static_cast<float>(wet_right[n] * in_gain + old_right[n] * out_gain);
        }
        fade_remaining_ = std::max(0, fade_remaining_ - block);
        if (fade_remaining_ == 0) fading_model_ = -1;
      }

      // The exact angles advance with the block; the phasors resync to them.
      for (auto& rotor : rotors_) {
        rotor.angle += rotor.hz * block / sample_rate_;
        rotor.angle -= std::floor(rotor.angle);
      }

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
