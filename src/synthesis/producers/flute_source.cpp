// SPDX-License-Identifier: GPL-3.0-or-later
#include "flute_source.h"

#include "futils.h"
#include "synth_constants.h"

#include <algorithm>
#include <cmath>

namespace vital {
  static_assert(kMaxBufferSize * kMaxOversample <= atlas::flute::Instrument::kMaxBlock,
                "the flute's block must hold a whole oversampled block");

  FluteSource::FluteSource(std::shared_ptr<atlas::flute::Instrument> instrument) :
      Processor(kNumInputs, kNumOutputs), instrument_(std::move(instrument)) { }

  void FluteSource::process(int num_samples) {
    if (num_samples <= 0)
      return;

    const int count = std::min(num_samples, kMaxBufferSize * kMaxOversample);
    const atlas::flute::Instrument& instrument = *instrument_;
    const float* audio = instrument.output();
    const bool hold_enabled = static_cast<int>(input(kAmpEnvMode)->at(0)[0] + 0.5f) == kFluteEnvelope;
    const bool sounding = instrument.sounding();
    const poly_float note_count = input(kNoteCount)->at(0);
    const poly_float active = input(kActiveVoices)->at(0);
    const poly_mask reset_mask = getResetMask(kReset);
    const Output* events = input(kVoiceEvent)->source;
    // Ring modulation only: there is no pitch to modulate from outside the model.
    const SlotModulation::Block modulation = modulation_.resolve(*this, kDistortionType, kDistortionAmount,
                                                                 kSecondaryDistortionType,
                                                                 kSecondaryDistortionAmount, false);

    poly_float* raw = output(kRaw)->buffer;
    poly_float* hold = output(kHold)->buffer;
    poly_float hold_value = 0.0f;

    for (int voice = 0; voice < kVoicesPerProcessor; ++voice) {
      const int lane = 2 * voice;
      VoiceState& state = voices_[static_cast<size_t>(voice)];
      const int old_id = state.id;
      const int id = static_cast<int>(std::lround(note_count[lane]));
      state.id = id;
      if (reset_mask[lane])
        state.killed = false;
      if (events->trigger_mask[lane]) {
        const int event = static_cast<int>(events->trigger_value[lane]);
        if (event == kVoiceOn)
          state.killed = false;
        else if (event == kVoiceKill)
          state.killed = true;
      }

      // The samples this lane carries. The old lane keeps the audio up to the
      // handover; a voice retriggered with the flute keeps all of it.
      const bool playing = active[lane] >= 0.5f;
      const bool owner = playing && instrument.owner() == id;
      int from = 0, to = 0;
      if (instrument.handedOver()) {
        const int at = std::min(instrument.handoverSample(), count);
        const bool carried = instrument.previousOwner() == old_id;
        if (owner) {
          from = carried ? 0 : at;
          to = count;
        }
        else if (playing && carried)
          to = at;
      }
      else if (owner)
        to = count;

      for (int i = 0; i < from; ++i) {
        raw[i].set(lane, 0.0f);
        raw[i].set(lane + 1, 0.0f);
      }
      for (int i = from; i < to; ++i) {
        const float sample = kOutputScale * audio[i];
        raw[i].set(lane, sample);
        raw[i].set(lane + 1, sample);
      }
      for (int i = std::max(from, to); i < count; ++i) {
        raw[i].set(lane, 0.0f);
        raw[i].set(lane + 1, 0.0f);
      }

      if (hold_enabled && owner && sounding && !state.killed) {
        hold_value.set(lane, 1.0f);
        hold_value.set(lane + 1, 1.0f);
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
