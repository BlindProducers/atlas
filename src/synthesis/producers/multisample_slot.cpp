// SPDX-License-Identifier: GPL-3.0-or-later
#include "multisample_slot.h"

#include <algorithm>
#include <cmath>

namespace vital {
  void MultiSampleSlot::setInstrument(std::shared_ptr<const multisample::Instrument> instrument, bool keep_sounding) {
    const bool keep = keep_sounding && instrument_ != nullptr && instrument != nullptr;
    // Notes of the instrument before this one stop either way: it is freed.
    oldest_live_ = keep ? generation_ : generation_ + 1;
    retired_ = keep ? std::move(instrument_) : nullptr;
    instrument_ = std::move(instrument);
    generation_++;

    const size_t num_regions = instrument_ ? instrument_->regions.size() : 0;
    const size_t num_groups = instrument_ ? static_cast<size_t>(instrument_->num_group_slots) : 0;
    if (keep) {
      // The same file: round robins, chokes and the keyswitch carry on.
      if (sequence_counters_.size() != num_regions)
        sequence_counters_.assign(num_regions, 0);
      group_stamps_.resize(num_groups, 0);
    }
    else {
      sequence_counters_.assign(num_regions, 0);
      group_stamps_.assign(num_groups, 0);
      last_keyswitch_ = instrument_ ? instrument_->sw_default : -1;
    }
    for (MultiSamplePlayer& tail : tails_) {
      if (!isLive(tail))
        tail.clear();
    }
  }

  float MultiSampleSlot::ccValue(int cc) const {
    if (cc < 0 || cc >= multisample::kNumCcs)
      return 0.0f;
    if (midi_state_ != nullptr && midi_state_->received[static_cast<size_t>(cc)])
      return midi_state_->values[static_cast<size_t>(cc)];
    return instrument_ ? instrument_->cc_defaults[static_cast<size_t>(cc)] : 0.0f;
  }

  bool MultiSampleSlot::passesGates(const multisample::Region& region) const {
    for (const multisample::CcRange& gate : region.cc_gates) {
      const float value = ccValue(gate.cc);
      if (value < gate.low || value > gate.high)
        return false;
    }
    return true;
  }

  bool MultiSampleSlot::advanceSequence(int region_index) {
    if (region_index < 0 || region_index >= static_cast<int>(sequence_counters_.size()))
      return true;
    const multisample::Region& region = instrument_->regions[static_cast<size_t>(region_index)];
    if (region.seq_length <= 1)
      return true;
    const uint32_t count = sequence_counters_[static_cast<size_t>(region_index)]++;
    return static_cast<int>(count % static_cast<uint32_t>(region.seq_length)) == region.seq_position - 1;
  }

  void MultiSampleSlot::triggerGroup(int group_slot, uint64_t serial) {
    if (group_slot < 0 || group_slot >= static_cast<int>(group_stamps_.size()))
      return;
    group_stamps_[static_cast<size_t>(group_slot)] = std::max(group_stamps_[static_cast<size_t>(group_slot)], serial);
  }

  void MultiSampleSlot::noteOnKey(int key) {
    if (instrument_ && instrument_->isKeySwitch(key))
      last_keyswitch_ = key;
  }

  void MultiSampleSlot::startCcRegions(int cc, float previous, float value, int offset, float sample_rate) {
    const multisample::Instrument& instrument = *instrument_;
    const std::vector<int>& candidates = instrument.cc_triggered[static_cast<size_t>(cc)];
    if (candidates.empty())
      return;

    const float random = nextRandom();
    for (int index : candidates) {
      const multisample::Region& region = instrument.regions[static_cast<size_t>(index)];
      const bool now_in = value >= region.on_low && value <= region.on_high;
      const bool was_in = previous >= region.on_low && previous <= region.on_high;
      if (!now_in || was_in)
        continue;
      if (!passesGates(region) || !passesKeySwitch(region) || random < region.lorand || random >= region.hirand)
        continue;
      if (!advanceSequence(index))
        continue;

      const uint64_t serial = nextSerial();
      if (region.group_slot >= 0)
        triggerGroup(region.group_slot, serial);
      if (region.silent)
        continue;

      MultiSamplePlayer* target = nullptr;
      for (MultiSamplePlayer& tail : tails_) {
        if (!tail.active()) {
          target = &tail;
          break;
        }
      }
      if (target == nullptr) {
        target = &tails_[0];
        for (MultiSamplePlayer& tail : tails_) {
          if (tail.level() < target->level())
            target = &tail;
        }
      }

      MultiSamplePlayer::Start start;
      start.region = &region;
      // CC-triggered regions have no note: they play at full velocity from the key centre.
      start.velocity = 127;
      start.key = static_cast<int>(region.pitch_keycenter);
      start.random = random;
      start.offset = offset;
      start.serial = serial;
      start.generation = generation_;
      start.ignore_note_off = true;
      start.sample_rate = sample_rate;
      target->start(start, *this);
    }
  }

  bool MultiSampleSlot::renderTails(float* left, float* right, int num_samples, float sample_rate, int oversample) {
    const multisample::Instrument* instrument = instrument_.get();
    if (instrument == nullptr)
      return false;

    if (midi_state_ != nullptr && instrument->uses_cc_state) {
      for (int i = 0; i < midi_state_->num_events; ++i) {
        const MultiSampleMidiState::Event& event = midi_state_->events[static_cast<size_t>(i)];
        // Before any MIDI the CC sits at the instrument's default.
        const float previous = event.had_previous ? event.previous
                                                  : instrument->cc_defaults[static_cast<size_t>(event.cc)];
        startCcRegions(event.cc, previous, event.value, event.offset * std::max(1, oversample), sample_rate);
      }
    }

    bool any = false;
    for (MultiSamplePlayer& tail : tails_) {
      if (!tail.active())
        continue;
      tail.checkChoke(*this, sample_rate);
      tail.follow(*this);
      const multisample::Region& region = *tail.region();
      const double rate = std::exp2(tail.pitchOffset(region.pitch_keycenter) / 12.0) *
                          region.sample_rate / std::max(1.0f, sample_rate);
      float pan_left = 1.0f;
      float pan_right = 1.0f;
      MultiSamplePlayer::panGains(MultiSamplePlayer::ccPan(region, *this), pan_left, pan_right);
      tail.render(left, right, num_samples, rate, rate, MultiSamplePlayer::ccGain(region, *this), pan_left, pan_right);
      any = true;
    }
    return any;
  }

  bool MultiSampleSlot::tailsActive() const {
    for (const MultiSamplePlayer& tail : tails_) {
      if (tail.active())
        return true;
    }
    return false;
  }

  void MultiSampleSlot::fadeOutAll(float sample_rate) {
    for (MultiSamplePlayer& tail : tails_)
      tail.fadeOut(sample_rate);
  }

  void MultiSampleSlot::resetVoicesState() {
    for (MultiSamplePlayer& tail : tails_)
      tail.clear();
    std::fill(group_stamps_.begin(), group_stamps_.end(), 0);
  }
} // namespace vital
