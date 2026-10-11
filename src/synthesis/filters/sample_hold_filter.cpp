// SPDX-License-Identifier: GPL-3.0-or-later
#include "sample_hold_filter.h"
#include "utils.h"
#include <algorithm>
#include <cmath>

namespace vital {
  // A phase of 1 captures a new value on the next sample.
  SampleHoldFilter::SampleHoldFilter() : Processor(kNumInputs, 1), phase_(1.0f), held_(0.0f), previous_(0.0f) { }

  void SampleHoldFilter::reset(poly_mask reset_mask) {
    phase_ = utils::maskLoad(phase_, 1.0f, reset_mask);
    held_ = utils::maskLoad(held_, 0.0f, reset_mask);
    previous_ = utils::maskLoad(previous_, 0.0f, reset_mask);
  }

  mono_float SampleHoldFilter::crushBits(mono_float blend) {
    const mono_float amount = utils::clamp(blend * 0.5f, 0.0f, 1.0f);
    if (amount <= 0.0f)
      return 0.0f;
    return kMaxCrushBits - amount * (kMaxCrushBits - kMinCrushBits);
  }

  mono_float SampleHoldFilter::quantize(mono_float value, mono_float bits) {
    if (bits <= 0.0f)
      return value;
    const mono_float levels = std::exp2(bits - 1.0f);
    return std::round(value * levels) / levels;
  }

  void SampleHoldFilter::process(int num_samples) {
    const int style = utils::iclamp(static_cast<int>(std::round(input(kStyle)->at(0)[0])), 0, kNumStyles - 1);
    const Output* resets = input(kReset)->source;
    const poly_mask reset_mask = getResetMask(kReset);
    const mono_float inverse_rate = 1.0f / getSampleRate();
    const poly_float* audio_in = input(kAudio)->source->buffer;
    const poly_float* midi_cutoff = input(kMidiCutoff)->source->buffer;
    const bool audio_rate_cutoff = input(kMidiCutoff)->source->buffer_size > 1;
    const poly_float crush = input(kCrush)->at(0);
    poly_float* audio_out = output()->buffer;

    for (int lane = 0; lane < poly_float::kSize; ++lane) {
      mono_float phase = phase_[lane];
      mono_float held = held_[lane];
      mono_float previous = previous_[lane];
      const mono_float bits = crushBits(crush[lane]);
      const int reset_offset = reset_mask[lane] ? resets->trigger_offset[lane] : -1;

      for (int i = 0; i < num_samples; ++i) {
        // New voices capture their first input sample exactly on the note offset.
        if (i == reset_offset) {
          phase = 1.0f;
          held = previous = 0.0f;
        }
        const mono_float midi = midi_cutoff[audio_rate_cutoff ? i : 0][lane];
        const mono_float frequency = utils::midiNoteToFrequency(utils::clamp(midi, -60.0f, 150.0f));
        const mono_float phase_delta = std::min(frequency * inverse_rate, 1.0f);

        if (phase >= 1.0f) {
          phase -= std::floor(phase);
          previous = held;
          held = quantize(audio_in[i][lane], bits);
        }

        mono_float value = held;
        if (style == kSmooth)
          value = previous + (held - previous) * phase;
        else if (style == kInverted)
          value = -held;
        audio_out[i].set(lane, value);
        phase += phase_delta;
      }
      phase_.set(lane, phase);
      held_.set(lane, held);
      previous_.set(lane, previous);
    }
  }
} // namespace vital
