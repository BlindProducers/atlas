// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from OpenWurli 0.9.0, crates/openwurli-dsp/src/mlp_correction.rs and mlp_weights.rs
// (GPL-3.0-or-later).
//
// A tiny network (2 -> 16 -> 16 -> 11) run once per note-on, trained upstream on recordings of a
// 200A against the model, that corrects the frequencies and decay rates of modes 2-6. Only the
// H2 and H3 heads had training data; the untrained heads and the displacement-scale head emit
// identity. Outside the training range (MIDI 38-86) the corrections fade to identity over 12
// semitones.
#pragma once

namespace openwurli {
  struct MlpCorrections {
    static constexpr int kNumFreq = 5;
    static constexpr int kNumDecay = 5;

    // Frequency offsets for modes 2-6, cents.
    double freq_offsets_cents[kNumFreq];
    // Decay ratio offsets for modes 2-6 (> 1 = the model decays too fast); rates are divided by it.
    double decay_offsets[kNumDecay];
    // Displacement scale multiplier. Always 1: its head is not trained (and the voice ignores it).
    double ds_correction;

    static MlpCorrections identity();
    static MlpCorrections infer(int midi_note, double velocity);
  };
} // namespace openwurli
