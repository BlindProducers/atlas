// SPDX-License-Identifier: GPL-3.0-or-later
#include "spring_engine.h"

#include <algorithm>
#include <cmath>
#include <complex>

namespace atlas::analogue::spring {
  namespace {
    constexpr double kPi = 3.14159265358979323846;
    constexpr int kHistory = 8;                 // allpass history ring per section
    // Valimaki et al.'s stretched allpass, a1 = 0.75 with stretch fs / (2 fc):
    // each section delays the transition 7x the stretch, so chirps last about
    // 81 ms at 4.3 kHz. a1 is positive: each echo rises to the transition.
    constexpr double kAllpass = 0.75;
    // Pre-echoes (Valimaki et al.: 0.1 taps a fifth of the loop delay and a
    // stretch length ahead). Inside the loop they would make some frequencies
    // ring far longer than the tank's rating, so they are tapped from each
    // spring's output instead, ahead of the next echo.
    constexpr double kPreEchoGain = 0.1;
    // Slow random drift of each loop (Valimaki et al. jitter the loop delay to
    // break up the echoes' regularity), read through a first-order allpass so
    // it costs the loop no level.
    constexpr double kModulationHz = 3.0;
    constexpr double kModulationSeconds = 0.00015;
    // The springs pass little above 4.75 kHz (Valimaki et al. low-pass there
    // with an elliptic filter). Analog prototype with 0.5 dB ripple and a 60 dB
    // stopband from 5.34 kHz, below every loop rate's Nyquist (5.5 kHz or
    // more): scipy.signal.ellipap(8, 0.5, 60).
    constexpr double kTankLowpassHz = 4750.0;
    constexpr double kEllipticGain = 0.001;
    constexpr double kEllipticZeros[4] = { 1.133709500541, 1.230664577873, 1.611118198144, 4.113093373460 };
    constexpr double kEllipticPoles[4][2] = {
      { -0.301963397685, 0.297420606250 }, { -0.175710417666, 0.734066041505 },
      { -0.072151688999, 0.934804285770 }, { -0.018252567423, 1.003041785872 } };
    constexpr double kDcHz = 40.0;
    constexpr double kDriverHighpassHz = 1.0 / (2.0 * kPi * 0.002e-6 * 220.0e3);  // 6G15: 0.002 uF into 220k
    constexpr double kStereoDetune = 1.06;      // the second tank's springs are a little longer
    constexpr double kSwitchSeconds = 0.02, kSmoothingSeconds = 0.02;
    // Recovery-stage hiss before the tone network, scaled by (Noise / 0.5)^2.
    constexpr double kHiss = 6.0e-4;
    // The tone network loses about 6.5 dB at its brightest; the recovery
    // stage's gain makes that up.
    constexpr double kToneMakeup = 2.1135;
    // Wet level: at half Dwell and half Tone the reverb of sustained material
    // sits about 6 dB under the dry signal on the Long tank.
    constexpr double kWetGain = 2.5;

    // Short: Accutronics Type 1 class (2 springs), modelled on the measured
    // Olson X-82 springs (Bilbao & Parker), decay code 1 (1.2-2.0 s).
    // Medium: Type 8 class (3 springs), measured Leem KA-1210 springs, decay
    // code 2 (1.75-3.0 s). Long: Type 4 (Fender 4AB3C1B, 2 springs), the
    // Accutronics 33 and 41 ms delays and a 4.3 kHz transition, decay code 3
    // (2.75-4.0 s).
    const std::array<TankSpec, kNumTanks> kTanks {{
      { 2, { 0.0306, 0.0351, 0.0 }, { 3244.0, 2543.0, 0.0 }, 1.6 },
      { 3, { 0.0416, 0.0402, 0.0527 }, { 4887.0, 4672.0, 4471.0 }, 2.4 },
      { 2, { 0.0330, 0.0410, 0.0 }, { 4300.0, 4100.0, 0.0 }, 3.4 },
    }};

    double coefficientFor(double hz, double rate) { return -std::expm1(-2.0 * kPi * hz / rate); }

    // --- 6G15 tone network: a ladder of the recovery stage's source
    // resistance, 0.1 uF, the Tone pot's upper part, a shunt of its lower part
    // with 0.01 uF to ground, 250 pF and the Mixer's 250k load.
    using Poly = std::array<double, 5>;  // lowest order first

    Poly multiply(const Poly& a, const Poly& b) {
      Poly r {};
      for (int i = 0; i < 5; ++i)
        for (int j = 0; i + j < 5; ++j)
          r[i + j] += a[i] * b[j];
      return r;
    }
    Poly add(const Poly& a, const Poly& b) {
      Poly r {};
      for (int i = 0; i < 5; ++i) r[i] = a[i] + b[i];
      return r;
    }

    struct Rational { Poly numerator, denominator; };

    // H(s) = Vout / Vin of the open-ended ladder, from ABCD matrices with
    // polynomial entries; each element's denominator is factored out.
    Rational toneNetwork(double tone) {
      constexpr double kSource = 38.5e3, kC1 = 0.1e-6, kPot = 50.0e3, kC2 = 0.01e-6, kC3 = 250.0e-12, kLoad = 250.0e3;
      const double top = std::max(1.0, kPot * (1.0 - tone)), bottom = std::max(1.0, kPot * tone);
      std::array<Poly, 4> m { Poly { 1.0 }, Poly {}, Poly {}, Poly { 1.0 } };  // A B / C D
      Poly scale { 1.0 };
      const auto apply = [&](const std::array<Poly, 4>& e, const Poly& divisor) {
        m = { add(multiply(m[0], e[0]), multiply(m[1], e[2])), add(multiply(m[0], e[1]), multiply(m[1], e[3])),
              add(multiply(m[2], e[0]), multiply(m[3], e[2])), add(multiply(m[2], e[1]), multiply(m[3], e[3])) };
        scale = multiply(scale, divisor);
      };
      // Series impedance zn / zd: [[zd, zn], [0, zd]] / zd. Shunt admittance yn / yd: [[yd, 0], [yn, yd]] / yd.
      const auto series = [&](const Poly& zn, const Poly& zd) { apply({ zd, zn, Poly {}, zd }, zd); };
      const auto shunt = [&](const Poly& yn, const Poly& yd) { apply({ yd, Poly {}, yn, yd }, yd); };
      series({ kSource }, { 1.0 });
      series({ 1.0 }, { 0.0, kC1 });
      series({ top }, { 1.0 });
      shunt({ 0.0, kC2 }, { 1.0, bottom * kC2 });
      series({ 1.0 }, { 0.0, kC3 });
      shunt({ 1.0 }, { kLoad });
      // Open output: H = 1 / A_total = scale / A.
      return { scale, m[0] };
    }

    std::complex<double> evaluate(const Poly& p, std::complex<double> s) {
      std::complex<double> r = 0.0;
      for (int i = 4; i >= 0; --i) r = r * s + p[i];
      return r;
    }
  }

  const TankSpec& tankSpec(int tank) { return kTanks[std::clamp(tank, 0, kNumTanks - 1)]; }

  double toneResponse(double tone, double hz) {
    const Rational h = toneNetwork(std::clamp(tone, 0.0, 1.0));
    const std::complex<double> s(0.0, 2.0 * kPi * hz);
    return std::abs(evaluate(h.numerator, s) / evaluate(h.denominator, s));
  }

  Engine::Engine() { prepare(sample_rate_); }

  void Engine::prepare(double sample_rate) {
    sample_rate_ = sample_rate;
    // The springs pass nothing above ~5 kHz and the elliptic filter stops
    // everything above 4.95 kHz: run the loops at 11 kHz or more.
    decimation_ = std::max(1, static_cast<int>(sample_rate / 11000.0));
    reduced_rate_ = sample_rate / decimation_;
    smoothing_ = -std::expm1(-1.0 / (kSmoothingSeconds * sample_rate));
    switch_step_ = 1.0 / std::max(1.0, kSwitchSeconds * sample_rate);
    modulation_coefficient_ = coefficientFor(kModulationHz, reduced_rate_);
    modulation_depth_ = kModulationSeconds * reduced_rate_;
    dc_coefficient_ = coefficientFor(kDcHz, reduced_rate_);
    wet_gain_ = kWetGain;

    history_.assign(static_cast<size_t>(kSections) * kHistory * kGroups, vital::poly_float(0.0f));
    previous_.assign(static_cast<size_t>(kSections) * kGroups, vital::poly_float(0.0f));
    double longest = 0.0;
    for (const auto& tank : kTanks)
      for (int i = 0; i < tank.springs; ++i) longest = std::max(longest, tank.echo_seconds[i]);
    line_length_ = static_cast<int>(std::ceil(longest * kStereoDetune * reduced_rate_ + 4.0 * modulation_depth_)) + 8;
    lines_.assign(static_cast<size_t>(line_length_) * kLaneStride, 0.0f);

    // Elliptic low pass, decimator into the springs and interpolator out of
    // them: bilinear transform of each section, prewarped at the cutoff. The
    // highest-Q poles pair with the lowest zeros; low-Q sections run first.
    const double cutoff = 2.0 * sample_rate * std::tan(kPi * kTankLowpassHz / sample_rate);
    const double c = 2.0 * sample_rate;
    for (int i = 0; i < kEllipticSections; ++i) {
      const double zero = kEllipticZeros[kEllipticSections - 1 - i] * cutoff;
      const double re = kEllipticPoles[i][0] * cutoff, im = kEllipticPoles[i][1] * cutoff;
      const double n0 = zero * zero, d1 = -2.0 * re, d0 = re * re + im * im;
      const double a0 = c * c + d1 * c + d0;
      // H(s) = k prod (s^2 + n0) / (s^2 + d1 s + d0); each section takes an
      // equal share of k.
      const double scale = std::pow(kEllipticGain, 1.0 / kEllipticSections) / a0;
      for (auto* filter : { &decimator_[i], &interpolator_[i] }) {
        filter->b0 = scale * (c * c + n0); filter->b1 = scale * 2.0 * (n0 - c * c); filter->b2 = filter->b0;
        filter->a1 = 2.0 * (d0 - c * c) / a0; filter->a2 = (c * c - d1 * c + d0) / a0;
      }
    }
    // First-order high pass: the 6G15's coupling into the driver grid.
    const double k = std::tan(kPi * std::min(kDriverHighpassHz, 0.45 * sample_rate) / sample_rate);
    driver_highpass_.b0 = 1.0 / (1.0 + k); driver_highpass_.b1 = -1.0 / (1.0 + k); driver_highpass_.b2 = 0.0;
    driver_highpass_.a1 = (k - 1.0) / (k + 1.0); driver_highpass_.a2 = 0.0;
    configured_tank_ = -1;
    tone_ = -1.0;
    reset();
  }

  void Engine::clearSprings() {
    std::fill(history_.begin(), history_.end(), vital::poly_float(0.0f));
    std::fill(previous_.begin(), previous_.end(), vital::poly_float(0.0f));
    std::fill(lines_.begin(), lines_.end(), 0.0f);
    output_ = {};
    modulation_ = {};
    allpass_input_ = {};
    allpass_output_ = {};
    history_index_ = line_write_ = 0;
    tank_b_live_ = false;
  }

  void Engine::clearLanes(int first, int last) {
    constexpr int width = static_cast<int>(vital::poly_float::kSize);
    for (size_t row = 0; row < static_cast<size_t>(kSections) * kHistory; ++row)
      for (int lane = first; lane < last; ++lane) history_[row * kGroups + lane / width].set(lane % width, 0.0f);
    for (size_t row = 0; row < static_cast<size_t>(kSections); ++row)
      for (int lane = first; lane < last; ++lane) previous_[row * kGroups + lane / width].set(lane % width, 0.0f);
    for (size_t row = 0; row < static_cast<size_t>(line_length_); ++row)
      for (int lane = first; lane < last; ++lane) lines_[row * kLaneStride + lane] = 0.0f;
    for (int lane = first; lane < last; ++lane) {
      modulation_[lane] = 0.0;
      allpass_input_[lane] = allpass_output_[lane] = 0.0;
    }
  }

  void Engine::configureTank(int tank) {
    const TankSpec& spec = tankSpec(tank);
    configured_tank_ = tank;
    springs_ = spec.springs;
    // One shared integer delay K1 per tank; each spring then gets its own
    // section coefficients (below).
    double mean_stretch = 0.0;
    for (int i = 0; i < springs_; ++i) mean_stretch += reduced_rate_ / (2.0 * spec.transition_hz[i]);
    mean_stretch /= springs_;
    history_offset_ = std::clamp(static_cast<int>(std::lround(mean_stretch)) - 1, 1, kHistory - 2);
    decay_per_sample_ = static_cast<float>(std::pow(10.0, -3.0 / (spec.decay_seconds * reduced_rate_)));
    // Within each section, w[n - K1] and w[n - K1 - 1] lose rho^K1 and
    // rho^(K1 + 1); its own previous output loses rho.
    history_loss_ = static_cast<float>(std::pow(decay_per_sample_, history_offset_));
    previous_loss_ = decay_per_sample_;
    for (int lane = 0; lane < 2 * springs_; ++lane) {
      const int spring = lane % springs_;
      const bool second = lane >= springs_;
      const double transition = spec.transition_hz[spring] / (second ? kStereoDetune : 1.0);
      const double echo = spec.echo_seconds[spring] * (second ? kStereoDetune : 1.0);
      const double stretch = reduced_rate_ / (2.0 * transition);
      // Exact section coefficients for this spring at this rate. Each
      // section's group delay peaks where G(z) = z^-K1 A2(z) reaches a phase of
      // -pi: solve A2's coefficient so that happens at the spring's
      // transition. The peak delay is G's delay there times (1 + a1)/(1 - a1);
      // choose a1 so it equals Valimaki et al.'s 7x stretch, the chirp length.
      const double omega = 2.0 * kPi * transition / reduced_rate_;
      const double target_phase = -kPi + history_offset_ * omega;
      const auto phase = [omega](double a) {
        const std::complex<double> z = std::polar(1.0, -omega);
        return std::arg((a + z) / (1.0 + a * z));
      };
      double low = -0.99, high = 0.99;  // phase falls as the coefficient falls
      for (int iteration = 0; iteration < 60; ++iteration) {
        const double middle = 0.5 * (low + high);
        (phase(middle) > target_phase ? high : low) = middle;
      }
      const double a2 = 0.5 * (low + high);
      const double peak_delay = history_offset_ + (1.0 - a2 * a2) / (1.0 + 2.0 * a2 * std::cos(omega) + a2 * a2);
      const double ratio = 7.0 * stretch / peak_delay;
      const double a1 = (ratio - 1.0) / (ratio + 1.0);
      fraction_coefficient_[lane] = static_cast<float>(a2);
      allpass_coefficient_[lane] = static_cast<float>(a1);
      // Loop delay: the echo period less the chain's group delay at DC.
      const double dc_delay = history_offset_ + (1.0 - a2) / (1.0 + a2);
      const double chain_delay = kSections * dc_delay * (1.0 - a1) / (1.0 + a1);
      const double total = std::max(8.0, echo * reduced_rate_ - chain_delay);
      const int echo_length = static_cast<int>(total / 5.0);
      const int ripple_length = std::max(1, static_cast<int>(std::lround(stretch)));
      main_delay_[lane] = total;
      base_tap_[lane] = std::max(1, static_cast<int>(total) - echo_length - ripple_length);
      echo_tap_[lane] = base_tap_[lane] + echo_length;
      ripple_tap_[lane] = base_tap_[lane] + ripple_length;
      // Decay. Each frequency's round trip lasts the loop delay plus the
      // chain's group delay at that frequency, which near the transition is
      // many times longer. Every delay element in the loop loses the same
      // small factor per sample (the chain evaluated just inside the unit
      // circle), so each frequency loses level in proportion to the time it
      // spends in the loop and all decay at the tank's rate per second.
      tap_gain_[lane] = kPreEchoGain;
      loop_gain_[lane] = std::pow(decay_per_sample_, total);
    }
    clearSprings();
  }

  void Engine::reset() {
    if (configured_tank_ != parameters_.tank) configureTank(parameters_.tank);
    else clearSprings();
    for (auto& filter : decimator_) filter.clear();
    for (auto& filter : interpolator_) filter.clear();
    driver_highpass_.clear();
    for (auto& io : io_) {
      io.dc_state = 0.0;
      io.tone.z = {};
      io.held = 0.0;
    }
    phase_ = 0;
    dwell_ = parameters_.dwell; pan_ = parameters_.pan; noise_ = parameters_.noise; mix_ = parameters_.mix;
    {
      const double angle = (std::clamp(pan_, -1.0, 1.0) + 1.0) * 0.25 * kPi;
      pan_gains_ = { std::cos(angle), std::sin(angle), std::min(1.0, 1.0 - pan_), std::min(1.0, 1.0 + pan_) };
    }
    stereo_ = parameters_.mono ? 0.0 : 1.0;
    switch_gain_ = 1.0;
    pending_tank_ = -1;
    configureTone(parameters_.tone);
    random_ = 0x5eed1234u;
  }

  void Engine::setParameters(const Parameters& parameters) {
    parameters_ = parameters;
    parameters_.tank = std::clamp(parameters.tank, 0, kNumTanks - 1);
    pending_tank_ = parameters_.tank != configured_tank_ ? parameters_.tank : -1;
  }

  void Engine::configureTone(double tone) {
    if (std::abs(tone - tone_) < 1.0e-4) return;
    tone_ = tone;
    const Rational h = toneNetwork(tone);
    // Bilinear transform of the 3rd-order network: s^k becomes
    // c^k (1 - z^-1)^k (1 + z^-1)^(3 - k) over (1 + z^-1)^3.
    const double c = 2.0 * sample_rate_;
    std::array<double, 4> b {}, a {};
    for (int k = 0; k <= 3; ++k) {
      std::array<double, 4> term { 1.0, 0.0, 0.0, 0.0 };
      for (int i = 0; i < 3; ++i) {
        const double sign = i < k ? -1.0 : 1.0;
        for (int j = 3; j > 0; --j) term[j] += sign * term[j - 1];
      }
      const double power = std::pow(c, k);
      for (int j = 0; j < 4; ++j) {
        b[j] += h.numerator[k] * power * term[j];
        a[j] += h.denominator[k] * power * term[j];
      }
    }
    for (auto& io : io_) {
      for (int j = 0; j < 4; ++j) {
        io.tone.b[j] = kToneMakeup * b[j] / a[0];
        io.tone.a[j] = a[j] / a[0];
      }
    }
  }

  double Engine::random() {
    random_ ^= random_ << 13; random_ ^= random_ >> 17; random_ ^= random_ << 5;
    return random_ * (1.0 / 4294967296.0) - 0.5;
  }

  double Engine::drive(double x) const {
    // Dwell sets the level into the 6K6 driver and the tank's input coil; at
    // high Dwell both saturate, slightly asymmetrically (single-ended stage).
    // A Pade approximant of tanh, within 2% of it and much cheaper per sample.
    // Half Dwell gives unity for quiet signals.
    constexpr double kBias = 0.035;
    const auto soft = [](double v) {
      v = std::clamp(v, -3.0, 3.0);
      return v * (27.0 + v * v) / (27.0 + 9.0 * v * v);
    };
    const double gain = 6.0 * dwell_;
    return (soft(gain * x + kBias) - soft(kBias)) / 3.0;
  }

  // 100 interpolated stretched allpass sections, four springs per vector:
  // A(z) = (a1 + G(z)) / (1 + a1 G(z)), G(z) = z^-K1 (a2 + z^-1) / (1 + a2 z^-1).
  // G's output depends only on past values, so each section's critical path is
  // one multiply-add: y = a1 x + (1 - a1^2) g, with w = x - a1 g stored aside.
  void Engine::runChain(vital::poly_float* chain, int groups) {
    using vital::poly_float;
    constexpr int width = static_cast<int>(poly_float::kSize);
    const int now = history_index_;
    const int q0 = (now - history_offset_ + kHistory) & (kHistory - 1);
    const int q1 = (now - history_offset_ - 1 + kHistory) & (kHistory - 1);
    // Per-lane constants: g = k0 h0 + k1 previous + k2 h1, with every delay
    // element losing rho per sample (G evaluated at z / rho).
    const float rho = previous_loss_, c0 = history_loss_, c1 = history_loss_ * previous_loss_;
    poly_float a1[kGroups], b[kGroups], k0[kGroups], k1[kGroups], x[kGroups];
    const poly_float k2(c1);
    for (int group = 0; group < groups; ++group) {
      x[group] = chain[group];
      for (int i = 0; i < width; ++i) {
        const float coefficient = allpass_coefficient_[group * width + i];
        const float a2 = fraction_coefficient_[group * width + i];
        a1[group].set(i, coefficient);
        b[group].set(i, 1.0f - coefficient * coefficient);
        k0[group].set(i, a2 * c0);
        k1[group].set(i, -a2 * rho);
      }
    }
    for (int section = 0; section < kSections; ++section) {
      poly_float* history = &history_[static_cast<size_t>(section) * kHistory * kGroups];
      poly_float* previous = &previous_[static_cast<size_t>(section) * kGroups];
      for (int group = 0; group < groups; ++group) {
        const poly_float g = poly_float::mulAdd(poly_float::mulAdd(k2 * history[q1 * kGroups + group],
                                                                   k1[group], previous[group]),
                                                k0[group], history[q0 * kGroups + group]);
        history[now * kGroups + group] = x[group] - a1[group] * g;
        previous[group] = g;
        x[group] = poly_float::mulAdd(b[group] * g, a1[group], x[group]);
      }
    }
    for (int group = 0; group < groups; ++group) chain[group] = x[group];
    history_index_ = (now + 1) & (kHistory - 1);
  }

  // One reduced-rate step of every active spring lane.
  void Engine::runSprings(const std::array<double, 2>& inputs, std::array<double, 2>& outputs, int lanes) {
    const auto at = [&](int delay, int lane) {
      int index = line_write_ - delay;
      if (index < 0) index += line_length_;
      return static_cast<double>(lines_[static_cast<size_t>(index) * kLaneStride + lane]);
    };
    float chain[kLaneStride] = {};
    for (int lane = 0; lane < lanes; ++lane) {
      // Drift: smoothed noise on the main delay. The integer part is read
      // directly, the fraction (0.5-1.5 samples) through a first-order Thiran
      // allpass, which keeps every frequency's level.
      modulation_[lane] += (random() * 3.4641016 - modulation_[lane]) * modulation_coefficient_;
      const double delay = main_delay_[lane] + modulation_depth_ * modulation_[lane];
      const int whole = static_cast<int>(delay - 0.5);
      const double fraction = delay - whole;  // 0.5 .. 1.5
      const double coefficient = (1.0 - fraction) / (1.0 + fraction);
      const double input = at(whole, lane);
      const double main = coefficient * input + allpass_input_[lane] - coefficient * allpass_output_[lane];
      allpass_input_[lane] = input;
      allpass_output_[lane] = main;
      const double taps = main;
      const int tank = lane < springs_ ? 0 : 1;
      chain[lane] = static_cast<float>(inputs[tank] + loop_gain_[lane] * taps);
    }

    constexpr int width = static_cast<int>(vital::poly_float::kSize);
    const int groups = (lanes + width - 1) / width;
    vital::poly_float vectors[kGroups];
    for (int group = 0; group < groups; ++group)
      for (int i = 0; i < width; ++i) vectors[group].set(i, chain[group * width + i]);
    runChain(vectors, groups);
    for (int group = 0; group < groups; ++group)
      for (int i = 0; i < width; ++i) chain[group * width + i] = vectors[group][i];

    outputs = { 0.0, 0.0 };
    for (int lane = 0; lane < lanes; ++lane) {
      lines_[static_cast<size_t>(line_write_) * kLaneStride + lane] = chain[lane];
      // Each echo's pre-echoes: copies of the previous echo, arriving a fifth
      // of the loop delay and a stretch length before this one,
      // z^-L0 (g_echo + z^-Lecho)(g_ripple + z^-Lripple) less its main tap.
      const double x = tap_gain_[lane];
      const double pre = x * at(echo_tap_[lane] - 1, lane) + x * at(ripple_tap_[lane] - 1, lane) +
                         x * x * at(base_tap_[lane] - 1, lane);
      outputs[lane < springs_ ? 0 : 1] += chain[lane] + pre;
    }
    if (++line_write_ == line_length_) line_write_ = 0;
    const double normalise = 1.0 / std::sqrt(static_cast<double>(springs_));
    outputs[0] *= normalise;
    outputs[1] *= normalise;
  }

  void Engine::process(const float* in_left, const float* in_right, float* out_left, float* out_right, int count) {
    // Pan changes slowly: its gains are computed once per block.
    const double block_smoothing = 1.0 - std::pow(1.0 - smoothing_, count);
    pan_ += (parameters_.pan - pan_) * block_smoothing;
    const double angle = (std::clamp(pan_, -1.0, 1.0) + 1.0) * 0.25 * kPi;
    const std::array<double, 4> target { std::cos(angle), std::sin(angle), std::min(1.0, 1.0 - pan_), std::min(1.0, 1.0 + pan_) };
    std::array<double, 4> gains = pan_gains_, steps;
    for (int i = 0; i < 4; ++i) steps[i] = (target[i] - gains[i]) / std::max(1, count);
    pan_gains_ = target;
    // Equal-power dry/wet, likewise per block.
    const double previous_mix = mix_;
    mix_ += (parameters_.mix - mix_) * block_smoothing;
    double dry_gain = std::cos(0.5 * kPi * previous_mix), wet_gain = std::sin(0.5 * kPi * previous_mix);
    const double dry_step = (std::cos(0.5 * kPi * mix_) - dry_gain) / std::max(1, count);
    const double wet_step = (std::sin(0.5 * kPi * mix_) - wet_gain) / std::max(1, count);

    for (int n = 0; n < count; ++n) {
      const auto& p = parameters_;
      dwell_ += (p.dwell - dwell_) * smoothing_;
      for (int i = 0; i < 4; ++i) gains[i] += steps[i];
      noise_ += (p.noise - noise_) * smoothing_;
      dry_gain += dry_step;
      wet_gain += wet_step;
      const double stereo_target = p.mono ? 0.0 : 1.0;
      stereo_ += (stereo_target - stereo_) * smoothing_;
      if (std::abs(stereo_ - stereo_target) < 1.0e-5) stereo_ = stereo_target;

      // A tank change fades out, swaps the springs, and fades back in.
      if (pending_tank_ >= 0) {
        switch_gain_ -= switch_step_;
        if (switch_gain_ <= 0.0) {
          switch_gain_ = 0.0;
          configureTank(pending_tank_);
          pending_tank_ = -1;
        }
      }
      else if (switch_gain_ < 1.0)
        switch_gain_ = std::min(1.0, switch_gain_ + switch_step_);

      // Mono: both inputs into tank A. Stereo: left into A, right into B.
      const double left = in_left[n], right = in_right[n];
      const std::array<double, 2> tank_input { (1.0 - stereo_) * 0.5 * (left + right) + stereo_ * left, stereo_ * right };
      const bool second = stereo_ > 0.0;
      if (!second && tank_b_live_) {
        // Back in mono: tank B rests empty, so stereo restarts cleanly.
        clearLanes(springs_, 2 * springs_);
        io_[1].held = io_[1].dc_state = 0.0;
        tank_b_live_ = false;
      }
      tank_b_live_ = tank_b_live_ || second;
      const int tanks = second ? 2 : 1;

      // Both tanks' driver and decimator side by side (tank B idles on zero).
      double pair[2] = { tank_input[0], tank_input[1] };
      driver_highpass_.tick(pair);
      pair[0] = drive(pair[0]);
      pair[1] = second ? drive(pair[1]) : 0.0;
      for (auto& filter : decimator_) filter.tick(pair);
      const std::array<double, 2> decimated { pair[0], second ? pair[1] : 0.0 };

      const bool step = phase_ == 0;
      if (step) {
        std::array<double, 2> outputs;
        runSprings(decimated, outputs, tanks * springs_);
        for (int tank = 0; tank < tanks; ++tank) {
          auto& io = io_[tank];
          io.dc_state += (outputs[tank] - io.dc_state) * dc_coefficient_;
          io.held = (outputs[tank] - io.dc_state) * decimation_;
        }
      }
      if (++phase_ == decimation_) phase_ = 0;

      double held[2] = { step ? io_[0].held : 0.0, step ? io_[1].held : 0.0 };
      for (auto& filter : interpolator_) filter.tick(held);
      std::array<double, 2> wet {};
      const double hiss_level = kHiss * (noise_ / 0.5) * (noise_ / 0.5);
      for (int tank = 0; tank < tanks; ++tank)
        wet[tank] = wet_gain_ * io_[tank].tone.tick(held[tank] + hiss_level * random()) * switch_gain_;

      // Mono: equal-power pan of the one tank. Stereo: Pan balances the two.
      const double mono_left = wet[0] * gains[0], mono_right = wet[0] * gains[1];
      const double stereo_left = wet[0] * gains[2], stereo_right = wet[1] * gains[3];
      const double wet_left = (1.0 - stereo_) * mono_left + stereo_ * stereo_left;
      const double wet_right = (1.0 - stereo_) * mono_right + stereo_ * stereo_right;

      out_left[n] = static_cast<float>(dry_gain * left + wet_gain * wet_left);
      out_right[n] = static_cast<float>(dry_gain * right + wet_gain * wet_right);
    }
    configureTone(std::clamp(parameters_.tone, 0.0, 1.0));
  }
}
