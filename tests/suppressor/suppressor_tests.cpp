// SPDX-License-Identifier: GPL-3.0-or-later
// Suppressor behaviour tests: latency-aligned dry and delta paths, resonance
// taming inside the frequency window and not outside it, release, lane
// isolation and allocation-free processing.
#include "JuceHeader.h"
#include "suppressor_module.h"
#include "suppressor/suppressor_engine.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <new>
#include <vector>

namespace { std::atomic<bool> tracking { false }; std::atomic<size_t> allocations { 0 }; }
void* operator new(std::size_t n) {
  if (tracking) ++allocations;
  if (void* p = std::malloc(n)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {
  using Engine = atlas::suppressor::Engine;
  using Parameters = atlas::suppressor::Parameters;
  constexpr double kRate = 48000.0;
  int failures = 0;
  void expect(bool ok, const std::string& message) {
    if (!ok) { ++failures; std::cerr << "FAIL: " << message << std::endl; }
  }
  std::string number(double v) { char t[32]; std::snprintf(t, sizeof(t), "%.2f", v); return t; }

  // White noise with a resonant peak (about +12 dB, Q 8) at `peak_hz`.
  std::vector<float> resonantNoise(int count, double peak_hz, uint32_t seed, bool resonance = true) {
    std::vector<float> out(count);
    const double w = 2.0 * M_PI * peak_hz / kRate, q = 8.0, gain = std::pow(10.0, 12.0 / 40.0);
    const double alpha = std::sin(w) / (2.0 * q);
    const double b0 = 1.0 + alpha * gain, b1 = -2.0 * std::cos(w), b2 = 1.0 - alpha * gain;
    const double a0 = 1.0 + alpha / gain, a1 = b1, a2 = 1.0 - alpha / gain;
    double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    for (int i = 0; i < count; ++i) {
      seed = seed * 1664525u + 1013904223u;
      const double x = (double(seed >> 8) / 16777216.0 - 0.5) * 0.2;
      double y = x;
      if (resonance) {
        y = (b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2) / a0;
        x2 = x1; x1 = x; y2 = y1; y1 = y;
      }
      out[i] = float(y);
    }
    return out;
  }

  // Level in dB of a tone region relative to its surroundings (a third of an octave each side).
  double prominenceDb(const std::vector<float>& signal, int start, double hz) {
    constexpr int kOrder = 14, kSize = 1 << kOrder, kBins = kSize / 2;
    juce::dsp::FFT fft(kOrder);
    std::vector<float> data(2 * kSize, 0.0f);
    for (int i = 0; i < kSize && start + i < int(signal.size()); ++i)
      data[i] = signal[start + i] * (0.5f - 0.5f * std::cos(2.0f * float(M_PI) * i / kSize));
    fft.performRealOnlyForwardTransform(data.data(), true);
    std::vector<double> level(kBins);
    for (int bin = 0; bin < kBins; ++bin)
      level[bin] = 10.0 * std::log10(data[2 * bin] * data[2 * bin] + data[2 * bin + 1] * data[2 * bin + 1] + 1e-20);
    const int centre = int(hz * kSize / kRate + 0.5);
    const int near = std::max(1, centre / 24), wide = std::max(near + 2, int(centre * 0.26));
    double local = 0, around = 0; int n_local = 0, n_around = 0;
    for (int bin = centre - wide; bin <= centre + wide; ++bin) {
      if (bin < 1 || bin >= kBins) continue;
      if (std::abs(bin - centre) <= near) { local += level[bin]; ++n_local; }
      else { around += level[bin]; ++n_around; }
    }
    return local / n_local - around / n_around;
  }

  void run(Engine& engine, const std::vector<float>& in, std::vector<float>& out, int block = 128) {
    out.assign(in.size(), 0.0f);
    std::vector<float> right_out(in.size());
    for (int offset = 0; offset < int(in.size()); offset += block) {
      const int count = std::min(block, int(in.size()) - offset);
      engine.process(in.data() + offset, in.data() + offset, out.data() + offset, right_out.data() + offset, count);
    }
  }

  void testDryAndDelta() {
    Engine engine;
    engine.prepare(kRate);
    const auto input = resonantNoise(int(kRate), 2000.0, 7u);
    std::vector<float> output;
    Parameters p;
    p.amount = 0.0f;
    engine.setParameters(p);
    run(engine, input, output, 97);
    float start_peak = 0.0f;
    for (int i = 0; i < Engine::kLatencySamples; ++i)
      start_peak = std::max(start_peak, std::abs(output[i]));
    expect(start_peak < 1e-6f, "the first latency period is silent, peak " + number(start_peak * 1e9) + " nano");
    double worst = 0.0;
    for (int i = Engine::kLatencySamples; i < int(input.size()); ++i)
      worst = std::max(worst, double(std::abs(output[i] - input[i - Engine::kLatencySamples])));
    expect(worst < 1e-4, "Amount 0 passes the aligned signal unchanged, worst " + number(worst * 1e6) + " ppm");

    // Delta plus the processed signal is the dry signal: nothing is lost.
    p.amount = 1.0f; p.width = 1000.0f;
    Engine normal, delta;
    normal.prepare(kRate); delta.prepare(kRate);
    normal.setParameters(p);
    p.delta = true;
    delta.setParameters(p);
    std::vector<float> wet, removed;
    run(normal, input, wet, 128);
    run(delta, input, removed, 128);
    worst = 0.0;
    double removed_energy = 0.0;
    for (int i = Engine::kLatencySamples; i < int(input.size()); ++i) {
      worst = std::max(worst, double(std::abs(wet[i] + removed[i] - input[i - Engine::kLatencySamples])));
      removed_energy += double(removed[i]) * removed[i];
    }
    expect(worst < 1e-5, "processed plus Delta reconstructs the dry signal at the reported latency");
    expect(removed_energy > 1e-4, "Delta carries what was removed");
  }

  void testTamesResonanceInsideTheWindow() {
    const auto input = resonantNoise(int(kRate * 2), 2000.0, 11u);
    const int measure = int(kRate * 1.5);
    const double before = prominenceDb(input, measure, 2000.0);
    expect(before > 7.0, "the test signal has a resonance at 2 kHz, prominence " + number(before));

    Parameters p;
    p.amount = 1.0f;
    p.frequency = float(Engine::hzToPosition(2000.0, kRate));
    p.width = 100.0f;
    Engine engine;
    engine.prepare(kRate);
    engine.setParameters(p);
    std::vector<float> output;
    run(engine, input, output);
    const double after = prominenceDb(output, measure + Engine::kLatencySamples, 2000.0);
    std::cout << "2 kHz resonance: " << number(before) << " dB before, " << number(after) << " dB after\n";
    expect(before - after > 4.0, "Amount 100% cuts a resonance inside the window by more than 4 dB");

    // The window centred two octaves lower, a quarter octave wide, leaves it alone.
    p.frequency = float(Engine::hzToPosition(500.0, kRate));
    p.width = 0.0f;
    Engine outside;
    outside.prepare(kRate);
    outside.setParameters(p);
    run(outside, input, output);
    const double untouched = prominenceDb(output, measure + Engine::kLatencySamples, 2000.0);
    expect(std::abs(untouched - before) < 1.0, "a resonance outside the window is untouched, " + number(untouched));
    expect(Engine::windowWeight(2000.0f, 2000.0f, 100.0f, 0.2f) == 1.0f &&
           Engine::windowWeight(16000.0f, 2000.0f, 100.0f, 0.2f) == 0.0f &&
           Engine::windowWeight(4000.0f, 2000.0f, 100.0f, 0.2f) < 1.0f &&
           Engine::windowWeight(4000.0f, 2000.0f, 100.0f, 0.2f) > 0.0f &&
           Engine::windowWeight(4000.0f, 2000.0f, 100.0f, 0.9f) == 1.0f,
           "the window is flat over its Slope fraction and falls to zero at its edge");
    expect(std::abs(Engine::positionToHz(Engine::hzToPosition(1234.0, kRate), kRate) - 1234.0) < 1e-6,
           "the frequency axis round-trips");
  }

  void testReleaseHoldsTheCutLonger() {
    auto cutThen = [](float release) {
      Parameters p;
      p.amount = 1.0f; p.width = 1000.0f; p.release = release;
      Engine engine;
      engine.prepare(kRate);
      engine.setParameters(p);
      auto input = resonantNoise(int(kRate), 2000.0, 3u);
      const auto flat = resonantNoise(int(kRate * 0.25), 2000.0, 5u, false);
      input.insert(input.end(), flat.begin(), flat.end());
      std::vector<float> output(input.size()), right(input.size());
      const int bin = int(2000.0 * Engine::kFftSize / kRate + 0.5);
      float during = 0.0f, after = 0.0f;
      const int tail = int(kRate * 0.08);
      for (int offset = 0; offset < int(input.size()); offset += 128) {
        const int count = std::min(128, int(input.size()) - offset);
        engine.process(input.data() + offset, input.data() + offset, output.data() + offset, right.data() + offset, count);
        if (offset + count == int(kRate)) during = engine.reductionDb(bin);
        if (offset + count == int(kRate) + tail) after = engine.reductionDb(bin);
      }
      return std::pair<float, float>(during, after);
    };
    const auto fast = cutThen(1.0f), slow = cutThen(8.0f);
    std::cout << "reduction at 2 kHz: release 1 " << number(fast.first) << " -> " << number(fast.second)
              << " dB; release 8 " << number(slow.first) << " -> " << number(slow.second) << " dB\n";
    expect(fast.first > 3.0f && slow.first > 3.0f, "the resonance is being cut while it plays");
    expect(fast.second < 0.5f * fast.first, "Release 1 lets the cut go within 80 ms");
    expect(slow.second > 2.0f * fast.second, "Release 8 holds the cut much longer");
  }

  void testModuleLanesAndAllocations() {
    vital::SuppressorModule module;
    module.init();
    module.setSampleRate(int(kRate));
    module.enable(true);
    module.getControls().at("suppressor_amount")->set(80.0f);
    module.getControls().at("suppressor_width")->set(1000.0f);
    module.getControls().at("suppressor_smoothing")->set(25.0f);
    module.getControls().at("suppressor_release")->set(1.0f);
    module.getControls().at("suppressor_slope")->set(20.0f);
    expect(module.getLatencySamples() == 2048, "the module reports 2048 samples of latency");

    // Lanes 0/1 and 2/3 carry different material; each stereo pair must match
    // a standalone engine fed with its own channels.
    const auto a = resonantNoise(int(kRate / 2), 2000.0, 21u);
    const auto b = resonantNoise(int(kRate / 2), 700.0, 22u);
    const auto c = resonantNoise(int(kRate / 2), 3000.0, 23u);
    const auto d = resonantNoise(int(kRate / 2), 1200.0, 24u);
    std::vector<vital::poly_float> input(a.size());
    for (size_t i = 0; i < a.size(); ++i)
      input[i] = vital::poly_float(a[i], b[i], c[i], d[i]);
    Engine first, second;
    first.prepare(kRate); second.prepare(kRate);
    Parameters p;
    p.amount = 0.8f; p.width = 1000.0f;
    first.setParameters(p); second.setParameters(p);
    std::vector<float> l0(a.size()), r0(a.size()), l1(a.size()), r1(a.size());
    const int blocks[] = { 1, 17, 64, 127, 63, 33, 128 };
    double worst = 0.0;
    bool finite = true;
    double module_seconds = 0.0;
    for (int offset = 0, k = 0; offset < int(a.size()); ++k) {
      const int count = std::min(blocks[k % 7], int(a.size()) - offset);
      allocations = 0; tracking = true;
      const auto started = std::chrono::steady_clock::now();
      module.processWithInput(input.data() + offset, count);
      module_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
      tracking = false;
      expect(allocations == 0, "processing allocates no memory");
      first.process(a.data() + offset, b.data() + offset, l0.data() + offset, r0.data() + offset, count);
      second.process(c.data() + offset, d.data() + offset, l1.data() + offset, r1.data() + offset, count);
      for (int i = 0; i < count; ++i) {
        const vital::poly_float out = module.output()->buffer[i];
        for (int lane = 0; lane < 4; ++lane) finite = finite && std::isfinite(out[lane]);
        worst = std::max({ worst, double(std::abs(out[0] - l0[offset + i])), double(std::abs(out[1] - r0[offset + i])),
                           double(std::abs(out[2] - l1[offset + i])), double(std::abs(out[3] - r1[offset + i])) });
      }
      offset += count;
    }
    std::cout << "suppressor CPU: " << number(module_seconds * 1000.0 / (double(a.size()) / kRate)) << " ms per second of audio (two stereo pairs)\n";
    expect(finite, "finite output on every lane");
    expect(worst == 0.0, "each SIMD stereo pair matches its own engine exactly, worst " + number(worst));
  }
}

int main() {
  juce::ScopedJuceInitialiser_GUI juce;
  testDryAndDelta();
  testTamesResonanceInsideTheWindow();
  testReleaseHoldsTheCutLonger();
  testModuleLanesAndAllocations();
  if (failures == 0)
    std::cout << "Suppressor tests passed\n";
  return failures == 0 ? 0 : 1;
}
