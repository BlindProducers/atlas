// SPDX-License-Identifier: GPL-3.0-or-later
#include "linear_circuit.h"

#include <cassert>
#include <cmath>
#include <utility>

namespace atlas::analogue {
  namespace {
    constexpr double kPi = 3.14159265358979323846;
    constexpr double kThermalVoltage = 0.025852;  // kT/q at 27 C

    double magnitude(double x) { return std::abs(x); }
    double magnitude(const std::complex<double>& x) { return std::abs(x); }

    // Solves matrix * x = rhs in place for `columns` right-hand sides with
    // partial pivoting. matrix is n x n row-major, rhs n x columns row-major.
    template <typename T>
    void solve(T* matrix, T* rhs, int* pivots, int n, int columns) {
      for (int k = 0; k < n; ++k) {
        int best = k;
        for (int i = k + 1; i < n; ++i)
          if (magnitude(matrix[i * n + k]) > magnitude(matrix[best * n + k])) best = i;
        pivots[k] = best;
        if (best != k) {
          for (int j = 0; j < n; ++j) std::swap(matrix[k * n + j], matrix[best * n + j]);
          for (int j = 0; j < columns; ++j) std::swap(rhs[k * columns + j], rhs[best * columns + j]);
        }
        const T pivot = matrix[k * n + k];
        assert(magnitude(pivot) > 0.0);
        for (int i = k + 1; i < n; ++i) {
          const T factor = matrix[i * n + k] / pivot;
          if (factor == T(0.0)) continue;
          for (int j = k; j < n; ++j) matrix[i * n + j] -= factor * matrix[k * n + j];
          for (int j = 0; j < columns; ++j) rhs[i * columns + j] -= factor * rhs[k * columns + j];
        }
      }
      for (int k = n - 1; k >= 0; --k)
        for (int j = 0; j < columns; ++j) {
          T sum = rhs[k * columns + j];
          for (int i = k + 1; i < n; ++i) sum -= matrix[k * n + i] * rhs[i * columns + j];
          rhs[k * columns + j] = sum / matrix[k * n + k];
        }
    }
  }

  int LinearCircuit::node() { return ++nodes_; }

  int LinearCircuit::resistor(int a, int b, double ohms) {
    resistors_.push_back({ a, b, ohms });
    return static_cast<int>(resistors_.size()) - 1;
  }

  int LinearCircuit::capacitor(int a, int b, double farads) {
    capacitors_.push_back({ a, b, farads });
    return static_cast<int>(capacitors_.size()) - 1;
  }

  void LinearCircuit::inductor(int a, int b, double henries) { inductors_.push_back({ a, b, henries }); }

  void LinearCircuit::opAmp(int plus, int minus, int output) { op_amps_.push_back({ plus, minus, output }); }

  void LinearCircuit::transconductance(int from, int to, int control_plus, int control_minus, double siemens) {
    transconductances_.push_back({ from, to, control_plus, control_minus, siemens });
  }

  void LinearCircuit::transistor(int base, int collector, int emitter, double collector_amps, double beta) {
    const double gm = collector_amps / kThermalVoltage;
    resistor(base, emitter, beta / gm);
    transconductance(collector, emitter, base, emitter, gm);
  }

  int LinearCircuit::port(int control_plus, int control_minus, const std::vector<std::pair<int, double>>& injections,
                          double small_signal_siemens) {
    ports_.push_back({ control_plus, control_minus, injections, small_signal_siemens });
    return static_cast<int>(ports_.size()) - 1;
  }

  int LinearCircuit::transistorPort(int base, int collector, int emitter, double collector_amps, double beta) {
    // The collector current leaves the collector node; the base current
    // (1/beta of it) leaves the base; both arrive at the emitter.
    return port(base, emitter, { { collector, -1.0 }, { base, -1.0 / beta }, { emitter, 1.0 + 1.0 / beta } },
                collector_amps / kThermalVoltage);
  }

  void LinearCircuit::input(int node) { input_node_ = node; }

  int LinearCircuit::output(int node) {
    output_nodes_.push_back(node);
    return static_cast<int>(output_nodes_.size()) - 1;
  }

  template <typename T, typename Admittance>
  void LinearCircuit::assemble(std::vector<T>& matrix, Admittance reactive, bool linearise_ports) const {
    const int n = unknowns();
    matrix.assign(static_cast<size_t>(n) * n, T(0.0));
    const auto at = [&](int row, int column) -> T& { return matrix[static_cast<size_t>(row) * n + column]; };
    const auto conductance = [&](int a, int b, T g) {
      if (a) at(a - 1, a - 1) += g;
      if (b) at(b - 1, b - 1) += g;
      if (a && b) { at(a - 1, b - 1) -= g; at(b - 1, a - 1) -= g; }
    };
    // A current `siemens * v(plus, minus)` injected into `node`.
    const auto injection = [&](int node, int plus, int minus, double siemens) {
      if (!node) return;
      if (plus) at(node - 1, plus - 1) -= T(siemens);
      if (minus) at(node - 1, minus - 1) += T(siemens);
    };
    for (const auto& r : resistors_) conductance(r.a, r.b, T(1.0 / r.value));
    for (const auto& c : capacitors_) conductance(c.a, c.b, reactive(true, c.value));
    for (const auto& l : inductors_) conductance(l.a, l.b, reactive(false, l.value));
    for (const auto& t : transconductances_) {
      injection(t.from, t.plus, t.minus, -t.siemens);
      injection(t.to, t.plus, t.minus, t.siemens);
    }
    if (linearise_ports)
      for (const auto& p : ports_)
        for (const auto& [node, weight] : p.injections) injection(node, p.plus, p.minus, weight * p.siemens);
    // The source's current flows into the input node.
    at(input_node_ - 1, nodes_) -= T(1.0);
    at(nodes_, input_node_ - 1) += T(1.0);
    for (int i = 0; i < static_cast<int>(op_amps_.size()); ++i) {
      const auto& o = op_amps_[i];
      const int column = nodes_ + 1 + i;
      at(o.output - 1, column) -= T(1.0);
      if (o.plus) at(column, o.plus - 1) += T(1.0);
      if (o.minus) at(column, o.minus - 1) -= T(1.0);
    }
  }

  void LinearCircuit::prepare(double sample_rate) {
    sample_rate_ = sample_rate;
    system_.states = static_cast<int>(capacitors_.size() + inductors_.size());
    system_.ports = static_cast<int>(ports_.size());
    system_.outputs = static_cast<int>(output_nodes_.size());
    assert(system_.states <= DiscreteSystem::kMaxStates && system_.ports <= DiscreteSystem::kMaxPorts &&
           system_.outputs <= DiscreteSystem::kMaxOutputs && input_node_ > 0);
    const int n = unknowns();
    matrix_.resize(static_cast<size_t>(n) * n);
    solution_.resize(static_cast<size_t>(n) * (system_.states + system_.ports + 1));
    pivots_.resize(n);
    update();
    reset();
  }

  void LinearCircuit::reset() {
    for (auto& channel : state_) channel.fill(0.0);
  }

  void LinearCircuit::update() {
    auto& s = system_;
    const int n = unknowns(), states = s.states, ports = s.ports, columns = states + ports + 1;
    const int input_column = states + ports;
    // The trapezoidal rule's 2/T, or its prewarped equivalent.
    double scale = 2.0 * sample_rate_;
    if (prewarp_hz_ > 0.0) {
      const double w = 2.0 * kPi * prewarp_hz_;
      scale = w / std::tan(0.5 * w / sample_rate_);
    }
    const auto discrete = [scale](bool capacitor, double value) {
      return capacitor ? value * scale : 1.0 / (value * scale);
    };
    // assemble() refills the matrix in place; its size never changes after prepare().
    assemble(matrix_, discrete, false);

    // Right-hand sides: each state's companion current source, each port's
    // unit current, then the input.
    std::fill(solution_.begin(), solution_.end(), 0.0);
    const auto rhs = [&](int node, int column) -> double& {
      return solution_[static_cast<size_t>(node - 1) * columns + column];
    };
    const int caps = static_cast<int>(capacitors_.size());
    const auto element = [&](int k) -> const Element& { return k < caps ? capacitors_[k] : inductors_[k - caps]; };
    for (int k = 0; k < states; ++k) {
      const auto& e = element(k);
      if (e.a) rhs(e.a, k) -= 1.0;
      if (e.b) rhs(e.b, k) += 1.0;
    }
    for (int p = 0; p < ports; ++p)
      for (const auto& [node, weight] : ports_[p].injections)
        if (node) rhs(node, states + p) += weight;
    solution_[static_cast<size_t>(nodes_) * columns + input_column] = 1.0;
    solve(matrix_.data(), solution_.data(), pivots_.data(), n, columns);

    const auto voltage = [&](int node, int column) {
      return node ? solution_[static_cast<size_t>(node - 1) * columns + column] : 0.0;
    };
    for (int k = 0; k < states; ++k) {
      const auto& e = element(k);
      const bool is_capacitor = k < caps;
      const double factor = (is_capacitor ? -2.0 : 2.0) * discrete(is_capacitor, e.value);
      const auto across = [&](int column) { return voltage(e.a, column) - voltage(e.b, column); };
      for (int l = 0; l < states; ++l)
        s.a[k][l] = factor * across(l) + (k == l ? (is_capacitor ? -1.0 : 1.0) : 0.0);
      for (int p = 0; p < ports; ++p) s.c[k][p] = factor * across(states + p);
      s.b[k] = factor * across(input_column);
    }
    for (int j = 0; j < s.outputs; ++j) {
      const int node = output_nodes_[j];
      for (int l = 0; l < states; ++l) s.d[j][l] = voltage(node, l);
      for (int p = 0; p < ports; ++p) s.f[j][p] = voltage(node, states + p);
      s.e[j] = voltage(node, input_column);
    }
    for (int q = 0; q < ports; ++q) {
      const auto across = [&](int column) { return voltage(ports_[q].plus, column) - voltage(ports_[q].minus, column); };
      for (int l = 0; l < states; ++l) s.g[q][l] = across(l);
      for (int p = 0; p < ports; ++p) s.k[q][p] = across(states + p);
      s.h[q] = across(input_column);
    }
  }

  void LinearCircuit::tick(int channel, double input, double* outputs) {
    const auto& s = system_;
    assert(s.ports == 0);
    auto& h = state_[channel];
    for (int j = 0; j < s.outputs; ++j) {
      double y = s.e[j] * input;
      for (int k = 0; k < s.states; ++k) y += s.d[j][k] * h[k];
      outputs[j] = y;
    }
    std::array<double, DiscreteSystem::kMaxStates> next;
    for (int k = 0; k < s.states; ++k) {
      double x = s.b[k] * input;
      for (int l = 0; l < s.states; ++l) x += s.a[k][l] * h[l];
      next[k] = x;
    }
    for (int k = 0; k < s.states; ++k) h[k] = next[k];
  }

  std::complex<double> LinearCircuit::analogResponse(int output, double hz) const {
    const std::complex<double> s(0.0, 2.0 * kPi * hz);
    std::vector<std::complex<double>> matrix;
    assemble(matrix, [s](bool capacitor, double value) {
      return capacitor ? s * value : 1.0 / (s * value);
    }, true);
    const int n = unknowns();
    std::vector<std::complex<double>> rhs(n, 0.0);
    std::vector<int> pivots(n);
    rhs[nodes_] = 1.0;
    solve(matrix.data(), rhs.data(), pivots.data(), n, 1);
    return rhs[output_nodes_[output] - 1];
  }

  std::complex<double> LinearCircuit::digitalResponse(int output, double hz) const {
    using Complex = std::complex<double>;
    const auto& s = system_;
    const int n = s.states, ports = s.ports;
    // Linearised ports, i = diag(siemens) p, close the loop:
    // i = (I - S K)^-1 S (G h + H u).
    std::vector<Complex> loop(static_cast<size_t>(ports) * ports), gain(static_cast<size_t>(ports) * (n + 1));
    std::vector<int> pivots(std::max(n, ports) + 1);
    for (int q = 0; q < ports; ++q) {
      const double siemens = ports_[q].siemens;
      for (int p = 0; p < ports; ++p) loop[q * ports + p] = (q == p ? 1.0 : 0.0) - siemens * s.k[q][p];
      for (int l = 0; l < n; ++l) gain[q * (n + 1) + l] = siemens * s.g[q][l];
      gain[q * (n + 1) + n] = siemens * s.h[q];
    }
    if (ports) solve(loop.data(), gain.data(), pivots.data(), ports, n + 1);
    // Effective linear system with the ports folded in.
    const auto port_current = [&](int p, int column) { return gain[p * (n + 1) + column]; };
    std::vector<Complex> a(static_cast<size_t>(n) * n), b(n), d(n);
    Complex e = s.e[output];
    for (int k = 0; k < n; ++k) {
      for (int l = 0; l < n; ++l) {
        Complex value = s.a[k][l];
        for (int p = 0; p < ports; ++p) value += s.c[k][p] * port_current(p, l);
        a[k * n + l] = value;
      }
      b[k] = s.b[k];
      d[k] = s.d[output][k];
      for (int p = 0; p < ports; ++p) {
        b[k] += s.c[k][p] * port_current(p, n);
        d[k] += s.f[output][p] * port_current(p, k);
      }
    }
    for (int p = 0; p < ports; ++p) e += s.f[output][p] * port_current(p, n);

    // H(z) = E + D (zI - A)^-1 B
    const Complex z = std::polar(1.0, 2.0 * kPi * hz / sample_rate_);
    std::vector<Complex> matrix(static_cast<size_t>(n) * n);
    for (int k = 0; k < n; ++k)
      for (int l = 0; l < n; ++l) matrix[k * n + l] = (k == l ? z : 0.0) - a[k * n + l];
    solve(matrix.data(), b.data(), pivots.data(), n, 1);
    Complex y = e;
    for (int k = 0; k < n; ++k) y += d[k] * b[k];
    return y;
  }
}
