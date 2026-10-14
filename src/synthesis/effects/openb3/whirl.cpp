// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from setBfree's whirl.c and eqcomp.c as used by OpenB3 (GPL v2 or
// later): Copyright (C) 2003-2004 Fredrik Kilander, 2008-2018 Robin Gareus,
// 2010 Ken Restivo, 2012 Will Panther. See README.atlas.md.
#include "whirl.h"

#include <algorithm>
#include <cmath>

namespace atlas::openb3 {
  namespace {
    constexpr double kPi = 3.14159265358979323846;
    constexpr unsigned int kDisplacementMask = Whirl::kDisplacementSize - 1;
    constexpr unsigned int kMinimumBuffer = 1 << 11;  // WHIRL_BUF_SIZE_SAMPLES
    constexpr int kHistoryMask = 7;                   // AGMASK
    constexpr double kDenormalHack = 1e-14;           // cfgParser.h's DENORMAL_HACK

    // eqcomp.h's filter types and coefficient slots.
    enum { kLowPass = 0, kHighPass, kBandPass0, kBandPass1, kNotch, kAllPass, kPeaking, kLowShelf, kHighShelf };
    enum { kB0, kB1, kB2, kA0, kA1, kA2 };
    // whirl.h's filterCoeff.
    enum { a0, a1, a2, b0, b1, b2, z0, z1 };

    // initValues(): the rotors (Herrera, Hanson & Abel) and the cabinet.
    constexpr float kHornRpmSlow = 60.0 * 0.672;
    constexpr float kHornRpmFast = 60.0 * 7.056;
    constexpr float kDrumRpmSlow = 60.0 * 0.600;
    constexpr float kDrumRpmFast = 60.0 * 5.955;
    constexpr float kHornAcceleration = 0.161f, kHornDeceleration = 0.321f;
    constexpr float kDrumAcceleration = 4.127f, kDrumDeceleration = 1.371f;
    constexpr float kAirSpeed = 340.0f;
    constexpr float kMicDistanceCm = 42.0f;
    constexpr float kHornXOffsetCm = 0.0f, kHornZOffsetCm = 0.0f;
    constexpr float kHornRadiusCm = 19.2f, kDrumRadiusCm = 22.0f;
    constexpr double kDrumFilterHz = 811.9695, kDrumFilterQ = 1.6016, kDrumFilterGain = -38.9291;
    constexpr float kHornFilterAHz = 4500, kHornFilterAQ = 2.7456, kHornFilterAGain = -30.0;
    constexpr float kHornFilterBHz = 300.0, kHornFilterBQ = 1.0, kHornFilterBGain = -30.0;
    constexpr float kHornLevel = 0.7f, kLeakLevel = 0.15f;

    // eqCompute(): RBJ cookbook biquads.
    void compute(int type, double hz, double q, double gain, double* c, double rate) {
      const double A = std::pow(10.0, (gain / 40.0));
      const double omega = (2.0 * kPi * hz) / rate;
      const double sn = std::sin(omega);
      const double cs = std::cos(omega);
      const double alpha = sn / (2.0 * q);
      const double beta = std::sqrt(A) / q;
      switch (type) {
        case kLowPass:
          c[kB0] = (1.0 - cs) / 2.0; c[kB1] = 1.0 - cs; c[kB2] = (1.0 - cs) / 2.0;
          c[kA0] = 1.0 + alpha; c[kA1] = -2.0 * cs; c[kA2] = 1.0 - alpha;
          break;
        case kHighPass:
          c[kB0] = (1.0 + cs) / 2.0; c[kB1] = -(1.0 + cs); c[kB2] = (1.0 + cs) / 2.0;
          c[kA0] = 1.0 + alpha; c[kA1] = -2.0 * cs; c[kA2] = 1.0 - alpha;
          break;
        case kBandPass0:
          c[kB0] = sn / 2.0; c[kB1] = 0.0; c[kB2] = -sn / 2.0;
          c[kA0] = 1.0 + alpha; c[kA1] = -2.0 * cs; c[kA2] = 1.0 - alpha;
          break;
        case kBandPass1:
          c[kB0] = alpha; c[kB1] = 0.0; c[kB2] = -alpha;
          c[kA0] = 1.0 + alpha; c[kA1] = -2.0 * cs; c[kA2] = 1.0 - alpha;
          break;
        case kNotch:
          c[kB0] = 1.0; c[kB1] = -2.0 * cs; c[kB2] = 1.0;
          c[kA0] = 1.0 + alpha; c[kA1] = -2.0 * cs; c[kA2] = 1.0 - alpha;
          break;
        case kAllPass:
          c[kB0] = 1.0 - alpha; c[kB1] = -2.0 * cs; c[kB2] = 1.0 + alpha;
          c[kA0] = 1.0 + alpha; c[kA1] = -2.0 * cs; c[kA2] = 1.0 - alpha;
          break;
        case kPeaking:
          c[kB0] = 1.0 + (alpha * A); c[kB1] = -2.0 * cs; c[kB2] = 1.0 - (alpha * A);
          c[kA0] = 1.0 + (alpha / A); c[kA1] = -2.0 * cs; c[kA2] = 1.0 - (alpha / A);
          break;
        case kLowShelf:
          c[kB0] = A * ((A + 1) - ((A - 1) * cs) + (beta * sn));
          c[kB1] = (2.0 * A) * ((A - 1) - ((A + 1) * cs));
          c[kB2] = A * ((A + 1) - ((A - 1) * cs) - (beta * sn));
          c[kA0] = (A + 1) + ((A - 1) * cs) + (beta * sn);
          c[kA1] = -2.0 * ((A - 1) + ((A + 1) * cs));
          c[kA2] = (A + 1) + ((A - 1) * cs) - (beta * sn);
          break;
        case kHighShelf:
          c[kB0] = A * ((A + 1) + ((A - 1) * cs) + (beta * sn));
          c[kB1] = -(2.0 * A) * ((A - 1) + ((A + 1) * cs));
          c[kB2] = A * ((A + 1) + ((A - 1) * cs) - (beta * sn));
          c[kA0] = (A + 1) - ((A - 1) * cs) + (beta * sn);
          c[kA1] = 2.0 * ((A - 1) - ((A + 1) * cs));
          c[kA2] = (A + 1) - ((A - 1) * cs) - (beta * sn);
          break;
      }
      c[kB0] /= c[kA0];
      c[kB1] /= c[kA0];
      c[kB2] /= c[kA0];
      c[kA1] /= c[kA0];
      c[kA2] /= c[kA0];
    }

    struct ComponentTables {
      std::vector<std::array<float, 5>> forward, backward;
    };

    // initTables(): the horn's angle-dependent response, five components
    // drawn from Smith et al.'s figure 8, linearly interpolated between the
    // given points around the circle and normalised to unit gain.
    ComponentTables buildComponents() {
      ComponentTables tables;
      tables.forward.assign(Whirl::kDisplacementSize, { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f });
      tables.backward.assign(Whirl::kDisplacementSize, { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f });
      auto& bfw = tables.forward;
      double ipx = 0.0, ipy = 0.0;
      const auto mark = [&](double degrees, double level) {
        ipx = degrees;
        ipy = level;
      };
      const auto draw = [&](double degrees, double level, int partial) {
        double d = ipx;
        while (d < 0.0)
          d += 360.0;
        const int from = static_cast<int>((d * static_cast<double>(Whirl::kDisplacementSize)) / 360.0);
        ipx = degrees;
        double e = ipx;
        while (e < d)
          e += 360.0;
        const int to = static_cast<int>((e * static_cast<double>(Whirl::kDisplacementSize)) / 360.0);
        const double range = static_cast<double>(to - from);
        for (int i = from; i <= to; ++i) {
          const double x = static_cast<double>(i - from);
          const double w = ipy + ((x / range) * (level - ipy));
          bfw[static_cast<size_t>(i) & kDisplacementMask][static_cast<size_t>(partial)] = static_cast<float>(w);
        }
        ipy = level;
      };

      mark(-180.0, 1.052);
      draw(-166.4, .881, 0); draw(-150.5, .881, 0); draw(-135.3, .881, 0); draw(-122.4, .792, 0);
      draw(-106.5, .792, 0); draw(-91.2, .836, 0); draw(-75.8, .881, 0); draw(-59.4, .851, 0);
      draw(-44.7, .941, 0); draw(-30.0, 1.298, 0); draw(-14.7, 2.119, 0); draw(0.0, 2.820, 0);
      draw(15.6, 2.313, 0); draw(30.0, 1.492, 0); draw(44.7, .926, 0); draw(60.0, .836, 0);
      draw(74.7, .866, 0); draw(90.6, .792, 0); draw(100.0, .777, 0); draw(105.0, .777, 0);
      draw(120.0, .836, 0); draw(135.3, .836, 0); draw(150.0, .881, 0); draw(164.5, .874, 0);
      draw(180.0, 1.052, 0);

      mark(-180.0, -0.07);
      draw(-150.0, 0.10, 1); draw(-135.0, -0.10, 1); draw(-122.2, 0.16, 1); draw(-105.0, 0.15, 1);
      draw(-91.2, 0.37, 1); draw(-75.3, 0.32, 1); draw(-60.1, 0.39, 1); draw(-44.5, 0.70, 1);
      draw(-30.0, 0.53, 1); draw(-12.0, -0.40, 1); draw(0.0, -0.81, 1); draw(2.7, -0.77, 1);
      draw(15.0, -0.52, 1); draw(33.1, 0.38, 1); draw(43.7, 0.68, 1); draw(57.7, 0.49, 1);
      draw(74.1, 0.19, 1); draw(89.4, 0.33, 1); draw(105.0, 0.03, 1); draw(120.0, 0.12, 1);
      draw(134.0, -0.13, 1); draw(153.3, 0.08, 1); draw(180.0, -0.07, 1);

      mark(-180.0, 0.40);
      draw(-165.0, 0.20, 2); draw(-150.0, 0.48, 2); draw(-135.0, 0.27, 2); draw(-121.2, 0.22, 2);
      draw(-89.2, 0.30, 2); draw(-69.2, 0.22, 2); draw(-58.0, 0.11, 2); draw(-40.2, -0.43, 2);
      draw(-29.0, -0.53, 2); draw(-15.6, -0.43, 2); draw(0.0, 0.00, 2); draw(14.3, -0.44, 2);
      draw(30.3, -0.60, 2); draw(60.3, 0.11, 2); draw(74.9, 0.32, 2); draw(91.5, 0.23, 2);
      draw(104.9, 0.32, 2); draw(121.7, 0.19, 2); draw(135.0, 0.27, 2); draw(150.0, 0.45, 2);
      draw(165.0, 0.20, 2); draw(180.0, 0.40, 2);

      mark(-180.0, -0.08);
      draw(-165.2, -0.19, 3); draw(-150.0, 0.00, 3); draw(-133.9, -0.20, 3); draw(-120.0, -0.15, 3);
      draw(-106.0, 0.09, 3); draw(-89.3, -0.15, 3); draw(-76.3, 0.00, 3); draw(-60.3, 0.29, 3);
      draw(-44.6, -0.02, 3); draw(-15.6, -0.22, 3); draw(0.0, 0.24, 3); draw(14.5, 0.11, 3);
      draw(30.1, -0.10, 3); draw(44.6, 0.17, 3); draw(60.4, 0.22, 3); draw(75.9, 0.16, 3);
      draw(90.4, -0.05, 3); draw(104.9, 0.07, 3); draw(122.8, -0.07, 3); draw(136.2, -0.07, 3);
      draw(150.0, 0.08, 3); draw(165.0, -0.19, 3); draw(180.0, -0.08, 3);

      mark(-180.0, 0.13);
      draw(-165.2, 0.00, 4); draw(-150.0, 0.17, 4); draw(-135.2, -0.20, 4); draw(-120.5, 0.00, 4);
      draw(-105.0, 0.00, 4); draw(-90.0, 0.04, 4); draw(-75.0, -0.09, 4); draw(-60.3, -0.14, 4);
      draw(-45.0, 0.16, 4); draw(-15.6, 0.00, 4); draw(0.0, 0.22, 4); draw(15.6, -0.21, 4);
      draw(30.1, -0.09, 4); draw(45.0, 0.10, 4); draw(60.3, -0.07, 4); draw(74.8, -0.15, 4);
      draw(90.4, -0.03, 4); draw(104.9, -0.14, 4); draw(120.5, 0.00, 4); draw(135.2, -0.26, 4);
      draw(150.0, 0.16, 4); draw(165.0, -0.02, 4); draw(180.0, 0.13, 4);

      double sum = 0.0;
      for (int i = 0; i < Whirl::kDisplacementSize; ++i) {
        double column = 0.0;
        for (int j = 0; j < 5; ++j)
          column += std::fabs(bfw[static_cast<size_t>(i)][static_cast<size_t>(j)]);
        if (sum < column)
          sum = column;
      }
      for (int i = 0; i < Whirl::kDisplacementSize; ++i) {
        for (int j = 0; j < 5; ++j) {
          float& value = bfw[static_cast<size_t>(i)][static_cast<size_t>(j)];
          value = static_cast<float>(value * (1.0 / sum));
          tables.backward[static_cast<size_t>(Whirl::kDisplacementSize - i - 1)][static_cast<size_t>(j)] = value;
        }
      }
      return tables;
    }

    const ComponentTables& components() {
      static const ComponentTables tables = buildComponents();
      return tables;
    }
  }

  Whirl::Whirl() {
    prepare(sample_rate_);
  }

  // setIIRFilter(): out-of-range settings leave the filter as it was.
  void Whirl::setFilter(Filter& filter, int type, double hz, double q, double gain, double rate) {
    if (q <= 0.1 || q >= 6.00 || hz / rate <= 0.0002 || hz / rate >= 0.4998 || gain <= -48.0 || gain >= 48.0 ||
        type < kLowPass || type > kHighShelf)
      return;
    double c[6];
    compute(type, hz, q, gain, c, rate);
    filter.w[a1] = static_cast<float>(c[kA1]);
    filter.w[a2] = static_cast<float>(c[kA2]);
    filter.w[b0] = static_cast<float>(c[kB0]);
    filter.w[b1] = static_cast<float>(c[kB1]);
    filter.w[b2] = static_cast<float>(c[kB2]);
  }

  void Whirl::prepare(double sample_rate) {
    sample_rate_ = sample_rate;
    components();

    // computeOffsets(): each rotor's distance to the microphones around
    // the circle, in samples.
    const double horn_radius = (kHornRadiusCm * sample_rate_ / 100.0) / kAirSpeed;
    const double drum_radius = (kDrumRadiusCm * sample_rate_ / 100.0) / kAirSpeed;
    const double mic_distance = (kMicDistanceCm * sample_rate_ / 100.0) / kAirSpeed;
    const double mic_x = (kHornXOffsetCm * sample_rate_ / 100.0) / kAirSpeed;
    const double mic_z = (kHornZOffsetCm * sample_rate_ / 100.0) / kAirSpeed;
    horn_forward_.assign(kDisplacementSize, 0.0f);
    horn_backward_.assign(kDisplacementSize, 0.0f);
    drum_forward_.assign(kDisplacementSize, 0.0f);
    drum_backward_.assign(kDisplacementSize, 0.0f);
    double max_horn = 0.0, max_drum = 0.0;
    for (int i = 0; i < kDisplacementSize; ++i) {
      const double v = (2.0 * kPi * static_cast<double>(i)) / static_cast<double>(kDisplacementSize);
      double a = mic_distance - (horn_radius * std::cos(v));
      double b = mic_z + horn_radius * std::sin(v);
      const double distance = std::sqrt((a * a) + (b * b));
      const size_t mirrored = static_cast<size_t>(kDisplacementSize - (i + 1));
      horn_forward_[static_cast<size_t>(i)] = static_cast<float>(distance + mic_x);
      horn_backward_[mirrored] = static_cast<float>(distance - mic_x);
      max_horn = std::max({ max_horn, static_cast<double>(horn_forward_[static_cast<size_t>(i)]),
                            static_cast<double>(horn_backward_[mirrored]) });
      a = mic_distance - (drum_radius * std::cos(v));
      b = drum_radius * std::sin(v);
      drum_forward_[static_cast<size_t>(i)] = static_cast<float>(std::sqrt((a * a) + (b * b)));
      drum_backward_[mirrored] = drum_forward_[static_cast<size_t>(i)];
      max_drum = std::max(max_drum, static_cast<double>(drum_forward_[static_cast<size_t>(i)]));
    }

    // Spacing between reflections in samples, normalised for 22.1 kHz.
    constexpr float kHornSpacing[6] = { 12.0f, 18.0f, 53.0f, 50.0f, 106.0f, 116.0f };
    constexpr float kDrumSpacing[6] = { 36.0f, 39.0f, 79.0f, 86.0f, 123.0f, 116.0f };
    constexpr int kSize = kDisplacementSize;
    horn_phase_ = { 0, kSize >> 1, (kSize * 2) / 6, (kSize * 5) / 6, (kSize * 1) / 6, (kSize * 4) / 6 };
    drum_phase_ = horn_phase_;
    double longest = 0.0;
    for (int i = 0; i < 6; ++i) {
      horn_spacing_[static_cast<size_t>(i)] =
          static_cast<float>(kHornSpacing[i] * sample_rate_ / 22100.0 + horn_radius + 1.0);
      drum_spacing_[static_cast<size_t>(i)] =
          static_cast<float>(kDrumSpacing[i] * sample_rate_ / 22100.0 + drum_radius + 1.0);
      longest = std::max({ longest, max_horn + horn_spacing_[static_cast<size_t>(i)],
                           max_drum + drum_spacing_[static_cast<size_t>(i)] });
    }
    // whirl.c's 2048 samples cover rates to about 200 kHz; higher rates get
    // a longer line.
    unsigned int size = kMinimumBuffer;
    while (size < longest + 4.0)
      size <<= 1;
    buffer_mask_ = size - 1;
    horn_left_.assign(size, 0.0f);
    horn_right_.assign(size, 0.0f);
    drum_left_.assign(size, 0.0f);
    drum_right_.assign(size, 0.0f);

    // computeRotationSpeeds(): revolutions per sample for each speed pair.
    const double horn_fast = kHornRpmFast / (sample_rate_ * 60.0);
    const double horn_slow = kHornRpmSlow / (sample_rate_ * 60.0);
    const double drum_fast = kDrumRpmFast / (sample_rate_ * 60.0);
    const double drum_slow = kDrumRpmSlow / (sample_rate_ * 60.0);
    rev_options_ = { { { 0.0, 0.0 }, { 0.0, drum_slow }, { 0.0, drum_fast },
                       { horn_slow, 0.0 }, { horn_slow, drum_slow }, { horn_slow, drum_fast },
                       { horn_fast, 0.0 }, { horn_fast, drum_slow }, { horn_fast, drum_fast } } };
    reset(false);
  }

  void Whirl::reset(bool spinning) {
    std::fill(horn_left_.begin(), horn_left_.end(), 0.0f);
    std::fill(horn_right_.begin(), horn_right_.end(), 0.0f);
    std::fill(drum_left_.begin(), drum_left_.end(), 0.0f);
    std::fill(drum_right_.begin(), drum_right_.end(), 0.0f);
    history_0_.fill(0.0f);
    history_1_.fill(0.0f);
    history_2_.fill(0.0f);
    history_index_0_ = history_index_1_ = history_index_2_ = 0;
    out_position_ = 0;
    z_.fill(0.0f);
    fragment_position_ = 0;

    // initialize().
    horn_level_ = kHornLevel;
    leakage_ = kLeakLevel * kHornLevel;
    drum_filter_left_ = drum_filter_right_ = horn_filter_a_ = horn_filter_b_ = Filter();
    setFilter(drum_filter_left_, kHighShelf, kDrumFilterHz, kDrumFilterQ, kDrumFilterGain, sample_rate_);
    setFilter(drum_filter_right_, kHighShelf, kDrumFilterHz, kDrumFilterQ, kDrumFilterGain, sample_rate_);
    setFilter(horn_filter_a_, kLowPass, kHornFilterAHz, kHornFilterAQ, kHornFilterAGain, sample_rate_);
    setFilter(horn_filter_b_, kLowShelf, kHornFilterBHz, kHornFilterBQ, kHornFilterBGain, sample_rate_);

    horn_angle_ = drum_angle_ = 0.0;
    horn_increment_ = drum_increment_ = 0.0;
    horn_acdc_ = drum_acdc_ = 0;
    const int speed = speed_;
    speed_ = -1;
    setSpeed(speed);
    if (spinning) {
      horn_increment_ = horn_target_;
      drum_increment_ = drum_target_;
      horn_acdc_ = drum_acdc_ = 0;
    }
  }

  void Whirl::useRevOption(int option) {
    const int i = option % 9;
    horn_target_ = rev_options_[static_cast<size_t>(i)][0];
    drum_target_ = rev_options_[static_cast<size_t>(i)][1];
    if (horn_increment_ < horn_target_)
      horn_acdc_ = 1;
    else if (horn_target_ < horn_increment_)
      horn_acdc_ = -1;
    if (drum_increment_ < drum_target_)
      drum_acdc_ = 1;
    else if (drum_target_ < drum_increment_)
      drum_acdc_ = -1;
  }

  void Whirl::setSpeed(int speed) {
    speed = std::clamp(speed, 0, kNumSpeeds - 1);
    if (speed == speed_)
      return;
    speed_ = speed;
    // revselects[]: both slow (4), stopped (0), both fast (8).
    useRevOption(speed == kFast ? 8 : speed == kSlow ? 4 : 0);
  }

  void Whirl::setInertia(double scale) {
    inertia_ = std::max(0.0, scale);
    horn_acceleration_ = static_cast<float>(kHornAcceleration * inertia_);
    horn_deceleration_ = static_cast<float>(kHornDeceleration * inertia_);
    drum_acceleration_ = static_cast<float>(kDrumAcceleration * inertia_);
    drum_deceleration_ = static_cast<float>(kDrumDeceleration * inertia_);
  }

  void Whirl::setMicAngle(double degrees) {
    mic_angle_ = 1.0 - std::clamp(degrees, 0.0, 180.0) / 180.0;
  }

  // whirlProc2()'s speed changes: first order toward the target, once per
  // fragment.
  void Whirl::updateSpeeds() {
    const double fragment = static_cast<double>(kFragment);
    if (horn_acdc_) {
      const double l = std::exp(-1.0 / (sample_rate_ / fragment *
                                         (horn_acdc_ > 0 ? horn_acceleration_ : horn_deceleration_)));
      horn_increment_ += (1 - l) * (horn_target_ - horn_increment_);
      if (std::fabs(horn_target_ - horn_increment_) < (.05 / (60.f * sample_rate_))) {
        horn_acdc_ = 0;
        horn_increment_ = horn_target_;
      }
    }
    if (drum_acdc_) {
      const double l = std::exp(-1.0 / (sample_rate_ / fragment *
                                         (drum_acdc_ > 0 ? drum_acceleration_ : drum_deceleration_)));
      drum_increment_ += (1 - l) * (drum_target_ - drum_increment_);
      if (std::fabs(drum_target_ - drum_increment_) < (.05 / (60.f * sample_rate_))) {
        drum_acdc_ = 0;
        drum_increment_ = drum_target_;
      }
    }
  }

  void Whirl::process(const float* input, float* horn_left, float* horn_right, float* drum_left, float* drum_right,
                      int samples) {
    for (int offset = 0; offset < samples;) {
      if (fragment_position_ == 0)
        updateSpeeds();
      const int count = std::min(samples - offset, kFragment - fragment_position_);
      renderFragment(input + offset, horn_left + offset, horn_right + offset, drum_left + offset,
                     drum_right + offset, count);
      fragment_position_ = (fragment_position_ + count) % kFragment;
      offset += count;
    }
  }

  void Whirl::renderFragment(const float* input, float* out_horn_left, float* out_horn_right,
                             float* out_drum_left, float* out_drum_right, int samples) {
    double horn_angle = horn_angle_;
    double drum_angle = drum_angle_;
    unsigned int outpos = out_position_;
    const double forward_angle = mic_angle_ * .25;
    const double backward_angle = 1. + mic_angle_ * -.25;
    const float leakage = leakage_;
    const float horn_level = horn_level_;
    const double horn_increment = horn_increment_;
    const double drum_increment = drum_increment_;
    const unsigned int mask = buffer_mask_;
    const auto& bfw = components().forward;
    const auto& bbw = components().backward;
    float* const HLbuf = horn_left_.data();
    float* const HRbuf = horn_right_.data();
    float* const DLbuf = drum_left_.data();
    float* const DRbuf = drum_right_.data();
    float* const z = z_.data();

    const auto add_history = [](std::array<float, 8>& history, int& index, float sample) {
      index = (index + kHistoryMask) & kHistoryMask;
      history[static_cast<size_t>(index)] = sample;
    };
    const auto iir = [](Filter& filter, float x) {
      float* W = filter.w;
      const float temp = x - (W[a1] * W[z0]) - (W[a2] * W[z1]);
      const float y = (temp * W[b0]) + (W[b1] * W[z0]) + (W[b2] * W[z1]);
      W[z1] = W[z0];
      W[z0] = temp;
      return y;
    };

    for (int i = 0; i < samples; ++i) {
      unsigned int n;
      float x = static_cast<float>(static_cast<double>(input[i]) + kDenormalHack);
      const float xx = x;

      // HN_MOTION(): the horn's path P to a microphone, Doppler delayed and
      // shaped by the response for its angle.
      const auto horn_motion = [&](int P, float* buffer, const std::vector<float>& displacement,
                                   const std::vector<std::array<float, 5>>& response,
                                   const std::array<float, 8>& history, int index, double angle) {
        const float h1 = static_cast<float>(angle * kDisplacementSize + horn_phase_[static_cast<size_t>(P)]);
        const float hd = std::fmod(h1, 1.f);
        const unsigned int hl = static_cast<unsigned int>(std::floor(h1)) & kDisplacementMask;
        const unsigned int hh = (hl + 1) & kDisplacementMask;
        const float intp = displacement[hl] * (1.f - hd) + hd * displacement[hh];
        const unsigned int k = static_cast<unsigned int>(std::round(h1)) & kDisplacementMask;
        const float t = horn_spacing_[static_cast<size_t>(P)] + intp + static_cast<float>(outpos);
        const float r = std::floor(t);
        const auto& bw = response[k];
        float xa;
        xa = bw[0] * x;
        xa += bw[1] * history[static_cast<size_t>(index)];
        xa += bw[2] * history[static_cast<size_t>((index + 1) & kHistoryMask)];
        xa += bw[3] * history[static_cast<size_t>((index + 2) & kHistoryMask)];
        xa += bw[4] * history[static_cast<size_t>((index + 3) & kHistoryMask)];
        const float q = xa * (t - r);
        n = static_cast<unsigned int>(r) & mask;
        buffer[n] += xa - q;
        n = (n + 1) & mask;
        buffer[n] += q;
      };
      // DR_MOTION(): the drum's path P to a microphone.
      const auto drum_motion = [&](int P, float* buffer, const std::vector<float>& displacement) {
        const float d1 = static_cast<float>(drum_angle * kDisplacementSize + drum_phase_[static_cast<size_t>(P)]);
        const float dd = std::fmod(d1, 1.f);
        const unsigned int dl = static_cast<unsigned int>(std::floor(d1)) & kDisplacementMask;
        const unsigned int dh = (dl + 1) & kDisplacementMask;
        const float intp = displacement[dl] * (1.f - dd) + dd * displacement[dh];
        const float t = drum_spacing_[static_cast<size_t>(P)] + intp + static_cast<float>(outpos);
        const float r = std::floor(t);
        const float q = x * (t - r);
        n = static_cast<unsigned int>(r) & mask;
        buffer[n] += x - q;
        n = (n + 1) & mask;
        buffer[n] += q;
      };
      // FILTER_C(): a two-point average before each reflection.
      const auto reflection_filter = [&](int I) {
        const float temp = x;
        x = static_cast<float>((0.4 * x) + (0.4 * z[I]));
        z[I] = temp;
      };

      // The horn's feed: filters A and B; part of it leaks out unturned.
      x = iir(horn_filter_a_, x);
      x = iir(horn_filter_b_, x);
      const float leak = x * leakage;

      horn_motion(0, HLbuf, horn_forward_, bbw, history_0_, history_index_0_, horn_angle + forward_angle);
      horn_motion(1, HRbuf, horn_backward_, bfw, history_0_, history_index_0_, horn_angle + backward_angle);
      add_history(history_0_, history_index_0_, x);
      reflection_filter(0);
      horn_motion(2, HLbuf, horn_backward_, bfw, history_1_, history_index_1_, horn_angle + forward_angle);
      horn_motion(3, HRbuf, horn_forward_, bbw, history_1_, history_index_1_, horn_angle + backward_angle);
      add_history(history_1_, history_index_1_, x);
      reflection_filter(1);
      horn_motion(4, HLbuf, horn_forward_, bbw, history_2_, history_index_2_, horn_angle + forward_angle);
      horn_motion(5, HRbuf, horn_backward_, bfw, history_2_, history_index_2_, horn_angle + backward_angle);
      add_history(history_2_, history_index_2_, x);

      // The drum hears the input unfiltered.
      x = xx;
      drum_motion(0, DLbuf, drum_forward_);
      drum_motion(1, DRbuf, drum_backward_);
      reflection_filter(2);
      drum_motion(2, DLbuf, drum_backward_);
      drum_motion(3, DRbuf, drum_forward_);
      reflection_filter(3);
      drum_motion(4, DLbuf, drum_forward_);
      drum_motion(5, DRbuf, drum_backward_);

      out_drum_left[i] = iir(drum_filter_left_, DLbuf[outpos]);
      out_horn_left[i] = horn_level * HLbuf[outpos] + leak;
      out_drum_right[i] = iir(drum_filter_right_, DRbuf[outpos]);
      out_horn_right[i] = horn_level * HRbuf[outpos] + leak;

      HLbuf[outpos] = 0.0f;
      HRbuf[outpos] = 0.0f;
      DLbuf[outpos] = 0.0f;
      DRbuf[outpos] = 0.0f;

      outpos = (outpos + 1) & mask;
      horn_angle = std::fmod(horn_angle + horn_increment, 1.0);
      drum_angle = std::fmod(drum_angle + drum_increment, 1.0);
    }

    for (Filter* filter : { &horn_filter_a_, &horn_filter_b_, &drum_filter_left_, &drum_filter_right_ }) {
      if (std::isnan(filter->w[z0])) filter->w[z0] = 0;
      if (std::isnan(filter->w[z1])) filter->w[z1] = 0;
    }
    for (float& value : z_) {
      if (std::isnan(value)) value = 0;
    }

    horn_angle_ = horn_angle;
    drum_angle_ = drum_angle;
    out_position_ = outpos;
  }
}
