// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "oscillator_warp_fixture.h"
#include <fstream>

// Wavetable oscillators must sound exactly as they did before the Analog
// oscillator type existed. The reference is recorded from the earlier engine.
namespace wavetable_reference_tests {
  using O = vital::SynthOscillator;

  struct WavetableCase {
    int active;
    int unison;
    int distortion;
    int morph;
    bool resets;
  };

  std::vector<WavetableCase> wavetableCases() {
    std::vector<WavetableCase> cases;
    for (int active : {1, 2, 3}) for (int unison : {1, 2, 3, 16})
      for (int distortion : {int(O::kNone), int(O::kSync), int(O::kPulseWidth), int(O::kFmOscillatorA), int(O::kRmOscillatorB)})
        for (int morph : {int(O::kNoSpectralMorph), int(O::kVocode), int(O::kShepardTone)})
          cases.push_back({ active, unison, distortion, morph, (distortion + morph + unison) % 2 == 0 });
    return cases;
  }

  // Renders both outputs, including note resets and a mono overlap retrigger,
  // so the reference covers reset, blend, level and pan code paths.
  std::vector<float> renderWavetableCase(const WavetableCase& c) {
    oscillator_warp_tests::Fixture<O> f(48000, c.unison, c.active);
    f.set(O::kDistortionType, c.distortion);
    f.set(O::kDistortionAmount, .37f);
    f.set(O::kSpectralMorphType, c.morph);
    f.set(O::kSpectralMorphAmount, .21f);
    f.set(O::kUnisonFrameSpread, 12.0f);
    f.set(O::kStereoSpread, .6f);
    f.set(O::kPan, .3f);
    f.set(O::kAmplitude, .8f);
    const vital::poly_mask active_mask = vital::poly_float::equal(
        vital::poly_float(c.active & 1 ? 1 : 0, c.active & 1 ? 1 : 0, c.active & 2 ? 1 : 0, c.active & 2 ? 1 : 0), 1.0f);
    std::vector<float> result;
    for (int b = 0; b < 16; ++b) {
      if (c.resets && (b == 0 || b == 9)) {
        f.set(O::kNotePressed, b == 9 ? 2.0f : 1.0f);
        f.controls[O::kReset].output()->trigger(active_mask, vital::kVoiceOn, vital::poly_int(b == 9 ? 37 : 0));
      }
      f.process(128);
      f.controls[O::kReset].output()->clearTrigger();
      for (int i = 0; i < 128; ++i) for (int lane = 0; lane < vital::poly_float::kSize; ++lane) {
        result.push_back(f.oscillator->output(O::kRaw)->buffer[i][lane]);
        result.push_back(f.oscillator->output(O::kLevelled)->buffer[i][lane]);
      }
    }
    return result;
  }

  // Wavetable oscillators must sound exactly as before the Analog type existed.
  int wavetableReference(const char* path, bool write) {
    const auto cases = wavetableCases();
    if (write) {
      std::ofstream file(path, std::ios::binary);
      for (const auto& c : cases) {
        const auto samples = renderWavetableCase(c);
        file.write(reinterpret_cast<const char*>(samples.data()), std::streamsize(samples.size() * sizeof(float)));
      }
      std::cout << "Wrote " << cases.size() << " wavetable reference cases\n";
      return file ? 0 : 1;
    }

    std::ifstream file(path, std::ios::binary);
    expect(bool(file), "Wavetable audio reference exists");
    if (!file)
      return 1;
    int index = 0;
    double maximum_error = 0;
    for (const auto& c : cases) {
      const auto actual = renderWavetableCase(c);
      std::vector<float> expected(actual.size());
      file.read(reinterpret_cast<char*>(expected.data()), std::streamsize(expected.size() * sizeof(float)));
      expect(bool(file), "Complete wavetable audio reference");
      for (size_t i = 0; i < actual.size(); ++i)
        maximum_error = std::max(maximum_error, std::abs(double(actual[i]) - expected[i]));
      expect(actual == expected, "Wavetable type preserves the original audio exactly, case " + std::to_string(index));
      ++index;
    }
    std::cout << "Wavetable reference: " << index << " cases, maximum error=" << maximum_error << '\n';
    return failures ? 1 : 0;
  }
}
