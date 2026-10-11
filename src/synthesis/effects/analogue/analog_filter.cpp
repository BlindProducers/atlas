// SPDX-License-Identifier: GPL-3.0-or-later
#include "analog_filter.h"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace atlas::analogue {
  namespace {
    constexpr double kPi = 3.14159265358979323846;

    // Polynomial with real coefficients, lowest order first.
    using Poly = std::vector<double>;

    Poly multiply(const Poly& a, const Poly& b) {
      Poly result(a.size() + b.size() - 1, 0.0);
      for (size_t i = 0; i < a.size(); ++i)
        for (size_t j = 0; j < b.size(); ++j)
          result[i + j] += a[i] * b[j];
      return result;
    }

    Complex evaluate(const Poly& p, Complex s) {
      Complex result = 0.0;
      for (size_t i = p.size(); i-- > 0;)
        result = result * s + p[i];
      return result;
    }
  }

  Complex AnalogFilter::response(Complex s) const {
    Complex result = direct;
    for (int i = 0; i < count; ++i) {
      result += residues[i] / (s - poles[i]);
      if (paired[i])
        result += std::conj(residues[i]) / (s - std::conj(poles[i]));
    }
    return result;
  }

  Complex AnalogFilter::responseAtHz(double hz) const {
    return response(Complex(0.0, 2.0 * kPi * hz));
  }

  AnalogSection lowPass2(double hz, double q) {
    AnalogSection section;
    section.w0 = 2.0 * kPi * hz;
    section.q = q;
    section.b0 = section.w0 * section.w0;
    return section;
  }

  AnalogSection lowPass1(double hz) {
    AnalogSection section;
    section.first_order = true;
    section.w0 = 2.0 * kPi * hz;
    section.b0 = section.w0;
    return section;
  }

  AnalogSection highPass1(double hz) {
    AnalogSection section;
    section.first_order = true;
    section.w0 = 2.0 * kPi * hz;
    section.b1 = 1.0;
    section.b0 = 0.0;
    return section;
  }

  AnalogSection shelf1(double zero_hz, double pole_hz) {
    // (1 + s/wz) / (1 + s/wp) = (wp / wz) (s + wz) / (s + wp)
    const double wz = 2.0 * kPi * zero_hz, wp = 2.0 * kPi * pole_hz;
    AnalogSection section;
    section.first_order = true;
    section.w0 = wp;
    section.b1 = wp / wz;
    section.b0 = wp;
    return section;
  }

  AnalogSection sallenKeyLowPass(double r1, double r2, double c1, double c2) {
    // Unity-gain Sallen-Key: w0 = 1/sqrt(R1 R2 C1 C2), Q = sqrt(R1 R2 C1 C2) / (C2 (R1 + R2)).
    const double rc = std::sqrt(r1 * r2 * c1 * c2);
    AnalogSection section;
    section.w0 = 1.0 / rc;
    section.q = rc / (c2 * (r1 + r2));
    section.b0 = section.w0 * section.w0;
    return section;
  }

  std::vector<AnalogSection> butterworthLowPass(int order, double hz) {
    std::vector<AnalogSection> sections;
    for (int k = 0; k < order / 2; ++k) {
      const double angle = kPi * (2.0 * k + 1.0) / (2.0 * order);
      sections.push_back(lowPass2(hz, 1.0 / (2.0 * std::sin(angle))));
    }
    if (order % 2)
      sections.push_back(lowPass1(hz));
    return sections;
  }

  AnalogFilter fromPolesAndResidues(const std::vector<PoleResidue>& terms, double direct) {
    AnalogFilter filter;
    filter.direct = direct;
    for (const auto& term : terms) {
      assert(filter.count < AnalogFilter::kMaxTerms);
      const int index = filter.count++;
      const bool complex_pole = term.pole.imag() != 0.0;
      filter.poles[index] = complex_pole ? Complex(term.pole.real(), std::abs(term.pole.imag())) : term.pole;
      filter.residues[index] = complex_pole && term.pole.imag() < 0.0 ? std::conj(term.residue) : term.residue;
      filter.paired[index] = complex_pole;
    }
    return filter;
  }

  AnalogFilter toPartialFractions(const std::vector<AnalogSection>& sections, double gain) {
    Poly numerator { gain }, denominator { 1.0 };
    std::vector<Complex> poles;
    for (const auto& section : sections) {
      if (section.first_order) {
        numerator = multiply(numerator, { section.b0, section.b1 });
        denominator = multiply(denominator, { section.w0, 1.0 });
        poles.emplace_back(-section.w0, 0.0);
      }
      else {
        numerator = multiply(numerator, { section.b0, section.b1, section.b2 });
        denominator = multiply(denominator, { section.w0 * section.w0, section.w0 / section.q, 1.0 });
        const double real = -section.w0 / (2.0 * section.q);
        const double discriminant = real * real - section.w0 * section.w0;
        if (discriminant < 0.0) {
          const double imag = std::sqrt(-discriminant);
          poles.emplace_back(real, imag);
          poles.emplace_back(real, -imag);
        }
        else {
          poles.emplace_back(real + std::sqrt(discriminant), 0.0);
          poles.emplace_back(real - std::sqrt(discriminant), 0.0);
        }
      }
    }

    AnalogFilter filter;
    // Proper rational function: split off the direct term when degrees match.
    const size_t order = denominator.size() - 1;
    numerator.resize(std::max(numerator.size(), denominator.size()), 0.0);
    if (numerator.size() > order && numerator[order] != 0.0) {
      filter.direct = numerator[order];
      for (size_t i = 0; i <= order; ++i)
        numerator[i] -= filter.direct * denominator[i];
    }
    numerator.resize(order);

    for (size_t i = 0; i < poles.size(); ++i) {
      const Complex pole = poles[i];
      if (pole.imag() < 0.0)
        continue;
      Complex product = 1.0;
      for (size_t j = 0; j < poles.size(); ++j)
        if (j != i)
          product *= pole - poles[j];
      assert(filter.count < AnalogFilter::kMaxTerms);
      const int index = filter.count++;
      filter.poles[index] = pole;
      filter.residues[index] = evaluate(numerator, pole) / product;
      filter.paired[index] = pole.imag() > 0.0;
    }
    return filter;
  }
}
