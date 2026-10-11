#include "JuceHeader.h"
#include "spectral_gate_module.h"
#include "reorderable_effect_chain.h"

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <new>
#include <vector>

namespace {
  std::atomic<bool> track_allocations { false };
  std::atomic<size_t> allocations { 0 };
}
void* operator new(std::size_t size) {
  if (track_allocations.load())
    ++allocations;
  if (void* result = std::malloc(size))
    return result;
  throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {
  using Gate = vital::SpectralGateModule;
  using Signal = std::vector<vital::poly_float>;
  constexpr int N = Gate::kFftSize;
  int failures = 0;

  void expect(bool condition, const char* message) {
    if (!condition) {
      std::cerr << "FAIL: " << message << '\n';
      ++failures;
    }
  }

  void configure(Gate& gate, float cutoff, float balance, float mix, float tilt = 0.0f, bool enable_tilt = false, int fft_index = 3) {
    const auto controls = gate.getControls();
    controls.at("spectral_gate_fft_size")->set(fft_index);
    controls.at("spectral_gate_cutoff")->set(cutoff);
    controls.at("spectral_gate_balance")->set(balance);
    controls.at("spectral_gate_mix")->set(mix);
    controls.at("spectral_gate_tilt")->set(tilt);
    controls.at("spectral_gate_tilt_enabled")->set(enable_tilt ? 1.0f : 0.0f);
    gate.hardReset();
  }

  Signal render(vital::SynthModule& module, const Signal& input, int block_size) {
    Signal output(input.size());
    for (int i = 0; i < static_cast<int>(input.size());) {
      const int block = std::min(block_size, static_cast<int>(input.size()) - i);
      module.processWithInput(input.data() + i, block);
      std::copy_n(module.output()->buffer, block, output.data() + i);
      i += block;
    }
    return output;
  }

  float maximumDifference(const Signal& a, const Signal& b, int delay = 0) {
    float error = 0.0f;
    for (int i = 0; i < static_cast<int>(a.size()); ++i) {
      for (int lane = 0; lane < vital::poly_float::kSize; ++lane) {
        const float expected = i >= delay ? b[i - delay][lane] : 0.0f;
        if (!std::isfinite(a[i][lane]))
          return INFINITY;
        error = std::max(error, std::abs(a[i][lane] - expected));
      }
    }
    return error;
  }

  float tone(int bin, int sample) {
    return std::sin(2.0 * juce::MathConstants<double>::pi * bin * sample / N);
  }

  float amplitude(const Signal& signal, int bin) {
    double real = 0.0, imaginary = 0.0;
    for (int i = 2 * N; i < static_cast<int>(signal.size()); ++i) {
      const double phase = 2.0 * juce::MathConstants<double>::pi * bin * i / N;
      real += signal[i][0] * std::cos(phase);
      imaginary += signal[i][0] * std::sin(phase);
    }
    return 2.0 * std::sqrt(real * real + imaginary * imaginary) / (signal.size() - 2 * N);
  }

  void testReconstructionAndBlocks() {
    Gate gate;
    gate.init();
    Signal input(6 * N);
    uint32_t noise = 12345;
    for (auto& sample : input) {
      for (int lane = 0; lane < vital::poly_float::kSize; ++lane) {
        noise = noise * 1664525u + 1013904223u;
        sample.set(lane, static_cast<float>(noise >> 8) / 16777216.0f - 0.5f);
      }
    }
    // Covers all SIMD lanes, startup, exact latency, full dry and mixed output.
    for (int sample_rate : { 44100, 48000, 96000 }) {
      gate.setSampleRate(sample_rate);
      for (int oversample : { 1, 2, 4 }) {
        gate.setOversampleAmount(oversample);
        for (float mix : { 0.0f, 0.5f, 1.0f }) {
          configure(gate, 0.0f, 1.0f, mix);
          const auto output = render(gate, input, 127);
          expect(maximumDifference(output, input, N) < 3.0e-6f,
                 "open gate must reconstruct every lane with coherent dry/wet and exactly N samples of delay");
        }
      }
    }
    configure(gate, 0.71f, 0.65f, 0.8f, -0.75f, true);
    const auto reference = render(gate, input, 1);
    for (int block : { 7, 64, 127, vital::kMaxBufferSize }) {
      gate.hardReset();
      expect(maximumDifference(render(gate, input, block), reference) < 1.0e-6f,
             "gated output must be independent of host block boundaries");
    }
    // Process in place, including FFT boundaries.
    gate.hardReset();
    Signal in_place = input;
    for (int i = 0; i < static_cast<int>(in_place.size()); i += 128) {
      std::copy_n(in_place.data() + i, 128, gate.output()->buffer);
      gate.processWithInput(gate.output()->buffer, 128);
      std::copy_n(gate.output()->buffer, 128, in_place.data() + i);
    }
    expect(maximumDifference(in_place, reference) < 1.0e-6f, "in-place processing must be safe");
  }

  void testSelectableFftSizes() {
    Gate gate;
    gate.init();
    auto* size_control = gate.getControls().at("spectral_gate_fft_size");
    for (int index = 0; index < Gate::kNumFftSizes; ++index) {
      const int size = 1 << (Gate::kMinFftOrder + index);
      Signal input(6 * size);
      for (int sample = 0; sample < static_cast<int>(input.size()); ++sample) {
        const float value = 0.25f * std::sin(0.037 * sample) + 0.1f * std::cos(0.17 * sample);
        input[sample] = vital::poly_float(value, -value, value * 0.2f, -value * 0.3f);
      }
      for (int rate : { 44100, 48000, 96000 }) {
        gate.setSampleRate(rate);
        for (float mix : { 0.0f, 0.5f, 1.0f }) {
          configure(gate, 0.0f, 1.0f, mix, 0.0f, false, index);
          const auto output = render(gate, input, 127);
          expect(gate.getFftSize() == size && gate.getFftSize() <= 8192,
                 "selected FFT size must reach the DSP and never exceed 8192");
          expect(maximumDifference(output, input, size) < 5.0e-6f,
                 "every FFT size must reconstruct all lanes with exactly its selected delay");
        }
      }
      configure(gate, 0.65f, 0.7f, 1.0f, 0.5f, true, index);
      const auto expected = render(gate, input, 1);
      gate.hardReset();
      expect(maximumDifference(render(gate, input, 127), expected) < 1.0e-6f,
             "gating and tilt remain independent of block boundaries at every FFT size");
    }
    std::array<vital::poly_float, 128> block;
    block.fill(0.2f);
    allocations = 0;
    track_allocations = true;
    for (int index : { 0, 5, 1, 4, 2, 3, 99, -1 }) {
      size_control->set(index);
      for (int i = 0; i < 80; ++i)
        gate.processWithInput(block.data(), 128);
    }
    track_allocations = false;
    expect(allocations == 0, "FFT switching and first use of all plans must not allocate");
    expect(gate.getFftSize() == Gate::kFftSize, "invalid FFT choices fall back to the safe default");

    configure(gate, 0.0f, 1.0f, 1.0f, 0.0f, false, 3);
    render(gate, Signal(3 * N, vital::poly_float(0.2f)), 128);
    const auto previous = gate.output()->buffer[127];
    size_control->set(5);
    gate.processWithInput(block.data(), 128);
    expect(std::abs(gate.output()->buffer[0][0] - previous[0]) < 0.005f,
           "live FFT changes fade from the previous output instead of cutting abruptly");
  }

  void testStrongWeakAndTilt() {
    Gate gate;
    gate.init();
    Signal input(8 * N);
    for (int i = 0; i < static_cast<int>(input.size()); ++i)
      input[i] = 0.5f * tone(64, i) + 0.02f * tone(200, i);
    for (float balance : { 0.0f, 0.7f, 1.0f }) {
      configure(gate, 0.8f, balance, 1.0f);
      const auto output = render(gate, input, 127);
      expect(std::abs(amplitude(output, 64) - 0.5f * std::pow(balance, 3.0f)) < 1.0e-5f,
             "strong tone must follow SpectralSuite's cubic balance curve");
      expect(std::abs(amplitude(output, 200) - 0.02f * std::pow(1.0f - balance, 4.0f)) < 1.0e-5f,
             "weak tone must follow SpectralSuite's quartic balance curve");
    }
    for (int i = 0; i < static_cast<int>(input.size()); ++i)
      input[i] = 0.2f * tone(32, i) + 0.2f * tone(960, i);
    configure(gate, 0.8f, 1.0f, 1.0f, 1.0f, true);
    const auto low = render(gate, input, 64);
    expect(amplitude(low, 32) > 0.19f && amplitude(low, 960) < 1.0e-5f,
           "positive tilt must favor low frequencies");
    configure(gate, 0.8f, 1.0f, 1.0f, -1.0f, true);
    const auto high = render(gate, input, 64);
    expect(amplitude(high, 960) > 0.19f && amplitude(high, 32) < 1.0e-5f,
           "negative tilt must favor high frequencies");
    configure(gate, 0.8f, 1.0f, 1.0f, -1.0f, false);
    const auto flat = render(gate, input, 64);
    configure(gate, 0.8f, 1.0f, 1.0f, 1.0f, false);
    expect(maximumDifference(flat, render(gate, input, 64)) < 1.0e-6f,
           "tilt must have no effect when disabled");
  }

  void testRealtimeAndReset() {
    Gate gate;
    gate.init();
    configure(gate, 0.6f, 0.7f, 1.0f);
    std::array<vital::poly_float, 128> input;
    input.fill(0.2f);
    allocations = 0;
    track_allocations = true;
    for (int i = 0; i < N / 128 + 1; ++i)
      gate.processWithInput(input.data(), 128);
    gate.enable(false);
    gate.enable(true);
    gate.processWithInput(input.data(), 0);
    track_allocations = false;
    expect(allocations == 0, "first FFT, processing, and enable/reset must not allocate");
    Signal silence(3 * N, vital::poly_float(0.0f));
    expect(maximumDifference(render(gate, silence, 127), silence) == 0.0f,
           "reenabling must clear spectral tails");
    render(gate, Signal(N, vital::poly_float(0.5f)), 128);
    gate.setSampleRate(96000);
    expect(maximumDifference(render(gate, silence, 128), silence) == 0.0f,
           "sample rate changes must clear spectral tails");
  }

  void testSlots() {
    vital::Value tempo(2.0f), keytrack(60.0f);
    vital::ReorderableEffectChain chain(tempo.output(), keytrack.output(), "bus_3_");
    chain.init();
    const auto controls = chain.getControls();
    for (int slot : { 1, 2 }) {
      const std::string prefix = "bus_3_fx_slot_" + std::to_string(slot) + "_";
      controls.at(prefix + "type")->set(vital::constants::kSpectralGate + 1);
      controls.at(prefix + "enabled")->set(1.0f);
      controls.at(prefix + "spectral_gate_cutoff")->set(0.0f);
      controls.at(prefix + "spectral_gate_balance")->set(1.0f);
    }
    Signal input(4 * N, vital::poly_float(0.0f));
    input[0] = vital::poly_float(1.0f, -0.5f, 0.25f, -0.125f);
    expect(maximumDifference(render(chain, input, 127), input, 2 * N) < 3.0e-6f,
           "multiple bus slots must have independent spectral buffers");
    Signal loud(N, vital::poly_float(0.5f));
    render(chain, loud, 128);
    for (int slot : { 1, 2 })
      controls.at("bus_3_fx_slot_" + std::to_string(slot) + "_enabled")->set(0.0f);
    expect(maximumDifference(render(chain, input, 128), input) == 0.0f,
           "disabled effect slots must pass input through immediately");
    for (int slot : { 1, 2 })
      controls.at("bus_3_fx_slot_" + std::to_string(slot) + "_enabled")->set(1.0f);
    Signal silence(4 * N, vital::poly_float(0.0f));
    expect(maximumDifference(render(chain, silence, 128), silence) == 0.0f,
           "slot bypass must discard buffered audio before reenabling");
    controls.at("bus_3_fx_slot_1_spectral_gate_fft_size")->set(0.0f);
    controls.at("bus_3_fx_slot_2_spectral_gate_fft_size")->set(5.0f);
    chain.hardReset();
    Signal mixed_sizes(3 * (256 + 8192), vital::poly_float(0.0f));
    mixed_sizes[0] = vital::poly_float(0.5f, -0.25f, 0.1f, -0.05f);
    expect(maximumDifference(render(chain, mixed_sizes, 127), mixed_sizes, 256 + 8192) < 3.0e-6f,
           "different FFT sizes in serial slots must retain independent history and add their exact delays");
  }
}

int main() {
  ScopedJuceInitialiser_GUI juce;
  testReconstructionAndBlocks();
  testSelectableFftSizes();
  testStrongWeakAndTilt();
  testRealtimeAndReset();
  testSlots();
  if (failures != 0)
    return 1;
  std::cout << "Atlas spectral gate tests passed (reconstruction, strong/weak, tilt, block sizes, rates, reset, slots, allocations)\n";
  return 0;
}
