// SPDX-License-Identifier: GPL-3.0-or-later
// Spectral Compressor behaviour tests: downward and upward compression toward
// the target, tilt, curves, makeup, latency-aligned mix and allocation-free
// processing on every SIMD lane.
#include "JuceHeader.h"
#include "spectral_compressor_module.h"
#include "spectral_compressor/spectral_compressor_engine.h"
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
  using Engine = atlas::spectral_compressor::Engine;
  using Parameters = atlas::spectral_compressor::Parameters;
  using atlas::spectral_compressor::kFlat;
  using atlas::spectral_compressor::kSmooth;
  using atlas::spectral_compressor::kLoud;
  constexpr double kRate = 48000.0;
  int failures = 0;
  void expect(bool ok, const std::string& message) {
    if (!ok) { ++failures; std::cerr << "FAIL: " << message << std::endl; }
  }
  std::string number(double v) { char t[32]; std::snprintf(t, sizeof(t), "%.2f", v); return t; }

  std::vector<float> sine(int count, double hz, double dbfs) {
    std::vector<float> out(count);
    const double amplitude = std::pow(10.0, dbfs / 20.0);
    for (int i = 0; i < count; ++i)
      out[i] = float(amplitude * std::sin(2.0 * M_PI * hz * i / kRate));
    return out;
  }

  // Peak amplitude of the last `window` samples, in dBFS.
  double levelDb(const std::vector<float>& signal, int window) {
    float peak = 0.0f;
    for (int i = std::max(0, int(signal.size()) - window); i < int(signal.size()); ++i)
      peak = std::max(peak, std::abs(signal[i]));
    return 20.0 * std::log10(peak + 1e-12);
  }

  std::vector<float> run(const Parameters& p, const std::vector<float>& in, int block = 128) {
    Engine engine;
    engine.prepare(kRate);
    engine.setParameters(p);
    std::vector<float> out(in.size()), right(in.size());
    for (int offset = 0; offset < int(in.size()); offset += block) {
      const int count = std::min(block, int(in.size()) - offset);
      engine.process(in.data() + offset, in.data() + offset, out.data() + offset, right.data() + offset, count);
    }
    return out;
  }

  std::vector<float> noise(int count, double dbfs, uint32_t seed = 1u) {
    std::vector<float> out(count);
    const double rms = std::pow(10.0, dbfs / 20.0);
    double sum = 0.0;
    for (int i = 0; i < count; ++i) {
      seed = seed * 1664525u + 1013904223u;
      out[i] = float(double(seed >> 8) / 16777216.0 * 2.0 - 1.0);
      sum += double(out[i]) * out[i];
    }
    const float scale = float(rms / std::sqrt(sum / count));
    for (float& v : out) v *= scale;
    return out;
  }

  // RMS of the last `window` samples, in dBFS.
  double rmsDb(const std::vector<float>& signal, int window) {
    double sum = 0.0;
    const int start = std::max(0, int(signal.size()) - window);
    for (int i = start; i < int(signal.size()); ++i) sum += double(signal[i]) * signal[i];
    return 10.0 * std::log10(sum / std::max(1, int(signal.size()) - start) + 1e-20);
  }

  void testCalibrationDownwardAndUpward() {
    Parameters p;
    p.target_db = -30.0f; p.tilt = 0.0f; p.curve = kFlat; p.smooth = 0.0f;
    // Equal attack and release (100 ms each) make the detector read the mean level.
    p.attack = 1.0f; p.release = 0.1f;
    const int seconds = int(kRate);
    const int window = int(kRate / 4);
    // Calibration: white noise at -20 dBFS reads about -20 in every band.
    {
      Engine engine;
      engine.prepare(kRate);
      engine.setParameters(p);
      const auto input = noise(seconds, -20.0);
      std::vector<float> out(seconds), right(seconds);
      for (int offset = 0; offset < seconds; offset += 128)
        engine.process(input.data() + offset, input.data() + offset, out.data() + offset, right.data() + offset,
                       std::min(128, seconds - offset));
      double mean = 0.0; int count = 0;
      for (int bin = 8; bin < Engine::kNumBins - 8; ++bin) { mean += engine.levelDb(bin); ++count; }
      mean /= count;
      std::cout << "white noise at -20 dBFS reads " << number(mean) << " dB per band\n";
      expect(std::abs(mean + 20.0) < 1.5, "band levels are calibrated to broadband material");
    }
    expect(std::abs(rmsDb(run(p, noise(seconds, -20.0)), window) + 20.0) < 0.5,
           "with no compression noise passes at its level");
    p.downward = 1.0f;
    const double down = rmsDb(run(p, noise(seconds, -20.0)), window);
    std::cout << "downward 100%: -20 dBFS noise -> " << number(down) << " dBFS (target -30)\n";
    expect(std::abs(down + 30.0) < 2.0, "Downward 100% holds louder noise at the target");
    p.downward = 0.5f;
    expect(std::abs(rmsDb(run(p, noise(seconds, -20.0)), window) + 25.0) < 2.0, "Downward 50% halves the excess in dB");
    p.downward = 0.0f; p.upward = 1.0f;
    const double up = rmsDb(run(p, noise(seconds, -45.0)), window);
    std::cout << "upward 100%: -45 dBFS noise -> " << number(up) << " dBFS (target -30)\n";
    expect(std::abs(up + 30.0) < 2.0, "Upward 100% lifts quieter noise to the target");
    expect(rmsDb(run(p, noise(seconds, -70.0)), window) < -40.0, "a lift is limited to 24 dB");
    expect(rmsDb(run(p, std::vector<float>(seconds, 0.0f)), window) < -100.0, "silence is not lifted");
    // A single tone reads far above its dBFS, so it is squashed hard: spectral behaviour.
    p.upward = 0.0f; p.downward = 1.0f;
    const double tone = rmsDb(run(p, sine(seconds, 1000.0, -6.0)), window);
    std::cout << "downward 100%: -6 dBFS sine -> " << number(tone) << " dBFS (target -30)\n";
    expect(tone < -40.0, "a lone tone is pulled well below a broadband target");
  }

  void testTiltCurvesMakeupAndMix() {
    Parameters p;
    p.target_db = -30.0f; p.curve = kFlat; p.smooth = 0.0f; p.downward = 1.0f;
    p.attack = 0.0f; p.release = 0.1f; p.tilt = -6.0f;
    const int seconds = int(kRate);
    // Tilt -6 dB/oct: the threshold two octaves above 1 kHz is 12 dB lower, so
    // for loud noise the gain there is 12 dB lower.
    {
      Engine engine;
      engine.prepare(kRate);
      engine.setParameters(p);
      const auto input = noise(seconds, -10.0);
      std::vector<float> out(seconds), right(seconds);
      for (int offset = 0; offset < seconds; offset += 128)
        engine.process(input.data() + offset, input.data() + offset, out.data() + offset, right.data() + offset,
                       std::min(128, seconds - offset));
      auto bin_of = [](double hz) { return int(hz * Engine::kFftSize / kRate + 0.5); };
      auto gain_near = [&](double hz) {
        double g = 0.0;
        for (int bin = bin_of(hz) - 2; bin <= bin_of(hz) + 2; ++bin) g += engine.gainDb(bin);
        return g / 5.0;
      };
      const double difference = gain_near(1000.0) - gain_near(4000.0);
      std::cout << "tilt -6: gain 1 kHz minus gain 4 kHz = " << number(difference) << " dB\n";
      expect(std::abs(difference - 12.0) < 2.5, "Tilt -6 dB/oct lowers the threshold 12 dB two octaves up");
    }
    expect(std::abs(Engine::curveDb(kFlat, 100.0f)) < 1e-6f && Engine::curveDb(kSmooth, 30.0f) > 5.0f &&
           Engine::curveDb(kSmooth, 1000.0f) == 0.0f && Engine::curveDb(kLoud, 1500.0f) < -5.9f &&
           std::abs(Engine::curveDb(kLoud, 100.0f)) < 0.1f,
           "Flat is flat, Smooth allows louder lows, Loud dips in the mids");
    p.tilt = 0.0f; p.downward = 0.0f; p.makeup_db = 6.0f;
    expect(std::abs(rmsDb(run(p, noise(seconds, -20.0)), seconds / 4) + 14.0) < 0.5, "Makeup adds 6 dB");
    p.makeup_db = 0.0f; p.downward = 1.0f; p.mix = 0.0f;
    const auto input = sine(seconds, 1000.0, -6.0);
    const auto dry = run(p, input, 61);
    double worst = 0.0;
    for (int i = Engine::kLatencySamples; i < seconds; ++i)
      worst = std::max(worst, double(std::abs(dry[i] - input[i - Engine::kLatencySamples])));
    expect(worst == 0.0, "Mix 0 is the dry signal delayed by exactly the latency");
    expect(vital::SpectralCompressorModule::kLatencySamples == 1024 && Engine::kNumBins == 513,
           "1024 samples of latency and 512 bands");
  }

  void testModuleLanesAndAllocations() {
    vital::SpectralCompressorModule module;
    module.init();
    module.setSampleRate(int(kRate));
    module.enable(true);
    module.getControls().at("spectral_compressor_downward")->set(100.0f);
    module.getControls().at("spectral_compressor_upward")->set(40.0f);
    module.getControls().at("spectral_compressor_target")->set(-24.0f);
    module.getControls().at("spectral_compressor_attack")->set(25.0f);
    module.getControls().at("spectral_compressor_release")->set(25.0f);
    module.getControls().at("spectral_compressor_smooth")->set(50.0f);
    module.getControls().at("spectral_compressor_tilt")->set(-3.0f);
    // The voice handler gives both SIMD pairs the same stereo signal, so the
    // module runs one engine on lanes 0/1 and mirrors it into lanes 2/3.
    const int count_total = int(kRate / 2);
    const auto a = sine(count_total, 1000.0, -6.0), b = sine(count_total, 300.0, -30.0);
    std::vector<vital::poly_float> input(count_total);
    for (int i = 0; i < count_total; ++i) input[i] = vital::poly_float(a[i], b[i], a[i], b[i]);
    Parameters p;
    p.downward = 1.0f; p.upward = 0.4f; p.target_db = -24.0f;
    Engine reference;
    reference.prepare(kRate);
    reference.setParameters(p);
    std::vector<float> left(count_total), right(count_total);
    const int blocks[] = { 1, 17, 64, 127, 63, 33, 128 };
    double worst = 0.0; bool finite = true;
    double module_seconds = 0.0;
    for (int offset = 0, k = 0; offset < count_total; ++k) {
      const int count = std::min(blocks[k % 7], count_total - offset);
      allocations = 0; tracking = true;
      const auto started = std::chrono::steady_clock::now();
      module.processWithInput(input.data() + offset, count);
      module_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
      tracking = false;
      expect(allocations == 0, "processing allocates no memory");
      reference.process(a.data() + offset, b.data() + offset, left.data() + offset, right.data() + offset, count);
      for (int i = 0; i < count; ++i) {
        const vital::poly_float out = module.output()->buffer[i];
        for (int lane = 0; lane < 4; ++lane) finite = finite && std::isfinite(out[lane]);
        worst = std::max({ worst, double(std::abs(out[0] - left[offset + i])), double(std::abs(out[1] - right[offset + i])),
                           double(std::abs(out[2] - left[offset + i])), double(std::abs(out[3] - right[offset + i])) });
      }
      offset += count;
    }
    std::cout << "spectral compressor CPU: " << number(module_seconds * 1000.0 / (double(count_total) / kRate)) << " ms per second of audio\n";
    expect(finite, "finite output on every lane");
    expect(worst == 0.0, "lanes 0/1 match a standalone engine exactly and lanes 2/3 mirror them, worst " + number(worst));
  }
}

int main() {
  juce::ScopedJuceInitialiser_GUI juce;
  testCalibrationDownwardAndUpward();
  testTiltCurvesMakeupAndMix();
  testModuleLanesAndAllocations();
  if (failures == 0)
    std::cout << "Spectral Compressor tests passed\n";
  return failures == 0 ? 0 : 1;
}
