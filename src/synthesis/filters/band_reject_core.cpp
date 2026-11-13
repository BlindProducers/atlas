// SPDX-License-Identifier: GPL-3.0-or-later
// Built outside the fast-math unity file and without FMA contraction
// (CMakeLists.txt): the script's arithmetic in double precision, operation
// for operation, so the reference test finds the script's samples and arm64
// and x64 round alike.
#include "band_reject_core.h"

#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace vital {
  namespace {
    constexpr double kPi = 3.141592653589793;
    constexpr double kLn2 = 0.6931471805599453;
    // The soft clip x - x^3 / 6.75, which reaches 1 at 1.5: the script's literal.
    constexpr double kSaturation = 0.148148148148148;

    double clampKnob(double x) { return x < 0.0 ? 0.0 : x > 1.0 ? 1.0 : x; }

    double saturate(double x) {
      const double a = std::fabs(x);
      if (a >= 1.5)
        return x > 0.0 ? 1.0 : -1.0;
      return x - (((x * x) * x) * kSaturation);
    }

    double flushed(double x) { return std::fabs(x) < DBL_MIN ? 0.0 : x; }
  } // namespace

  double BandRejectCore::exp2(double x) {
    // 2^n e^r with n the nearest integer, so |r| <= ln 2 / 2, where the
    // series to r^13 is within 1e-17 of e^r.
    static constexpr double kInverseFactorials[] = {
      1.0 / 6227020800.0, 1.0 / 479001600.0, 1.0 / 39916800.0, 1.0 / 3628800.0, 1.0 / 362880.0, 1.0 / 40320.0,
      1.0 / 5040.0, 1.0 / 720.0, 1.0 / 120.0, 1.0 / 24.0, 1.0 / 6.0, 0.5, 1.0
    };
    const double n = std::floor(x + 0.5);
    const double r = (x - n) * kLn2;
    double p = kInverseFactorials[0];
    for (int i = 1; i < 13; ++i)
      p = p * r + kInverseFactorials[i];
    p = p * r + 1.0;
    // 2^n from its exponent field. The cutoff and drive keep n far inside the normal range.
    int exponent = static_cast<int>(n);
    exponent = exponent < -1022 ? -1022 : exponent > 1023 ? 1023 : exponent;
    const uint64_t bits = static_cast<uint64_t>(exponent + 1023) << 52;
    double scale;
    std::memcpy(&scale, &bits, sizeof(scale));
    return p * scale;
  }

  double BandRejectCore::sin(double x) {
    // The series to x^15: its next term is below 1e-16 of sin x for x <= 0.8.
    const double u = x * x;
    double p = -1.0 / 1307674368000.0;
    p = p * u + 1.0 / 6227020800.0;
    p = p * u - 1.0 / 39916800.0;
    p = p * u + 1.0 / 362880.0;
    p = p * u - 1.0 / 5040.0;
    p = p * u + 1.0 / 120.0;
    p = p * u - 1.0 / 6.0;
    p = p * u + 1.0;
    return x * p;
  }

  double BandRejectCore::cos(double x) {
    // The series to x^16: its next term is below 1e-17 for x <= 0.8.
    const double u = x * x;
    double p = 1.0 / 20922789888000.0;
    p = p * u - 1.0 / 87178291200.0;
    p = p * u + 1.0 / 479001600.0;
    p = p * u - 1.0 / 3628800.0;
    p = p * u + 1.0 / 40320.0;
    p = p * u - 1.0 / 720.0;
    p = p * u + 1.0 / 24.0;
    p = p * u - 0.5;
    return p * u + 1.0;
  }

  double BandRejectCore::damping(double resonance) {
    // DigitalSvf: Q from 0.5 to 16 on the cube of the resonance, the damping its inverse.
    const double r = clampKnob(resonance);
    return 1.0 / (0.5 + 15.5 * ((r * r) * r));
  }

  void BandRejectCore::reset() {
    low_.low = low_.band = low_.previous = 0.0;
    high_.low = high_.band = high_.previous = 0.0;
  }

  void BandRejectCore::flushDenormals() {
    auto flush = [](Section& section) {
      section.low = flushed(section.low);
      section.band = flushed(section.band);
      section.previous = flushed(section.previous);
    };
    flush(low_);
    flush(high_);
  }

  void BandRejectCore::setCutoff(double midi, double width_percent, double sample_rate) {
    // In the script's knob units: the lowpass 0.64 + 0.6336 Var semitones
    // below the cutoff, the highpass as far above, both within the knob's range.
    const double knob = midi / kSemitones;
    const double spread = (0.64 + (0.6336 * width_percent)) / kSemitones;
    setSection(low_, clampKnob(knob - spread), sample_rate, false);
    setSection(high_, clampKnob(knob + spread), sample_rate, true);
  }

  void BandRejectCore::setSection(Section& section, double knob, double sample_rate, bool highpass) {
    const double cutoff = kMinCutoff * exp2(kOctaves * knob);
    // EEL2 reads $pi * fc / (2 * srate) as $pi * (fc / (2 * srate)).
    const double theta = kPi * (cutoff / (2.0 * sample_rate));
    const double sine = sin(theta);
    section.f = 2.0 * sine;
    // Where the two half steps would be unstable, Serum runs the section once
    // per sample with the coefficient sin(pi fc / fs), the damping 1 over
    // that and other output gains. The test at the most damping there is, 2,
    // says whether any damping gets there.
    section.once = (section.f * 2.0 >= 2.0) || (((section.f * section.f) + ((2.0 * section.f) * 2.0)) >= 4.0);
    if (!section.once)
      return;
    const double cosine = cos(theta);
    section.f1 = (2.0 * sine) * cosine;
    section.k1 = 1.0 / section.f1;
    // The lowpass plays lp + tan(theta) / 2 bp, the highpass cos(2 theta) / 4 hp.
    section.gain1 = highpass ? ((cosine - sine) * (cosine + sine)) / 4.0 : (sine / cosine) / 2.0;
  }

  template<bool highpass>
  double BandRejectCore::run(Section& section, double x, double k) {
    if (section.once && ((section.f * k >= 2.0) || (((section.f * section.f) + ((2.0 * section.f) * k)) >= 4.0))) {
      const double lp = section.low, bp = section.band;
      const double hp = (x - lp) - (section.k1 * bp);
      const double bpa = bp + (section.f1 * hp);
      section.band = bpa;
      section.low = lp + (section.f1 * bpa);
      return highpass ? section.gain1 * hp : lp + (section.gain1 * bpa);
    }
    // Two half steps on the held input, then 1/4 of the last half step of the
    // previous sample, 1/2 of the first and 1/4 of the second. The lowpass
    // plays the state before a half step, the highpass what it drove.
    double outputs[2];
    for (double& output : outputs) {
      const double lp = section.low, bp = section.band;
      const double hp = (x - lp) - (k * bp);
      const double bpa = bp + (section.f * hp);
      section.band = bpa;
      section.low = lp + (section.f * bpa);
      output = highpass ? hp : lp;
    }
    const double y = ((0.25 * section.previous) + (0.5 * outputs[0])) + (0.25 * outputs[1]);
    section.previous = outputs[1];
    return y;
  }

  double BandRejectCore::tick(double input, double drive, double damping) {
    // Both sections clip the driven input the same way, so once is enough.
    const double x = saturate(drive * input);
    return run<false>(low_, x, damping) + run<true>(high_, x, damping);
  }
} // namespace vital
