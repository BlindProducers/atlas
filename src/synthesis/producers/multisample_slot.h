// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "multisample_instrument.h"
#include "multisample_midi_state.h"
#include "multisample_player.h"

#include <array>
#include <cstdint>
#include <memory>
#include <random>
#include <vector>

namespace vital {
  // The multi-sample state of one oscillator slot, shared by every voice copy
  // of its module: the instrument, and the audio-thread state that spans
  // voices (round-robin counters, keyswitch, choke groups, CC-triggered tails).
  class MultiSampleSlot {
    public:
      static constexpr int kNumTailPlayers = 16;

      // Message thread, with processing paused. `keep_sounding` (the same
      // instrument with other control values) lets notes already playing go
      // on with the previous instrument, which the slot keeps until the next
      // change; otherwise they stop.
      void setInstrument(std::shared_ptr<const multisample::Instrument> instrument, bool keep_sounding = false);
      std::shared_ptr<const multisample::Instrument> instrument() const { return instrument_; }
      // The previous instrument kept for notes still playing it.
      std::shared_ptr<const multisample::Instrument> retired() const { return retired_; }
      void setMidiState(MultiSampleMidiState* state) { midi_state_ = state; }
      MultiSampleMidiState* midiState() const { return midi_state_; }

      // ---- Audio thread ----
      const multisample::Instrument* get() const { return instrument_.get(); }
      uint32_t generation() const { return generation_; }
      // Players started at an older generation point into a freed instrument.
      bool isLive(const MultiSamplePlayer& player) const { return isLiveGeneration(player.generation()); }
      bool isLiveGeneration(uint32_t generation) const { return generation >= oldest_live_; }

      // The current value of a CC: received from MIDI, else the instrument's set_cc default.
      float ccValue(int cc) const;
      bool passesGates(const multisample::Region& region) const;
      // Round robin: advances the region's counter and says whether it plays now.
      bool advanceSequence(int region_index);
      float nextRandom() { return distribution_(random_); }
      uint64_t nextSerial() { return ++serial_; }
      // A region of `group_slot` started; players with off_by on it stop.
      void triggerGroup(int group_slot, uint64_t serial);
      uint64_t groupStamp(int group_slot) const {
        return group_slot >= 0 && group_slot < static_cast<int>(group_stamps_.size()) ? group_stamps_[group_slot] : 0;
      }
      void noteOnKey(int key);
      int lastKeySwitch() const { return last_keyswitch_; }
      bool passesKeySwitch(const multisample::Region& region) const {
        return region.sw_last < 0 || region.sw_last == last_keyswitch_;
      }

      // CC-triggered regions (no key, e.g. pedal noises) play here, outside
      // the voices. Renders stereo into `left` / `right` at `sample_rate`;
      // `oversample` scales the host-rate event offsets.
      bool renderTails(float* left, float* right, int num_samples, float sample_rate, int oversample);
      bool tailsActive() const;
      void fadeOutAll(float sample_rate);
      void resetVoicesState();

    private:
      void startCcRegions(int cc, float previous, float value, int offset, float sample_rate);

      std::shared_ptr<const multisample::Instrument> instrument_;
      std::shared_ptr<const multisample::Instrument> retired_;
      uint32_t generation_ = 0;
      uint32_t oldest_live_ = 0;
      MultiSampleMidiState* midi_state_ = nullptr;

      std::vector<uint32_t> sequence_counters_;
      std::vector<uint64_t> group_stamps_;
      int last_keyswitch_ = -1;
      uint64_t serial_ = 0;
      std::mt19937 random_ { 0x5eed };
      std::uniform_real_distribution<float> distribution_ { 0.0f, 1.0f };
      std::array<MultiSamplePlayer, kNumTailPlayers> tails_;
  };
} // namespace vital
