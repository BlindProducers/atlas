// SPDX-License-Identifier: GPL-3.0-or-later
#include "piano_source.h"

#include "futils.h"
#include "synth_constants.h"

#include <algorithm>
#include <cmath>

namespace vital {
  static_assert(kMaxBufferSize * kMaxOversample <= pm::Instrument::kMaxBlock,
                "the piano instrument's block must hold a whole oversampled block");

  PianoSource::PianoSource(std::shared_ptr<pm::Instrument> instrument) :
      Processor(kNumInputs, kNumOutputs), instrument_(std::move(instrument)) { }

  void PianoSource::process(int num_samples) {
    if (num_samples <= 0)
      return;

    const int count = std::min(num_samples, kMaxBufferSize * kMaxOversample);
    const float sample_rate = getSampleRate();
    pm::Instrument& instrument = *instrument_;

    const poly_float note_input = input(kMidi)->at(0) + input(kTranspose)->at(0);
    const poly_float velocity_input = input(kVelocity)->at(0) * 127.0f;
    const poly_float lifted_input = input(kKeyLifted)->at(0);
    const bool hold_enabled = static_cast<int>(input(kAmpEnvMode)->at(0)[0] + 0.5f) == kPianoEnvelope;
    // A non-finite control reads as its default.
    const auto control = [this](int index, int lane, float low, float high, float default_value) {
      const float value = input(index)->at(0)[lane];
      return std::isfinite(value) ? std::max(low, std::min(high, value)) : default_value;
    };
    const int fade_samples = std::max(1, static_cast<int>(kVoiceKillTime * sample_rate));

    // A voice starting from silence, or one the handler has freed, drops its note.
    const poly_mask reset_mask = getResetMask(kReset);
    const poly_mask freed_mask = poly_float::lessThan(input(kActiveVoices)->at(0), 0.5f);
    const Output* events = input(kVoiceEvent)->source;
    // Ring modulation only: frequency modulation would need a rotation per sample per resonator.
    const SlotModulation::Block modulation = modulation_.resolve(*this, kDistortionType, kDistortionAmount,
                                                                 kSecondaryDistortionType,
                                                                 kSecondaryDistortionAmount, false);

    poly_float* raw = output(kRaw)->buffer;
    poly_float* hold = output(kHold)->buffer;
    float left[kMaxBufferSize * kMaxOversample];
    float right[kMaxBufferSize * kMaxOversample];
    poly_float hold_value = 0.0f;

    for (int voice = 0; voice < kVoicesPerProcessor; ++voice) {
      const int lane = 2 * voice;
      VoiceState& state = voices_[static_cast<size_t>(voice)];
      pm::Instrument::Lane& note = state.lane;
      if (reset_mask[lane] || freed_mask[lane]) {
        instrument.clear(note);
        state.killed = false;
        state.width = control(kWidth, lane, 0.0f, 2.0f, 1.0f);
      }

      int event = -1;
      int offset = count;
      if (events->trigger_mask[lane]) {
        event = static_cast<int>(events->trigger_value[lane]);
        offset = std::max(0, std::min(count - 1, static_cast<int>(events->trigger_offset[lane])));
      }

      const float action_noise = control(kActionNoise, lane, 0.0f, 2.0f, 1.0f);
      // The release velocity, 1 to 127; a note-off without one (0) is taken as 64, as the original does.
      const float lifted = lifted_input[lane];
      float lift = lifted > 0.0f ? std::max(0.0f, std::min(1.0f, lifted - 1.0f)) * 127.0f : 0.0f;
      if (lift <= 0.0f)
        lift = 64.0f;
      // The key is up while the sustain pedal keeps the voice on: the damper and the key's return
      // follow the key, as the pedal is the engine's own to handle.
      if (note.note >= 0 && note.keyDown && lifted > 0.0f && event != kVoiceOff && event != kVoiceKill)
        instrument.release(note, lift, action_noise, 0);

      // A note another voice still sounds (repeated under the pedal) changes hands before anything
      // renders: this voice plays the strings from where the old one stopped, normally the strike
      // itself. If the old voice had rendered past it (its strike was not announced), this voice
      // stays silent for the block and the hammer meets the strings at the block's end instead.
      int taken = -1;
      int strike_index = -1;
      if (event == kVoiceOn) {
        const int key = static_cast<int>(std::lround(note_input[lane]));
        strike_index = key - pm::kLowestMidi;
        if (strike_index >= 0 && strike_index < pm::kNumNotes)
          taken = instrument.takeOver(note, strike_index, fade_samples);
      }
      const bool rendered_by_other = taken > offset;
      std::fill(left, left + count, 0.0f);
      std::fill(right, right + count, 0.0f);
      if (!rendered_by_other) {
        const int from = std::max(0, taken);
        instrument.renderNote(note, left + from, right + from, from, offset - from, count);
      }
      if (event == kVoiceOn) {
        if (strike_index >= 0 && strike_index < pm::kNumNotes) {
          pm::Instrument::Blow blow;
          blow.dynamics = control(kDynamics, lane, 0.0f, 1.0f, 1.0f);
          // Hardness 0 to 200% scales the felt stiffness by a quarter to four times.
          blow.hardness = std::pow(4.0f, control(kHardness, lane, 0.0f, 2.0f, 1.0f) - 1.0f);
          blow.actionNoise = action_noise;
          instrument.strike(note, strike_index, velocity_input[lane], blow, fade_samples, offset);
        }
        else
          instrument.clear(note);
        state.killed = false;
      }
      else if (event == kVoiceOff && note.keyDown)
        instrument.release(note, lift, action_noise, offset);     // a key let go under the pedal was released already
      else if (event == kVoiceKill)
        state.killed = true;
      if (offset < count && !rendered_by_other)
        instrument.renderNote(note, left + offset, right + offset, offset, count - offset, count);

      // Stereo width: the sides scaled, ramped over the block.
      const float width = control(kWidth, lane, 0.0f, 2.0f, 1.0f);
      const float delta_width = (width - state.width) / count;
      float current_width = state.width;
      state.width = width;
      for (int i = 0; i < count; ++i) {
        current_width += delta_width;
        const float mid = 0.5f * (left[i] + right[i]), difference = 0.5f * (left[i] - right[i]) * current_width;
        raw[i].set(lane, kOutputScale * (mid + difference));
        raw[i].set(lane + 1, kOutputScale * (mid - difference));
      }

      if (hold_enabled && !state.killed && (note.sounding() || note.fade >= 0)) {
        hold_value.set(lane, 1.0f);
        hold_value.set(lane + 1, 1.0f);
      }
    }
    for (int i = 0; i < count; ++i)
      hold[i] = hold_value;
    modulation.applyRingMod(raw, count);

    // The body hears the slot's width and sympathetic resonance as the last voice read them.
    instrument.setWidth(control(kWidth, 0, 0.0f, 2.0f, 1.0f));
    instrument.setSympathetic(control(kSympathetic, 0, 0.0f, 2.0f, 1.0f));

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
