// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from OpenWurli 0.9.0, crates/openwurli-dsp/src/mlp_correction.rs (GPL-3.0-or-later).
#include "wurli_mlp.h"

#include "wurli_math.h"

namespace openwurli {
  namespace {
#include "wurli_mlp_weights.inc"

    constexpr double kMidiMin = 21.0;
    constexpr double kMidiMax = 108.0;
    // Only H2 and H3 cleared the training data filters; heads from here on emit identity rather
    // than whatever their untrained weights and the clamps produce.
    constexpr int kNumTrainedHarmonics = 2;
    // The ds head is not trained since the true-pitch fix upstream.
    constexpr bool kDsTrained = false;
    constexpr int kDsIndex = 10;
    // Training range (D2-D6) and the fade to identity outside it.
    constexpr double kTrainMidiLo = 38.0;
    constexpr double kTrainMidiHi = 86.0;
    constexpr double kFadeSemitones = 12.0;

    static_assert(kNumOutputs == MlpCorrections::kNumFreq + MlpCorrections::kNumDecay + 1, "MLP outputs");
  } // namespace

  MlpCorrections MlpCorrections::identity() {
    MlpCorrections corrections;
    for (int i = 0; i < kNumFreq; ++i)
      corrections.freq_offsets_cents[i] = 0.0;
    for (int i = 0; i < kNumDecay; ++i)
      corrections.decay_offsets[i] = 1.0;
    corrections.ds_correction = 1.0;
    return corrections;
  }

  MlpCorrections MlpCorrections::infer(int midi_note, double velocity) {
    using math::clamp;
    const double midi = static_cast<double>(midi_note);

    double fade = 1.0;
    if (midi < kTrainMidiLo)
      fade = clamp((midi - (kTrainMidiLo - kFadeSemitones)) / kFadeSemitones, 0.0, 1.0);
    else if (midi > kTrainMidiHi)
      fade = clamp(((kTrainMidiHi + kFadeSemitones) - midi) / kFadeSemitones, 0.0, 1.0);
    if (fade <= 0.0)
      return identity();

    const double input[2] = { clamp((midi - kMidiMin) / (kMidiMax - kMidiMin), 0.0, 1.0),
                              clamp(velocity, 0.0, 1.0) };

    double h1[kHiddenSize];
    for (int i = 0; i < kHiddenSize; ++i) {
      double sum = kB1[i];
      for (int j = 0; j < 2; ++j)
        sum += kW1[i][j] * input[j];
      h1[i] = sum > 0.0 ? sum : 0.0;
    }

    double h2[kHiddenSize];
    for (int i = 0; i < kHiddenSize; ++i) {
      double sum = kB2[i];
      for (int j = 0; j < kHiddenSize; ++j)
        sum += kW2[i][j] * h1[j];
      h2[i] = sum > 0.0 ? sum : 0.0;
    }

    double raw[kNumOutputs];
    for (int i = 0; i < kNumOutputs; ++i) {
      double sum = kB3[i];
      for (int j = 0; j < kHiddenSize; ++j)
        sum += kW3[i][j] * h2[j];
      raw[i] = sum * kTargetStds[i] + kTargetMeans[i];
    }

    MlpCorrections corrections = identity();
    for (int h = 0; h < kNumFreq && h < kNumTrainedHarmonics; ++h)
      corrections.freq_offsets_cents[h] = clamp(raw[h] * fade, -100.0, 100.0);
    for (int h = 0; h < kNumDecay && h < kNumTrainedHarmonics; ++h) {
      const double raw_decay = clamp(raw[kNumFreq + h], 0.3, 3.0);
      corrections.decay_offsets[h] = 1.0 + (raw_decay - 1.0) * fade;
    }
    if (kDsTrained) {
      const double raw_ds = clamp(raw[kDsIndex], 0.7, 1.05);
      corrections.ds_correction = 1.0 + (raw_ds - 1.0) * fade;
    }
    return corrections;
  }
} // namespace openwurli
