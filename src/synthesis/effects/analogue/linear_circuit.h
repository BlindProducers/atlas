// SPDX-License-Identifier: GPL-3.0-or-later
// A small analogue circuit (resistors, capacitors, inductors, ideal op-amps,
// transconductances such as a transistor's small-signal gain, and a few
// nonlinear current ports) driven by one voltage source, discretised with the
// trapezoidal rule into a state-space system in the manner of the nodal DK
// method (Yeh; Holters & Zolzer, DAFx-11):
//
//   p[n] = G h[n-1] + H u[n] + K i[n]      port voltages
//   i[n] = f(p[n])                          nonlinear port currents
//   y[n] = D h[n-1] + E u[n] + F i[n]      outputs
//   h[n] = A h[n-1] + B u[n] + C i[n]      states
//
// The states are the trapezoidal companion sources of the capacitors and
// inductors, so they keep their physical meaning (charge and flux) when a
// resistor changes: a pot or a light-dependent resistor can move while audio
// runs, as on the hardware. Changing a value needs update(), a small dense
// solve that does not allocate.
//
// Nodes are numbered from 1; 0 is ground.
#pragma once

#include <array>
#include <complex>
#include <utility>
#include <vector>

namespace atlas::analogue {
  struct DiscreteSystem {
    static constexpr int kMaxStates = 8;
    static constexpr int kMaxPorts = 2;
    static constexpr int kMaxOutputs = 4;

    int states = 0, ports = 0, outputs = 0;
    std::array<std::array<double, kMaxStates>, kMaxStates> a {};
    std::array<double, kMaxStates> b {};
    std::array<std::array<double, kMaxPorts>, kMaxStates> c {};
    std::array<std::array<double, kMaxStates>, kMaxOutputs> d {};
    std::array<double, kMaxOutputs> e {};
    std::array<std::array<double, kMaxPorts>, kMaxOutputs> f {};
    std::array<std::array<double, kMaxStates>, kMaxPorts> g {};
    std::array<double, kMaxPorts> h {};
    std::array<std::array<double, kMaxPorts>, kMaxPorts> k {};
  };

  class LinearCircuit {
   public:
    static constexpr int kGround = 0;
    static constexpr int kMaxChannels = 2;

    // Building. Not real-time safe; do it once, then prepare().
    int node();
    int resistor(int a, int b, double ohms);    // returns a handle for setResistance
    int capacitor(int a, int b, double farads); // returns a handle for setCapacitance
    void inductor(int a, int b, double henries);
    // Ideal op-amp: drives `output` so that the inputs are equal.
    void opAmp(int plus, int minus, int output);
    // A current of siemens * v(control_plus, control_minus) flowing from node
    // `from` through the source into node `to` (an NPN's collector to emitter).
    void transconductance(int from, int to, int control_plus, int control_minus, double siemens);
    // Hybrid-pi small-signal NPN at a collector current and current gain.
    void transistor(int base, int collector, int emitter, double collector_amps, double beta);
    // A nonlinear current i = f(v(control_plus, control_minus)), injected
    // into each listed node with its weight. small_signal_siemens is df/dv
    // at rest, used by the response functions. Returns the port index.
    int port(int control_plus, int control_minus, const std::vector<std::pair<int, double>>& injections,
             double small_signal_siemens);
    // An NPN whose base-emitter junction is a nonlinear port: the port current
    // is the collector's signal current, and 1/beta of it flows in the base.
    int transistorPort(int base, int collector, int emitter, double collector_amps, double beta);
    void input(int node);
    int output(int node);  // returns the output index

    void prepare(double sample_rate);
    // Bilinear frequency warping is exact at this frequency (0: none, the
    // plain trapezoidal rule). Takes effect at the next update().
    void setPrewarp(double hz) { prewarp_hz_ = hz; }
    void setResistance(int handle, double ohms) { resistors_[handle].value = ohms; }
    void setCapacitance(int handle, double farads) { capacitors_[handle].value = farads; }
    // Recomputes the discrete system after value changes. Real-time safe.
    void update();
    const DiscreteSystem& system() const { return system_; }

    // One sample of one channel of a circuit without ports: writes every
    // output, then advances.
    void reset();
    void tick(int channel, double input, double* outputs);

    // Continuous-time small-signal response of an output (exact, for tests).
    std::complex<double> analogResponse(int output, double hz) const;
    // The discretised system's small-signal response (for tests).
    std::complex<double> digitalResponse(int output, double hz) const;

   private:
    struct Element { int a, b; double value; };
    struct Transconductance { int from, to, plus, minus; double siemens; };
    struct OpAmp { int plus, minus, output; };
    struct Port { int plus, minus; std::vector<std::pair<int, double>> injections; double siemens; };

    int unknowns() const { return nodes_ + 1 + static_cast<int>(op_amps_.size()); }
    template <typename T, typename Admittance>
    void assemble(std::vector<T>& matrix, Admittance reactive, bool linearise_ports) const;

    int nodes_ = 0, input_node_ = 0;
    std::vector<Element> resistors_, capacitors_, inductors_;
    std::vector<Transconductance> transconductances_;
    std::vector<OpAmp> op_amps_;
    std::vector<Port> ports_;
    std::vector<int> output_nodes_;
    double sample_rate_ = 48000.0, prewarp_hz_ = 0.0;

    DiscreteSystem system_;
    std::array<std::array<double, DiscreteSystem::kMaxStates>, kMaxChannels> state_ {};
    // update() scratch, sized by prepare().
    std::vector<double> matrix_, solution_;
    std::vector<int> pivots_;
  };
}
