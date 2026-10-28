#pragma once

// vital::FourierTransform against juce::dsp::FFT: the same bin layout and
// scaling whichever engine it uses, and one transform object shared by
// several threads, as vital::FFT<bits>::transform() is. With PFFFT, also
// juce::dsp::FFT's PFFFT engine (juce8-pffft-fft-engine.patch) against a
// direct DFT in double precision: every transform, misaligned buffers, the
// sizes Atlas uses and one object shared by several threads.

#include "fourier_transform.h"

#include <complex>
#include <cstring>

namespace fourier_transform_tests {
  int failures = 0;

  void expect(bool condition, const std::string& message) {
    if (!condition) {
      std::cerr << "FAIL: " << message << '\n';
      ++failures;
    }
  }

  constexpr int kBits = 11, kSize = 1 << kBits;

  float largestDifference(const float* a, const float* b, int count) {
    float largest = 0.0f;
    for (int i = 0; i < count; ++i)
      largest = std::max(largest, std::abs(a[i] - b[i]));
    return largest;
  }

  float largestMagnitude(const float* a, int count) {
    float largest = 0.0f;
    for (int i = 0; i < count; ++i)
      largest = std::max(largest, std::abs(a[i]));
    return largest;
  }

  void testMatchesJuceLayoutAndScaling() {
    juce::Random random(11);
    vital::FourierTransform transform(kBits);
    juce::dsp::FFT reference(kBits);

    // Forward: bins 0 to N/2, unscaled, DC and Nyquist with no imaginary part.
    std::vector<float> signal(kSize);
    for (float& sample : signal)
      sample = random.nextFloat() * 2.0f - 1.0f;
    std::vector<float> ours(2 * kSize, 0.0f), theirs(2 * kSize, 0.0f);
    std::copy(signal.begin(), signal.end(), ours.begin());
    std::copy(signal.begin(), signal.end(), theirs.begin());
    transform.transformRealForward(ours.data());
    reference.performRealOnlyForwardTransform(theirs.data(), true);
    const float scale = largestMagnitude(theirs.data(), kSize + 2);
    const float forward_error = largestDifference(ours.data(), theirs.data(), kSize + 2) / scale;
    expect(forward_error < 1.0e-5f, "forward bins match juce::dsp::FFT (relative error " +
                                        std::to_string(forward_error) + ")");
    expect(ours[1] == 0.0f && ours[kSize + 1] == 0.0f, "DC and Nyquist have no imaginary part");

    // Inverse: scaled by 1/N, the real signal back, the second half cleared.
    std::vector<float> spectrum(theirs.begin(), theirs.begin() + kSize + 2);
    std::fill(ours.begin(), ours.end(), 7.0f);
    std::copy(spectrum.begin(), spectrum.end(), ours.begin());
    std::copy(spectrum.begin(), spectrum.end(), theirs.begin());
    transform.transformRealInverse(ours.data());
    reference.performRealOnlyInverseTransform(theirs.data());
    const float inverse_error = largestDifference(ours.data(), signal.data(), kSize);
    const float reference_error = largestDifference(ours.data(), theirs.data(), kSize);
    expect(inverse_error < 1.0e-5f, "the inverse gives the signal back (error " + std::to_string(inverse_error) + ")");
    expect(reference_error < 1.0e-5f, "the inverse matches juce::dsp::FFT (error " + std::to_string(reference_error) + ")");
    bool cleared = true;
    for (int i = kSize; i < 2 * kSize; ++i)
      cleared = cleared && ours[i] == 0.0f;
    expect(cleared, "the inverse clears the buffer's second half");

    // A pure tone lands in its own bin with the expected sign.
    std::vector<float> tone(2 * kSize, 0.0f);
    for (int i = 0; i < kSize; ++i)
      tone[i] = std::sin(2.0f * juce::MathConstants<float>::pi * 5.0f * i / kSize);
    transform.transformRealForward(tone.data());
    expect(std::abs(tone[10]) < 1.0e-2f && std::abs(tone[11] + kSize / 2.0f) < 1.0e-1f,
           "a sine at bin 5 is -N/2 imaginary there, as juce::dsp::FFT gives");
  }

  // vital::FFT<bits>::transform() is one object for the whole process, used by
  // wavetable rendering on the message thread and possibly other threads.
  void testSharedTransformIsThreadSafe() {
    std::vector<float> spectrum(2 * kSize, 0.0f);
    for (int i = 1; i < 40; ++i) {
      spectrum[2 * i] = 1.0f / i;
      spectrum[2 * i + 1] = 0.5f / i;
    }
    std::vector<float> expected = spectrum;
    vital::FFT<kBits>::transform()->transformRealInverse(expected.data());
    std::atomic<int> mismatches { 0 };
    auto worker = [&] {
      std::vector<float> data(2 * kSize);
      for (int round = 0; round < 2000; ++round) {
        std::copy(spectrum.begin(), spectrum.end(), data.begin());
        vital::FFT<kBits>::transform()->transformRealInverse(data.data());
        if (largestDifference(data.data(), expected.data(), kSize) != 0.0f)
          ++mismatches;
      }
    };
    std::thread first(worker), second(worker);
    worker();
    first.join();
    second.join();
    expect(mismatches.load() == 0, "a transform shared by three threads gives the same result in each");
  }

  // The DFT of `count` complex values, in double precision.
  std::vector<std::complex<double>> directDft(const std::vector<std::complex<double>>& input, bool inverse) {
    const int count = int(input.size());
    std::vector<std::complex<double>> output(count);
    const double sign = inverse ? 1.0 : -1.0;
    for (int bin = 0; bin < count; ++bin) {
      std::complex<double> sum;
      for (int i = 0; i < count; ++i) {
        const double angle = sign * 2.0 * juce::MathConstants<double>::pi * double((long long) bin * i % count) / count;
        sum += input[i] * std::complex<double>(std::cos(angle), std::sin(angle));
      }
      output[bin] = sum;
    }
    return output;
  }

  std::vector<float> randomSignal(juce::Random& random, int count) {
    std::vector<float> signal(count);
    for (float& sample : signal)
      sample = random.nextFloat() * 2.0f - 1.0f;
    return signal;
  }

  // Runs `transform` on a copy of `data` that starts one float past a
  // 16-byte boundary, so the engine has to take its scratch path.
  template<typename Transform>
  std::vector<float> misaligned(const std::vector<float>& data, Transform&& transform) {
    std::vector<float> storage(data.size() + 4);
    float* start = storage.data() + 1;
    std::copy(data.begin(), data.end(), start);
    transform(start);
    return std::vector<float>(start, start + data.size());
  }

  void testJuceFftOnPffft() {
   #if ATLAS_USE_PFFFT
    juce::Random random(23);
    // The engine is PFFFT: its bins are vital::FourierTransform's to the bit.
    {
      vital::FourierTransform ours(kBits);
      juce::dsp::FFT theirs(kBits);
      std::vector<float> a(2 * kSize, 0.0f), b(2 * kSize, 0.0f);
      const auto signal = randomSignal(random, kSize);
      std::copy(signal.begin(), signal.end(), a.begin());
      std::copy(signal.begin(), signal.end(), b.begin());
      ours.transformRealForward(a.data());
      theirs.performRealOnlyForwardTransform(b.data(), true);
      expect(std::memcmp(a.data(), b.data(), (kSize + 2) * sizeof(float)) == 0,
             "juce::dsp::FFT runs on PFFFT (the same bins as vital::FourierTransform, bit for bit)");
    }

    for (int order : { 5, 7, 8, 10, 11, 12 }) {
      const int size = 1 << order;
      const std::string name = " (order " + std::to_string(order) + ")";
      juce::dsp::FFT fft(order);
      const auto signal = randomSignal(random, size);
      std::vector<std::complex<double>> complex_signal(size);
      for (int i = 0; i < size; ++i)
        complex_signal[i] = signal[i];
      const auto expected = directDft(complex_signal, false);
      double peak = 0.0;
      for (const auto& bin : expected)
        peak = std::max(peak, std::abs(bin));

      // Real forward: all N bins (the negative frequencies mirrored), unscaled.
      std::vector<float> data(2 * size, 3.0f);
      std::copy(signal.begin(), signal.end(), data.begin());
      const std::vector<float> input = data;
      fft.performRealOnlyForwardTransform(data.data(), true);
      double error = 0.0;
      for (int bin = 0; bin < size; ++bin)
        error = std::max(error, std::abs(std::complex<double>(data[2 * bin], data[2 * bin + 1]) - expected[bin]));
      expect(error / peak < 1.0e-5, "real forward matches the DFT, mirrored half included" + name);
      expect(data[1] == 0.0f && data[size + 1] == 0.0f, "DC and Nyquist have no imaginary part" + name);
      const auto forward_misaligned = misaligned(input, [&](float* d) { fft.performRealOnlyForwardTransform(d, true); });
      expect(forward_misaligned == data, "a misaligned buffer gives the same forward transform" + name);

      // Real inverse: only bins 0 to N/2 count; scaled by 1/N; second half cleared.
      std::vector<float> spectrum(2 * size, 5.0f);
      std::copy(data.begin(), data.begin() + size + 2, spectrum.begin());
      const std::vector<float> spectrum_input = spectrum;
      fft.performRealOnlyInverseTransform(spectrum.data());
      float inverse_error = 0.0f;
      bool cleared = true;
      for (int i = 0; i < size; ++i)
        inverse_error = std::max(inverse_error, std::abs(spectrum[i] - signal[i]));
      for (int i = size; i < 2 * size; ++i)
        cleared = cleared && spectrum[i] == 0.0f;
      expect(inverse_error < 1.0e-5f, "real inverse gives the signal back" + name);
      expect(cleared, "real inverse clears the second half" + name);
      const auto inverse_misaligned = misaligned(spectrum_input, [&](float* d) { fft.performRealOnlyInverseTransform(d); });
      expect(inverse_misaligned == spectrum, "a misaligned buffer gives the same inverse transform" + name);

      // Magnitudes, which juce::dsp::FFT computes from the real forward transform.
      std::vector<float> magnitudes(2 * size, 0.0f);
      std::copy(signal.begin(), signal.end(), magnitudes.begin());
      fft.performFrequencyOnlyForwardTransform(magnitudes.data(), true);
      double magnitude_error = 0.0;
      for (int bin = 0; bin <= size / 2; ++bin)
        magnitude_error = std::max(magnitude_error, std::abs(magnitudes[bin] - std::abs(expected[bin])));
      expect(magnitude_error / peak < 1.0e-5, "magnitudes match the DFT" + name);

      // Complex forward (unscaled) and inverse (scaled by 1/N).
      std::vector<juce::dsp::Complex<float>> complex_input(size), complex_output(size), round_trip(size);
      for (int i = 0; i < size; ++i) {
        complex_input[i] = { random.nextFloat() - 0.5f, random.nextFloat() - 0.5f };
        complex_signal[i] = { complex_input[i].real(), complex_input[i].imag() };
      }
      const auto complex_expected = directDft(complex_signal, false);
      fft.perform(complex_input.data(), complex_output.data(), false);
      double complex_error = 0.0, complex_peak = 0.0;
      for (int bin = 0; bin < size; ++bin) {
        complex_peak = std::max(complex_peak, std::abs(complex_expected[bin]));
        complex_error = std::max(complex_error, std::abs(std::complex<double>(complex_output[bin].real(),
                                                         complex_output[bin].imag()) - complex_expected[bin]));
      }
      expect(complex_error / complex_peak < 1.0e-5, "complex forward matches the DFT" + name);
      fft.perform(complex_output.data(), round_trip.data(), true);
      float round_trip_error = 0.0f;
      for (int i = 0; i < size; ++i)
        round_trip_error = std::max(round_trip_error, std::abs(round_trip[i] - complex_input[i]));
      expect(round_trip_error < 1.0e-5f, "complex inverse gives the input back" + name);
    }

    // The largest size (Spectral Lock at high rates): misaligned and complex
    // transforms take their scratch from the heap there.
    {
      constexpr int kOrder = 15, kLarge = 1 << kOrder;
      juce::dsp::FFT fft(kOrder);
      const auto signal = randomSignal(random, kLarge);
      std::vector<float> data(2 * kLarge, 0.0f);
      std::copy(signal.begin(), signal.end(), data.begin());
      const auto forward = misaligned(data, [&](float* d) { fft.performRealOnlyForwardTransform(d, true); });
      std::vector<float> spectrum(2 * kLarge, 0.0f);
      std::copy(forward.begin(), forward.begin() + kLarge + 2, spectrum.begin());
      const auto back = misaligned(spectrum, [&](float* d) { fft.performRealOnlyInverseTransform(d); });
      float error = 0.0f;
      for (int i = 0; i < kLarge; ++i)
        error = std::max(error, std::abs(back[i] - signal[i]));
      expect(error < 1.0e-4f, "order 15, misaligned: forward then inverse gives the signal back");
      std::vector<juce::dsp::Complex<float>> complex_input(kLarge), complex_spectrum(kLarge), round_trip(kLarge);
      for (int i = 0; i < kLarge; ++i)
        complex_input[i] = { signal[i], -signal[i] };
      fft.perform(complex_input.data(), complex_spectrum.data(), false);
      fft.perform(complex_spectrum.data(), round_trip.data(), true);
      float complex_error = 0.0f;
      for (int i = 0; i < kLarge; ++i)
        complex_error = std::max(complex_error, std::abs(round_trip[i] - complex_input[i]));
      expect(complex_error < 1.0e-4f, "order 15: complex forward then inverse gives the input back");
    }

    // One object shared by several threads (atlas::sharedFft): real
    // transforms on aligned buffers take no lock and give the same results.
    {
      const juce::dsp::FFT shared(kBits);
      const auto signal = randomSignal(random, kSize);
      std::vector<float> expected(2 * kSize, 0.0f);
      std::copy(signal.begin(), signal.end(), expected.begin());
      shared.performRealOnlyForwardTransform(expected.data(), true);
      shared.performRealOnlyInverseTransform(expected.data());
      std::atomic<int> mismatches { 0 };
      auto worker = [&] {
        std::vector<float> data(2 * kSize);
        for (int round = 0; round < 2000; ++round) {
          std::fill(data.begin(), data.end(), 0.0f);
          std::copy(signal.begin(), signal.end(), data.begin());
          shared.performRealOnlyForwardTransform(data.data(), true);
          shared.performRealOnlyInverseTransform(data.data());
          if (data != expected)
            ++mismatches;
        }
      };
      std::thread first(worker), second(worker);
      worker();
      first.join();
      second.join();
      expect(mismatches.load() == 0, "a juce::dsp::FFT shared by three threads gives the same result in each");
    }
   #endif
  }

  int run() {
    testMatchesJuceLayoutAndScaling();
    testSharedTransformIsThreadSafe();
    testJuceFftOnPffft();
    if (failures == 0)
      std::cout << "Atlas Fourier transform tests passed (" << (ATLAS_USE_PFFFT ? "PFFFT" : "juce::dsp::FFT") << ")\n";
    return failures == 0 ? 0 : 1;
  }
}
