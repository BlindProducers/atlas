// SPDX-License-Identifier: GPL-3.0-or-later
#include "ring_mod_filter.h"
#include "utils.h"
#include <algorithm>
#include <cmath>

namespace vital {
  namespace {
    // Residual that turns a naive unit-height downward step at phase 0 into a
    // band-limited one. Multiply by half the step size.
    force_inline mono_float polyBlep(mono_float phase, mono_float phase_delta) {
      if (phase < phase_delta) {
        const mono_float t = phase / phase_delta;
        return t + t - t * t - 1.0f;
      }
      if (phase > 1.0f - phase_delta) {
        const mono_float t = (phase - 1.0f) / phase_delta;
        return t * t + t + t + 1.0f;
      }
      return 0.0f;
    }

    force_inline mono_float wrap(mono_float phase) { return phase - std::floor(phase); }
  } // namespace

  RingModFilter::RingModFilter() : Processor(kNumInputs, 1), phase_(0.0f) { }

  void RingModFilter::reset(poly_mask reset_mask) {
    phase_ = utils::maskLoad(phase_, 0.0f, reset_mask);
  }

  mono_float RingModFilter::carrier(int waveform, mono_float phase, mono_float phase_delta) {
    switch (waveform) {
      case kSquare: {
        // +1 on the first half cycle, -1 on the second: rises at 0, falls at 0.5.
        const mono_float naive = phase < 0.5f ? 1.0f : -1.0f;
        return naive + polyBlep(phase, phase_delta) - polyBlep(wrap(phase + 0.5f), phase_delta);
      }
      case kSaw: {
        // Rising saw through zero at phase 0, resetting at phase 0.5.
        const mono_float shifted = wrap(phase + 0.5f);
        return 2.0f * shifted - 1.0f - polyBlep(shifted, phase_delta);
      }
      case kTriangle:
        return 1.0f - 4.0f * std::abs(wrap(phase + 0.25f) - 0.5f);
      case kSine:
      default:
        return std::sin(2.0f * kPi * phase);
    }
  }

  void RingModFilter::process(int num_samples) {
    const int waveform = utils::iclamp(static_cast<int>(std::round(input(kStyle)->at(0)[0])), 0, kNumWaveforms - 1);
    const Output* resets = input(kReset)->source;
    const poly_mask reset_mask = getResetMask(kReset);
    const mono_float inverse_rate = 1.0f / getSampleRate();
    const poly_float* audio_in = input(kAudio)->source->buffer;
    const poly_float* midi_cutoff = input(kMidiCutoff)->source->buffer;
    const bool audio_rate_cutoff = input(kMidiCutoff)->source->buffer_size > 1;
    poly_float* audio_out = output()->buffer;

    for (int lane = 0; lane < poly_float::kSize; ++lane) {
      mono_float phase = phase_[lane];
      const int reset_offset = reset_mask[lane] ? resets->trigger_offset[lane] : -1;
      for (int i = 0; i < num_samples; ++i) {
        // New voices start the carrier at phase 0 exactly on their note offset.
        if (i == reset_offset)
          phase = 0.0f;
        const mono_float midi = midi_cutoff[audio_rate_cutoff ? i : 0][lane];
        const mono_float frequency = utils::midiNoteToFrequency(utils::clamp(midi, -60.0f, 150.0f));
        const mono_float phase_delta = std::min(frequency * inverse_rate, 0.499f);
        audio_out[i].set(lane, audio_in[i][lane] * carrier(waveform, phase, phase_delta));
        phase = wrap(phase + phase_delta);
      }
      phase_.set(lane, phase);
    }
  }
} // namespace vital
