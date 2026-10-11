// SPDX-License-Identifier: GPL-3.0-or-later
#include "wah_engine.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>

namespace atlas::analogue::wah {
  namespace {
    constexpr double kPi = 3.14159265358979323846;
    constexpr double kThermalVoltage = 0.025852;
    constexpr double kSmoothingSeconds = 0.01;
    constexpr double kControlSeconds = 0.01;
    constexpr double kFadeSeconds = 0.02;

    // ------------------------------------------------------------ Pedals
    // Values from the GCB-95 BOM (ElectroSmash; Holters & Zolzer, DAFx-11) and
    // the V846-HW's factory schematic. Bias currents from the SPICE operating
    // point. The inductor's effective loss (50 / 40 ohms) reproduces the
    // measured pedals' +18-19 dB peaks; the DC resistance alone overshoots by
    // 3-6 dB. Pot tapers: the GCB-95's semi-log track matches J.O. Smith's
    // measured mid-travel 838 Hz; the V846 uses a standard audio taper.
    // The GCB-95 has an MPSA13 follower at its input (0.01 uF into about
    // 1 Meg: a 16 Hz high pass); the V846 drives Q1's network directly.
    struct PedalSpec {
      double emitter_ohms, bias_ohms, beta, q1_amps, q2_amps, inductor_loss_ohms, taper_base;
      double collector_volts, emitter_volts, buffer_gain, buffer_hz;
    };
    constexpr PedalSpec kPedalSpecs[2] = {
      { 390.0, 82.0e3, 1000.0, 0.2006e-3, 0.380e-3, 50.0, 19.4, 4.59, 0.075, 0.966, 16.0 },
      { 470.0, 100.0e3, 300.0, 0.215e-3, 0.3185e-3, 40.0, 81.0, 4.27, 0.097, 1.0, 0.0 },
    };
    constexpr double kPotOhms = 100.0e3;
    // Holters & Zolzer measured the mounted pot reaching 0.8% from the hot end
    // (heel) to 0.1% from ground (toe).
    constexpr double kHeelFraction = 0.992, kToeFraction = 0.001;
    constexpr double kEarlyVolts = 100.0;
    // Q1's base-emitter law is solved over this signal voltage range: from
    // deep cutoff to past full saturation.
    constexpr double kJunctionLowVolts = -0.35, kJunctionHighVolts = 0.25;
    constexpr double kLoadOhms = 1.0e6;

    // ------------------------------------------------------------ Mu-Tron
    constexpr double kIntegratorOhms = 220.0e3;
    constexpr double kHighFarads = 1800.0e-12, kLowFarads = 4000.0e-12;
    constexpr double kSummerOhms = 4.7e3, kPeakFixedOhms = 12.0e3, kPeakPotOhms = 150.0e3;
    constexpr double kRailVolts = 7.5, kRailKnee = 6.0;
    // Envelope follower: half-wave precision rectifier into C9.
    constexpr double kUpRectifierGain = 1.0e6 / 22.0e3;
    constexpr double kDownRectifierGain = 1.0e6 / (22.0e3 * 12.0e3 / (22.0e3 + 12.0e3));
    constexpr double kRectifierMaxVolts = 7.0;
    constexpr double kEnvelopeFarads = 4.7e-6, kAttackOhms = 330.0;
    constexpr double kDischargeOhms = 47.0e3, kDischargeVolts = -9.0, kFeedbackOhms = 1.0003e6;
    constexpr double kDownLoadOhms = 120.0e3, kDownReferenceVolts = 3.6;
    // LED drive: R23 = 1.8k (the hand-traced schematic; a per-unit trim), A6
    // saturating at 3.36 mA. Down rests at 7.2 V minus the envelope.
    constexpr double kSenseOhms = 1.8e3, kLedSaturationVolts = 6.05, kDownRestVolts = 7.2;
    // Vactrol: Parker & D'Angelo's static fit (DAFx-13) with the original
    // Hamamatsu P873-13's lag (rated at most 20 ms; cells darken more slowly
    // than they brighten).
    constexpr double kVactrolA = 3.464, kVactrolExponent = 1.4, kVactrolFloorOhms = 1136.212;
    constexpr double kDarkOhms = 100.0e6;
    constexpr double kVactrolOnSeconds = 0.005, kVactrolOffSeconds = 0.03;

    double coefficientFor(double seconds, double rate) { return 1.0 - std::exp(-1.0 / (seconds * rate)); }

    double peakPotOhms(double peak) { return kPeakPotOhms * (std::pow(10.0, 2.0 * peak) - 1.0) / 99.0; }

    double frequencyFor(double ldr_ohms, double farads) {
      const double parallel = kIntegratorOhms * ldr_ohms / (kIntegratorOhms + ldr_ohms);
      return 1.0 / (2.0 * kPi * farads * parallel);
    }

    // A solved junction table (value, slope per step) at a fractional index:
    // cubic Hermite, flat beyond its ends.
    inline double junctionCurrent(const float* table, double index) {
      constexpr double kLast = Engine::kJunctionPoints - 1;
      if (index <= 0.0) return table[0];
      if (index >= kLast) return table[2 * (Engine::kJunctionPoints - 1)];
      const int k = static_cast<int>(index);
      const double f = index - k;
      const float* q = table + 2 * k;
      const double y0 = q[0], m0 = q[1], y1 = q[2], m1 = q[3];
      const double d = y1 - y0;
      return y0 + f * (m0 + f * (3.0 * d - 2.0 * m0 - m1 + f * (m0 + m1 - 2.0 * d)));
    }

    // An op-amp output against its rails: linear to the knee, then soft.
    inline double rail(double x) {
      const double magnitude = std::abs(x);
      if (magnitude <= kRailKnee) return x;
      const double y = kRailKnee + (kRailVolts - kRailKnee) * std::tanh((magnitude - kRailKnee) / (kRailVolts - kRailKnee));
      return x < 0.0 ? -y : y;
    }
  }

  std::complex<double> PedalCircuit::response(double hz) const {
    std::complex<double> buffer = buffer_gain;
    if (buffer_hz > 0.0) {
      const std::complex<double> s(0.0, hz / buffer_hz);
      buffer *= s / (1.0 + s);
    }
    return buffer * circuit.analogResponse(output, hz);
  }

  void PedalCircuit::setPot(double fraction) {
    circuit.setResistance(pot_top, std::max(kPotOhms * (1.0 - fraction), 1.0));
    circuit.setResistance(pot_bottom, std::max(kPotOhms * fraction, 1.0));
  }

  PedalCircuit makePedalCircuit(int model) {
    const PedalSpec& spec = kPedalSpecs[std::clamp(model, 0, 1)];
    PedalCircuit pedal;
    LinearCircuit& c = pedal.circuit;
    const int in = c.node(), n1 = c.node(), b1 = c.node(), e1 = c.node(), c1 = c.node();
    const int fb = c.node(), li = c.node(), bias = c.node(), top = c.node(), wiper = c.node(), b2 = c.node(), e2 = c.node();
    constexpr int gnd = LinearCircuit::kGround;
    c.input(in);
    // Input: 68k and 0.01 uF into Q1's base.
    c.resistor(in, n1, 68.0e3);
    c.capacitor(n1, b1, 0.01e-6);
    // Q1 common emitter, 22k collector load to the (AC-grounded) supply.
    c.resistor(c1, gnd, 22.0e3);
    c.resistor(e1, gnd, spec.emitter_ohms);
    c.resistor(c1, e1, kEarlyVolts / spec.q1_amps);
    // The tank: 1.5k from the base, 500 mH (with its core loss) parallel 33k,
    // to the bias node, AC-grounded by 4.7 uF.
    c.resistor(b1, fb, 1.5e3);
    c.inductor(fb, li, 0.5);
    c.resistor(li, bias, spec.inductor_loss_ohms);
    c.resistor(fb, bias, 33.0e3);
    c.resistor(c1, bias, 470.0e3);
    c.resistor(bias, gnd, spec.bias_ohms);
    c.capacitor(bias, gnd, 4.7e-6);
    // 0.22 uF from the collector to the pot's top, which is the output; the
    // wiper feeds Q2 through another 0.22 uF. Q2 follows and drives the
    // 0.01 uF sweep capacitor back into the tank. The two couplings' small
    // phase shifts in this feedback set the resonance's damping, so they stay.
    c.capacitor(c1, top, 0.22e-6);
    pedal.pot_top = c.resistor(top, wiper, kPotOhms * 0.5);
    pedal.pot_bottom = c.resistor(wiper, gnd, kPotOhms * 0.5);
    c.capacitor(wiper, b2, 0.22e-6);
    c.resistor(c1, b2, 470.0e3);
    c.transistor(b2, gnd, e2, spec.q2_amps, spec.beta);
    c.resistor(e2, gnd, kEarlyVolts / spec.q2_amps);
    c.resistor(e2, gnd, 10.0e3);
    c.capacitor(e2, fb, 0.01e-6);
    c.resistor(top, gnd, kLoadOhms);
    c.transistorPort(b1, c1, e1, spec.q1_amps, spec.beta);
    pedal.output = c.output(top);
    pedal.buffer_gain = spec.buffer_gain;
    pedal.buffer_hz = spec.buffer_hz;
    return pedal;
  }

  double potFraction(int model, double position) {
    const double base = kPedalSpecs[std::clamp(model, 0, 1)].taper_base;
    const auto rotation = [base](double fraction) { return std::log1p(fraction * (base - 1.0)) / std::log(base); };
    const double heel = rotation(kHeelFraction), toe = rotation(kToeFraction);
    const double theta = heel + (toe - heel) * std::clamp(position, 0.0, 1.0);
    return (std::pow(base, theta) - 1.0) / (base - 1.0);
  }

  double muTronFrequency(double ldr_ohms, int range) {
    return frequencyFor(ldr_ohms, range == kHigh ? kHighFarads : kLowFarads);
  }

  double muTronQ(double peak) {
    return (kSummerOhms + kPeakFixedOhms + peakPotOhms(peak)) / (2.0 * kSummerOhms);
  }

  double vactrolOhms(double led_amps) {
    if (led_amps <= 1.0e-9) return kDarkOhms;
    return std::min(kVactrolA / std::pow(led_amps, kVactrolExponent) + kVactrolFloorOhms, kDarkOhms);
  }

  Engine::Engine() { prepare(sample_rate_); }

  void Engine::prepare(double sample_rate) {
    const bool rebuild = pedals_[0].table.empty() || sample_rate != sample_rate_;
    sample_rate_ = sample_rate;
    smoothing_ = coefficientFor(kSmoothingSeconds, sample_rate);
    control_smoothing_ = coefficientFor(kControlSeconds, sample_rate / kControlInterval);
    vactrol_on_ = coefficientFor(kVactrolOnSeconds, sample_rate / kControlInterval);
    vactrol_off_ = coefficientFor(kVactrolOffSeconds, sample_rate / kControlInterval);
    fade_length_ = std::max(1, static_cast<int>(kFadeSeconds * sample_rate));
    if (rebuild)
      for (int model = 0; model < 2; ++model) buildPedal(model);
    reset();
  }

  void Engine::buildPedal(int model) {
    const PedalSpec& spec = kPedalSpecs[model];
    Pedal& pedal = pedals_[model];
    PedalCircuit circuit = makePedalCircuit(model);
    circuit.circuit.prepare(sample_rate_);
    pedal.table.resize(kTableSize);
    pedal.junction.resize(static_cast<size_t>(kTableSize) * kJunctionPoints * 2);
    // Q1 saturates when its collector falls to about 0.2 V above the emitter;
    // the collector's AC load is 22k || 470k || 470k || the pot || the amp.
    const double load = 1.0 / (1.0 / 22.0e3 + 2.0 / 470.0e3 + 1.0 / kPotOhms + 1.0 / kLoadOhms);
    const double saturation = (spec.collector_volts - spec.emitter_volts - 0.2) / load;
    double mid_peak_hz = 1000.0;
    for (int i = 0; i < kTableSize; ++i) {
      const double position = double(i) / (kTableSize - 1);
      circuit.setPot(potFraction(model, position));
      // Find the resonance (golden-section search on log frequency) and warp
      // the discretisation to put it exactly where the circuit has it.
      const auto level = [&](double log_hz) { return std::abs(circuit.circuit.analogResponse(circuit.output, std::exp(log_hz))); };
      double lo = std::log(150.0), hi = std::log(5000.0);
      constexpr double kGolden = 0.6180339887498949;
      double x1 = hi - kGolden * (hi - lo), x2 = lo + kGolden * (hi - lo);
      double f1 = level(x1), f2 = level(x2);
      for (int step = 0; step < 40; ++step) {
        if (f1 < f2) { lo = x1; x1 = x2; f1 = f2; x2 = lo + kGolden * (hi - lo); f2 = level(x2); }
        else { hi = x2; x2 = x1; f2 = f1; x1 = hi - kGolden * (hi - lo); f1 = level(x1); }
      }
      const double peak_hz = std::exp(0.5 * (lo + hi));
      if (i == kTableSize / 2) mid_peak_hz = peak_hz;
      circuit.circuit.setPrewarp(std::min(peak_hz, 0.4 * sample_rate_));
      circuit.circuit.update();
      const DiscreteSystem& s = circuit.circuit.system();
      PedalCoefficients& t = pedal.table[i];
      for (int k = 0; k < kStates; ++k) {
        for (int l = 0; l < kStates; ++l) t.a[k][l] = s.a[k][l];
        t.b[k] = s.b[k];
        t.c[k] = s.c[k][0];
        t.d[k] = s.d[0][k];
        t.g[k] = s.g[0][k];
      }
      t.e = s.e[0]; t.f = s.f[0][0]; t.h = s.h[0]; t.k = s.k[0][0];

      // Q1's port: v = p + K i(v). For this position's K, solve the collector
      // signal current i over the drive p, from deep cutoff to saturation:
      // an exponential that stops at cutoff and softens into saturation
      // where the collector meets the emitter.
      const double ic = spec.q1_amps, limit = 1.0 / saturation;
      const auto law = [&](double v, double& slope) {
        const double e = std::exp(v / kThermalVoltage);
        double current = ic * (e - 1.0);
        slope = ic / kThermalVoltage * e;
        if (current > 0.0) {
          const double r = current * limit, q = 1.0 / std::sqrt(1.0 + r * r);
          current *= q;
          slope *= q * q * q;
        }
        return current;
      };
      double unused;
      const double start = kJunctionLowVolts - t.k * law(kJunctionLowVolts, unused);
      const double end = kJunctionHighVolts - t.k * law(kJunctionHighVolts, unused);
      const double step = (end - start) / (kJunctionPoints - 1);
      pedal.junction_start[i] = start;
      pedal.junction_scale[i] = 1.0 / step;
      float* row = pedal.junction.data() + static_cast<size_t>(i) * kJunctionPoints * 2;
      double v = kJunctionLowVolts;
      for (int n = 0; n < kJunctionPoints; ++n) {
        const double p = start + step * n;
        // Newton with bisection safeguards; the residual rises with v.
        double low = kJunctionLowVolts - 1.0, high = kJunctionHighVolts + 1.0, current = 0.0, slope = 0.0;
        for (int iteration = 0; iteration < 100; ++iteration) {
          current = law(v, slope);
          const double residual = v - p - t.k * current;
          if (residual > 0.0) high = v; else low = v;
          double next = v - residual / (1.0 - t.k * slope);
          if (!(next > low && next < high)) next = 0.5 * (low + high);
          if (std::abs(next - v) < 1.0e-13) { v = next; break; }
          v = next;
        }
        current = law(v, slope);
        row[2 * n] = static_cast<float>(current);
        row[2 * n + 1] = static_cast<float>(slope / (1.0 - t.k * slope) * step);  // di/dp per step
      }
    }
    // The collector output inverts; flip it so a dry/wet blend adds in phase
    // at the resonance.
    circuit.setPot(potFraction(model, 0.5));
    pedal.polarity = circuit.circuit.analogResponse(circuit.output, mid_peak_hz).real() < 0.0 ? -1.0 : 1.0;
    pedal.buffered = spec.buffer_hz > 0.0;
    pedal.buffer_gain = spec.buffer_gain;
    // The buffer's input coupling, bilinear: y = a (x - x1) + c y1.
    const double w = std::tan(kPi * spec.buffer_hz / sample_rate_);
    pedal.buffer_a = spec.buffer_hz > 0.0 ? 1.0 / (1.0 + w) : 1.0;
    pedal.buffer_c = spec.buffer_hz > 0.0 ? (1.0 - w) / (1.0 + w) : 0.0;
  }

  void Engine::reset() {
    for (auto& pedal : pedals_) {
      for (auto& channel : pedal.state) channel.fill(0.0);
      pedal.buffer_state = {};
      pedal.position = -1.0;
    }
    mutron_.s1 = {};
    mutron_.s2 = {};
    envelope_volts_ = 0.0;
    position_ = parameters_.position;
    peak_ = parameters_.peak;
    mix_ = parameters_.mix;
    capacitance_ = parameters_.range == kHigh ? kHighFarads : kLowFarads;
    for (int m = 0; m < kNumModes; ++m) mutron_.mode_gain[m] = m == parameters_.mode ? 1.0 : 0.0;
    current_model_ = parameters_.model;
    fading_model_ = -1;
    fade_remaining_ = 0;
    // Start the controls settled at their resting values.
    controlMuTron(true);
    mutron_.g = mutron_.g_target;
  }

  void Engine::setParameters(const Parameters& parameters) {
    parameters_ = parameters;
    parameters_.model = std::clamp(parameters.model, 0, kNumModels - 1);
    parameters_.mode = std::clamp(parameters.mode, 0, kNumModes - 1);
    parameters_.range = std::clamp(parameters.range, 0, kNumRanges - 1);
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
      else {
        startModel(current_model_);
      }
    }
  }

  void Engine::startModel(int model) {
    // A model coming in starts from rest, as a pedal just plugged in.
    if (model == kMuTron) {
      mutron_.s1 = {};
      mutron_.s2 = {};
      controlMuTron(true);
      mutron_.g = mutron_.g_target;
      return;
    }
    Pedal& pedal = pedals_[model];
    for (auto& channel : pedal.state) channel.fill(0.0);
    pedal.buffer_state = {};
    pedal.position = -1.0;
  }

  void Engine::follow(const float* in_left, const float* in_right, int count) {
    // The Mu-Tron III's follower. Envelope's size is the gain into the
    // rectifier (50% = the instrument as it is); the pedals and Up use the
    // Up circuit, the Mu-Tron's Down its own higher-gain, faster-release one.
    const double amount = std::abs(parameters_.envelope);
    const double drive = 4.0 * amount * amount * kInstrumentVolts;
    const bool down = current_model_ == kMuTron && parameters_.envelope < 0.0;
    const double rectifier = down ? kDownRectifierGain : kUpRectifierGain;
    const double release = std::pow(4.0, 1.0 - 2.0 * parameters_.decay);  // conductance scale
    const double step = 1.0 / (sample_rate_ * kEnvelopeFarads);
    const double leak = release * (1.0 / kDischargeOhms + 1.0 / kFeedbackOhms + (down ? 1.0 / kDownLoadOhms : 0.0));
    const double leak_current = release * (kDischargeVolts / kDischargeOhms + (down ? kDownReferenceVolts / kDownLoadOhms : 0.0));
    double v = envelope_volts_;
    for (int n = 0; n < count; ++n) {
      const double x = drive * std::max(in_left[n], in_right[n]);
      const double rectified = std::min(rectifier * std::max(x, 0.0), kRectifierMaxVolts);
      const double charge = rectified > v ? 1.0 / kAttackOhms : 0.0;
      // Implicit Euler on C9: charging through 330 ohms while the rectifier
      // conducts, always discharging towards the negative rail.
      v = (v + step * (charge * rectified + leak_current)) / (1.0 + step * (charge + leak));
      v = std::max(v, 0.0);
    }
    envelope_volts_ = v;
  }

  void Engine::control() {
    const auto& p = parameters_;
    position_ += (p.position - position_) * control_smoothing_;
    peak_ += (p.peak - peak_) * control_smoothing_;
    const double farads = p.range == kHigh ? kHighFarads : kLowFarads;
    capacitance_ += (farads - capacitance_) * control_smoothing_;
    const bool up = p.envelope >= 0.0;

    // Pedals: the envelope rocks the pedal from Position.
    const double swing = envelope_volts_ / kLedSaturationVolts;
    pedal_position_ = std::clamp(position_ + (up ? swing : -swing), 0.0, 1.0);
    for (int model = 0; model < 2; ++model) {
      if (model != current_model_ && model != fading_model_) continue;
      Pedal& pedal = pedals_[model];
      if (pedal_position_ == pedal.position) continue;
      pedal.position = pedal_position_;
      const double index = pedal_position_ * (kTableSize - 1);
      const int i = std::min(static_cast<int>(index), kTableSize - 2);
      const double t = index - i;
      pedal.entry = i;
      pedal.entry_fraction = t;
      const PedalCoefficients& lo = pedal.table[i];
      const PedalCoefficients& hi = pedal.table[i + 1];
      PedalCoefficients& now = pedal.now;
      const auto lerp = [t](double a, double b) { return a + (b - a) * t; };
      for (int k = 0; k < kStates; ++k) {
        for (int l = 0; l < kStates; ++l) now.a[k][l] = lerp(lo.a[k][l], hi.a[k][l]);
        now.b[k] = lerp(lo.b[k], hi.b[k]);
        now.c[k] = lerp(lo.c[k], hi.c[k]);
        now.d[k] = lerp(lo.d[k], hi.d[k]);
        now.g[k] = lerp(lo.g[k], hi.g[k]);
      }
      now.e = lerp(lo.e, hi.e); now.f = lerp(lo.f, hi.f); now.h = lerp(lo.h, hi.h); now.k = lerp(lo.k, hi.k);
    }

    if (current_model_ == kMuTron || fading_model_ == kMuTron) controlMuTron(false);
  }

  void Engine::controlMuTron(bool settle) {
    // Position is the resting control voltage, the envelope adds (Up) or
    // subtracts (Down) from it; the LED current follows, the cells follow
    // the light with their lag (or at once when the Mu-Tron is switched in).
    const bool up = parameters_.envelope >= 0.0;
    const double volts = up ? position_ * kLedSaturationVolts + envelope_volts_
                            : position_ * kDownRestVolts - envelope_volts_;
    const double target = std::log(vactrolOhms(std::clamp(volts, 0.0, kLedSaturationVolts) / kSenseOhms));
    if (settle) ldr_log_ = target;
    else ldr_log_ += (target - ldr_log_) * (target < ldr_log_ ? vactrol_on_ : vactrol_off_);
    ldr_ohms_ = std::exp(ldr_log_);
    const double hz = std::min(frequencyFor(ldr_ohms_, capacitance_), 0.45 * sample_rate_);
    mutron_.g_target = std::tan(kPi * hz / sample_rate_);
    const double peak_ohms = kPeakFixedOhms + peakPotOhms(peak_);
    mutron_.damping = 2.0 * kSummerOhms / (kSummerOhms + peak_ohms);
    mutron_.input_gain = 2.0 * peak_ohms / (kSummerOhms + peak_ohms);
  }

  void Engine::renderPedal(int model, const float* in_left, const float* in_right, double* left, double* right,
                           int count) {
    // Both channels run side by side so their dependency chains overlap.
    Pedal& pedal = pedals_[model];
    const PedalCoefficients& s = pedal.now;
    const int j = pedal.entry;
    const double t = pedal.entry_fraction;
    const float* lo = pedal.junction.data() + static_cast<size_t>(j) * kJunctionPoints * 2;
    const float* hi = lo + kJunctionPoints * 2;
    const double lo_start = pedal.junction_start[j], lo_scale = pedal.junction_scale[j];
    const double hi_start = pedal.junction_start[j + 1], hi_scale = pedal.junction_scale[j + 1];
    const double scale = pedal.polarity / kInstrumentVolts;
    double x[kStates][2];
    for (int k = 0; k < kStates; ++k)
      for (int c = 0; c < 2; ++c) x[k][c] = pedal.state[c][k];

    for (int n = 0; n < count; ++n) {
      double u[2] = { in_left[n] * kInstrumentVolts, in_right[n] * kInstrumentVolts };
      if (pedal.buffered) {
        for (int c = 0; c < 2; ++c) {
          auto& buffer = pedal.buffer_state[c];
          const double high = pedal.buffer_a * (u[c] - buffer[0]) + pedal.buffer_c * buffer[1];
          buffer[0] = u[c];
          buffer[1] = high;
          u[c] = pedal.buffer_gain * high;
        }
      }
      double i[2];
      for (int c = 0; c < 2; ++c) {
        double p = s.h * u[c];
        for (int k = 0; k < kStates; ++k) p += s.g[k] * x[k][c];
        // Q1's collector signal current for this drive, from the solved
        // junction law at the two nearest pedal positions.
        const double a = junctionCurrent(lo, (p - lo_start) * lo_scale);
        const double b = junctionCurrent(hi, (p - hi_start) * hi_scale);
        i[c] = a + (b - a) * t;
      }
      double next[kStates][2];
      for (int c = 0; c < 2; ++c) {
        double y = s.e * u[c] + s.f * i[c];
        for (int k = 0; k < kStates; ++k) y += s.d[k] * x[k][c];
        (c == 0 ? left : right)[n] = y * scale;
      }
      for (int k = 0; k < kStates; ++k)
        for (int c = 0; c < 2; ++c) {
          double sum = s.b[k] * u[c] + s.c[k] * i[c];
          for (int l = 0; l < kStates; ++l) sum += s.a[k][l] * x[l][c];
          next[k][c] = sum;
        }
      for (int k = 0; k < kStates; ++k)
        for (int c = 0; c < 2; ++c) x[k][c] = next[k][c];
    }

    for (int c = 0; c < 2; ++c)
      for (int k = 0; k < kStates; ++k) pedal.state[c][k] = x[k][c];
  }

  void Engine::renderMuTron(const float* in_left, const float* in_right, double* left, double* right, int count) {
    // Trapezoidal (TPT) state-variable filter: the integrators' states are
    // their capacitor voltages, so the cells can move while it rings.
    MuTron& m = mutron_;
    const double g_start = m.g, g_step = (m.g_target - m.g) / count;
    const double k = m.damping, gain = m.input_gain;
    double g = g_start;
    for (int n = 0; n < count; ++n) {
      g += g_step;
      const double scale = 1.0 / (1.0 + g * (k + g));
      for (int mode = 0; mode < kNumModes; ++mode)
        m.mode_gain[mode] += ((mode == parameters_.mode ? 1.0 : 0.0) - m.mode_gain[mode]) * smoothing_;
      for (int channel = 0; channel < 2; ++channel) {
        const double u = gain * (channel == 0 ? in_left[n] : in_right[n]) * kInstrumentVolts;
        double& s1 = m.s1[channel];
        double& s2 = m.s2[channel];
        const double hp = (u - (k + g) * s1 - s2) * scale;
        const double bp = rail(g * hp + s1);
        s1 = 2.0 * bp - s1;
        const double lp = rail(g * bp + s2);
        s2 = 2.0 * lp - s2;
        const double y = m.mode_gain[kLowPass] * lp + m.mode_gain[kBandPass] * bp + m.mode_gain[kHighPass] * rail(hp);
        (channel == 0 ? left : right)[n] = y / kInstrumentVolts;
      }
    }
    m.g = m.g_target;
  }

  void Engine::render(int model, const float* in_left, const float* in_right, double* left, double* right, int count) {
    if (model == kMuTron) renderMuTron(in_left, in_right, left, right, count);
    else renderPedal(model, in_left, in_right, left, right, count);
  }

  void Engine::process(const float* in_left, const float* in_right, float* out_left, float* out_right, int count) {
    for (int offset = 0; offset < count;) {
      const int block = std::min(kControlInterval, count - offset);
      const float* l = in_left + offset;
      const float* r = in_right + offset;
      follow(l, r, block);
      control();

      double wet_left[kControlInterval], wet_right[kControlInterval];
      render(current_model_, l, r, wet_left, wet_right, block);
      if (fading_model_ >= 0) {
        double old_left[kControlInterval], old_right[kControlInterval];
        render(fading_model_, l, r, old_left, old_right, block);
        for (int n = 0; n < block; ++n) {
          const double t = std::clamp(1.0 - double(fade_remaining_ - n) / fade_length_, 0.0, 1.0);
          const double in_gain = std::sin(0.5 * kPi * t), out_gain = std::cos(0.5 * kPi * t);
          wet_left[n] = wet_left[n] * in_gain + old_left[n] * out_gain;
          wet_right[n] = wet_right[n] * in_gain + old_right[n] * out_gain;
        }
        fade_remaining_ = std::max(0, fade_remaining_ - block);
        if (fade_remaining_ == 0) fading_model_ = -1;
      }

      for (int n = 0; n < block; ++n) {
        mix_ += (parameters_.mix - mix_) * smoothing_;
        const double dry_gain = std::cos(0.5 * kPi * mix_), wet_gain = std::sin(0.5 * kPi * mix_);
        out_left[offset + n] = static_cast<float>(dry_gain * l[n] + wet_gain * wet_left[n]);
        out_right[offset + n] = static_cast<float>(dry_gain * r[n] + wet_gain * wet_right[n]);
      }
      offset += block;
    }
  }
}
