// Spectral Balance measurement tests: target curves, latency-aligned bypass,
// resonance taming, tilt leveling, range limits, allocation-free processing,
// chain integration and the CPU budget (no more than the Spectral Compressor).
#include "JuceHeader.h"
#include "spectral_balance_module.h"
#include "spectral_compressor_module.h"
#include "reorderable_effect_chain.h"
#include "spectral_balance/spectral_balance_engine.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <new>
#include <vector>

namespace {
  std::atomic<bool> tracking { false };
  std::atomic<size_t> allocations { 0 };
}
void* operator new(std::size_t size) {
  if (tracking.load()) ++allocations;
  if (void* p = std::malloc(size)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {
  using Engine = atlas::spectral_balance::Engine;
  using Parameters = atlas::spectral_balance::Parameters;
  namespace balance = atlas::spectral_balance;
  constexpr double kRate = 48000.0;
  int failures = 0;

  void expect(bool condition, const std::string& message) {
    if (!condition) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
  }
  std::string number(double value) {
    char text[32]; std::snprintf(text, sizeof(text), "%.2f", value); return text;
  }

  // Paul Kellet's pink noise filter over a fixed-seed white source.
  std::vector<float> pinkNoise(int samples, uint32_t seed = 0x2545F491u, float amplitude = 0.1f) {
    std::vector<float> result(samples);
    float b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;
    for (int i = 0; i < samples; ++i) {
      seed = seed * 1664525u + 1013904223u;
      const float white = static_cast<float>(seed >> 8) / 8388608.0f - 1.0f;
      b0 = 0.99886f * b0 + white * 0.0555179f; b1 = 0.99332f * b1 + white * 0.0750759f;
      b2 = 0.96900f * b2 + white * 0.1538520f; b3 = 0.86650f * b3 + white * 0.3104856f;
      b4 = 0.55000f * b4 + white * 0.5329522f; b5 = -0.7616f * b5 - white * 0.0168980f;
      result[i] = amplitude * (b0 + b1 + b2 + b3 + b4 + b5 + b6 + white * 0.5362f) * 0.25f;
      b6 = white * 0.115926f;
    }
    return result;
  }

  // RBJ peaking EQ in place.
  void peak(std::vector<float>& signal, double hz, double gain_db, double q) {
    const double a = std::pow(10.0, gain_db / 40.0), w = 2.0 * M_PI * hz / kRate, alpha = std::sin(w) / (2.0 * q);
    const double b0 = 1 + alpha * a, b1 = -2 * std::cos(w), b2 = 1 - alpha * a;
    const double a0 = 1 + alpha / a, a1 = -2 * std::cos(w), a2 = 1 - alpha / a;
    double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    for (float& sample : signal) {
      const double x = sample;
      const double y = (b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2) / a0;
      x2 = x1; x1 = x; y2 = y1; y1 = y;
      sample = static_cast<float>(y);
    }
  }

  // Welch power spectrum (4096-point Hann, hop 1024) over [start, end).
  struct Spectrum {
    static constexpr int kOrder = 12, kSize = 1 << kOrder;
    std::vector<double> power;
    double binHz() const { return kRate / kSize; }
    explicit Spectrum(const std::vector<float>& signal, int start, int end) : power(kSize / 2 + 1, 0.0) {
      juce::dsp::FFT fft(kOrder);
      std::vector<float> buffer(2 * kSize);
      int frames = 0;
      for (int offset = start; offset + kSize <= end; offset += kSize / 4, ++frames) {
        for (int i = 0; i < kSize; ++i)
          buffer[i] = signal[offset + i] * (0.5f - 0.5f * std::cos(2.0f * float(M_PI) * i / kSize));
        std::fill(buffer.begin() + kSize, buffer.end(), 0.0f);
        fft.performRealOnlyForwardTransform(buffer.data(), true);
        for (int bin = 0; bin <= kSize / 2; ++bin)
          power[bin] += double(buffer[2 * bin]) * buffer[2 * bin] + double(buffer[2 * bin + 1]) * buffer[2 * bin + 1];
      }
      for (double& value : power) value /= std::max(frames, 1);
    }
    // Mean power per bin between two frequencies, in dB.
    double densityDb(double low, double high) const {
      double sum = 0; int count = 0;
      for (int bin = 1; bin < int(power.size()); ++bin) {
        const double hz = bin * binHz();
        if (hz >= low && hz < high) { sum += power[bin]; ++count; }
      }
      return 10.0 * std::log10(sum / std::max(count, 1) + 1e-30);
    }
    // Energy in a band, in dB (per-octave energy when bands are log spaced).
    double energyDb(double low, double high) const {
      double sum = 0;
      for (int bin = 1; bin < int(power.size()); ++bin) {
        const double hz = bin * binHz();
        if (hz >= low && hz < high) sum += power[bin];
      }
      return 10.0 * std::log10(sum + 1e-30);
    }
    // Least-squares slope of third-octave band energy against octaves.
    double slopeDbPerOctave(double low, double high) const {
      double sx = 0, sy = 0, sxx = 0, sxy = 0; int n = 0;
      for (double edge = low; edge * std::cbrt(2.0) <= high * 1.0001; edge *= std::cbrt(2.0)) {
        const double x = std::log2(edge * std::pow(2.0, 1.0 / 6.0)), y = energyDb(edge, edge * std::cbrt(2.0));
        sx += x; sy += y; sxx += x * x; sxy += x * y; ++n;
      }
      return (n * sxy - sx * sy) / (n * sxx - sx * sx);
    }
    // Narrow level at a resonance against its third-octave neighbours.
    double prominenceDb(double hz) const {
      const double narrow = densityDb(hz * std::pow(2.0, -1.0 / 24), hz * std::pow(2.0, 1.0 / 24));
      const double below = densityDb(hz * std::pow(2.0, -2.0 / 3), hz * std::pow(2.0, -1.0 / 3));
      const double above = densityDb(hz * std::pow(2.0, 1.0 / 3), hz * std::pow(2.0, 2.0 / 3));
      return narrow - 0.5 * (below + above);
    }
  };

  std::vector<float> run(Engine& engine, const Parameters& parameters, const std::vector<float>& input, int block = 128) {
    engine.prepare(kRate);
    engine.setParameters(parameters);
    std::vector<float> output(input.size());
    std::vector<float> right(block);
    for (int offset = 0; offset < int(input.size()); offset += block) {
      const int count = std::min(block, int(input.size()) - offset);
      engine.process(input.data() + offset, input.data() + offset, output.data() + offset, right.data(), count);
    }
    return output;
  }

  void testTargets() {
    using namespace balance;
    const struct { int target; float slope; } tilts[] = {
      { kFlat, 3.0f }, { kTilt15, 1.5f }, { kTilt3, 0.0f }, { kTilt45, -1.5f }, { kTilt6, -3.0f } };
    for (const auto& tilt : tilts) {
      for (float hz : { 100.0f, 1000.0f, 7000.0f }) {
        const float step = Engine::targetDb(tilt.target, 0.0f, hz * 2) - Engine::targetDb(tilt.target, 0.0f, hz);
        expect(std::abs(step - tilt.slope) < 1e-3f, "straight tilt target " + std::to_string(tilt.target) +
               " must be " + number(tilt.slope) + " dB per octave of energy per octave, got " + number(step));
      }
      const float biased = Engine::targetDb(tilt.target, 1.0f, 2000.0f) - Engine::targetDb(tilt.target, 1.0f, 1000.0f);
      expect(std::abs(biased - (tilt.slope + 1.0f)) < 1e-3f, "Bias adds dB per octave to every target");
    }
    expect(Engine::targetDb(kPerceptual, 0, 100) == Engine::targetDb(kPerceptual, 0, 200),
           "the perceptual target is flat per octave through the low mids");
    expect(Engine::targetDb(kPerceptual, 0, 4000) < Engine::targetDb(kPerceptual, 0, 1000) &&
           Engine::targetDb(kPerceptual, 0, 40) < Engine::targetDb(kPerceptual, 0, 100),
           "the perceptual target falls above 250 Hz and below 60 Hz");
    expect(std::abs(Engine::bandCentreHz(0) - 30.0f * std::pow(16000.0f / 30.0f, 0.5f / 24)) < 0.01f,
           "bands are log spaced from 30 Hz to 16 kHz");
  }

  void testBypassAndLatency() {
    Engine engine;
    const auto input = pinkNoise(kRate);
    Parameters dry; dry.mix = 0.0f; dry.amount = 1.0f;
    auto output = run(engine, dry, input, 97);
    bool exact = true;
    for (int i = Engine::kLatencySamples; i < int(input.size()); ++i)
      exact = exact && output[i] == input[i - Engine::kLatencySamples];
    expect(exact, "Mix 0 must be the input delayed by exactly the reported latency, bit for bit");
    for (int i = 0; i < Engine::kLatencySamples; ++i) exact = exact && output[i] == 0.0f;
    expect(exact, "the first latency period is silent");

    Parameters unity; unity.amount = 0.0f; unity.mix = 1.0f;
    output = run(engine, unity, input);
    double worst = 0;
    for (int i = Engine::kLatencySamples; i < int(input.size()); ++i)
      worst = std::max(worst, std::abs(double(output[i]) - input[i - Engine::kLatencySamples]));
    expect(worst < 1e-4, "Amount 0 passes the aligned signal unchanged, worst difference " + number(worst * 1e6) + " ppm");
  }

  void testTamesResonances() {
    const int seconds = 6, analysis_start = int(4 * kRate), analysis_end = int(seconds * kRate);
    auto input = pinkNoise(int(seconds * kRate));
    peak(input, 120.0, 12.0, 8.0);
    peak(input, 3000.0, 12.0, 8.0);
    Engine engine;
    Parameters p; p.amount = 1.0f; p.target = balance::kTilt3; p.tame_recover = 1.0f;
    const auto output = run(engine, p, input);
    const Spectrum before(input, analysis_start, analysis_end);
    const Spectrum after(output, analysis_start + Engine::kLatencySamples, analysis_end);
    for (double hz : { 120.0, 3000.0 }) {
      const double low = hz * std::pow(2.0, -1.0 / 24), high = hz * std::pow(2.0, 1.0 / 24);
      const double cut = after.densityDb(low, high) - before.densityDb(low, high);
      std::cout << "resonance " << hz << " Hz: cut " << number(cut) << " dB, prominence " << number(before.prominenceDb(hz))
                << " dB -> " << number(after.prominenceDb(hz)) << " dB\n";
      expect(cut <= -5.0, "a 12 dB resonance at " + number(hz) + " Hz must be cut by at least 5 dB, cut " + number(cut));
      expect(after.prominenceDb(hz) > -3.0, "the resonance must not be turned into a hole");
    }
    const double neutral = after.densityDb(500, 800) - before.densityDb(500, 800);
    expect(std::abs(neutral) < 1.5, "pink noise toward the pink target stays level away from the resonances, moved " + number(neutral) + " dB");

    Parameters recover = p; recover.tame_recover = -1.0f;
    const auto recovered = run(engine, recover, input);
    const Spectrum untamed(recovered, analysis_start + Engine::kLatencySamples, analysis_end);
    for (double hz : { 120.0, 3000.0 }) {
      const double low = hz * std::pow(2.0, -1.0 / 24), high = hz * std::pow(2.0, 1.0 / 24);
      const double change = untamed.densityDb(low, high) - before.densityDb(low, high);
      expect(std::abs(change) < 1.5, "Recover only must leave the resonance at " + number(hz) + " Hz, changed " + number(change) + " dB");
    }
  }

  void testLevelsTilt() {
    const int seconds = 8, analysis_start = int(5 * kRate), analysis_end = int(seconds * kRate);
    const auto input = pinkNoise(int(seconds * kRate));
    const Spectrum before(input, analysis_start, analysis_end);
    const double input_slope = before.slopeDbPerOctave(200, 2500);
    expect(std::abs(input_slope) < 0.6, "pink test noise is flat per octave, measured " + number(input_slope));

    Engine engine;
    Parameters p; p.amount = 1.0f; p.speed = 1.0f;
    const struct { int target; float bias; double expected; const char* name; } cases[] = {
      { balance::kFlat, 0.0f, 3.0, "Flat" }, { balance::kTilt3, 1.5f, 1.5, "pink plus 1.5 dB per octave Bias" },
      { balance::kTilt3, 0.0f, 0.0, "pink" }, { balance::kTilt45, 0.0f, -1.5, "-4.5 dB per octave" } };
    for (const auto& item : cases) {
      p.target = item.target; p.bias = item.bias;
      const auto output = run(engine, p, input);
      const Spectrum after(output, analysis_start + Engine::kLatencySamples, analysis_end);
      const double slope = after.slopeDbPerOctave(200, 2500);
      std::cout << "target " << item.name << ": slope " << number(slope) << " dB/oct\n";
      expect(std::abs(slope - item.expected) < 0.75, std::string("pink noise leveled toward ") + item.name +
             " must come out at " + number(item.expected) + " dB per octave, measured " + number(slope));
      if (item.expected == 0.0) {
        const double moved = after.densityDb(200, 2500) - before.densityDb(200, 2500);
        expect(std::abs(moved) < 1.5, "a sound already at its target is left near its level, moved " + number(moved) + " dB");
      }
    }
    // The gain curve itself honours the limits.
    p.target = balance::kFlat; p.bias = 3.0f;
    run(engine, p, input);
    float lowest = 0, highest = 0;
    for (float gain : engine.gainsDb()) { lowest = std::min(lowest, gain); highest = std::max(highest, gain); }
    expect(lowest >= -Engine::kMaxCutDb - 1e-3f && highest <= Engine::kMaxLiftDb + 1e-3f,
           "gains stay within 12 dB of cut and 6 dB of lift at Amount 100%");
    expect(lowest < -6.0f && highest > 3.0f, "an extreme target produces substantial correction");
  }

  void testRangeLimits() {
    const int seconds = 6, analysis_start = int(4 * kRate), analysis_end = int(seconds * kRate);
    const auto input = pinkNoise(int(seconds * kRate));
    Engine engine;
    Parameters p; p.amount = 1.0f; p.target = balance::kFlat; p.range_low = 1000.0f; p.range_high = 2000.0f;
    const auto output = run(engine, p, input);
    const Spectrum before(input, analysis_start, analysis_end);
    const Spectrum after(output, analysis_start + Engine::kLatencySamples, analysis_end);
    for (const auto& band : { std::pair<double, double>{ 100, 600 }, std::pair<double, double>{ 3500, 12000 } }) {
      const double moved = after.densityDb(band.first, band.second) - before.densityDb(band.first, band.second);
      expect(std::abs(moved) < 1.0, "outside the Range nothing changes, moved " + number(moved) + " dB between " +
             number(band.first) + " and " + number(band.second) + " Hz");
    }
    const double inside = after.energyDb(1400, 2000) - before.energyDb(1400, 2000) -
                          (after.energyDb(1000, 1400) - before.energyDb(1000, 1400));
    expect(inside > 0.5, "inside the Range the Flat target still brightens, measured " + number(inside) + " dB");
  }

  vital::poly_float* fill(std::vector<vital::poly_float>& buffer, const std::vector<float>& signal, int offset, int count) {
    for (int i = 0; i < count; ++i) {
      const float sample = signal[(offset + i) % signal.size()];
      buffer[i] = vital::poly_float(sample, 0.7f * sample, sample, 0.7f * sample);
    }
    return buffer.data();
  }

  void testModule() {
    vital::SpectralBalanceModule module;
    module.init(); module.setSampleRate(int(kRate));
    const auto controls = module.getControls();
    expect(controls.count("spectral_balance_amount") && controls.count("spectral_balance_target") &&
           controls.count("spectral_balance_range_low") && controls.count("spectral_balance_tame_recover"),
           "the module creates its controls");
    controls.at("spectral_balance_amount")->set(1.0f);
    controls.at("spectral_balance_target")->set(float(balance::kFlat));
    controls.at("spectral_balance_range_low")->set(std::log2(60.0f));
    const auto input = pinkNoise(int(kRate));
    std::vector<vital::poly_float> block(128);
    module.processWithInput(fill(block, input, 0, 128), 128);
    tracking = true; allocations = 0;
    for (int offset = 128; offset < int(input.size()); offset += 128)
      module.processWithInput(fill(block, input, offset, 128), 128);
    tracking = false;
    expect(allocations == 0, "processing allocates nothing");
    const auto& parameters = module.engine().parameters();
    expect(std::abs(parameters.range_low - 60.0f) < 0.1f && parameters.target == balance::kFlat &&
           std::abs(parameters.amount - 1.0f) < 1e-3f,
           "controls reach the engine (Range in Hz from log2 Hz): amount " + number(parameters.amount) +
           " target " + std::to_string(parameters.target) + " range_low " + number(parameters.range_low));
    bool mirrored = true, finite = true;
    for (int i = 0; i < 128; ++i) {
      const vital::poly_float out = module.output()->buffer[i];
      mirrored = mirrored && out[0] == out[2] && out[1] == out[3];
      finite = finite && std::isfinite(out[0]) && std::isfinite(out[1]);
    }
    expect(mirrored && finite, "lanes 2 and 3 mirror the processed stereo pair");
    expect(module.getLatencySamples() == Engine::kLatencySamples, "the module reports the engine latency");

    // In a chain slot, enabled and bypassed.
    vital::Value bpm(2.0f), keytrack(0.0f);
    for (const std::string prefix : { "", "bus_2_" }) {
      vital::ReorderableEffectChain chain(bpm.output(), keytrack.output(), prefix, nullptr, nullptr, nullptr, nullptr);
      chain.init(); chain.setSampleRate(int(kRate));
      auto chain_controls = chain.getControls();
      chain_controls.at(prefix + "fx_slot_1_type")->set(vital::constants::kSpectralBalance + 1);
      chain_controls.at(prefix + "fx_slot_1_enabled")->set(1);
      double energy = 0;
      for (int offset = 0; offset < int(4 * kRate); offset += 128) {
        chain.processWithInput(fill(block, input, offset, 128), 128);
        for (int i = 0; i < 128; ++i) energy += std::abs(chain.output()->buffer[i][0]);
      }
      expect(energy > 0 && std::isfinite(energy), "Spectral Balance runs in chain '" + prefix + "'");
      chain_controls.at(prefix + "fx_slot_1_enabled")->set(0);
      chain.processWithInput(fill(block, input, 0, 128), 128);
      bool dry = true;
      for (int i = 0; i < 128; ++i) dry = dry && chain.output()->buffer[i][0] == block[i][0];
      expect(dry, "bypassing the slot returns dry audio in chain '" + prefix + "'");
    }
  }

  template<class Function> double milliseconds(Function function) {
    double best = 1e9;
    for (int run = 0; run < 3; ++run) {
      const auto start = std::chrono::steady_clock::now();
      function();
      best = std::min(best, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    }
    return best;
  }

  void testCpuBudget() {
    const auto input = pinkNoise(int(10 * kRate));
    std::vector<vital::poly_float> block(128);
    vital::SpectralBalanceModule balance_module;
    balance_module.init(); balance_module.setSampleRate(int(kRate));
    balance_module.getControls().at("spectral_balance_amount")->set(1.0f);
    vital::SpectralCompressorModule compressor;
    compressor.init(); compressor.setSampleRate(int(kRate));
    compressor.getControls().at("spectral_compressor_downward")->set(50.0f);
    compressor.getControls().at("spectral_compressor_upward")->set(50.0f);
    const auto drive = [&](vital::SynthModule& module) {
      for (int offset = 0; offset < int(input.size()); offset += 128)
        module.processWithInput(fill(block, input, offset, 128), 128);
    };
    const double balance_ms = milliseconds([&] { drive(balance_module); });
    const double compressor_ms = milliseconds([&] { drive(compressor); });
    std::cout << "10 s of audio: Spectral Balance " << number(balance_ms) << " ms, Spectral Compressor "
              << number(compressor_ms) << " ms\n";
    // The balance runs a 2048-point STFT against the compressor's 1024 and a
    // per-bin log, so a tight bound depends on the machine.
    expect(balance_ms <= 2.0 * compressor_ms, "Spectral Balance must cost at most twice the Spectral Compressor's CPU");
  }
}

int main() {
  juce::ScopedJuceInitialiser_GUI juce;
  testTargets();
  testBypassAndLatency();
  testTamesResonances();
  testLevelsTilt();
  testRangeLimits();
  testModule();
  testCpuBudget();
  if (failures) return 1;
  std::cout << "Spectral Balance: targets, aligned bypass, resonance taming, tilt leveling, range, module, chain and CPU budget passed.\n";
  return 0;
}
