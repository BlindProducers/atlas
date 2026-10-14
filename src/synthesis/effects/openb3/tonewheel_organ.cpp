// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from setBfree's tonegen.c as used by OpenB3 (GPL v2 or later):
// Copyright (C) 2003-2004 Fredrik Kilander, 2008-2018 Robin Gareus,
// 2012 Will Panther. See README.atlas.md.
#include "tonewheel_organ.h"

#include <algorithm>
#include <cmath>

namespace atlas::openb3 {
  namespace {
    constexpr double kTwoPi = 6.28318530717958647692;
    // tonegen.c's percussion: osc.perc.gain scales the envelope's start,
    // osc.perc.soft lowers it; the drawbars play at 0.60512 while Normal
    // percussion is on (percDrawbarNormalGain).
    constexpr float kPercussionScaling = 11.0f;
    constexpr float kPercussionSoftReset = 0.5012f;
    constexpr float kPercussionNormalDrawbarGain = 0.60512f;
    // Per-sample decays at 48 kHz (percEnvGainDecayFast* and Slow*; OpenB3
    // never recomputes them from osc.perc.fast and .slow).
    constexpr float kPercussionFastDecay = 0.9995f;
    constexpr float kPercussionSlowDecay = 0.9999f;

    uint32_t next(uint32_t& state) {
      state ^= state << 13;
      state ^= state >> 17;
      state ^= state << 5;
      return state;
    }
  }

  int fragmentLength(double sample_rate) {
    return std::clamp(static_cast<int>(std::lround(kReferenceFragment * sample_rate / kReferenceRate)), 1, kMaxFragment);
  }

  int makeContactEnvelopes(float* envelopes, bool closing, float amount, double sample_rate, int fragment,
                           uint32_t& state) {
    if (state == 0)
      state = 0x9e3779b9u;
    amount = std::clamp(amount, 0.0f, 1.0f);
    // Up to 50%, how sharp the contacts are; past it, how worn.
    const double sharp = std::min(1.0, amount / 0.5);
    const double wear = std::max(0.0, (amount - 0.5) / 0.5);
    const double base_level = closing ? 0.5 : 0.25;
    const double level = wear > 0.0 ? base_level + wear * (1.0 - base_level) : base_level * sharp;
    const double stretch = 1.0 + 7.0 * wear;
    // The two-point filter's weight on the newest sample: 0.5 in tonegen.c.
    const double smoothing = 0.5 + 0.5 * wear;

    // initToneGenerator() and initEnvelopes(): the burst lengths.
    double minimum, maximum;
    if (closing) {
      minimum = std::min<double>(fragment, std::floor(sample_rate * 8.0 / 22050.0));
      maximum = std::min<double>(fragment, std::ceil(sample_rate * 40.0 / 22050.0));
    }
    else {
      minimum = 8.0 * fragment / kReferenceFragment;
      maximum = 40.0 * fragment / kReferenceFragment;
    }
    minimum *= stretch;
    maximum *= stretch;
    // A fragment at 50%, as in tonegen.c. Below, the contacts close through
    // a smooth ramp that widens to four fragments (about 5 ms) at 0%; above,
    // worn contacts chatter on past a fragment.
    const int length = std::clamp(wear > 0.0 ? std::max(fragment, static_cast<int>(std::ceil(maximum)) + fragment / 4)
                                             : static_cast<int>(std::lround(fragment * (1.0 + 3.0 * (1.0 - sharp)))),
                                  2, kMaxContactLength);
    const int ramp = std::max(1, static_cast<int>(std::lround((length - 1) * (1.0 - sharp))) + 1);

    for (int b = 0; b < kNumClickEnvelopes; ++b) {
      float* envelope = envelopes + b * length;
      const int bound = std::max(1, static_cast<int>(std::lround(maximum - minimum)));
      int burst = static_cast<int>(std::lround(minimum)) + static_cast<int>(next(state) % static_cast<uint32_t>(bound));
      burst = std::clamp(burst, 1, length - 1);
      const int span = std::max(1, length - std::max(burst, ramp));
      const int start = static_cast<int>(next(state) % static_cast<uint32_t>(span));
      double filtered = 0.0;
      for (int i = 0; i < length; ++i) {
        // The contact closes (or opens) through a raised cosine of `ramp`
        // samples from its moment; at 50% that is tonegen.c's step.
        double closed = 0.0;
        if (i >= start) {
          const double t = std::min(1.0, static_cast<double>(i - start + 1) / ramp);
          closed = 0.5 - 0.5 * std::cos(kTwoPi * 0.5 * t);
        }
        // With noise while it makes contact.
        const double raw = i >= start && i < start + burst ? closed * (1.0 - level * (next(state) / 4294967295.0))
                                                           : closed;
        filtered = i == 0 ? raw * smoothing : filtered + smoothing * (raw - filtered);
        envelope[i] = static_cast<float>(filtered);
      }
    }
    return length;
  }

  // ---- Organ -----------------------------------------------------------------

  void Organ::prepare(double sample_rate) {
    sample_rate_ = sample_rate;
    fragment_ = openb3::fragmentLength(sample_rate);
    key_compression_.assign(4096, 1.0f);
    percussion_.assign(4096, 0.0f);
    reset();
  }

  void Organ::reset() {
    time_ = next_time_ = 0.0;
    samples_ = 0;
    keys_before_ = 0;
    checked_in_ = 0;
    key_compression_level_ = 1.0f;
    key_compression_delta_ = 0.0f;
    percussion_gain_ = 0.0f;
    trigger_offset_ = 0;
  }

  void Organ::setPercussion(bool soft, bool fast) {
    percussion_soft_ = soft;
    percussion_fast_ = fast;
  }

  void Organ::fillPercussion(int from) {
    const float base = percussion_fast_ ? kPercussionFastDecay : kPercussionSlowDecay;
    const float decay = static_cast<float>(std::pow(static_cast<double>(base), kReferenceRate / sample_rate_));
    float gain = percussion_gain_;
    for (int n = from; n < samples_; ++n) {
      percussion_[static_cast<size_t>(n)] = gain;
      gain *= decay;
    }
    percussion_gain_ = gain;
  }

  void Organ::beginBlock(int samples, int keys_before, int keys_now) {
    samples = std::max(0, samples);
    if (static_cast<size_t>(samples) > percussion_.size()) {
      percussion_.resize(static_cast<size_t>(samples));
      key_compression_.resize(static_cast<size_t>(samples));
    }
    time_ = next_time_;
    next_time_ += samples;
    samples_ = samples;
    keys_before_ = keys_before;

    // tonegen.c resets the percussion envelope after every fragment that ends
    // with no key down; the next key starts it from there.
    if (keys_before == 0) {
      percussion_gain_ = kPercussionScaling * (percussion_soft_ ? kPercussionSoftReset : 1.0f);
      trigger_offset_ = samples;
    }
    else
      trigger_offset_ = -1;
    fillPercussion(0);

    const float target = tables().key_compression[static_cast<size_t>(std::clamp(keys_now, 0, kKeyCompressionSize - 1))];
    long long position = std::llround(time_) % fragment_;
    for (int n = 0; n < samples; ++n) {
      if (position == 0)
        key_compression_delta_ = (target - key_compression_level_) / static_cast<float>(fragment_);
      key_compression_[static_cast<size_t>(n)] = key_compression_level_;
      key_compression_level_ += key_compression_delta_;
      if (++position == fragment_)
        position = 0;
    }
  }

  void Organ::keyPressed(int offset) {
    if (keys_before_ != 0 || offset >= trigger_offset_)
      return;
    trigger_offset_ = std::max(0, offset);
    percussion_gain_ = kPercussionScaling * (percussion_soft_ ? kPercussionSoftReset : 1.0f);
    fillPercussion(trigger_offset_);
  }

  // ---- Voice -----------------------------------------------------------------

  void Voice::clear() {
    active_ = held_ = false;
    num_wheels_ = 0;
    key_ = -1;
    ramp_ = kNoRamp;
    ramp_position_ = 0;
    output_from_ = output_to_ = 1.0f;
    percussion_previous_ = 0.0;
  }

  void Voice::targets(const Registration& registration) {
    float drawbar[kNumDrawbars];
    for (int d = 0; d < kNumDrawbars; ++d)
      drawbar[d] = static_cast<float>(std::clamp(registration.drawbars[static_cast<size_t>(d)], 0.0f, 8.0f) / 8.0);
    // Percussion takes over the 1' bus's contact as its trigger.
    if (registration.percussion)
      drawbar[kPercussionTriggerBus] = 0.0f;
    const int percussion_bus = registration.percussion_third ? kPercussionThirdBus : kPercussionSecondBus;
    for (int i = 0; i < num_wheels_; ++i) {
      Wheel& wheel = wheels_[static_cast<size_t>(i)];
      float sum = 0.0f;
      for (int d = 0; d < kNumDrawbars; ++d)
        sum += wheel.bus[static_cast<size_t>(d)] * drawbar[d];
      wheel.swell_to = sum;
      wheel.percussion_to = registration.percussion ? wheel.bus[static_cast<size_t>(percussion_bus)] : 0.0f;
    }
    output_to_ = registration.percussion && !registration.percussion_soft ? kPercussionNormalDrawbarGain : 1.0f;
  }

  float Voice::rampValue(const Wheel& wheel) const {
    if (ramp_ == kClickRamp)
      return envelopes_[static_cast<size_t>(wheel.envelope * ramp_length_ + ramp_position_)];
    if (ramp_ == kReleaseRamp)
      return release_envelopes_[static_cast<size_t>(wheel.envelope * ramp_length_ + ramp_position_)];
    return static_cast<float>(ramp_position_) / static_cast<float>(ramp_length_);
  }

  void Voice::startRamp(Ramp ramp) {
    // A ramp starts from wherever the previous one has got to.
    const float linear = ramp_ == kNoRamp ? 1.0f
                                          : static_cast<float>(ramp_position_) / static_cast<float>(ramp_length_);
    for (int i = 0; i < num_wheels_; ++i) {
      Wheel& wheel = wheels_[static_cast<size_t>(i)];
      const float e = ramp_ == kNoRamp ? 1.0f : rampValue(wheel);
      wheel.swell_from = wheel.swell_from + e * (wheel.swell_to - wheel.swell_from);
      wheel.percussion_from = wheel.percussion_from + e * (wheel.percussion_to - wheel.percussion_from);
    }
    output_from_ = output_from_ + linear * (output_to_ - output_from_);
    ramp_ = ramp;
    ramp_position_ = 0;
  }

  void Voice::noteOn(int key, double time, const Registration& registration, const float* envelopes, int length,
                     const Organ& organ, const double* frequencies) {
    key = std::clamp(key, 0, kNumKeys - 1);
    const bool fresh = !active_ || key != key_;
    if (fresh) {
      clear();
      key_ = key;
      for (const Contribution& contribution : tables().contributions[static_cast<size_t>(key)]) {
        if (num_wheels_ == 0 || wheels_[static_cast<size_t>(num_wheels_ - 1)].wheel != contribution.wheel) {
          if (num_wheels_ == kMaxWheelsPerKey)
            break;
          Wheel& wheel = wheels_[static_cast<size_t>(num_wheels_)];
          wheel = Wheel();
          wheel.wheel = contribution.wheel;
          wheel.envelope = num_wheels_ & (kNumClickEnvelopes - 1);
          ++num_wheels_;
        }
        wheels_[static_cast<size_t>(num_wheels_ - 1)].bus[static_cast<size_t>(contribution.bus)] = contribution.level;
      }
      time_ = time;
    }
    frequencies_ = frequencies;
    registration_ = registration;

    // The contacts close through the click: a new key's wheels from silence,
    // a sounding key's from where they are.
    startRamp(kClickRamp);
    fragment_ = organ.fragmentLength();
    ramp_length_ = std::clamp(length, 1, kMaxContactLength);
    std::copy(envelopes, envelopes + kNumClickEnvelopes * ramp_length_, envelopes_.begin());
    targets(registration);
    if (fresh)
      output_from_ = output_to_;
    active_ = held_ = true;
  }

  void Voice::noteOff(const float* envelopes, int length) {
    if (!active_ || !held_)
      return;
    held_ = false;
    startRamp(envelopes != nullptr ? kReleaseRamp : kLinearRamp);
    ramp_length_ = envelopes != nullptr ? std::clamp(length, 1, kMaxContactLength) : fragment_;
    if (envelopes != nullptr)
      std::copy(envelopes, envelopes + kNumClickEnvelopes * ramp_length_, release_envelopes_.begin());
    for (int i = 0; i < num_wheels_; ++i) {
      Wheel& wheel = wheels_[static_cast<size_t>(i)];
      wheel.swell_to = 0.0f;
      wheel.percussion_to = 0.0f;
    }
    output_to_ = output_from_;
  }

  void Voice::setRegistration(const Registration& registration, const double* frequencies) {
    frequencies_ = frequencies;
    const bool same = registration.drawbars == registration_.drawbars &&
                      registration.percussion == registration_.percussion &&
                      registration.percussion_soft == registration_.percussion_soft &&
                      registration.percussion_third == registration_.percussion_third;
    registration_ = registration;
    if (same || !active_ || !held_)
      return;
    startRamp(kLinearRamp);
    ramp_length_ = fragment_;
    targets(registration);
  }

  void Voice::setPitchRatio(double ratio) {
    ratio_ = ratio;
  }

  void Voice::syncPhasors(double sample_rate) {
    const double nyquist = 0.5 * sample_rate;
    for (int i = 0; i < num_wheels_; ++i) {
      Wheel& wheel = wheels_[static_cast<size_t>(i)];
      const double hz = frequencies_[wheel.wheel];
      const double cycles = hz / sample_rate * time_;
      const double angle = kTwoPi * (cycles - std::floor(cycles));
      wheel.cosine = std::cos(angle);
      wheel.sine = std::sin(angle);
      const double step = kTwoPi * hz * ratio_ / sample_rate;
      wheel.step_cosine = std::cos(step);
      wheel.step_sine = std::sin(step);
      // As tonegen.c's writeSamples, nothing at or above the Nyquist rate.
      wheel.audible = hz * ratio_ < nyquist;
    }
  }

  void Voice::render(const Organ& organ, double* output, int start, int end) {
    if (!active_ || frequencies_ == nullptr || end <= start)
      return;
    // Each call starts from the exact phases of the voice's clock.
    syncPhasors(organ.sampleRate());

    const float* key_compression = organ.keyCompression();
    const float* percussion = organ.percussionEnvelope();
    for (int n = start; n < end; ++n) {
      double swell = 0.0, tap = 0.0;
      float output_gain = output_to_;
      if (ramp_ != kNoRamp) {
        const float linear = static_cast<float>(ramp_position_) / static_cast<float>(ramp_length_);
        output_gain = output_from_ + linear * (output_to_ - output_from_);
        for (int i = 0; i < num_wheels_; ++i) {
          Wheel& wheel = wheels_[static_cast<size_t>(i)];
          const float e = rampValue(wheel);
          const float swell_gain = wheel.swell_from + e * (wheel.swell_to - wheel.swell_from);
          const float percussion_gain = wheel.percussion_from + e * (wheel.percussion_to - wheel.percussion_from);
          const double x = wheel.audible ? wheel.sine : 0.0;
          swell += x * swell_gain;
          tap += x * percussion_gain;
          const double c = wheel.cosine * wheel.step_cosine - wheel.sine * wheel.step_sine;
          wheel.sine = wheel.sine * wheel.step_cosine + wheel.cosine * wheel.step_sine;
          wheel.cosine = c;
        }
        if (++ramp_position_ >= ramp_length_) {
          ramp_ = kNoRamp;
          ramp_position_ = 0;
          for (int i = 0; i < num_wheels_; ++i) {
            Wheel& wheel = wheels_[static_cast<size_t>(i)];
            wheel.swell_from = wheel.swell_to;
            wheel.percussion_from = wheel.percussion_to;
          }
          output_from_ = output_to_;
        }
      }
      else {
        for (int i = 0; i < num_wheels_; ++i) {
          Wheel& wheel = wheels_[static_cast<size_t>(i)];
          const double x = wheel.audible ? wheel.sine : 0.0;
          swell += x * wheel.swell_to;
          tap += x * wheel.percussion_to;
          const double c = wheel.cosine * wheel.step_cosine - wheel.sine * wheel.step_sine;
          wheel.sine = wheel.sine * wheel.step_cosine + wheel.cosine * wheel.step_sine;
          wheel.cosine = c;
        }
      }
      // tonegen.c's HIPASS_PERCUSSION: the previous tap minus this one.
      const double high_passed = percussion_previous_ - tap;
      percussion_previous_ = tap;
      output[n] += static_cast<double>(output_gain) * key_compression[n] *
                   (swell + high_passed * percussion[n]);
      time_ += ratio_;
    }

    // Released and faded out: the contacts are open.
    if (!held_ && ramp_ == kNoRamp)
      clear();
  }
}
