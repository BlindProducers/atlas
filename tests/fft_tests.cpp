// SPDX-License-Identifier: GPL-3.0-or-later
// atlas::Fft and atlas::ComplexFft (PFFFT) against juce::dsp::FFT, the layout
// and scaling they stand in for, and a benchmark of both (vDSP on macOS).
//   AtlasFftTests              correctness
//   AtlasFftTests --benchmark  timing
#if defined(__APPLE__)
// Before JuceHeader.h, whose `using namespace juce` clashes with Carbon's names.
#include <Accelerate/Accelerate.h>
#endif

#include "JuceHeader.h"
#include "atlas_fft.h"
#include "fourier_transform.h"
#include "pffft/pffft.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <new>
#include <random>
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
  int failures = 0;

  void expect(bool condition, const std::string& message) {
    if (!condition) {
      std::cerr << "FAIL: " << message << '\n';
      ++failures;
    }
  }

  std::vector<float> noise(int count, unsigned seed) {
    std::mt19937 random(seed);
    std::uniform_real_distribution<float> distribution(-1.0f, 1.0f);
    std::vector<float> values(static_cast<size_t>(count));
    for (float& value : values)
      value = distribution(random);
    return values;
  }

  // Largest difference over [0, count), relative to the reference's peak.
  float relativeError(const float* reference, const float* actual, int count) {
    float peak = 1.0e-30f, error = 0.0f;
    for (int i = 0; i < count; ++i) {
      peak = std::max(peak, std::abs(reference[i]));
      error = std::max(error, std::abs(reference[i] - actual[i]));
    }
    return error / peak;
  }

  constexpr float kTolerance = 2.0e-6f;

  // A 2N-float buffer whose data starts `offset` floats past a 16-byte boundary.
  struct OffsetBuffer {
    OffsetBuffer(int floats, int offset) : storage(static_cast<size_t>(floats + 8), 0.0f) {
      auto address = reinterpret_cast<std::uintptr_t>(storage.data());
      auto aligned = (address + 15) & ~std::uintptr_t(15);
      data = reinterpret_cast<float*>(aligned) + offset;
    }
    std::vector<float> storage;
    float* data;
  };

  void testRealForward() {
    for (int order = 5; order <= 15; ++order) {
      const int n = 1 << order;
      juce::dsp::FFT reference_fft(order);
      atlas::Fft fft(order);
      const std::vector<float> input = noise(n, 100u + unsigned(order));

      for (bool non_negative : { true, false }) {
        std::vector<float> reference(static_cast<size_t>(2 * n), 0.0f);
        std::copy(input.begin(), input.end(), reference.begin());
        reference_fft.performRealOnlyForwardTransform(reference.data(), non_negative);
        const int compared = non_negative ? n + 2 : 2 * n;

        for (int offset = 0; offset < 4; ++offset) {
          OffsetBuffer buffer(2 * n, offset);
          std::fill(buffer.data, buffer.data + 2 * n, 123.0f);
          std::copy(input.begin(), input.end(), buffer.data);
          track_allocations = true;
          allocations = 0;
          fft.performRealOnlyForwardTransform(buffer.data, non_negative);
          track_allocations = false;
          const std::string label = "real forward order " + std::to_string(order) + " offset " +
                                    std::to_string(offset) + (non_negative ? " half" : " full");
          const float error = relativeError(reference.data(), buffer.data, compared);
          expect(error < kTolerance, label + ": error " + std::to_string(error));
          expect(buffer.data[1] == 0.0f && buffer.data[n + 1] == 0.0f, label + ": DC and Nyquist are real");
          expect(allocations == 0, label + ": allocates");
        }
      }
    }
  }

  void testRealInverse() {
    for (int order = 5; order <= 15; ++order) {
      const int n = 1 << order;
      juce::dsp::FFT reference_fft(order);
      atlas::Fft fft(order);
      // Bins 0 .. n/2 with stray imaginary parts at DC and Nyquist, which both ignore.
      const std::vector<float> spectrum = noise(n + 2, 200u + unsigned(order));

      std::vector<float> reference(static_cast<size_t>(2 * n), 0.0f);
      std::copy(spectrum.begin(), spectrum.end(), reference.begin());
      reference_fft.performRealOnlyInverseTransform(reference.data());

      for (int offset = 0; offset < 4; ++offset) {
        OffsetBuffer buffer(2 * n, offset);
        std::fill(buffer.data, buffer.data + 2 * n, 123.0f);
        std::copy(spectrum.begin(), spectrum.end(), buffer.data);
        track_allocations = true;
        allocations = 0;
        fft.performRealOnlyInverseTransform(buffer.data);
        track_allocations = false;
        const std::string label = "real inverse order " + std::to_string(order) + " offset " + std::to_string(offset);
        const float error = relativeError(reference.data(), buffer.data, n);
        expect(error < kTolerance, label + ": error " + std::to_string(error));
        expect(std::all_of(buffer.data + n, buffer.data + 2 * n, [](float value) { return value == 0.0f; }),
               label + ": upper half cleared");
        expect(allocations == 0, label + ": allocates");
      }

      // Forward then inverse returns the signal.
      const std::vector<float> input = noise(n, 300u + unsigned(order));
      std::vector<float> round_trip(static_cast<size_t>(2 * n), 0.0f);
      std::copy(input.begin(), input.end(), round_trip.begin());
      fft.performRealOnlyForwardTransform(round_trip.data(), true);
      fft.performRealOnlyInverseTransform(round_trip.data());
      expect(relativeError(input.data(), round_trip.data(), n) < kTolerance,
             "round trip order " + std::to_string(order));
    }
  }

  void testComplex() {
    using Complex = std::complex<float>;
    for (int order = 4; order <= 13; ++order) {
      const int n = 1 << order;
      juce::dsp::FFT reference_fft(order);
      atlas::ComplexFft fft(order);
      const std::vector<float> values = noise(2 * n, 400u + unsigned(order));
      std::vector<Complex> input(static_cast<size_t>(n));
      std::memcpy(input.data(), values.data(), sizeof(float) * values.size());

      for (bool inverse : { false, true }) {
        std::vector<Complex> reference(static_cast<size_t>(n));
        reference_fft.perform(input.data(), reference.data(), inverse);
        const std::string kind = std::string(inverse ? "inverse" : "forward") + " order " + std::to_string(order);

        std::vector<Complex> output(static_cast<size_t>(n));
        track_allocations = true;
        allocations = 0;
        fft.perform(input.data(), output.data(), inverse);
        track_allocations = false;
        expect(allocations == 0, "complex " + kind + ": allocates");
        expect(relativeError(reinterpret_cast<const float*>(reference.data()),
                             reinterpret_cast<const float*>(output.data()), 2 * n) < kTolerance,
               "complex " + kind);

        std::vector<Complex> in_place = input;
        fft.perform(in_place.data(), in_place.data(), inverse);
        expect(relativeError(reinterpret_cast<const float*>(reference.data()),
                             reinterpret_cast<const float*>(in_place.data()), 2 * n) < kTolerance,
               "complex in place " + kind);

        // Misaligned output (std::complex<float> only guarantees 4 bytes).
        OffsetBuffer misaligned(2 * n, 1);
        fft.perform(input.data(), reinterpret_cast<Complex*>(misaligned.data), inverse);
        expect(relativeError(reinterpret_cast<const float*>(reference.data()), misaligned.data, 2 * n) < kTolerance,
               "complex misaligned " + kind);
      }
    }
  }

  void testVitalTransform() {
    constexpr int kBits = 11;
    constexpr int n = 1 << kBits;
    juce::dsp::FFT reference_fft(kBits);
    vital::FourierTransform transform(kBits);
    const std::vector<float> input = noise(n, 500u);

    std::vector<float> reference(2 * n, 0.0f), actual(2 * n, 0.0f), shared(2 * n, 0.0f);
    std::copy(input.begin(), input.end(), reference.begin());
    std::copy(input.begin(), input.end(), actual.begin());
    std::copy(input.begin(), input.end(), shared.begin());
    reference_fft.performRealOnlyForwardTransform(reference.data(), true);
    transform.transformRealForward(actual.data());
    vital::FFT<kBits>::transformRealForward(shared.data());
    expect(relativeError(reference.data(), actual.data(), n + 2) < kTolerance, "vital forward");
    expect(relativeError(reference.data(), shared.data(), n + 2) < kTolerance, "vital shared forward");

    transform.transformRealInverse(actual.data());
    vital::FFT<kBits>::transformRealInverse(shared.data());
    expect(relativeError(input.data(), actual.data(), n) < kTolerance, "vital round trip");
    expect(relativeError(input.data(), shared.data(), n) < kTolerance, "vital shared round trip");
  }

  // Benchmark: the fastest of several rounds, in nanoseconds per call.
  template<class Function>
  double timeNanoseconds(int calls, Function function) {
    double best = 1.0e30;
    for (int round = 0; round < 9; ++round) {
      const auto start = std::chrono::steady_clock::now();
      for (int i = 0; i < calls; ++i)
        function();
      const auto end = std::chrono::steady_clock::now();
      best = std::min(best, std::chrono::duration<double, std::nano>(end - start).count() / calls);
    }
    return best;
  }

  volatile float sink = 0.0f;

  void benchmark() {
    std::cout << "PFFFT SIMD lanes: " << pffft_simd_size() << "\n\n";
    std::cout << "Real forward (non-negative bins) + inverse, ns per pair\n";
    std::cout << std::setw(7) << "size" << std::setw(14) << "JUCE FFT" << std::setw(14) << "raw vDSP"
              << std::setw(14) << "vDSP DFT" << std::setw(14) << "atlas::Fft" << std::setw(16) << "JUCE / atlas"
              << '\n';
    for (int order : { 8, 9, 10, 11, 12, 13, 15 }) {
      const int n = 1 << order;
      const int calls = std::max(64, (1 << 24) / (n * order));
      const std::vector<float> input = noise(n, 600u + unsigned(order));
      std::vector<float> buffer(static_cast<size_t>(2 * n), 0.0f);

      juce::dsp::FFT juce_fft(order);
      std::copy(input.begin(), input.end(), buffer.begin());
      const double juce_time = timeNanoseconds(calls, [&] {
        juce_fft.performRealOnlyForwardTransform(buffer.data(), true);
        juce_fft.performRealOnlyInverseTransform(buffer.data());
      });
      sink = sink + buffer[3];

      double vdsp_time = 0.0;
     #if JUCE_MAC
      // vDSP with only the scaling the transforms need, no layout work.
      FFTSetup setup = vDSP_create_fftsetup(vDSP_Length(order), 2);
      std::copy(input.begin(), input.end(), buffer.begin());
      vdsp_time = timeNanoseconds(calls, [&] {
        DSPSplitComplex split = { buffer.data(), buffer.data() + 1 };
        vDSP_fft_zrip(setup, &split, 2, vDSP_Length(order), kFFTDirection_Forward);
        vDSP_fft_zrip(setup, &split, 2, vDSP_Length(order), kFFTDirection_Inverse);
        const float scale = 0.5f / float(n);
        vDSP_vsmul(buffer.data(), 1, &scale, buffer.data(), 1, vDSP_Length(n));
      });
      vDSP_destroy_fftsetup(setup);
      sink = sink + buffer[3];

      // vDSP's newer DFT interface on split even/odd input, no layout work.
      double dft_time = 0.0;
      vDSP_DFT_Setup forward = vDSP_DFT_zrop_CreateSetup(nullptr, vDSP_Length(n), vDSP_DFT_FORWARD);
      vDSP_DFT_Setup backward = vDSP_DFT_zrop_CreateSetup(forward, vDSP_Length(n), vDSP_DFT_INVERSE);
      std::vector<float> even(input.begin(), input.begin() + n / 2), odd(input.begin() + n / 2, input.end());
      std::vector<float> real(static_cast<size_t>(n / 2)), imaginary(static_cast<size_t>(n / 2));
      dft_time = timeNanoseconds(calls, [&] {
        vDSP_DFT_Execute(forward, even.data(), odd.data(), real.data(), imaginary.data());
        vDSP_DFT_Execute(backward, real.data(), imaginary.data(), even.data(), odd.data());
        const float scale = 0.25f / float(n);
        vDSP_vsmul(even.data(), 1, &scale, even.data(), 1, vDSP_Length(n / 2));
        vDSP_vsmul(odd.data(), 1, &scale, odd.data(), 1, vDSP_Length(n / 2));
      });
      vDSP_DFT_DestroySetup(backward);
      vDSP_DFT_DestroySetup(forward);
      sink = sink + even[3];
     #else
      const double dft_time = 0.0;
     #endif

      atlas::Fft fft(order);
      std::copy(input.begin(), input.end(), buffer.begin());
      const double atlas_time = timeNanoseconds(calls, [&] {
        fft.performRealOnlyForwardTransform(buffer.data(), true);
        fft.performRealOnlyInverseTransform(buffer.data());
      });
      sink = sink + buffer[3];

      std::cout << std::setw(7) << n << std::fixed << std::setprecision(0)
                << std::setw(14) << juce_time << std::setw(14) << vdsp_time << std::setw(14) << dft_time
                << std::setw(14) << atlas_time << std::setprecision(2) << std::setw(15) << juce_time / atlas_time
                << "x\n";
    }

    std::cout << "\nComplex forward + inverse, ns per pair\n";
    std::cout << std::setw(7) << "size" << std::setw(14) << "JUCE FFT" << std::setw(14) << "atlas"
              << std::setw(16) << "JUCE / atlas" << '\n';
    for (int order : { 10, 11, 12 }) {
      const int n = 1 << order;
      const int calls = std::max(64, (1 << 24) / (n * order));
      std::vector<std::complex<float>> a(static_cast<size_t>(n)), b(static_cast<size_t>(n));
      const std::vector<float> values = noise(2 * n, 700u + unsigned(order));
      std::memcpy(a.data(), values.data(), sizeof(float) * values.size());

      juce::dsp::FFT juce_fft(order);
      const double juce_time = timeNanoseconds(calls, [&] {
        juce_fft.perform(a.data(), b.data(), false);
        juce_fft.perform(b.data(), a.data(), true);
      });
      atlas::ComplexFft fft(order);
      const double atlas_time = timeNanoseconds(calls, [&] {
        fft.perform(a.data(), b.data(), false);
        fft.perform(b.data(), a.data(), true);
      });
      sink = sink + a[3].real();
      std::cout << std::setw(7) << n << std::fixed << std::setprecision(0) << std::setw(14) << juce_time
                << std::setw(14) << atlas_time << std::setprecision(2) << std::setw(15) << juce_time / atlas_time
                << "x\n";
    }
  }
} // namespace

int main(int argc, char** argv) {
  juce::ScopedJuceInitialiser_GUI juce;
  if (argc > 1 && std::strcmp(argv[1], "--benchmark") == 0) {
    benchmark();
    return 0;
  }

  testRealForward();
  testRealInverse();
  testComplex();
  testVitalTransform();
  if (failures != 0)
    return 1;
  std::cout << "Atlas FFT tests passed (real and complex against juce::dsp::FFT, misaligned buffers, "
               "Vital's transform, no allocations)\n";
  return 0;
}
