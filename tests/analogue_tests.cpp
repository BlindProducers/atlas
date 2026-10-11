// Analogue effect regression tests: the shared BBD engine and the Analogue
// Chorus, Flanger and Echo effects built on it.
#include "JuceHeader.h"
#include "analogue/analog_filter.h"
#include "analogue/bbd.h"
#include "analogue/chorus_engine.h"
#include "analogue_chorus_module.h"
#include "analogue/flanger_engine.h"
#include "analogue_flanger_module.h"
#include "analogue/echo_engine.h"
#include "analogue_echo_module.h"
#include "analogue/rotary_engine.h"
#include "analogue_rotary_module.h"
#include "analogue/spring_engine.h"
#include "analogue_spring_module.h"
#include "analogue/linear_circuit.h"
#include "analogue/wah_engine.h"
#include "analogue_wah_module.h"
#include "reorderable_effect_chain.h"
#include "reverb_module.h"
#include "synth_parameters.h"
#include "synth_plugin.h"
#include <chrono>
#include <functional>
#include "synth_constants.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <iostream>
#include <new>
#include <string>
#include <vector>

namespace { thread_local bool track = false; thread_local size_t allocation_count = 0; }
void* operator new(std::size_t n) { if (track) ++allocation_count; if (void* p = std::malloc(n)) return p; throw std::bad_alloc(); }
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {
  using namespace atlas::analogue;
  constexpr double kPi = 3.14159265358979323846;
  int failures = 0;

  void expect(bool ok, const std::string& message) {
    if (!ok) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
  }

  BbdChip linearChip(int stages) {
    BbdChip chip;
    chip.stages = stages;
    chip.headroom = 1000.0;  // far above the test level: linear transfer
    chip.transfer_inefficiency = 0.0;
    return chip;
  }

  // Steady-state gain of one sine through the line, by quadrature correlation.
  double sineGain(BbdLine& line, double rate, double hz) {
    line.reset();
    const int settle = static_cast<int>(rate * (0.1 + line.delay()));
    const int measure = static_cast<int>(rate * 0.25);
    double c = 0.0, s = 0.0;
    for (int n = 0; n < settle + measure; ++n) {
      const double phase = 2.0 * kPi * hz * n / rate;
      const double y = line.process(std::sin(phase));
      if (n >= settle) { c += y * std::cos(phase); s += y * std::sin(phase); }
    }
    return 2.0 * std::sqrt(c * c + s * s) / measure;
  }

  void testAnalogFilter() {
    const auto butterworth = toPartialFractions(butterworthLowPass(4, 10000.0));
    expect(std::abs(butterworth.dcGain() - 1.0) < 1.0e-9, "Butterworth partial fractions keep unity DC gain");
    expect(std::abs(std::abs(butterworth.responseAtHz(10000.0)) - std::sqrt(0.5)) < 1.0e-9,
           "Butterworth partial fractions are -3 dB at the corner");
    // 1 nF / 10k Sallen-Key with C1 = 2 C2 is a Q = 0.707 section at 1/(2 pi sqrt(2) 10 us).
    const auto section = sallenKeyLowPass(10.0e3, 10.0e3, 2.0e-9, 1.0e-9);
    const double expected_hz = 1.0 / (2.0 * kPi * std::sqrt(2.0) * 10.0e3 * 1.0e-9);
    expect(std::abs(section.w0 / (2.0 * kPi) - expected_hz) < 1.0e-6 * expected_hz, "Sallen-Key corner from components");
    expect(std::abs(section.q - std::sqrt(0.5)) < 1.0e-9, "Sallen-Key Q from components");
  }

  // References rendered from SPICE models of the original circuits.
  std::vector<std::pair<double, double>> spiceReference(const std::string& name) {
    std::vector<std::pair<double, double>> rows;
    std::ifstream file(std::string(ATLAS_ANALOGUE_FIXTURES) + "/" + name + ".csv");
    std::string line;
    std::getline(file, line);
    while (std::getline(file, line)) {
      std::stringstream fields(line);
      std::string hz, db;
      std::getline(fields, hz, ',');
      std::getline(fields, db, ',');
      rows.emplace_back(std::stod(hz), std::stod(db));
    }
    expect(!rows.empty(), "SPICE reference " + name + " is present");
    return rows;
  }

  double worstDifference(const AnalogFilter& filter, const std::vector<std::pair<double, double>>& reference,
                         double max_hz, double offset_db = 0.0) {
    double worst = 0.0;
    for (const auto& [hz, db] : reference)
      if (hz <= max_hz)
        worst = std::max(worst, std::abs(20.0 * std::log10(std::abs(filter.responseAtHz(hz))) - db - offset_db));
    return worst;
  }

  void testSpiceReferences() {
    // Juno-60 shared Sallen-Key pair from its component values.
    const auto juno = spiceReference("juno60_sallen_key");
    const auto pair = toPartialFractions({ sallenKeyLowPass(22.0e3, 22.0e3, 820.0e-12, 680.0e-12),
                                           sallenKeyLowPass(22.0e3, 22.0e3, 1.8e-9, 270.0e-12) });
    expect(worstDifference(pair, juno, 40000.0) < 0.05, "Juno-60 Sallen-Key pair matches SPICE");
    // Holters & Parker's filter, fitted to a real Juno-60, agrees with the
    // schematic (Sallen-Key pair plus the per-BBD 10k/2.2n pole) within its
    // 0.28 dB DC trim and half a dB up to 15 kHz.
    AnalogFilter schematic = toPartialFractions({ sallenKeyLowPass(22.0e3, 22.0e3, 820.0e-12, 680.0e-12),
                                                  sallenKeyLowPass(22.0e3, 22.0e3, 1.8e-9, 270.0e-12),
                                                  lowPass1(1.0 / (2.0 * kPi * 10.0e3 * 2.2e-9)) });
    const AnalogFilter measured = fromPolesAndResidues({
      { { -46580.0, 0.0 }, { 251589.0, 0.0 } },
      { { -55482.0, 25082.0 }, { -130428.0, -4165.0 } },
      { { -26292.0, 59437.0 }, { 4634.0, 22873.0 } } });
    double worst = 0.0;
    for (double hz = 100.0; hz <= 15000.0; hz *= 1.05) {
      const double a = 20.0 * std::log10(std::abs(measured.responseAtHz(hz)));
      const double b = 20.0 * std::log10(std::abs(schematic.responseAtHz(hz)));
      worst = std::max(worst, std::abs(a - b + 0.28));
    }
    expect(worst < 0.5, "Holters-Parker Juno-60 filter agrees with the schematic (worst " + std::to_string(worst) + " dB)");

    // Boss CE-2 pre-emphasis: 47k feedback over 10k + 6.8n.
    const auto ce = spiceReference("ce2_pre_emphasis");
    const auto shelf = toPartialFractions({ shelf1(1.0 / (2.0 * kPi * 57.0e3 * 6.8e-9), 1.0 / (2.0 * kPi * 10.0e3 * 6.8e-9)) });
    expect(worstDifference(shelf, ce, 40000.0) < 0.05, "CE-2 pre-emphasis matches SPICE");
  }

  void testBbdResponse() {
    const auto filter = toPartialFractions(butterworthLowPass(4, 10000.0));
    for (double rate : { 44100.0, 48000.0, 96000.0 }) {
      BbdLine line;
      line.configure(linearChip(1024), filter, filter, rate);
      line.setDelay(0.005);
      line.reset();
      expect(std::abs(line.clockHz() - 102400.0) < 1.0e-6, "1024 stages at 5 ms clock at 102.4 kHz");
      // Expected: analog input and output filters times the bucket hold (sinc).
      double worst = 0.0;
      for (double hz : { 50.0, 1000.0, 5000.0, 9000.0, 12000.0, 15000.0 }) {
        const double x = kPi * hz / line.clockHz();
        const double expected = std::abs(filter.responseAtHz(hz) * filter.responseAtHz(hz)) * std::sin(x) / x;
        worst = std::max(worst, std::abs(20.0 * std::log10(sineGain(line, rate, hz) / expected)));
      }
      expect(worst < 0.1, "BBD response matches analog filters and bucket hold within 0.1 dB at " +
             std::to_string(static_cast<int>(rate)) + " Hz (worst " + std::to_string(worst) + " dB)");
    }
  }

  void testBbdDelayAndClock() {
    const double rate = 48000.0;
    const auto filter = toPartialFractions(butterworthLowPass(2, 15000.0));
    BbdLine line;
    line.configure(linearChip(8192), filter, filter, rate);
    for (double delay : { 0.010, 0.1, 1.2 }) {
      line.setDelay(delay);
      line.reset();
      expect(std::abs(line.delay() - delay) < 1.0e-9, "BBD reports the requested delay");
      // A step arrives after the bucket delay plus the filters' small group delay.
      int arrival = -1;
      const int length = static_cast<int>(rate * (delay + 0.05));
      for (int n = 0; n < length; ++n)
        if (line.process(1.0) > 0.5 && arrival < 0) arrival = n;
      const double error_ms = (arrival / rate - delay) * 1000.0;
      // A step can only leave the chain on a clock tick.
      const double tolerance_ms = 0.1 + 1000.0 / line.clockHz();
      expect(arrival > 0 && error_ms > 0.0 && error_ms < tolerance_ms,
             "step arrives at the bucket delay (" + std::to_string(delay) + " s, error " + std::to_string(error_ms) + " ms)");
    }

    // A slow clock: 8192 stages at 1.2 s clock at 3.41 kHz, so a 3 kHz tone
    // mostly folds below the bucket Nyquist, as on hardware.
    line.setDelay(1.2);
    line.reset();
    expect(std::abs(line.clockHz() - 8192.0 / 2.4) < 1.0e-6, "8192 stages at 1.2 s clock at 3.41 kHz");
    expect(sineGain(line, rate, 3000.0) < 0.2, "slow clock loses the top end and aliases it");
  }

  void testBbdGrouping() {
    // Grouped ticks behave like the full-rate chain in band (here the
    // echo's 3 kHz filters at an 8192-stage, 10-200 ms delay).
    const double rate = 48000.0;
    const auto filter = toPartialFractions(butterworthLowPass(4, 3000.0));
    BbdChip full = linearChip(8192), grouped = linearChip(8192);
    grouped.max_events_per_sample = 2.0;
    for (double delay : { 0.01, 0.05, 0.2 }) {
      BbdLine a, b;
      a.configure(full, filter, filter, rate);
      b.configure(grouped, filter, filter, rate);
      double worst = 0.0;
      for (double hz : { 100.0, 1000.0, 2500.0 }) {
        a.setDelay(delay); b.setDelay(delay);
        worst = std::max(worst, std::abs(20.0 * std::log10(sineGain(b, rate, hz) / sineGain(a, rate, hz))));
      }
      expect(worst < 0.05, "Grouped BBD ticks match the full-rate chain in band at " + std::to_string(delay) +
             " s (worst " + std::to_string(worst) + " dB)");
    }
  }

  void testBbdGlide() {
    // A delay change bends pitch continuously: no sample-to-sample jump
    // beyond what the shifted sine itself produces.
    const double rate = 48000.0;
    const auto filter = toPartialFractions(butterworthLowPass(4, 10000.0));
    BbdLine line;
    line.configure(linearChip(1024), filter, filter, rate);
    line.setDelay(0.005);
    line.reset();
    double previous = 0.0, worst = 0.0;
    allocation_count = 0;
    track = true;
    for (int n = 0; n < static_cast<int>(rate); ++n) {
      const double t = n / rate;
      line.setDelay(0.005 + 0.005 * std::clamp((t - 0.3) / 0.2, 0.0, 1.0));
      const double y = line.process(std::sin(2.0 * kPi * 1000.0 * t));
      if (n > 2000) worst = std::max(worst, std::abs(y - previous));
      previous = y;
    }
    track = false;
    expect(allocation_count == 0, "BBD processing does not allocate");
    // Slowing the clock lowers pitch, so the step bound stays below the steady tone's.
    expect(worst < 1.01 * 2.0 * kPi * 1000.0 / rate, "BBD glide is continuous");
  }

  void testBbdRobustness() {
    const double rate = 44100.0;
    BbdChip chip;
    chip.stages = 4096;
    chip.headroom = 1.0;
    chip.even_distortion = 0.02;
    chip.noise = 1.0e-4;
    chip.clock_leakage = 1.0e-3;
    const auto filter = toPartialFractions(butterworthLowPass(3, 6000.0));
    BbdLine line;
    line.configure(chip, filter, filter, rate);
    bool finite = true;
    double peak = 0.0;
    for (int n = 0; n < static_cast<int>(rate * 2); ++n) {
      // Extreme modulation and level: delay swept 10 ms - 2 s, input 20x headroom.
      line.setDelay(0.01 + 1.99 * (0.5 + 0.5 * std::sin(2.0 * kPi * 3.0 * n / rate)));
      const double y = line.process(20.0 * std::sin(2.0 * kPi * 220.0 * n / rate));
      finite = finite && std::isfinite(y);
      peak = std::max(peak, std::abs(y));
    }
    expect(finite, "BBD stays finite under extreme modulation and level");
    expect(peak < 4.0, "BBD output is bounded by the bucket headroom");
  }

  // ---------------------------------------------------------------- Chorus

  using Signal = std::vector<vital::poly_float>;

  Signal stereoSine(int count, double rate, double hz, double amplitude = 0.5) {
    Signal result(count);
    for (int i = 0; i < count; ++i) {
      const float left = static_cast<float>(amplitude * std::sin(2.0 * kPi * hz * i / rate));
      const float right = static_cast<float>(amplitude * std::sin(2.0 * kPi * hz * 1.01 * i / rate));
      result[i] = vital::poly_float(left, right, left, right);
    }
    return result;
  }

  struct ChorusFixture {
    vital::AnalogueChorusModule module;
    vital::control_map controls;
    explicit ChorusFixture(int rate = 48000) {
      module.init(); module.setSampleRate(rate); module.setOversampleAmount(1);
      controls = module.getControls();
    }
    void set(const std::string& id, float value) { controls.at("analoguechorus_" + id)->set(value); }
    Signal render(const Signal& input) {
      Signal out(input.size());
      allocation_count = 0; track = true;
      for (size_t i = 0; i < input.size(); i += vital::kMaxBufferSize) {
        const int size = static_cast<int>(std::min<size_t>(vital::kMaxBufferSize, input.size() - i));
        module.processWithInput(input.data() + i, size);
        std::copy_n(module.output()->buffer, size, out.data() + i);
      }
      track = false;
      expect(allocation_count == 0, "Analogue Chorus processing does not allocate");
      return out;
    }
  };

  double laneRms(const Signal& signal, int lane, size_t from = 0) {
    double sum = 0.0;
    for (size_t i = from; i < signal.size(); ++i) sum += double(signal[i][lane]) * signal[i][lane];
    return std::sqrt(sum / std::max<size_t>(1, signal.size() - from));
  }

  // Frequency of the pitch modulation on one lane of a chorused sine: track
  // instantaneous frequency from zero crossings, then find the strongest
  // modulation rate in [low, high] Hz.
  double modulationRate(const Signal& signal, int lane, double rate, double low, double high, size_t from) {
    std::vector<double> times, frequencies;
    double previous_crossing = -1.0;
    for (size_t i = from + 1; i < signal.size(); ++i) {
      const double a = signal[i - 1][lane], b = signal[i][lane];
      if (a < 0.0 && b >= 0.0) {
        const double crossing = (i - 1 + a / (a - b)) / rate;
        if (previous_crossing >= 0.0) {
          times.push_back(0.5 * (crossing + previous_crossing));
          frequencies.push_back(1.0 / (crossing - previous_crossing));
        }
        previous_crossing = crossing;
      }
    }
    double mean = 0.0;
    for (double f : frequencies) mean += f;
    mean /= std::max<size_t>(1, frequencies.size());
    double best = 0.0, best_power = -1.0;
    for (double hz = low; hz <= high; hz += (high - low) / 4000.0) {
      double c = 0.0, s = 0.0;
      for (size_t k = 0; k < frequencies.size(); ++k) {
        c += (frequencies[k] - mean) * std::cos(2.0 * kPi * hz * times[k]);
        s += (frequencies[k] - mean) * std::sin(2.0 * kPi * hz * times[k]);
      }
      if (c * c + s * s > best_power) { best_power = c * c + s * s; best = hz; }
    }
    return best;
  }

  void testChorusDryAndLanes() {
    ChorusFixture f;
    f.set("mix", 0.0f);
    const Signal input = stereoSine(4800, 48000.0, 440.0);
    const Signal out = f.render(input);
    bool dry = true, mirrored = true;
    for (size_t i = 0; i < out.size(); ++i) {
      dry = dry && out[i][0] == input[i][0] && out[i][1] == input[i][1];
      mirrored = mirrored && out[i][2] == out[i][0] && out[i][3] == out[i][1];
    }
    expect(dry, "Analogue Chorus at 0% Mix returns the dry signal exactly");
    expect(mirrored, "Analogue Chorus lanes 2 and 3 mirror left and right");
  }

  void testChorusModels() {
    using namespace atlas::analogue::chorus;
    for (int model = 0; model < kNumModels; ++model) {
      for (int mode = 0; mode < numModes(model); ++mode) {
        ChorusFixture f;
        f.set("model", model);
        f.set("juno_mode", model == kJuno ? mode : 0);
        f.set("dimension_mode", model == kDimension ? mode : 0);
        f.set("mix", 1.0f);
        const Signal out = f.render(stereoSine(48000, 48000.0, 330.0));
        bool finite = true;
        double peak = 0.0;
        for (const auto& frame : out)
          for (int lane = 0; lane < 2; ++lane) {
            finite = finite && std::isfinite(frame[lane]);
            peak = std::max(peak, double(std::abs(frame[lane])));
          }
        const std::string name = "model " + std::to_string(model) + " mode " + std::to_string(mode);
        expect(finite && peak < 2.0, "Analogue Chorus " + name + " is finite and bounded");
        expect(laneRms(out, 0, 24000) > 0.1 && laneRms(out, 1, 24000) > 0.1, "Analogue Chorus " + name + " passes wet signal");
        // Juno I+II moves both lines in phase, so its wet signal is mono.
        double side = 0.0;
        for (size_t i = 24000; i < out.size(); ++i) side += std::abs(out[i][0] - out[i][1]);
        if (!(model == kJuno && mode == kJunoBoth))
          expect(side > 1.0, "Analogue Chorus " + name + " is stereo at full width");

        f.set("width", 0.0f);
        f.module.hardReset();
        const Signal mono = f.render(stereoSine(9600, 48000.0, 330.0));
        double difference = 0.0;
        for (const auto& frame : mono) difference = std::max(difference, double(std::abs(frame[0] - frame[1])));
        expect(difference < 1.0e-6, "Analogue Chorus " + name + " is mono at zero width");
      }
    }
  }

  void testChorusJunoRates() {
    using namespace atlas::analogue::chorus;
    const double rate = 48000.0;
    const struct { int mode; double rate_scale; double expected; double low, high; int seconds; } cases[] = {
      { kJunoI, 1.0, 0.513, 0.2, 2.0, 12 },
      { kJunoII, 1.0, 0.863, 0.2, 2.0, 10 },
      { kJunoBoth, 1.0, 9.75, 4.0, 16.0, 8 },
      { kJunoI, 2.0, 1.026, 0.2, 2.0, 8 },
    };
    for (const auto& c : cases) {
      ChorusFixture f;
      f.set("juno_mode", c.mode);
      f.set("rate", static_cast<float>(c.rate_scale));
      f.set("mix", 1.0f);
      f.set("noise", 0.0f);
      const Signal out = f.render(stereoSine(c.seconds * 48000, rate, 1000.0));
      const double measured = modulationRate(out, 0, rate, c.low, c.high, 4800);
      expect(std::abs(measured - c.expected) < 0.03 * c.expected,
             "Juno mode " + std::to_string(c.mode) + " at rate x" + std::to_string(c.rate_scale) +
             " sweeps at " + std::to_string(c.expected) + " Hz (measured " + std::to_string(measured) + ")");
    }
  }

  void testChorusCenterDelay() {
    // The LFO sweeps the clock period, so at zero depth the delay rests midway
    // between its extremes, plus the filters' small group delay.
    using namespace atlas::analogue::chorus;
    const auto& spec = modeSpec(kJuno, kJunoI);
    const double expected = 0.5 * (spec.min_delay + spec.max_delay);
    ChorusFixture f;
    f.set("depth", 0.0f);
    f.set("mix", 1.0f);
    f.set("noise", 0.0f);
    Signal impulse(4800, vital::poly_float(0.0f));
    impulse[0] = vital::poly_float(1.0f);
    const Signal out = f.render(impulse);
    size_t peak = 0;
    for (size_t i = 0; i < out.size(); ++i) if (std::abs(out[i][0]) > std::abs(out[peak][0])) peak = i;
    const double measured = peak / 48000.0;
    expect(measured > expected && measured < expected + 0.0002,
           "Juno at zero depth delays by the centre clock (" + std::to_string(expected * 1000.0) +
           " ms, measured " + std::to_string(measured * 1000.0) + " ms)");
  }

  void testChorusNoise() {
    ChorusFixture quiet, authentic, loud;
    quiet.set("noise", 0.0f); authentic.set("noise", 0.5f); loud.set("noise", 1.0f);
    for (auto* f : { &quiet, &authentic, &loud }) f->set("mix", 1.0f);
    const Signal silence(48000, vital::poly_float(0.0f));
    const double quiet_rms = laneRms(quiet.render(silence), 0, 4800);
    const double authentic_rms = laneRms(authentic.render(silence), 0, 4800);
    const double loud_rms = laneRms(loud.render(silence), 0, 4800);
    expect(quiet_rms == 0.0, "Analogue Chorus at zero noise is silent with silent input");
    expect(authentic_rms > 1.0e-6 && authentic_rms < 1.0e-3, "Analogue Chorus hisses quietly at the original level");
    expect(std::abs(loud_rms / authentic_rms - 4.0) < 0.4, "Analogue Chorus noise at 100% is 12 dB above the original");
  }

  void testChorusModelSwitch() {
    // Changing model crossfades; no step larger than the signal's own slope.
    ChorusFixture f;
    f.set("mix", 1.0f);
    f.set("noise", 0.0f);
    const Signal input = stereoSine(48000, 48000.0, 220.0, 0.4);
    Signal out(input.size());
    for (size_t i = 0; i < input.size(); i += vital::kMaxBufferSize) {
      if (i == 24000 - 24000 % vital::kMaxBufferSize) f.set("model", atlas::analogue::chorus::kEnsemble);
      const int size = static_cast<int>(std::min<size_t>(vital::kMaxBufferSize, input.size() - i));
      f.module.processWithInput(input.data() + i, size);
      std::copy_n(f.module.output()->buffer, size, out.data() + i);
    }
    double worst = 0.0;
    for (size_t i = 4800; i < out.size(); ++i) worst = std::max(worst, double(std::abs(out[i][0] - out[i - 1][0])));
    expect(worst < 0.03, "Analogue Chorus model change is click-free (largest step " + std::to_string(worst) + ")");
  }

  void testChorusChains() {
    vital::Value bpm(2.0f), keytrack(0.0f);
    for (const std::string prefix : { "", "bus_1_", "bus_2_", "bus_3_" }) {
      vital::ReorderableEffectChain chain(bpm.output(), keytrack.output(), prefix, nullptr, nullptr, nullptr,
                                          nullptr, nullptr);
      chain.init();
      chain.setSampleRate(48000);
      auto controls = chain.getControls();
      controls.at(prefix + "fx_slot_1_type")->set(vital::constants::kAnalogueChorus + 1);
      controls.at(prefix + "fx_slot_1_enabled")->set(1);
      controls.at(prefix + "fx_slot_1_analoguechorus_mix")->set(1);
      Signal input = stereoSine(128, 48000.0, 1000.0);
      double energy = 0.0;
      for (int block = 0; block < 8; ++block) {
        chain.processWithInput(input.data(), 128);
        for (int i = 0; i < 128; ++i) energy += std::abs(chain.output()->buffer[i][0]);
      }
      expect(energy > 0.0 && std::isfinite(energy), "Analogue Chorus runs in chain '" + prefix + "'");
      controls.at(prefix + "fx_slot_1_enabled")->set(0);
      chain.processWithInput(input.data(), 128);
      bool dry = true;
      for (int i = 0; i < 128; ++i) dry = dry && chain.output()->buffer[i][0] == input[i][0];
      expect(dry, "Bypassing the Analogue Chorus slot returns dry audio in chain '" + prefix + "'");
    }
  }

  // --------------------------------------------------------------- Flanger

  struct FlangerFixture {
    vital::Value bps { 2.0f };
    vital::AnalogueFlangerModule module { bps.output() };
    vital::control_map controls;
    explicit FlangerFixture(int rate = 48000) {
      module.init(); module.setSampleRate(rate); module.setOversampleAmount(1);
      controls = module.getControls();
    }
    void set(const std::string& id, float value) { controls.at("analogueflanger_" + id)->set(value); }
    Signal render(const Signal& input) {
      Signal out(input.size());
      allocation_count = 0; track = true;
      for (size_t i = 0; i < input.size(); i += vital::kMaxBufferSize) {
        const int size = static_cast<int>(std::min<size_t>(vital::kMaxBufferSize, input.size() - i));
        module.processWithInput(input.data() + i, size);
        std::copy_n(module.output()->buffer, size, out.data() + i);
      }
      track = false;
      expect(allocation_count == 0, "Analogue Flanger processing does not allocate");
      return out;
    }
  };

  Signal monoSine(int count, double rate, double hz, double amplitude = 0.5) {
    Signal result(count);
    for (int i = 0; i < count; ++i)
      result[i] = vital::poly_float(static_cast<float>(amplitude * std::sin(2.0 * kPi * hz * i / rate)));
    return result;
  }

  void testFlangerDry() {
    FlangerFixture f;
    f.set("mix", 0.0f);
    for (int medium = 0; medium < 2; ++medium) {
      f.set("medium", medium);
      f.module.hardReset();
      const Signal input = stereoSine(4800, 48000.0, 440.0);
      const Signal out = f.render(input);
      bool dry = true, mirrored = true;
      for (size_t i = 0; i < out.size(); ++i) {
        dry = dry && out[i][0] == input[i][0] && out[i][1] == input[i][1];
        mirrored = mirrored && out[i][2] == out[i][0] && out[i][3] == out[i][1];
      }
      expect(dry, "Analogue Flanger at 0% Mix returns the dry signal exactly (medium " + std::to_string(medium) + ")");
      expect(mirrored, "Analogue Flanger lanes 2 and 3 mirror left and right");
    }
  }

  // Steady-state level of the flanger output for one sine, 50% mix, static comb.
  double flangerGain(double hz, float feedback, float manual = 0.5f) {
    FlangerFixture f;
    f.set("depth", 0.0f); f.set("manual", manual); f.set("feedback", feedback);
    f.set("noise", 0.0f); f.set("mix", 0.5f);
    const Signal out = f.render(monoSine(24000, 48000.0, hz, 0.25));
    return laneRms(out, 0, 12000) / (0.25 / std::sqrt(2.0));
  }

  void testFlangerComb() {
    using namespace atlas::analogue::flanger;
    // Manual 50% centres the delay in the BBD range (delay is linear in Manual).
    const auto range = bbdRange();
    const double delay = 0.5 * (range.min_delay + range.max_delay);
    // The comb's first notch sits at half the inverse delay (plus the BBD
    // filters' small group delay, which lowers it slightly).
    const double expected_notch = 0.5 / delay;
    double notch_hz = 0.0, notch_gain = 1e9;
    for (double hz = 0.7 * expected_notch; hz <= 1.1 * expected_notch; hz += expected_notch / 200.0) {
      const double gain = flangerGain(hz, 0.0f);
      if (gain < notch_gain) { notch_gain = gain; notch_hz = hz; }
    }
    expect(notch_hz > 0.85 * expected_notch && notch_hz <= 1.0 * expected_notch,
           "BBD flanger first notch near half the inverse delay (" + std::to_string(expected_notch) +
           " Hz, found " + std::to_string(notch_hz) + ")");
    expect(20.0 * std::log10(notch_gain / flangerGain(2.0 * notch_hz, 0.0f)) < -25.0,
           "BBD flanger notch is at least 25 dB deep at 50% mix");
    // Positive regeneration lifts the comb's peaks above the unregenerated level.
    const double peak_plain = flangerGain(2.0 * notch_hz, 0.0f);
    const double peak_regen = flangerGain(2.0 * notch_hz, 0.8f);
    expect(peak_regen > 1.5 * peak_plain, "Positive feedback makes the comb resonant");
  }

  void testFlangerBounded() {
    for (int medium = 0; medium < 2; ++medium)
      for (float feedback : { -1.0f, 1.0f }) {
        FlangerFixture f;
        f.set("medium", medium); f.set("feedback", feedback); f.set("mix", 1.0f);
        f.set("depth", 1.0f); f.set("frequency", 2.0f); f.set("age", 1.0f); f.set("noise", 1.0f);
        const Signal out = f.render(monoSine(96000, 48000.0, 150.0, 4.0));
        bool finite = true;
        double peak = 0.0;
        for (const auto& frame : out) {
          finite = finite && std::isfinite(frame[0]) && std::isfinite(frame[1]);
          peak = std::max(peak, double(std::max(std::abs(frame[0]), std::abs(frame[1]))));
        }
        expect(finite && peak < 8.0, "Analogue Flanger stays bounded at full regeneration (medium " +
               std::to_string(medium) + ", feedback " + std::to_string(feedback) + ", peak " + std::to_string(peak) + ")");
      }
  }

  void testFlangerThroughZero() {
    // Tape mode, Manual at the zero point, no sweep: the moving head sits on
    // the reference head. In phase they reinforce; inverted they cancel.
    const auto level = [](float feedback) {
      FlangerFixture f;
      f.set("medium", atlas::analogue::flanger::kTape); f.set("depth", 0.0f); f.set("manual", 0.5f);
      f.set("feedback", feedback); f.set("noise", 0.0f); f.set("age", 0.0f); f.set("mix", 1.0f);
      return laneRms(f.render(monoSine(24000, 48000.0, 1000.0, 0.25)), 0, 12000) / (0.25 / std::sqrt(2.0));
    };
    const double in_phase = level(0.0f), inverted = level(-0.001f);
    expect(in_phase > 0.8, "Tape flanger at the zero point passes the signal in phase");
    expect(20.0 * std::log10(inverted / in_phase) < -40.0, "Tape flanger with inverted polarity cancels at the zero point");
  }

  void testFlangerSync() {
    // 2 beats per second, synced 1/4: one sweep per beat, 2 Hz.
    FlangerFixture f;
    f.set("sync", 1.0f); f.set("tempo", 8.0f);
    f.set("depth", 0.3f); f.set("feedback", 0.0f); f.set("noise", 0.0f); f.set("mix", 1.0f);
    const Signal out = f.render(monoSine(6 * 48000, 48000.0, 1500.0));
    const double measured = modulationRate(out, 0, 48000.0, 0.5, 6.0, 4800);
    expect(std::abs(measured - 2.0) < 0.06, "Synced flanger sweeps once per quarter note (measured " + std::to_string(measured) + " Hz)");
  }

  void testFlangerMediumSwitch() {
    FlangerFixture f;
    f.set("mix", 1.0f); f.set("noise", 0.0f); f.set("feedback", 0.0f);
    const Signal input = monoSine(48000, 48000.0, 220.0, 0.4);
    Signal out(input.size());
    for (size_t i = 0; i < input.size(); i += vital::kMaxBufferSize) {
      if (i == 24000 - 24000 % vital::kMaxBufferSize) f.set("medium", atlas::analogue::flanger::kTape);
      const int size = static_cast<int>(std::min<size_t>(vital::kMaxBufferSize, input.size() - i));
      f.module.processWithInput(input.data() + i, size);
      std::copy_n(f.module.output()->buffer, size, out.data() + i);
    }
    double worst = 0.0;
    for (size_t i = 4800; i < out.size(); ++i) worst = std::max(worst, double(std::abs(out[i][0] - out[i - 1][0])));
    expect(worst < 0.03, "Analogue Flanger medium change is click-free (largest step " + std::to_string(worst) + ")");
  }

  void testEffectChains(int effect, const std::string& id) {
    vital::Value bpm(2.0f), keytrack(0.0f);
    for (const std::string prefix : { "", "bus_1_", "bus_2_", "bus_3_" }) {
      vital::ReorderableEffectChain chain(bpm.output(), keytrack.output(), prefix, nullptr, nullptr, nullptr,
                                          nullptr, nullptr);
      chain.init();
      chain.setSampleRate(48000);
      auto controls = chain.getControls();
      controls.at(prefix + "fx_slot_1_type")->set(effect + 1);
      controls.at(prefix + "fx_slot_1_enabled")->set(1);
      controls.at(prefix + "fx_slot_1_" + id + "_mix")->set(1);
      Signal input = stereoSine(128, 48000.0, 1000.0);
      double energy = 0.0;
      for (int block = 0; block < 8; ++block) {
        chain.processWithInput(input.data(), 128);
        for (int i = 0; i < 128; ++i) energy += std::abs(chain.output()->buffer[i][0]);
      }
      expect(energy > 0.0 && std::isfinite(energy), id + " runs in chain '" + prefix + "'");
      controls.at(prefix + "fx_slot_1_enabled")->set(0);
      chain.processWithInput(input.data(), 128);
      bool dry = true;
      for (int i = 0; i < 128; ++i) dry = dry && chain.output()->buffer[i][0] == input[i][0];
      expect(dry, "Bypassing the " + id + " slot returns dry audio in chain '" + prefix + "'");
    }
  }

  // ------------------------------------------------------------------ Echo

  struct EchoFixture {
    vital::Value bps { 2.0f };
    vital::AnalogueEchoModule module { bps.output() };
    vital::control_map controls;
    explicit EchoFixture(int rate = 48000) {
      module.init(); module.setSampleRate(rate); module.setOversampleAmount(1);
      controls = module.getControls();
    }
    void set(const std::string& id, float value) { controls.at("analogueecho_" + id)->set(value); }
    // Free time in seconds; stored as a rate like Vital's Delay.
    void setTime(double seconds) { set("sync", 0.0f); set("frequency", static_cast<float>(std::log2(1.0 / seconds))); }
    // A clean single-repeat setup for measurements.
    void clean() {
      set("feedback", 0.0f); set("wow", 0.0f); set("flutter", 0.0f); set("noise", 0.0f); set("age", 0.0f);
      set("drive", 0.0f); set("mix", 1.0f); set("inertia", 0.0f);
      for (int head = 1; head <= 3; ++head) set("head_" + std::to_string(head) + "_pan", 0.0f);
    }
    void onlyHead(int head) {
      for (int h = 1; h <= 3; ++h) set("head_" + std::to_string(h), h == head ? 1.0f : 0.0f);
    }
    Signal render(const Signal& input) {
      Signal out(input.size());
      allocation_count = 0; track = true;
      for (size_t i = 0; i < input.size(); i += vital::kMaxBufferSize) {
        const int size = static_cast<int>(std::min<size_t>(vital::kMaxBufferSize, input.size() - i));
        module.processWithInput(input.data() + i, size);
        std::copy_n(module.output()->buffer, size, out.data() + i);
      }
      track = false;
      expect(allocation_count == 0, "Analogue Echo processing does not allocate");
      return out;
    }
  };

  // Time of the strongest response to a short raised-cosine burst at `at`.
  double echoArrival(EchoFixture& f, int length, int at = 480) {
    Signal input(length, vital::poly_float(0.0f));
    for (int i = 0; i < 48; ++i)
      input[at + i] = vital::poly_float(static_cast<float>(0.5 - 0.5 * std::cos(2.0 * kPi * i / 48.0)));
    const Signal out = f.render(input);
    size_t peak = 0;
    for (size_t i = 0; i < out.size(); ++i) if (std::abs(out[i][0]) > std::abs(out[peak][0])) peak = i;
    return (static_cast<double>(peak) - at - 24) / 48000.0;
  }

  void testEchoDry() {
    for (int medium = 0; medium < 2; ++medium) {
      EchoFixture f;
      f.set("medium", medium); f.set("mix", 0.0f);
      const Signal input = stereoSine(9600, 48000.0, 440.0);
      const Signal out = f.render(input);
      bool dry = true, mirrored = true;
      for (size_t i = 0; i < out.size(); ++i) {
        dry = dry && out[i][0] == input[i][0] && out[i][1] == input[i][1];
        mirrored = mirrored && out[i][2] == out[i][0] && out[i][3] == out[i][1];
      }
      expect(dry, "Analogue Echo at 0% Mix returns the dry signal exactly (medium " + std::to_string(medium) + ")");
      expect(mirrored, "Analogue Echo lanes 2 and 3 mirror left and right");
    }
  }

  void testEchoHeads() {
    // Free: RE-201 measured spacing. Synced 1/8 at 2 beats per second: 250 ms
    // and exactly 2x and 3x.
    const auto free = atlas::analogue::echo::headRatios(false);
    for (int head = 1; head <= 3; ++head) {
      EchoFixture f;
      f.clean(); f.setTime(0.1); f.onlyHead(head);
      const double measured = echoArrival(f, 48000);
      const double expected = 0.1 * free[head - 1];
      expect(std::abs(measured - expected) < 0.0005, "Free tape head " + std::to_string(head) + " repeats at " +
             std::to_string(expected * 1000.0) + " ms (measured " + std::to_string(measured * 1000.0) + ")");
    }
    for (int head = 1; head <= 3; ++head) {
      EchoFixture f;
      f.clean(); f.set("sync", 1.0f); f.set("tempo", 9.0f); f.onlyHead(head);
      const double measured = echoArrival(f, 48000);
      const double expected = 0.25 * head;
      expect(std::abs(measured - expected) < 0.0005, "Synced tape head " + std::to_string(head) + " lands on " +
             std::to_string(head) + " eighth notes (measured " + std::to_string(measured * 1000.0) + " ms)");
    }
    EchoFixture bbd;
    bbd.clean(); bbd.set("medium", 1.0f); bbd.setTime(0.3);
    const double measured = echoArrival(bbd, 24000);
    expect(std::abs(measured - 0.3) < 0.002, "BBD echo repeats at 300 ms (measured " + std::to_string(measured * 1000.0) + ")");
  }

  void testEchoFeedback() {
    for (int medium = 0; medium < 2; ++medium) {
      const auto tail = [medium](float feedback) {
        EchoFixture f;
        f.set("medium", medium); f.setTime(0.12); f.set("feedback", feedback); f.set("mix", 1.0f);
        f.set("noise", 0.0f); f.set("drive", 0.3f);
        Signal input(48000 * 6, vital::poly_float(0.0f));
        for (int i = 0; i < 4800; ++i)
          input[i] = vital::poly_float(static_cast<float>(0.4 * std::sin(2.0 * kPi * 300.0 * i / 48000.0)));
        const Signal out = f.render(input);
        double peak = 0.0, late = 0.0;
        bool finite = true;
        for (size_t i = 0; i < out.size(); ++i) {
          finite = finite && std::isfinite(out[i][0]) && std::isfinite(out[i][1]);
          peak = std::max(peak, double(std::abs(out[i][0])));
          if (i > out.size() - 48000) late = std::max(late, double(std::abs(out[i][0])));
        }
        expect(finite && peak < 2.0, "Analogue Echo stays bounded at feedback " + std::to_string(feedback));
        return late;
      };
      const std::string name = medium ? "BBD" : "Tape";
      expect(tail(1.2f) > 0.05, name + " echo self-oscillates above 100% feedback");
      expect(tail(0.6f) < 1.0e-3, name + " echo decays below 100% feedback");
    }
  }

  void testEchoGlide() {
    // Doubling the time slows the tape: the repeats bend flat, then recover.
    // Without inertia the speed jumps and the pitch only blips.
    const auto lowest = [](float inertia) {
      EchoFixture f;
      f.clean(); f.setTime(0.1); f.onlyHead(1); f.set("inertia", inertia);
      Signal input = monoSine(48000 * 3, 48000.0, 1000.0, 0.4);
      Signal out(input.size());
      for (size_t i = 0; i < input.size(); i += vital::kMaxBufferSize) {
        if (i == 48000 - 48000 % vital::kMaxBufferSize) f.setTime(0.2);
        const int size = static_cast<int>(std::min<size_t>(vital::kMaxBufferSize, input.size() - i));
        f.module.processWithInput(input.data() + i, size);
        std::copy_n(f.module.output()->buffer, size, out.data() + i);
      }
      double low = 1e9;
      for (size_t start = 48000 + 9600; start + 2400 < out.size(); start += 2400) {
        int crossings = 0; double first = -1.0, last = -1.0;
        for (size_t i = start + 1; i < start + 2400; ++i)
          if (out[i - 1][0] < 0.0f && out[i][0] >= 0.0f) {
            const double t = i - 1 + out[i - 1][0] / (out[i - 1][0] - out[i][0]);
            if (first < 0.0) first = t;
            last = t; ++crossings;
          }
        if (crossings > 2) low = std::min(low, (crossings - 1) / ((last - first) / 48000.0));
      }
      return low;
    };
    const double gliding = lowest(0.5f);
    expect(gliding > 960.0 && gliding < 990.0, "Tape glide bends the repeats flat (lowest " + std::to_string(gliding) + " Hz)");
    expect(lowest(0.0f) > 995.0, "Without inertia the steady repeats keep their pitch");
  }

  void testEchoDarkensWithTime() {
    const auto level = [](int medium, double seconds, double hz) {
      EchoFixture f;
      f.clean(); f.set("medium", medium); f.setTime(seconds); f.onlyHead(1);
      const int length = static_cast<int>(48000 * (seconds + 0.5));
      const Signal out = f.render(monoSine(length, 48000.0, hz, 0.1));
      return laneRms(out, 0, static_cast<size_t>(48000 * (seconds + 0.2)));
    };
    expect(level(0, 0.6, 3000.0) < 0.5 * level(0, 0.06, 3000.0), "Slower tape (longer echo) is darker");
    expect(level(1, 1.0, 1500.0) < 0.5 * level(1, 0.1, 1500.0), "Slower BBD clock (longer echo) is darker");
  }

  void testEchoMediumSwitch() {
    EchoFixture f;
    f.clean(); f.setTime(0.05); f.set("mix", 0.5f);
    const Signal input = monoSine(48000, 48000.0, 220.0, 0.4);
    Signal out(input.size());
    for (size_t i = 0; i < input.size(); i += vital::kMaxBufferSize) {
      if (i == 24000 - 24000 % vital::kMaxBufferSize) f.set("medium", atlas::analogue::echo::kBbd);
      const int size = static_cast<int>(std::min<size_t>(vital::kMaxBufferSize, input.size() - i));
      f.module.processWithInput(input.data() + i, size);
      std::copy_n(f.module.output()->buffer, size, out.data() + i);
    }
    double worst = 0.0;
    for (size_t i = 4800; i < out.size(); ++i) worst = std::max(worst, double(std::abs(out[i][0] - out[i - 1][0])));
    expect(worst < 0.03, "Analogue Echo medium change is click-free (largest step " + std::to_string(worst) + ")");
  }

  // ---------------------------------------------------------------- Rotary

  struct RotaryFixture {
    vital::AnalogueRotaryModule module;
    vital::control_map controls;
    explicit RotaryFixture(int rate = 48000) {
      module.init(); module.setSampleRate(rate); module.setOversampleAmount(1);
      controls = module.getControls();
    }
    void set(const std::string& id, float value) { controls.at("analoguerotary_" + id)->set(value); }
    Signal render(const Signal& input) {
      Signal out(input.size());
      allocation_count = 0; track = true;
      for (size_t i = 0; i < input.size(); i += vital::kMaxBufferSize) {
        const int size = static_cast<int>(std::min<size_t>(vital::kMaxBufferSize, input.size() - i));
        module.processWithInput(input.data() + i, size);
        std::copy_n(module.output()->buffer, size, out.data() + i);
      }
      track = false;
      expect(allocation_count == 0, "Analogue Rotary processing does not allocate");
      return out;
    }
  };

  void testRotaryDry() {
    RotaryFixture f;
    f.set("mix", 0.0f);
    const Signal input = stereoSine(9600, 48000.0, 440.0);
    const Signal out = f.render(input);
    bool dry = true, mirrored = true;
    for (size_t i = 0; i < out.size(); ++i) {
      dry = dry && out[i][0] == input[i][0] && out[i][1] == input[i][1];
      mirrored = mirrored && out[i][2] == out[i][0] && out[i][3] == out[i][1];
    }
    expect(dry, "Analogue Rotary at 0% Mix returns the dry signal exactly");
    expect(mirrored, "Analogue Rotary lanes 2 and 3 mirror left and right");
  }

  void testRotarySpeeds() {
    using namespace atlas::analogue::rotary;
    const auto run = [](Engine& engine, double seconds) {
      std::vector<float> silence(128, 0.0f), left(128), right(128);
      for (int block = 0; block < static_cast<int>(seconds * 48000.0 / 128); ++block)
        engine.process(silence.data(), silence.data(), left.data(), right.data(), 128);
    };
    for (int model = 0; model < kNumModels; ++model) {
      Engine engine;
      engine.prepare(48000.0);
      Parameters p;
      p.model = model;
      engine.setParameters(p);
      engine.reset();
      const auto& drum = drumSpec(model);
      expect(std::abs(engine.drumHz() - drum.slow_hz) < 1e-9, "Rotary starts at its slow speed");
      p.speed = kFast;
      engine.setParameters(p);
      double horn_time = -1.0, drum_time = -1.0;
      for (int step = 1; step <= 300; ++step) {
        run(engine, 0.1);
        if (horn_time < 0.0 && engine.hornHz() > 0.9 * hornSpec().fast_hz) horn_time = step * 0.1;
        if (drum_time < 0.0 && engine.drumHz() > 0.9 * drum.fast_hz) drum_time = step * 0.1;
      }
      expect(std::abs(engine.drumHz() - drum.fast_hz) < 0.01, "Rotary drum settles at its fast speed");
      if (model == kOrgan) {
        expect(std::abs(engine.hornHz() - hornSpec().fast_hz) < 0.01, "Rotary horn settles at its fast speed");
        expect(horn_time > 0.0 && drum_time > horn_time, "The light horn reaches speed before the heavy drum");
      }
      p.speed = kStop;
      engine.setParameters(p);
      run(engine, 30.0);
      expect(engine.drumHz() < 0.01 && (model != kOrgan || engine.hornHz() < 0.01), "Stop lets the rotors coast to rest");
    }
    Engine instant;
    instant.prepare(48000.0);
    Parameters p;
    p.inertia = 0.0;
    instant.setParameters(p);
    instant.reset();
    p.speed = kFast;
    instant.setParameters(p);
    run(instant, 0.01);
    expect(std::abs(instant.hornHz() - hornSpec().fast_hz) < 1e-6, "Without inertia the rotors change speed at once");
  }

  void testRotarySwirl() {
    // The horn alone at Fast: its directivity swings the level at the
    // rotation rate.
    RotaryFixture f;
    f.set("speed", 1.0f); f.set("balance", 1.0f); f.set("drive", 0.0f);
    const Signal out = f.render(monoSine(48000 * 4, 48000.0, 3000.0, 0.3));
    std::vector<double> envelope, times;
    for (size_t start = 48000; start + 240 <= out.size(); start += 240) {
      double sum = 0.0;
      for (size_t i = start; i < start + 240; ++i) sum += double(out[i][0]) * out[i][0];
      envelope.push_back(std::sqrt(sum / 240.0));
      times.push_back((start + 120) / 48000.0);
    }
    double mean = 0.0;
    for (double e : envelope) mean += e;
    mean /= envelope.size();
    double best = 0.0, best_power = -1.0;
    for (double hz = 1.0; hz <= 12.0; hz += 0.01) {
      double c = 0.0, s = 0.0;
      for (size_t k = 0; k < envelope.size(); ++k) {
        c += (envelope[k] - mean) * std::cos(2.0 * kPi * hz * times[k]);
        s += (envelope[k] - mean) * std::sin(2.0 * kPi * hz * times[k]);
      }
      if (c * c + s * s > best_power) { best_power = c * c + s * s; best = hz; }
    }
    const double expected = atlas::analogue::rotary::hornSpec().fast_hz;
    expect(std::abs(best - expected) < 0.1, "Horn swirl follows its rotation (" + std::to_string(best) + " Hz)");

    RotaryFixture mono;
    mono.set("spread", 0.0f);
    const Signal centred = mono.render(monoSine(9600, 48000.0, 1000.0, 0.3));
    double difference = 0.0;
    for (const auto& frame : centred) difference = std::max(difference, double(std::abs(frame[0] - frame[1])));
    expect(difference < 1.0e-6, "Rotary at zero spread is mono");
    expect(mono.module.getMonoModulationDestination("analoguerotary_speed") != nullptr,
           "Rotary Speed can be modulated (mod wheel as a half-moon switch)");
  }

  void testRotaryModelSwitch() {
    RotaryFixture f;
    f.set("speed", 1.0f);
    const Signal input = monoSine(48000, 48000.0, 220.0, 0.4);
    Signal out(input.size());
    for (size_t i = 0; i < input.size(); i += vital::kMaxBufferSize) {
      if (i == 24000 - 24000 % vital::kMaxBufferSize) f.set("model", atlas::analogue::rotary::kGuitar);
      const int size = static_cast<int>(std::min<size_t>(vital::kMaxBufferSize, input.size() - i));
      f.module.processWithInput(input.data() + i, size);
      std::copy_n(f.module.output()->buffer, size, out.data() + i);
    }
    double worst = 0.0, steady = 0.0;
    for (size_t i = 4800; i < out.size(); ++i) {
      const double step = std::abs(out[i][0] - out[i - 1][0]);
      worst = std::max(worst, step);
      if (i < 20000) steady = std::max(steady, step);
    }
    expect(worst < 1.5 * steady + 0.005, "Analogue Rotary model change is click-free (largest step " +
           std::to_string(worst) + ", steady " + std::to_string(steady) + ")");
  }

  // ---------------------------------------------------------------- Spring

  struct SpringFixture {
    vital::AnalogueSpringModule module;
    vital::control_map controls;
    explicit SpringFixture(int rate = 48000) {
      module.init(); module.setSampleRate(rate); module.setOversampleAmount(1);
      controls = module.getControls();
    }
    void set(const std::string& id, float value) { controls.at("analoguespring_" + id)->set(value); }
    Signal render(const Signal& input) {
      Signal out(input.size());
      allocation_count = 0; track = true;
      for (size_t i = 0; i < input.size(); i += vital::kMaxBufferSize) {
        const int size = static_cast<int>(std::min<size_t>(vital::kMaxBufferSize, input.size() - i));
        module.processWithInput(input.data() + i, size);
        std::copy_n(module.output()->buffer, size, out.data() + i);
      }
      track = false;
      expect(allocation_count == 0, "Analogue Spring processing does not allocate");
      return out;
    }
  };

  void testSpringDry() {
    SpringFixture f;
    f.set("mix", 0.0f);
    const Signal input = stereoSine(9600, 48000.0, 440.0);
    const Signal out = f.render(input);
    bool dry = true, mirrored = true;
    for (size_t i = 0; i < out.size(); ++i) {
      dry = dry && out[i][0] == input[i][0] && out[i][1] == input[i][1];
      mirrored = mirrored && out[i][2] == out[i][0] && out[i][3] == out[i][1];
    }
    expect(dry, "Analogue Spring at 0% Mix returns the dry signal exactly");
    expect(mirrored, "Analogue Spring lanes 2 and 3 mirror left and right");
  }

  void testSpringTone() {
    for (int tone : { 0, 50, 100 }) {
      double worst = 0.0;
      for (const auto& [hz, db] : spiceReference("fender6g15_tone_" + std::to_string(tone)))
        worst = std::max(worst, std::abs(20.0 * std::log10(atlas::analogue::spring::toneResponse(tone / 100.0, hz)) - db));
      expect(worst < 0.05, "6G15 tone network at " + std::to_string(tone) + "% matches SPICE (worst " + std::to_string(worst) + " dB)");
    }
  }

  // Broadband RT60 from the Schroeder integral of an impulse response.
  double springDecay(int tank, int rate) {
    using namespace atlas::analogue::spring;
    Engine engine;
    engine.prepare(rate);
    Parameters p;
    p.tank = tank; p.mix = 1.0; p.noise = 0.0; p.tone = 1.0;
    engine.setParameters(p);
    engine.reset();
    const int length = rate * 7;
    std::vector<float> in(length, 0.0f), left(length), right(length);
    in[0] = 0.5f;
    for (int i = 0; i < length; i += 128)
      engine.process(in.data() + i, in.data() + i, left.data() + i, right.data() + i, std::min(128, length - i));
    std::vector<double> integral(length);
    double sum = 0.0;
    for (int i = length - 1; i >= 0; --i) { sum += double(left[i]) * left[i]; integral[i] = sum; }
    double t5 = -1.0, t35 = -1.0;
    for (int i = 0; i < length; ++i) {
      const double db = 10.0 * std::log10(integral[i] / integral[0]);
      if (t5 < 0.0 && db < -5.0) t5 = double(i) / rate;
      if (t35 < 0.0 && db < -35.0) t35 = double(i) / rate;
    }
    return 2.0 * (t35 - t5);
  }

  void testSpringDecay() {
    using namespace atlas::analogue::spring;
    for (int rate : { 44100, 48000, 96000 })
      for (int tank = 0; tank < kNumTanks; ++tank) {
        const double measured = springDecay(tank, rate), expected = tankSpec(tank).decay_seconds;
        expect(std::abs(measured / expected - 1.0) < 0.1, "Spring tank " + std::to_string(tank) + " at " + std::to_string(rate) +
               " Hz decays in its rated time (" + std::to_string(expected) + " s, measured " + std::to_string(measured) + ")");
      }
  }

  void testSpringChirp() {
    // Each echo rises toward the spring's transition: in the Long tank's
    // direct pass (before its first echo returns at 33 ms), 3.6 kHz arrives
    // some 20 ms after 600 Hz.
    using namespace atlas::analogue::spring;
    Engine engine;
    engine.prepare(48000.0);
    Parameters p;
    p.mix = 1.0; p.noise = 0.0; p.tone = 1.0;
    engine.setParameters(p);
    engine.reset();
    const int length = 48000 / 5;
    std::vector<float> in(length, 0.0f), left(length), right(length);
    in[0] = 0.5f;
    engine.process(in.data(), in.data(), left.data(), right.data(), length);
    const auto arrival = [&](double hz) {
      int best_at = 0;
      double best = -1.0;
      for (int start = 0; start + 240 < 48 * 32; start += 12) {
        double c = 0.0, s = 0.0;
        for (int i = 0; i < 240; ++i) {
          const double window = 0.5 - 0.5 * std::cos(2.0 * kPi * i / 240.0);
          c += window * left[start + i] * std::cos(2.0 * kPi * hz * i / 48000.0);
          s += window * left[start + i] * std::sin(2.0 * kPi * hz * i / 48000.0);
        }
        if (c * c + s * s > best) { best = c * c + s * s; best_at = start; }
      }
      return (best_at + 120) / 48.0;
    };
    const double low = arrival(600.0), high = arrival(3600.0);
    expect(high > low + 10.0, "Spring echoes rise: 3.6 kHz arrives after 600 Hz (" + std::to_string(low) + " ms, " + std::to_string(high) + " ms)");
  }

  void testSpringMonoPan() {
    const auto render = [](bool mono, float pan, bool left_only) {
      SpringFixture f;
      f.set("mono", mono ? 1.0f : 0.0f); f.set("pan", pan); f.set("mix", 1.0f); f.set("noise", 0.0f);
      Signal input(48000);
      for (int i = 0; i < 48000; ++i) {
        const float value = i < 2400 ? static_cast<float>(0.4 * std::sin(2.0 * kPi * 700.0 * i / 48000.0)) : 0.0f;
        input[i] = vital::poly_float(value, left_only ? 0.0f : value, value, left_only ? 0.0f : value);
      }
      const Signal out = f.render(input);
      return std::make_pair(laneRms(out, 0), laneRms(out, 1));
    };
    const auto centre = render(true, 0.0f, false);
    expect(centre.first > 1.0e-3 && std::abs(centre.first - centre.second) < 1.0e-9, "Mono spring at centre is identical left and right");
    const auto left = render(true, -1.0f, false);
    expect(left.first > 1.0e-3 && left.second < 1.0e-9, "Mono spring panned hard left is silent on the right");
    const auto stereo = render(false, 0.0f, true);
    expect(stereo.first > 1.0e-3 && stereo.second < 1.0e-6 * stereo.first, "Stereo springs keep a left-only input on the left tank");
    const auto balanced = render(false, 1.0f, false);
    expect(balanced.second > 1.0e-3 && balanced.first < 1.0e-9, "In stereo, Pan fully right silences the left tank");
  }

  void testSpringLevels() {
    SpringFixture silent;
    silent.set("dwell", 0.0f); silent.set("noise", 0.0f); silent.set("mix", 1.0f);
    expect(laneRms(silent.render(monoSine(24000, 48000.0, 500.0, 0.4)), 0, 4800) < 1.0e-9, "Spring at zero Dwell gets no signal");
    SpringFixture quiet, hiss;
    quiet.set("noise", 0.0f); quiet.set("mix", 1.0f);
    hiss.set("noise", 0.5f); hiss.set("mix", 1.0f);
    const Signal silence(48000, vital::poly_float(0.0f));
    expect(laneRms(quiet.render(silence), 0) == 0.0, "Spring at zero Noise is silent without input");
    const double level = laneRms(hiss.render(silence), 0, 4800);
    expect(level > 3.0e-5 && level < 1.0e-3, "Spring hiss at the original level is quiet but present (" + std::to_string(level) + ")");
  }

  void testSpringTankSwitch() {
    SpringFixture f;
    f.set("mix", 0.5f); f.set("noise", 0.0f);
    const Signal input = monoSine(96000, 48000.0, 220.0, 0.3);
    Signal out(input.size());
    for (size_t i = 0; i < input.size(); i += vital::kMaxBufferSize) {
      if (i == 48000 - 48000 % vital::kMaxBufferSize) f.set("tank", atlas::analogue::spring::kShort);
      const int size = static_cast<int>(std::min<size_t>(vital::kMaxBufferSize, input.size() - i));
      f.module.processWithInput(input.data() + i, size);
      std::copy_n(f.module.output()->buffer, size, out.data() + i);
    }
    double worst = 0.0, steady = 0.0;
    for (size_t i = 4800; i < out.size(); ++i) {
      const double step = std::abs(out[i][0] - out[i - 1][0]);
      worst = std::max(worst, step);
      if (i < 40000) steady = std::max(steady, step);
    }
    expect(worst < 1.5 * steady + 0.005, "Analogue Spring tank change is click-free (largest step " +
           std::to_string(worst) + ", steady " + std::to_string(steady) + ")");
  }

  // ------------------------------------------------------------------- Wah

  struct WahFixture {
    vital::AnalogueWahModule module;
    vital::control_map controls;
    explicit WahFixture(int rate = 48000) {
      module.init(); module.setSampleRate(rate); module.setOversampleAmount(1);
      controls = module.getControls();
    }
    void set(const std::string& id, float value) { controls.at("analoguewah_" + id)->set(value); }
    Signal render(const Signal& input) {
      Signal out(input.size());
      allocation_count = 0; track = true;
      for (size_t i = 0; i < input.size(); i += vital::kMaxBufferSize) {
        const int size = static_cast<int>(std::min<size_t>(vital::kMaxBufferSize, input.size() - i));
        module.processWithInput(input.data() + i, size);
        std::copy_n(module.output()->buffer, size, out.data() + i);
      }
      track = false;
      expect(allocation_count == 0, "Analogue Wah processing does not allocate");
      return out;
    }
  };

  void testWahDry() {
    WahFixture f;
    f.set("mix", 0.0f);
    const Signal input = stereoSine(9600, 48000.0, 440.0);
    const Signal out = f.render(input);
    bool dry = true, mirrored = true;
    for (size_t i = 0; i < out.size(); ++i) {
      dry = dry && out[i][0] == input[i][0] && out[i][1] == input[i][1];
      mirrored = mirrored && out[i][2] == out[i][0] && out[i][3] == out[i][1];
    }
    expect(dry, "Analogue Wah at 0% Mix returns the dry signal exactly");
    expect(mirrored, "Analogue Wah lanes 2 and 3 mirror left and right");
  }

  // The pedal circuits against the full SPICE netlists (GCB-95 with its input
  // buffer, V846-HW), heel, mid travel and toe: the resonance's frequency and
  // height, and the response away from it. (SPICE's transistor junction
  // capacitances, left out here, detune the heel by about half a percent,
  // which a Q of 12 turns into half a dB within 20% of the resonance.)
  void testWahSpice() {
    using namespace atlas::analogue::wah;
    const std::pair<const char*, double> positions[] = { { "heel", 1.0 }, { "mid", 0.185 }, { "toe", 0.0 } };
    for (int model : { kCryBaby, kVox }) {
      for (const auto& [where, fraction] : positions) {
        const std::string name = std::string(model == kCryBaby ? "crybaby_" : "v846_") + where;
        PedalCircuit pedal = makePedalCircuit(model);
        pedal.circuit.prepare(48000.0);
        pedal.setPot(fraction);
        const auto reference = spiceReference(name);
        size_t top = 0;
        for (size_t i = 1; i < reference.size(); ++i)
          if (reference[i].second > reference[top].second) top = i;
        double peak_hz = 0.0, peak_db = -100.0;
        for (double hz = 0.8 * reference[top].first; hz < 1.25 * reference[top].first; hz *= 1.0005) {
          const double db = 20.0 * std::log10(std::abs(pedal.response(hz)));
          if (db > peak_db) { peak_db = db; peak_hz = hz; }
        }
        double worst = 0.0;
        for (const auto& [hz, db] : reference) {
          if (hz < 30.0 || hz > 10000.0 || std::abs(std::log(hz / peak_hz)) < std::log(1.2)) continue;
          worst = std::max(worst, std::abs(20.0 * std::log10(std::abs(pedal.response(hz))) - db));
        }
        expect(std::abs(peak_hz / reference[top].first - 1.0) < 0.01 && std::abs(peak_db - reference[top].second) < 0.3,
               name + " resonance matches SPICE (" + std::to_string(peak_hz) + " Hz, " + std::to_string(peak_db) +
               " dB; SPICE " + std::to_string(reference[top].first) + " Hz, " + std::to_string(reference[top].second) + " dB)");
        expect(worst < 0.4, name + " matches SPICE away from the resonance (worst " + std::to_string(worst) + " dB)");
      }
    }
  }

  double pedalPeakHz(int model, double position) {
    using namespace atlas::analogue::wah;
    PedalCircuit pedal = makePedalCircuit(model);
    pedal.circuit.prepare(48000.0);
    pedal.setPot(potFraction(model, position));
    double best_hz = 0.0, best = 0.0;
    for (double hz = 200.0; hz < 4000.0; hz *= 1.002) {
      const double level = std::abs(pedal.response(hz));
      if (level > best) { best = level; best_hz = hz; }
    }
    return best_hz;
  }

  void testWahSweep() {
    using namespace atlas::analogue::wah;
    // Dunlop's specification: heel 350-450 Hz, toe 1.5-2.5 kHz. J.O. Smith
    // measured a GCB-95 at 464 / 838 / 2252 Hz rocked back, middle, forward.
    const double heel = pedalPeakHz(kCryBaby, 0.0), mid = pedalPeakHz(kCryBaby, 0.5), toe = pedalPeakHz(kCryBaby, 1.0);
    expect(heel > 350.0 && heel < 450.0, "Cry Baby heel within Dunlop's range (" + std::to_string(heel) + " Hz)");
    expect(toe > 1500.0 && toe < 2500.0, "Cry Baby toe within Dunlop's range (" + std::to_string(toe) + " Hz)");
    expect(std::abs(mid / 838.0 - 1.0) < 0.1, "Cry Baby mid travel near the measured 838 Hz (" + std::to_string(mid) + " Hz)");
    const double vox_heel = pedalPeakHz(kVox, 0.0), vox_toe = pedalPeakHz(kVox, 1.0);
    expect(vox_heel > 380.0 && vox_heel < 480.0 && vox_toe > 1800.0 && vox_toe < 2600.0,
           "V846 sweeps about 430 Hz to 2.2 kHz (" + std::to_string(vox_heel) + ", " + std::to_string(vox_toe) + " Hz)");

    // The engine's small-signal response follows the circuit at its position.
    for (int model : { kCryBaby, kVox }) {
      for (double position : { 0.0, 0.3, 0.7, 1.0 }) {
        PedalCircuit pedal = makePedalCircuit(model);
        pedal.circuit.prepare(48000.0);
        pedal.setPot(potFraction(model, position));
        double worst = 0.0;
        for (double hz : { 150.0, 300.0, 500.0, 800.0, 1200.0, 1800.0, 2500.0, 4000.0 }) {
          WahFixture f;
          f.set("model", model); f.set("envelope", 0.0f); f.set("position", static_cast<float>(position));
          const Signal out = f.render(monoSine(19200, 48000.0, hz, 0.001));
          double c = 0.0, sn = 0.0;
          for (size_t i = 9600; i < out.size(); ++i) {
            const double phase = 2.0 * kPi * hz * i / 48000.0;
            c += out[i][0] * std::cos(phase); sn += out[i][0] * std::sin(phase);
          }
          const double gain = 2.0 * std::sqrt(c * c + sn * sn) / 9600.0 / 0.001;
          const double expected = std::abs(pedal.response(hz));
          worst = std::max(worst, std::abs(20.0 * std::log10(gain / expected)));
        }
        expect(worst < 0.5, "Wah model " + std::to_string(model) + " at " + std::to_string(position) +
               " follows its circuit (worst " + std::to_string(worst) + " dB)");
      }
    }
  }

  // Harmonic levels of a steady sine through the engine.
  std::vector<double> harmonics(WahFixture& f, double hz, double amplitude, int count = 8) {
    const Signal out = f.render(monoSine(48000, 48000.0, hz, amplitude));
    std::vector<double> levels;
    for (int k = 1; k <= count; ++k) {
      double c = 0.0, s = 0.0;
      for (size_t i = 24000; i < out.size(); ++i) {
        const double phase = 2.0 * kPi * hz * k * i / 48000.0;
        c += out[i][0] * std::cos(phase); s += out[i][0] * std::sin(phase);
      }
      levels.push_back(2.0 * std::sqrt(c * c + s * s) / 24000.0);
    }
    return levels;
  }

  double thdDb(const std::vector<double>& levels) {
    double sum = 0.0;
    for (size_t k = 1; k < levels.size(); ++k) sum += levels[k] * levels[k];
    return 20.0 * std::log10(std::sqrt(sum) / levels[0]);
  }

  void testWahDistortion() {
    using namespace atlas::analogue::wah;
    // Holters & Zolzer's measured GCB-95 at 719 Hz with the pot at 23% to
    // ground: THD about -65 dB at -40 dBV in and -19 dB at 0 dBV, smooth and
    // mostly second harmonic, with about 13 dB of compression (SPICE: -21 dB,
    // 9 dB).
    double position = 0.5;
    for (double lo = 0.0, hi = 1.0, i = 0; i < 50; ++i) {
      position = 0.5 * (lo + hi);
      if (potFraction(kCryBaby, position) > 0.23) lo = position; else hi = position;
    }
    const auto run = [&](double dbv) {
      WahFixture f;
      f.set("envelope", 0.0f); f.set("position", static_cast<float>(position));
      return harmonics(f, 719.0, std::sqrt(2.0) * std::pow(10.0, dbv / 20.0) / kInstrumentVolts);
    };
    const auto quiet = run(-40.0), loud = run(0.0);
    const double quiet_gain = quiet[0] / (std::sqrt(2.0) * 0.01 / kInstrumentVolts);
    const double loud_gain = loud[0] / (std::sqrt(2.0) / kInstrumentVolts);
    const double compression = 20.0 * std::log10(quiet_gain / loud_gain);
    std::cout << "  Cry Baby 719 Hz: THD " << thdDb(quiet) << " dB at -40 dBV, " << thdDb(loud) << " dB at 0 dBV; H2 "
              << 20.0 * std::log10(loud[1] / loud[0]) << " dBc, H3 " << 20.0 * std::log10(loud[2] / loud[0])
              << " dBc; compression " << compression << " dB\n";
    expect(thdDb(quiet) < -55.0, "Cry Baby is clean at -40 dBV (" + std::to_string(thdDb(quiet)) + " dB)");
    expect(thdDb(loud) > -26.0 && thdDb(loud) < -15.0, "Cry Baby distorts like the measured pedal at 0 dBV (" +
           std::to_string(thdDb(loud)) + " dB)");
    expect(loud[1] > loud[2], "Cry Baby distortion is mostly second harmonic");
    expect(compression > 6.0 && compression < 16.0, "Cry Baby compresses at 0 dBV (" + std::to_string(compression) + " dB)");
  }

  void testWahMuTron() {
    using namespace atlas::analogue::wah;
    // At rest (no light) the 220k resistors set the floor: about 402 Hz on Hi,
    // 181 Hz on Lo. Peak sets Q from 1.78 to 17.7 and the gain at f0 to
    // R_q / 4.7k (+8.1 to +30.7 dB) on every output.
    expect(std::abs(muTronFrequency(1.0e9, kHigh) - 402.0) < 2.0 && std::abs(muTronFrequency(1.0e9, kLow) - 181.0) < 1.0,
           "Mu-Tron floor from its 220k resistors");
    expect(std::abs(muTronQ(0.0) - 1.777) < 0.01 && std::abs(muTronQ(1.0) - 17.73) < 0.05, "Mu-Tron Peak spans Q 1.78 to 17.7");
    for (double peak : { 0.0, 1.0 }) {
      for (int range : { kLow, kHigh }) {
        WahFixture f;
        f.set("model", kMuTron); f.set("envelope", 0.0f); f.set("position", 0.0f);
        f.set("mode", kBandPass); f.set("range", static_cast<float>(range)); f.set("peak", static_cast<float>(peak));
        const double floor = muTronFrequency(1.0e8, range);
        const auto levels = harmonics(f, floor, 0.001, 1);
        const double expected = 2.0 * muTronQ(peak) - 1.0;  // R_q / R5
        expect(std::abs(20.0 * std::log10(levels[0] / 0.001 / expected)) < 0.3,
               "Mu-Tron BP gain at f0, Peak " + std::to_string(peak) + " range " + std::to_string(range) + " (" +
               std::to_string(20.0 * std::log10(levels[0] / 0.001)) + " dB)");
      }
    }
    // Static vactrol law (Parker & D'Angelo): 56k at 1 mA, 3.3k at 10 mA.
    expect(std::abs(vactrolOhms(1.0e-3) / 56.2e3 - 1.0) < 0.02 && std::abs(vactrolOhms(10.0e-3) / 3.33e3 - 1.0) < 0.03,
           "Vactrol static law");
  }

  void testWahEnvelope() {
    using namespace atlas::analogue::wah;
    // The follower, from the Mu-Tron III schematic, simulated in SPICE: a
    // 196 Hz tone giving 100 mV at the input stage's output charges C9 to
    // about 3.9 V, 90% within about 14 ms; after the note, the envelope
    // ramps down to 50 mV in about 77 ms.
    Engine engine;
    engine.prepare(48000.0);
    Parameters p;
    p.model = kMuTron;
    p.envelope = 0.5;  // the input stage at unity
    engine.setParameters(p);
    engine.reset();
    const double amplitude = 0.1 / kInstrumentVolts;
    std::vector<double> trace;
    std::vector<float> in(48000), left(48000), right(48000);
    for (size_t i = 0; i < in.size(); ++i)
      in[i] = i < 14400 ? static_cast<float>(amplitude * std::sin(2.0 * kPi * 196.0 * i / 48000.0)) : 0.0f;
    for (size_t i = 0; i < in.size(); i += 8) {
      engine.process(in.data() + i, in.data() + i, left.data() + i, right.data() + i, 8);
      trace.push_back(engine.envelopeVolts());
    }
    double peak = 0.0;
    for (size_t i = 0; i < 14400 / 8; ++i) peak = std::max(peak, trace[i]);
    double t90 = -1.0, release = -1.0;
    for (size_t i = 0; i < trace.size(); ++i) {
      if (t90 < 0.0 && trace[i] > 0.9 * peak) t90 = i * 8 / 48.0;
      if (i * 8 > 14400 && release < 0.0 && trace[i] < 0.05) release = (i * 8 - 14400) / 48.0;
    }
    std::cout << "  Mu-Tron follower: peak " << peak << " V, 90% in " << t90 << " ms, release to 50 mV in " << release << " ms\n";
    expect(std::abs(peak / 3.92 - 1.0) < 0.1, "Mu-Tron envelope level (" + std::to_string(peak) + " V)");
    expect(t90 > 8.0 && t90 < 22.0, "Mu-Tron envelope attack (" + std::to_string(t90) + " ms)");
    expect(release > 60.0 && release < 95.0, "Mu-Tron envelope release (" + std::to_string(release) + " ms)");

    // Up opens the filter as the note plays; Down closes it from the top.
    for (bool up : { true, false }) {
      Engine e;
      e.prepare(48000.0);
      Parameters q;
      q.model = kMuTron;
      q.envelope = up ? 0.5 : -0.5;
      q.position = up ? 0.0 : 1.0;
      e.setParameters(q);
      e.reset();
      const double rest = e.ldrOhms();
      e.process(in.data(), in.data(), left.data(), right.data(), 4800);
      const double playing = e.ldrOhms();
      expect(up ? playing < 0.2 * rest : playing > 5.0 * rest,
             std::string("Mu-Tron ") + (up ? "Up" : "Down") + " moves the cells (" + std::to_string(rest) + " to " +
             std::to_string(playing) + " ohms)");
    }

    // The pedals rock from Position with the same follower.
    Engine pedal;
    pedal.prepare(48000.0);
    Parameters r;
    r.envelope = 0.5;
    pedal.setParameters(r);
    pedal.reset();
    pedal.process(in.data(), in.data(), left.data(), right.data(), 4800);
    expect(pedal.pedalPosition() > 0.4, "Envelope rocks the pedal (" + std::to_string(pedal.pedalPosition()) + ")");
  }

  void testWahDecayAndMode() {
    using namespace atlas::analogue::wah;
    // Decay scales the follower's release time constant: 0% is a quarter of
    // the Mu-Tron's, 100% four times it. (A faster release also droops more
    // between cycles while the note plays, so it starts lower.)
    const auto release = [](double decay) {
      Engine engine;
      engine.prepare(48000.0);
      Parameters p;
      p.model = kMuTron;
      p.decay = decay;
      engine.setParameters(p);
      engine.reset();
      std::vector<float> in(96000), out_l(96000), out_r(96000);
      for (size_t i = 0; i < 14400; ++i) in[i] = static_cast<float>(0.2 * std::sin(2.0 * kPi * 196.0 * i / 48000.0));
      for (size_t i = 0; i < in.size(); i += 8) {
        engine.process(in.data() + i, in.data() + i, out_l.data() + i, out_r.data() + i, 8);
        if (i > 14400 && engine.envelopeVolts() < 0.05) return (i - 14400) / 48.0;
      }
      return -1.0;
    };
    const double fast = release(0.0), stock = release(0.5), slow = release(1.0);
    expect(fast / stock > 0.15 && fast / stock < 0.3 && slow / stock > 3.5 && slow / stock < 5.0,
           "Decay scales the release (" + std::to_string(fast) + ", " + std::to_string(stock) + ", " + std::to_string(slow) + " ms)");

    // Mode and Range changes glide instead of clicking.
    for (const char* control : { "mode", "range" }) {
      WahFixture f;
      f.set("model", kMuTron); f.set("envelope", 0.0f); f.set("position", 0.3f); f.set("peak", 0.3f);
      const Signal input = monoSine(48000, 48000.0, 330.0, 0.2);
      Signal out(input.size());
      for (size_t i = 0; i < input.size(); i += vital::kMaxBufferSize) {
        if (i == 24000 - 24000 % vital::kMaxBufferSize) f.set(control, std::string(control) == "mode" ? kHighPass : kHigh);
        const int size = static_cast<int>(std::min<size_t>(vital::kMaxBufferSize, input.size() - i));
        f.module.processWithInput(input.data() + i, size);
        std::copy_n(f.module.output()->buffer, size, out.data() + i);
      }
      double worst = 0.0, steady = 0.0;
      for (size_t i = 4800; i < out.size(); ++i) {
        const double step = std::abs(out[i][0] - out[i - 1][0]);
        worst = std::max(worst, step);
        if (i < 20000 || i > 40000) steady = std::max(steady, step);
      }
      expect(worst < 1.5 * steady + 0.005, std::string("Mu-Tron ") + control + " change is click-free (largest step " +
             std::to_string(worst) + ", steady " + std::to_string(steady) + ")");
    }
  }

  void testWahModelSwitch() {
    for (int target : { atlas::analogue::wah::kVox, atlas::analogue::wah::kMuTron }) {
      WahFixture f;
      f.set("envelope", 0.0f); f.set("position", 0.5f);
      const Signal input = monoSine(48000, 48000.0, 220.0, 0.3);
      Signal out(input.size());
      for (size_t i = 0; i < input.size(); i += vital::kMaxBufferSize) {
        if (i == 24000 - 24000 % vital::kMaxBufferSize) f.set("model", static_cast<float>(target));
        const int size = static_cast<int>(std::min<size_t>(vital::kMaxBufferSize, input.size() - i));
        f.module.processWithInput(input.data() + i, size);
        std::copy_n(f.module.output()->buffer, size, out.data() + i);
      }
      double worst = 0.0, steady = 0.0;
      for (size_t i = 4800; i < out.size(); ++i) {
        const double step = std::abs(out[i][0] - out[i - 1][0]);
        worst = std::max(worst, step);
        if (i < 20000 || i > 40000) steady = std::max(steady, step);
      }
      expect(worst < 1.5 * steady + 0.005, "Analogue Wah model change to " + std::to_string(target) +
             " is click-free (largest step " + std::to_string(worst) + ", steady " + std::to_string(steady) + ")");
    }
  }

  // ------------------------------------------------------------ Labels

  void testChoiceLabels() {
    // Every choice's label must name what the engine does with that value.
    const auto label = [](const std::string& id, int value) {
      const auto& details = vital::Parameters::getDetails(id);
      return value <= details.max && details.string_lookup ? details.string_lookup[value] : std::string("<none>");
    };
    using namespace atlas::analogue;
    expect(label("analoguechorus_model", chorus::kJuno) == "Juno" && label("analoguechorus_model", chorus::kDimension) == "Dimension" &&
           label("analoguechorus_model", chorus::kCe) == "CE" && label("analoguechorus_model", chorus::kEnsemble) == "Ensemble",
           "Chorus model labels match the engine");
    expect(label("analoguechorus_juno_mode", chorus::kJunoI) == "I" && label("analoguechorus_juno_mode", chorus::kJunoII) == "II" &&
           label("analoguechorus_juno_mode", chorus::kJunoBoth) == "I+II", "Juno mode labels match the engine");
    expect(label("analogueflanger_medium", flanger::kBbd) == "BBD" && label("analogueflanger_medium", flanger::kTape) == "Tape",
           "Flanger medium labels match the engine");
    expect(label("analogueecho_medium", echo::kTape) == "Tape" && label("analogueecho_medium", echo::kBbd) == "BBD",
           "Echo medium labels match the engine");
    expect(label("analoguerotary_model", rotary::kOrgan) == "Organ 122" && label("analoguerotary_model", rotary::kGuitar) == "Guitar 16",
           "Rotary model labels match the engine");
    expect(label("analoguerotary_speed", rotary::kSlow) == "Slow" && label("analoguerotary_speed", rotary::kFast) == "Fast" &&
           label("analoguerotary_speed", rotary::kStop) == "Stop", "Rotary speed labels match the engine");
    expect(label("analoguespring_tank", spring::kShort) == "Short" && label("analoguespring_tank", spring::kMedium) == "Medium" &&
           label("analoguespring_tank", spring::kLong) == "Long", "Spring tank labels match the engine");
    expect(label("analoguewah_model", wah::kCryBaby) == "Cry Baby" && label("analoguewah_model", wah::kVox) == "V846" &&
           label("analoguewah_model", wah::kMuTron) == "Mu-Tron III", "Wah model labels match the engine");
    expect(label("analoguewah_mode", wah::kLowPass) == "LP" && label("analoguewah_mode", wah::kBandPass) == "BP" &&
           label("analoguewah_mode", wah::kHighPass) == "HP", "Wah mode labels match the engine");
    expect(label("analoguewah_range", wah::kLow) == "Lo" && label("analoguewah_range", wah::kHigh) == "Hi",
           "Wah range labels match the engine");
    expect(vital::Parameters::getDetails("analoguewah_model").max == wah::kNumModels - 1 &&
           vital::Parameters::getDetails("analoguewah_mode").max == wah::kNumModes - 1 &&
           vital::Parameters::getDetails("analoguewah_range").max == wah::kNumRanges - 1,
           "Wah choice ranges match the engine");
    expect(vital::Parameters::getDetails("analoguechorus_model").max == chorus::kNumModels - 1 &&
           vital::Parameters::getDetails("analoguerotary_model").max == rotary::kNumModels - 1 &&
           vital::Parameters::getDetails("analoguerotary_speed").max == rotary::kNumSpeeds - 1 &&
           vital::Parameters::getDetails("analoguespring_tank").max == spring::kNumTanks - 1,
           "Choice ranges match the engines");
  }

  // ------------------------------------------------------- Factory presets

  void testFactoryPresets() {
    const juce::File folder { juce::String(ATLAS_ANALOGUE_PRESETS) };
    int bus_presets = 0, single_presets = 0;
    for (const auto& entry : juce::RangedDirectoryIterator(folder, false, "*.atfx;*.atf")) {
      const juce::File file = entry.getFile();
      const std::string name = file.getFileName().toStdString();
      const json data = json::parse(file.loadFileAsString().toStdString(), nullptr, false);
      expect(!data.is_discarded(), name + " is valid JSON");
      if (data.is_discarded()) continue;

      std::string effect;
      json parameters;
      if (file.hasFileExtension("atfx")) {
        ++bus_presets;
        // Atlas's own bus loader validates every key, range and integer choice.
        SynthPlugin synth;
        juce::String error;
        expect(synth.loadFxBusPreset(0, data, error), name + " loads as an FX bus preset (" + error.toStdString() + ")");
        effect = data["slots"][0]["effect"].get<std::string>();
        parameters = data["slots"][0]["parameters"];
      }
      else {
        ++single_presets;
        expect(data["type"] == "Atlas FX Preset" && data["version"] == 1, name + " is an Atlas FX preset");
        effect = data["effect"].get<std::string>();
        parameters = data["parameters"];
        for (auto it = parameters.begin(); it != parameters.end(); ++it) {
          const std::string id = "fx_slot_1_" + it.key();
          const bool known = vital::Parameters::isParameter(id);
          expect(known && it.key().rfind(effect + "_", 0) == 0, name + ": " + it.key() + " is an effect parameter");
          if (!known) continue;
          const auto& details = vital::Parameters::getDetails(id);
          const double value = it.value().get<double>();
          expect(value >= details.min && value <= details.max, name + ": " + it.key() + " is in range");
        }
      }

      // Every preset renders finite, bounded audio for a few seconds.
      const Signal input = monoSine(48000 * 3, 48000.0, 220.0, 0.3);
      Signal out;
      const auto apply = [&](vital::control_map& controls) {
        for (auto it = parameters.begin(); it != parameters.end(); ++it)
          controls.at(it.key())->set(it.value().get<float>());
      };
      if (effect == "analoguechorus") { ChorusFixture f; apply(f.controls); out = f.render(input); }
      else if (effect == "analogueflanger") { FlangerFixture f; apply(f.controls); out = f.render(input); }
      else if (effect == "analoguerotary") { RotaryFixture f; apply(f.controls); out = f.render(input); }
      else if (effect == "analoguespring") { SpringFixture f; apply(f.controls); out = f.render(input); }
      else if (effect == "analoguewah") { WahFixture f; apply(f.controls); out = f.render(input); }
      else { EchoFixture f; apply(f.controls); out = f.render(input); }
      bool finite = true;
      double peak = 0.0;
      for (const auto& frame : out) {
        finite = finite && std::isfinite(frame[0]) && std::isfinite(frame[1]);
        peak = std::max(peak, double(std::max(std::abs(frame[0]), std::abs(frame[1]))));
      }
      // A resonant envelope filter sweeping across a pure tone rings up to
      // the Mu-Tron's +31 dB at f0 (its manual warns of the jump), so the
      // wah's bound is looser.
      const double bound = effect == "analoguewah" ? 4.0 : 2.0;
      expect(finite && peak < bound && peak > 0.01, name + " renders bounded audio (peak " + std::to_string(peak) + ")");
    }
    expect(bus_presets >= 15 && bus_presets == single_presets, "Analogue factory presets ship in both formats");
  }

  // ------------------------------------------------------------ CPU budget

  // Seconds to process ten seconds of stereo noise at 48 kHz in 128-sample
  // blocks; best of three to reduce scheduler noise.
  double processingSeconds(vital::SynthModule& module) {
    Signal input(128);
    uint32_t random = 12345u;
    double best = 1e9;
    for (int attempt = 0; attempt < 3; ++attempt) {
      const auto start = std::chrono::steady_clock::now();
      for (int block = 0; block < 48000 * 10 / 128; ++block) {
        for (auto& frame : input) {
          random = random * 1664525u + 1013904223u;
          const float value = static_cast<float>(random) / 4294967296.0f - 0.5f;
          frame = vital::poly_float(value, -value, value, -value);
        }
        module.processWithInput(input.data(), 128);
      }
      best = std::min(best, std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
    }
    return best;
  }

  void testCpuBudget() {
    vital::ReverbModule reverb;
    reverb.init(); reverb.setSampleRate(48000);
    const double budget = processingSeconds(reverb);
    std::cout << "CPU budget (Reverb): " << budget * 1000.0 << " ms per 10 s\n";
    const auto check = [budget](const std::string& name, vital::SynthModule& module) {
      const double seconds = processingSeconds(module);
      std::cout << "  " << name << ": " << seconds * 1000.0 << " ms (" << 100.0 * seconds / budget << "% of Reverb)\n";
      expect(seconds <= budget, name + " stays within the Reverb's CPU cost");
    };
    for (int model = 0; model < atlas::analogue::chorus::kNumModels; ++model) {
      ChorusFixture f;
      f.set("model", model); f.set("mix", 0.5f);
      check("Analogue Chorus model " + std::to_string(model), f.module);
    }
    for (int medium = 0; medium < atlas::analogue::flanger::kNumMedia; ++medium) {
      FlangerFixture f;
      f.set("medium", medium); f.set("feedback", 0.9f); f.set("depth", 1.0f); f.set("age", 1.0f);
      check("Analogue Flanger medium " + std::to_string(medium), f.module);
    }
    // Worst cases: all three heads with full wear; the BBD at its shortest
    // delay, where the clock is fastest.
    {
      EchoFixture f;
      f.set("head_2", 1.0f); f.set("feedback", 0.9f); f.set("age", 1.0f);
      check("Analogue Echo tape", f.module);
    }
    {
      EchoFixture f;
      f.set("medium", 1.0f); f.setTime(0.01); f.set("feedback", 0.9f); f.set("age", 1.0f);
      check("Analogue Echo BBD", f.module);
    }
    for (int model = 0; model < atlas::analogue::rotary::kNumModels; ++model) {
      RotaryFixture f;
      f.set("model", model); f.set("speed", 1.0f); f.set("drive", 0.8f);
      check("Analogue Rotary model " + std::to_string(model), f.module);
    }
    for (int tank = 0; tank < atlas::analogue::spring::kNumTanks; ++tank)
      for (float mono : { 1.0f, 0.0f }) {
        SpringFixture f;
        f.set("tank", tank); f.set("mono", mono);
        check("Analogue Spring tank " + std::to_string(tank) + (mono > 0.5f ? " mono" : " stereo"), f.module);
      }
    // Worst case: the envelope moving the circuit all the time.
    for (int model = 0; model < atlas::analogue::wah::kNumModels; ++model) {
      WahFixture f;
      f.set("model", model); f.set("envelope", 1.0f); f.set("decay", 0.0f);
      check("Analogue Wah model " + std::to_string(model), f.module);
    }
  }
}

int main(int argc, char** argv) {
  juce::ScopedJuceInitialiser_GUI juce;
  const std::string only = argc > 1 ? argv[1] : "";
  if (only.empty() || only == "--bbd") {
    testAnalogFilter();
    testSpiceReferences();
    testBbdResponse();
    testBbdDelayAndClock();
    testBbdGrouping();
    testBbdGlide();
    testBbdRobustness();
  }
  if (only.empty() || only == "--chorus") {
    testChorusDryAndLanes();
    testChorusModels();
    testChorusJunoRates();
    testChorusCenterDelay();
    testChorusNoise();
    testChorusModelSwitch();
    testChorusChains();
  }
  if (only.empty() || only == "--flanger") {
    testFlangerDry();
    testFlangerComb();
    testFlangerBounded();
    testFlangerThroughZero();
    testFlangerSync();
    testFlangerMediumSwitch();
    testEffectChains(vital::constants::kAnalogueFlanger, "analogueflanger");
  }
  if (only.empty() || only == "--echo") {
    testEchoDry();
    testEchoHeads();
    testEchoFeedback();
    testEchoGlide();
    testEchoDarkensWithTime();
    testEchoMediumSwitch();
    testEffectChains(vital::constants::kAnalogueEcho, "analogueecho");
  }
  if (only.empty() || only == "--rotary") {
    testRotaryDry();
    testRotarySpeeds();
    testRotarySwirl();
    testRotaryModelSwitch();
    testEffectChains(vital::constants::kAnalogueRotary, "analoguerotary");
  }
  if (only.empty() || only == "--spring") {
    testSpringDry();
    testSpringTone();
    testSpringDecay();
    testSpringChirp();
    testSpringMonoPan();
    testSpringLevels();
    testSpringTankSwitch();
    testEffectChains(vital::constants::kAnalogueSpring, "analoguespring");
  }
  if (only.empty() || only == "--wah") {
    testWahDry();
    testWahSpice();
    testWahSweep();
    testWahDistortion();
    testWahMuTron();
    testWahEnvelope();
    testWahDecayAndMode();
    testWahModelSwitch();
    testEffectChains(vital::constants::kAnalogueWah, "analoguewah");
  }
  if (only.empty() || only == "--labels")
    testChoiceLabels();
  if (only.empty() || only == "--presets")
    testFactoryPresets();
  if (only == "--cpu")
    testCpuBudget();
  std::cout << "Atlas Analogue tests: " << failures << " failures\n";
  return failures == 0 ? 0 : 1;
}
