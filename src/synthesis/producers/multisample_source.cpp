// SPDX-License-Identifier: GPL-3.0-or-later
#include "multisample_source.h"

#include "futils.h"
#include "multisample_slot.h"
#include "synth_constants.h"

#include <algorithm>
#include <cmath>

namespace vital {
  namespace {
    constexpr int kMultiSampleScratchSize = kMaxBufferSize * kMaxOversample;
    constexpr mono_float kMultiSampleMaxAmplitude = 1.41421356237f;
  } // namespace

  MultiSampleSource::MultiSampleSource(std::shared_ptr<MultiSampleSlot> slot) :
      Processor(kNumInputs, kNumOutputs), slot_(std::move(slot)) { }

  MultiSamplePlayer& MultiSampleSource::freePlayer(int voice) {
    auto& players = voices_[static_cast<size_t>(voice)].players;
    for (MultiSamplePlayer& player : players) {
      if (!player.active())
        return player;
    }
    // Steal the quietest, preferring one already releasing.
    MultiSamplePlayer* quietest = &players[0];
    for (MultiSamplePlayer& player : players) {
      const bool better_stage = player.releasing() && !quietest->releasing();
      const bool same_stage = player.releasing() == quietest->releasing();
      if (better_stage || (same_stage && player.level() < quietest->level()))
        quietest = &player;
    }
    quietest->clear();
    return *quietest;
  }

  void MultiSampleSource::startRegion(int voice, int region_index, int velocity, float random, int offset,
                                      float gain, bool ignore_note_off, uint64_t serial) {
    const multisample::Region& region = slot_->get()->regions[static_cast<size_t>(region_index)];
    if (region.group_slot >= 0)
      slot_->triggerGroup(region.group_slot, serial);
    if (region.silent)
      return;

    MultiSamplePlayer::Start start;
    start.region = &region;
    start.velocity = velocity;
    start.key = voices_[static_cast<size_t>(voice)].key;
    start.random = random;
    start.offset = offset;
    start.gain = gain;
    start.serial = serial;
    start.generation = slot_->generation();
    start.ignore_note_off = ignore_note_off;
    start.sample_rate = static_cast<float>(getSampleRate());
    freePlayer(voice).start(start, *slot_);
  }

  void MultiSampleSource::noteOn(int voice, int key, int velocity, float tuned_note, int offset) {
    VoiceState& state = voices_[static_cast<size_t>(voice)];
    // Notes this voice played before (legato, a reused voice) release normally.
    for (MultiSamplePlayer& player : state.players)
      player.noteOff(offset);

    state.key = key;
    state.velocity = velocity;
    state.tuned_note = tuned_note;
    state.held_samples = -offset;
    state.held = true;

    const multisample::Instrument* instrument = slot_->get();
    if (instrument == nullptr || key < 0 || key >= multisample::kNumKeys)
      return;
    slot_->noteOnKey(key);

    const float random = slot_->nextRandom();
    // One serial per note-on: the regions it starts together (a stereo pair,
    // the mics of a multi-mic kit) never choke each other.
    const uint64_t serial = slot_->nextSerial();
    for (int index : instrument->attack_by_key[static_cast<size_t>(key)]) {
      const multisample::Region& region = instrument->regions[static_cast<size_t>(index)];
      if (velocity < region.lovel || velocity > region.hivel || !slot_->passesKeySwitch(region))
        continue;
      // Round-robin counters advance whatever the CC state, so layers stay in step.
      if (!slot_->advanceSequence(index))
        continue;
      if (!slot_->passesGates(region) || random < region.lorand || random >= region.hirand)
        continue;
      startRegion(voice, index, velocity, random, offset, 1.0f, false, serial);
    }
  }

  void MultiSampleSource::noteOff(int voice, int offset) {
    VoiceState& state = voices_[static_cast<size_t>(voice)];
    if (!state.held)
      return;
    state.held = false;
    for (MultiSamplePlayer& player : state.players)
      player.noteOff(offset);

    const multisample::Instrument* instrument = slot_->get();
    if (instrument == nullptr || state.key < 0 || state.key >= multisample::kNumKeys)
      return;
    const std::vector<int>& releases = instrument->release_by_key[static_cast<size_t>(state.key)];
    if (releases.empty())
      return;

    const float held_seconds = static_cast<float>(std::max<int64_t>(0, state.held_samples + offset)) /
                               static_cast<float>(getSampleRate());
    const float random = slot_->nextRandom();
    const uint64_t serial = slot_->nextSerial();
    for (int index : releases) {
      const multisample::Region& region = instrument->regions[static_cast<size_t>(index)];
      if (state.velocity < region.lovel || state.velocity > region.hivel || !slot_->passesKeySwitch(region))
        continue;
      if (!slot_->advanceSequence(index))
        continue;
      if (!slot_->passesGates(region) || random < region.lorand || random >= region.hirand)
        continue;
      const float gain = region.rt_decay > 0.0f ? std::pow(10.0f, -region.rt_decay * held_seconds / 20.0f) : 1.0f;
      startRegion(voice, index, state.velocity, random, offset, gain, true, serial);
    }
  }

  void MultiSampleSource::process(int num_samples) {
    poly_float* raw = output(kRaw)->buffer;
    poly_float* hold = output(kHold)->buffer;
    utils::zeroBuffer(raw, num_samples);
    const float sample_rate = static_cast<float>(getSampleRate());

    if (slot_ != nullptr && slot_->generation() != generation_) {
      // The instrument changed. Players of a freed one stop; after a control
      // change the previous one is kept, and its notes play on.
      // A held note of a stopped instrument starts no release samples.
      const bool replaced = !slot_->isLiveGeneration(generation_);
      for (VoiceState& state : voices_) {
        for (MultiSamplePlayer& player : state.players) {
          if (!slot_->isLive(player))
            player.clear();
        }
        if (replaced)
          state.held = false;
      }
      generation_ = slot_->generation();
    }

    // A voice starting from silence drops whatever its lane played before.
    const poly_mask reset_mask = getResetMask(kReset);
    for (int voice = 0; voice < kVoicesPerProcessor; ++voice) {
      if (!reset_mask[2 * voice])
        continue;
      for (MultiSamplePlayer& player : voices_[static_cast<size_t>(voice)].players)
        player.clear();
      voices_[static_cast<size_t>(voice)].held = false;
    }

    const Output* events = input(kVoiceEvent)->source;
    const poly_float key_input = input(kMidiKey)->at(0);
    const poly_float velocity_input = input(kVelocity)->at(0);
    const poly_float note_input = input(kNote)->at(0);
    const poly_float midi_input = input(kMidi)->at(0);
    const poly_float shift_input = input(kTranspose)->at(0) + input(kTune)->at(0);
    const bool hold_enabled = static_cast<int>(input(kAmpEnvMode)->at(0)[0] + 0.5f) == kInstrumentEnvelope;

    for (int voice = 0; voice < kVoicesPerProcessor; ++voice) {
      const int lane = 2 * voice;
      if (slot_ == nullptr || !events->trigger_mask[lane])
        continue;
      const int event = static_cast<int>(events->trigger_value[lane]);
      const int offset = std::max(0, std::min(num_samples - 1, static_cast<int>(events->trigger_offset[lane])));
      if (event == kVoiceOn) {
        const int key = static_cast<int>(std::lround(key_input[lane]));
        const int velocity = std::max(1, std::min(127, static_cast<int>(std::lround(velocity_input[lane] * 127.0f))));
        noteOn(voice, key, velocity, note_input[lane], offset);
      }
      else if (event == kVoiceOff)
        noteOff(voice, offset);
      else if (event == kVoiceKill) {
        voices_[static_cast<size_t>(voice)].held = false;
        for (MultiSamplePlayer& player : voices_[static_cast<size_t>(voice)].players)
          player.fadeOut(sample_rate);
      }
    }

    float left[kMultiSampleScratchSize];
    float right[kMultiSampleScratchSize];
    poly_float hold_value = 0.0f;
    for (int voice = 0; voice < kVoicesPerProcessor; ++voice) {
      const int lane = 2 * voice;
      VoiceState& state = voices_[static_cast<size_t>(voice)];
      bool sounding = false;
      for (int start = 0; start < num_samples && slot_ != nullptr; start += kMultiSampleScratchSize) {
        const int count = std::min(kMultiSampleScratchSize, num_samples - start);
        std::fill(left, left + count, 0.0f);
        std::fill(right, right + count, 0.0f);
        // Pitch bend, portamento and voice tuning are the bent note's distance from the tuned note.
        const float bend = midi_input[lane] - note_input[lane];
        bool rendered = false;
        for (MultiSamplePlayer& player : state.players) {
          if (!player.active())
            continue;
          player.checkChoke(*slot_, sample_rate);
          player.follow(*slot_);
          const multisample::Region& region = *player.region();
          const double semitones = player.pitchOffset(state.tuned_note) + bend + shift_input[lane];
          const double rate = std::exp2(std::max(-96.0, std::min(96.0, semitones)) / 12.0) *
                              region.sample_rate / sample_rate;
          float pan_left = 1.0f;
          float pan_right = 1.0f;
          MultiSamplePlayer::panGains(MultiSamplePlayer::ccPan(region, *slot_), pan_left, pan_right);
          player.render(left, right, count, player.lastRate() > 0.0 ? player.lastRate() : rate, rate,
                        MultiSamplePlayer::ccGain(region, *slot_), pan_left, pan_right);
          rendered = true;
          sounding = sounding || player.active();
        }
        if (!rendered)
          continue;
        for (int i = 0; i < count; ++i) {
          raw[start + i].set(lane, left[i]);
          raw[start + i].set(lane + 1, right[i]);
        }
      }
      state.held_samples += num_samples;
      if (sounding && hold_enabled) {
        hold_value.set(lane, 1.0f);
        hold_value.set(lane + 1, 1.0f);
      }
    }
    for (int i = 0; i < num_samples; ++i)
      hold[i] = hold_value;

    // Slot level and pan, as the Sample type applies them.
    const poly_float target_pan = futils::panAmplitude(utils::clamp(input(kPan)->at(0), -1.0f, 1.0f));
    if (!pan_initialized_) {
      pan_amplitude_ = target_pan;
      pan_initialized_ = true;
    }
    poly_float current_pan = utils::maskLoad(pan_amplitude_, target_pan, getResetMask(kReset));
    const poly_float delta_pan = (target_pan - current_pan) * (1.0f / std::max(1, num_samples));
    pan_amplitude_ = target_pan;

    const poly_float* level_input = input(kLevel)->source->buffer;
    poly_float* levelled = output(kLevelled)->buffer;
    const poly_float zero = 0.0f;
    const poly_float max = kMultiSampleMaxAmplitude;
    for (int i = 0; i < num_samples; ++i) {
      current_pan += delta_pan;
      const poly_float level = utils::clamp(level_input[i], zero, max);
      levelled[i] = current_pan * level * level * raw[i];
    }
  }
} // namespace vital
