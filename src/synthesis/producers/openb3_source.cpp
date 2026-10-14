// SPDX-License-Identifier: GPL-3.0-or-later
#include "openb3_source.h"

#include "futils.h"
#include "synth_constants.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace vital {
  namespace openb3 = atlas::openb3;

  void OpenB3Clock::process(int num_samples) {
    if (num_samples <= 0)
      return;
    // The voices check in the keys still down at the end of each block.
    const int keys = organ_->takeCheckIns();
    organ_->beginBlock(num_samples, keys, keys);
  }

  OpenB3Source::OpenB3Source() : Processor(kNumInputs, kNumOutputs),
                                 organ_(std::make_shared<openb3::Organ>()) {
    // Built on the message thread with the slot's module, so a note-on never
    // computes the wiring.
    openb3::tables();
    organ_->prepare(openb3::kReferenceRate);
  }

  openb3::Registration OpenB3Source::registration(int lane) const {
    openb3::Registration result;
    for (int d = 0; d < openb3::kNumDrawbars; ++d) {
      const float value = input(kDrawbar + d)->at(0)[lane];
      result.drawbars[static_cast<size_t>(d)] = std::isfinite(value) ? std::clamp(value, 0.0f, 8.0f) : 0.0f;
    }
    result.percussion = input(kPercussion)->at(0)[0] >= 0.5f;
    result.percussion_soft = input(kPercussionVolume)->at(0)[0] >= 0.5f;
    result.percussion_fast = input(kPercussionDecay)->at(0)[0] < 0.5f;
    result.percussion_third = input(kPercussionHarmonic)->at(0)[0] >= 0.5f;
    result.temperament = std::clamp(static_cast<int>(std::lround(input(kTemperament)->at(0)[0])), 0,
                                    openb3::kNumTemperaments - 1);
    return result;
  }

  void OpenB3Source::release(openb3::Voice& voice, float level) {
    if (!voice.active() || !voice.held())
      return;
    // Each key's contacts open at their own random moments.
    uint32_t seed = static_cast<uint32_t>(voice.key() + 1) * 2246822519u + ++strike_count_;
    const int length = openb3::makeContactEnvelopes(release_envelopes_.data(), false, level, organ_->sampleRate(),
                                                    organ_->fragmentLength(), seed);
    voice.noteOff(release_envelopes_.data(), length);
  }

  void OpenB3Source::strike(VoiceState& state, float note, int offset, const openb3::Registration& registration,
                            float click, float release_click, const double* frequencies) {
    const int key = std::clamp(static_cast<int>(std::lround(note)) - openb3::kFirstKeyNote, 0, openb3::kNumKeys - 1);
    // The lane moves to another key: the old one's contacts open and it fades.
    if (state.voice.active() && state.voice.key() != key) {
      std::swap(state.voice, state.fading);
      release(state.fading, release_click);
    }
    // Each strike's contacts close at their own random moments.
    uint32_t seed = static_cast<uint32_t>(key + 1) * 2654435761u + ++strike_count_;
    const int length = openb3::makeContactEnvelopes(envelopes_.data(), true, click, organ_->sampleRate(),
                                                    organ_->fragmentLength(), seed);
    state.voice.setPitchRatio(std::exp2((note - (key + openb3::kFirstKeyNote)) / 12.0));
    state.voice.noteOn(key, organ_->time() + offset, registration, envelopes_.data(), length, *organ_, frequencies);
    organ_->keyPressed(offset);
    state.key_down = true;
  }

  void OpenB3Source::render(VoiceState& state, double* output, int start, int end, int count, float from, float to) {
    std::fill(output + start, output + end, 0.0);
    if (!state.voice.active() && !state.fading.active())
      return;
    const double inverse_count = 1.0 / count;
    const bool bending = from != to;
    for (int position = start; position < end;) {
      const int length = bending ? std::min(kPitchStep, end - position) : end - position;
      const double note = from + (static_cast<double>(to) - from) * (position * inverse_count);
      for (openb3::Voice* voice : { &state.voice, &state.fading }) {
        if (!voice->active())
          continue;
        const double ratio = std::exp2((note - (voice->key() + openb3::kFirstKeyNote)) / 12.0);
        voice->setPitchRatio(std::isfinite(ratio) ? ratio : 1.0);
        voice->render(*organ_, output, position, position + length);
      }
      position += length;
    }
  }

  void OpenB3Source::process(int num_samples) {
    if (num_samples <= 0)
      return;

    const int count = std::min(num_samples, kMaxBufferSize * kMaxOversample);
    // The voices' rate is the organ's (the mono router's clock does not know
    // the oversampling). The first voice to see a new rate restarts the organ;
    // every voice drops what it played at the old one.
    if (organ_->sampleRate() != getSampleRate())
      organ_->prepare(getSampleRate());
    if (organ_->sampleRate() != voice_rate_) {
      for (VoiceState& state : voices_) {
        state.voice.clear();
        state.fading.clear();
        state.key_down = false;
      }
      voice_rate_ = organ_->sampleRate();
    }

    const poly_float note_input = input(kMidi)->at(0) + input(kTranspose)->at(0) + input(kTune)->at(0);
    const bool hold_enabled = static_cast<int>(input(kAmpEnvMode)->at(0)[0] + 0.5f) == kOrganEnvelope;
    const double* frequencies = nullptr;

    const poly_mask reset_mask = getResetMask(kReset);
    const Output* events = input(kVoiceEvent)->source;
    // Ring modulation only: frequency modulation would need a rotation per sample per wheel.
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
      const openb3::Registration settings = registration(lane);
      frequencies = openb3::tables().frequencies[static_cast<size_t>(settings.temperament)].data();
      const float click = std::clamp(input(kKeyClick)->at(0)[lane], 0.0f, 1.0f);
      const float release_click = std::clamp(input(kReleaseClick)->at(0)[lane], 0.0f, 1.0f);
      organ_->setPercussion(settings.percussion_soft, settings.percussion_fast);
      if (reset_mask[lane]) {
        state.voice.clear();
        state.fading.clear();
        state.key_down = false;
        state.note = note;
      }
      state.voice.setRegistration(settings, frequencies);
      state.fading.setRegistration(settings, frequencies);
      // A key released under Envelope 1 keeps its contacts closed; once the
      // organ shapes the notes again they open, or the voice would be held
      // open for ever.
      if (hold_enabled && !state.key_down)
        release(state.voice, release_click);

      int event = -1;
      int offset = count;
      if (events->trigger_mask[lane]) {
        event = static_cast<int>(events->trigger_value[lane]);
        offset = std::max(0, std::min(count - 1, static_cast<int>(events->trigger_offset[lane])));
      }

      render(state, mono, 0, offset, count, state.note, note);
      if (event == kVoiceOn)
        strike(state, state.note + (note - state.note) * (static_cast<float>(offset) / count), offset, settings, click,
               release_click, frequencies);
      else if (event == kVoiceOff) {
        state.key_down = false;
        // With Envelope 1 the wheels sound on through its release.
        if (hold_enabled)
          release(state.voice, release_click);
      }
      else if (event == kVoiceKill) {
        state.key_down = false;
        release(state.voice, release_click);
      }
      render(state, mono, offset, count, count, state.note, note);
      state.note = note;

      if (state.key_down)
        organ_->checkIn();
      if (hold_enabled && (state.voice.active() || state.fading.active())) {
        hold_value.set(lane, 1.0f);
        hold_value.set(lane + 1, 1.0f);
      }

      for (int i = 0; i < count; ++i) {
        const float sample = static_cast<float>(mono[i] * kOutputScale);
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
