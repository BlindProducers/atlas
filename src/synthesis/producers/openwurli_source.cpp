// SPDX-License-Identifier: GPL-3.0-or-later
#include "openwurli_source.h"

#include "futils.h"
#include "synth_constants.h"

#include <algorithm>
#include <cmath>

namespace vital {
  OpenWurliSource::OpenWurliSource() : Processor(kNumInputs, kNumOutputs) {
    // Built on the message thread with the slot's module, so a note-on never
    // computes the per-key tables.
    openwurli::prepareVoiceTables();
  }

  void OpenWurliSource::fadeOut(VoiceState& state) {
    if (!state.reed.active())
      return;
    // A fade still running is cut short by the newer one.
    state.fading = state.reed;
    state.fade_length = std::max(1, static_cast<int>(sample_rate_ * kFadeSeconds));
    state.fade_remaining = state.fade_length;
    state.reed.clear();
  }

  openwurli::VoiceSettings OpenWurliSource::voicing(int lane) const {
    // A non-finite control reads as its default.
    const auto value = [this, lane](int index, double low, double high, double default_value) {
      const double control = input(index)->at(0)[lane];
      return std::isfinite(control) ? std::max(low, std::min(high, control)) : default_value;
    };
    openwurli::VoiceSettings settings;
    // Bark 50% is OpenWurli's swing: 2^0.
    settings.swing_scale = std::exp2(2.0 * value(kBark, 0.0, 1.0, 0.5) - 1.0);
    settings.air_drag_scale = value(kDamping, 0.0, 2.5, 1.0);
    // Condition 100% spreads OpenWurli's +-3 cents to +-14.
    settings.detune_scale = 1.0 + value(kCondition, 0.0, 1.0, 0.0) * (11.0 / 3.0);
    // The decibels reach the top key, C7, 36 semitones above where the slope starts.
    settings.voicing_slope = settings.voicing_slope + value(kTrebleVoicing, -12.0, 12.0, 0.0) / 36.0;
    settings.register_trim = input(kRegisterBalance)->at(0)[lane] >= 0.5f;
    settings.c2_to_emitter = input(kPickupLowCut)->at(0)[lane] >= 0.5f;
    return settings;
  }

  void OpenWurliSource::strike(VoiceState& state, float note, float velocity, bool mlp,
                               const openwurli::VoiceSettings& settings) {
    fadeOut(state);
    state.key = std::max(openwurli::kMidiLo, std::min(openwurli::kMidiHi, static_cast<int>(std::lround(note))));
    // engine.rs seeds each strike's hammer noise from its key and a running count.
    const uint32_t seed = static_cast<uint32_t>(state.key) * 2654435761u + ++strike_count_;
    state.reed.noteOn(state.key, std::max(0.0f, std::min(1.0f, velocity)), sample_rate_, seed, mlp, settings);
  }

  void OpenWurliSource::render(VoiceState& state, double* output, int start, int end, int count,
                               float from, float to) {
    const double inverse_count = 1.0 / count;
    for (int position = start; position < end; position += kPitchStep) {
      const int length = std::min(kPitchStep, end - position);
      if (state.reed.active()) {
        const double note = from + (static_cast<double>(to) - from) * (position * inverse_count);
        const double ratio = std::exp2((note - state.key) / 12.0);
        state.reed.setPitchRatio(std::isfinite(ratio) ? ratio : 1.0);
        state.reed.render(output + position, length);
      }
      else
        std::fill(output + position, output + position + length, 0.0);

      if (state.fade_remaining > 0) {
        double faded[kPitchStep];
        state.fading.render(faded, length);
        const double inverse_length = 1.0 / state.fade_length;
        for (int i = 0; i < length; ++i)
          output[position + i] += faded[i] * (std::max(0, state.fade_remaining - i) * inverse_length);
        state.fade_remaining = std::max(0, state.fade_remaining - length);
        if (state.fade_remaining == 0)
          state.fading.clear();
      }
    }
  }

  void OpenWurliSource::process(int num_samples) {
    if (num_samples <= 0)
      return;

    const double sample_rate = getSampleRate();
    if (sample_rate != sample_rate_) {
      // A reed is built for one rate.
      for (VoiceState& state : voices_) {
        state.reed.clear();
        state.fading.clear();
        state.fade_remaining = 0;
      }
      sample_rate_ = sample_rate;
    }

    const int count = std::min(num_samples, kMaxBufferSize * kMaxOversample);
    const poly_float note_input = input(kMidi)->at(0) + input(kTranspose)->at(0) + input(kTune)->at(0);
    const poly_float velocity_input = input(kVelocity)->at(0);
    const bool mlp = input(kMlp)->at(0)[0] > 0.5f;
    const bool hold_enabled = static_cast<int>(input(kAmpEnvMode)->at(0)[0] + 0.5f) == kReedEnvelope;

    // A voice starting from silence drops whatever its lane played before.
    const poly_mask reset_mask = getResetMask(kReset);
    const Output* events = input(kVoiceEvent)->source;
    // Ring modulation only: frequency modulation would need a rotation per sample per mode.
    const SlotModulation::Block modulation = modulation_.resolve(*this, kDistortionType, kDistortionAmount,
                                                                 kSecondaryDistortionType,
                                                                 kSecondaryDistortionAmount, false);

    poly_float* raw = output(kRaw)->buffer;
    poly_float* hold = output(kHold)->buffer;
    double mono[kMaxBufferSize * kMaxOversample];
    poly_float hold_value = 0.0f;

    for (int voice = 0; voice < kVoicesPerProcessor; ++voice) {
      const int lane = 2 * voice;
      VoiceState& state = voices_[static_cast<size_t>(voice)];
      const float note = note_input[lane];
      if (reset_mask[lane]) {
        state.reed.clear();
        state.fading.clear();
        state.fade_remaining = 0;
        state.note = note;
      }

      int event = -1;
      int offset = count;
      if (events->trigger_mask[lane]) {
        event = static_cast<int>(events->trigger_value[lane]);
        offset = std::max(0, std::min(count - 1, static_cast<int>(events->trigger_offset[lane])));
      }

      render(state, mono, 0, offset, count, state.note, note);
      if (event == kVoiceOn) {
        strike(state, state.note + (note - state.note) * (static_cast<float>(offset) / count), velocity_input[lane], mlp,
               voicing(lane));
      }
      else if (event == kVoiceOff && state.reed.active())
        state.reed.noteOff();
      else if (event == kVoiceKill)
        fadeOut(state);
      render(state, mono, offset, count, count, state.note, note);
      state.note = note;

      if (state.reed.active() && state.reed.isSilent())
        state.reed.clear();
      if (hold_enabled && (state.reed.active() || state.fade_remaining > 0)) {
        hold_value.set(lane, 1.0f);
        hold_value.set(lane + 1, 1.0f);
      }

      for (int i = 0; i < count; ++i) {
        const float sample = static_cast<float>(mono[i] * kPickupToOutput);
        raw[i].set(lane, sample);
        raw[i].set(lane + 1, sample);
      }
    }
    for (int i = 0; i < count; ++i)
      hold[i] = hold_value;
    modulation.applyRingMod(raw, count);

    // Slot level and pan, as the Sample type applies them.
    const poly_float target_pan = futils::panAmplitude(utils::clamp(input(kPan)->at(0), -1.0f, 1.0f));
    if (!pan_initialized_) {
      pan_amplitude_ = target_pan;
      pan_initialized_ = true;
    }
    poly_float current_pan = utils::maskLoad(pan_amplitude_, target_pan, reset_mask);
    const poly_float delta_pan = (target_pan - current_pan) * (1.0f / count);
    pan_amplitude_ = target_pan;

    const Output* level_source = input(kLevel)->source;
    const bool audio_rate_level = level_source->buffer_size > 1;
    poly_float* levelled = output(kLevelled)->buffer;
    for (int i = 0; i < count; ++i) {
      current_pan += delta_pan;
      const poly_float level = utils::clamp(level_source->buffer[audio_rate_level ? i : 0], 0.0f, kMaxAmplitude);
      levelled[i] = current_pan * level * level * raw[i];
    }
  }
} // namespace vital
