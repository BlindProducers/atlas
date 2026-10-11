// SPDX-License-Identifier: GPL-3.0-or-later
// Continuous-time filters in partial-fraction form, used where a model has to
// evaluate an analog circuit's response at arbitrary instants (BBD clock
// ticks) rather than once per audio sample.
//
//   H(s) = direct + sum over all poles p_k of r_k / (s - p_k)
//
// Only one pole of each conjugate pair is stored; `paired` marks it so callers
// take twice the real part. Real poles are stored with a zero imaginary part.
#pragma once

#include <array>
#include <complex>
#include <vector>

namespace atlas::analogue {
  using Complex = std::complex<double>;

  struct AnalogFilter {
    static constexpr int kMaxTerms = 8;

    int count = 0;
    std::array<Complex, kMaxTerms> poles {}, residues {};
    std::array<bool, kMaxTerms> paired {};
    double direct = 0.0;

    Complex response(Complex s) const;
    Complex responseAtHz(double hz) const;
    double dcGain() const { return responseAtHz(0.0).real(); }
  };

  // Analog second-order section: (b2 s^2 + b1 s + b0) / (s^2 + (w0/q) s + w0^2).
  struct AnalogSection {
    double b2 = 0.0, b1 = 0.0, b0 = 1.0;
    double w0 = 1.0, q = 0.70710678118654752;
    bool first_order = false;  // b1 s + b0 over (s + w0); q and b2 unused
  };

  // Unity-DC low-pass sections.
  AnalogSection lowPass2(double hz, double q);
  AnalogSection lowPass1(double hz);
  // Coupling capacitor: s / (s + w).
  AnalogSection highPass1(double hz);
  // First-order shelf between corner frequencies, unity at DC:
  // (1 + s/w_zero) / (1 + s/w_pole). Pre-emphasis has zero_hz < pole_hz.
  AnalogSection shelf1(double zero_hz, double pole_hz);
  // Unity-gain Sallen-Key low pass from its component values
  // (R1, R2 in series; C1 feedback capacitor, C2 to ground).
  AnalogSection sallenKeyLowPass(double r1, double r2, double c1, double c2);
  // Butterworth low pass of the given order as cascaded sections.
  std::vector<AnalogSection> butterworthLowPass(int order, double hz);

  // A filter given directly as measured poles and residues (rad/s). Each
  // complex pole is listed once, standing for its conjugate pair.
  struct PoleResidue { Complex pole, residue; };
  AnalogFilter fromPolesAndResidues(const std::vector<PoleResidue>& terms, double direct = 0.0);

  // Expands cascaded sections into partial fractions. Poles must be distinct.
  AnalogFilter toPartialFractions(const std::vector<AnalogSection>& sections, double gain = 1.0);
}
