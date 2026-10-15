#pragma once

// vital::FourierTransform (PFFFT through atlas::Fft) against juce::dsp::FFT:
// the same bin layout and scaling, and the process-wide vital::FFT<bits> used
// by several threads at once.

#include "fourier_transform.h"

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

  // vital::FFT<bits> is one object for the whole process, used by wavetable
  // rendering on the message thread and possibly other threads.
  void testSharedTransformIsThreadSafe() {
    std::vector<float> spectrum(2 * kSize, 0.0f);
    for (int i = 1; i < 40; ++i) {
      spectrum[2 * i] = 1.0f / i;
      spectrum[2 * i + 1] = 0.5f / i;
    }
    std::vector<float> expected = spectrum;
    vital::FFT<kBits>::transformRealInverse(expected.data());
    std::atomic<int> mismatches { 0 };
    auto worker = [&] {
      std::vector<float> data(2 * kSize);
      for (int round = 0; round < 2000; ++round) {
        std::copy(spectrum.begin(), spectrum.end(), data.begin());
        vital::FFT<kBits>::transformRealInverse(data.data());
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

  int run() {
    testMatchesJuceLayoutAndScaling();
    testSharedTransformIsThreadSafe();
    if (failures == 0)
      std::cout << "Atlas Fourier transform tests passed (PFFFT)\n";
    return failures == 0 ? 0 : 1;
  }
}
