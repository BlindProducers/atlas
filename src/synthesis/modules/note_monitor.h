// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "common.h"
#include "value.h"
#include <algorithm>
#include <array>
#include <cstdint>

class Tuning;

namespace vital {
  // Held keys and pitch controls as the engine receives them, read by FX that
  // follow the played pitch. One monitor serves all main/bus FX instances. MIDI
  // is dispatched between audio sub-blocks, so a block sees one steady state.
  // Bends mirror VoiceHandler: a zoned wheel scaled by Pitch Bend Range and an
  // MPE per-channel bend over a fixed range, each read on the key's channel.
  class NoteMonitor {
    public:
      // VoiceHandler reserves channels 16..23 for Atlas arpeggiator destinations.
      static constexpr int kChannels = kNumMidiChannels + 8;
      // VoiceHandler::kLocalPitchBendRange; checked where both are visible.
      static constexpr float kLocalBendRange = 48.0f;
      enum Priority { kLastNote, kLowestNote };

      void noteOn(int note, int channel) {
        if (!valid(note, channel)) return;
        const int key = channel * 128 + note;
        stamps_[key] = ++stamp_;
        if (keys_[key] < 0xffff) ++keys_[key];
        seen_ = true;
        choose();
      }
      void noteOff(int note, int channel) {
        if (!valid(note, channel)) return;
        const int key = channel * 128 + note;
        if (keys_[key] > 0) --keys_[key];
        if (keys_[key] == 0 && !sustain_[channel]) stamps_[key] = 0;
        choose();
      }
      void sustainOn(int first, int last) {
        for (int channel = std::max(0, first); channel <= std::min(last, kChannels - 1); ++channel)
          sustain_[channel] = true;
      }
      void sustainOff(int first, int last) {
        for (int channel = std::max(0, first); channel <= std::min(last, kChannels - 1); ++channel) {
          sustain_[channel] = false;
          for (int key = channel * 128; key < (channel + 1) * 128; ++key)
            if (keys_[key] == 0) stamps_[key] = 0;
        }
        choose();
      }
      void allNotesOff(int first = 0, int last = kChannels - 1) {
        for (int channel = std::max(0, first); channel <= std::min(last, kChannels - 1); ++channel) {
          std::fill(stamps_.begin() + channel * 128, stamps_.begin() + (channel + 1) * 128, 0u);
          std::fill(keys_.begin() + channel * 128, keys_.begin() + (channel + 1) * 128, uint16_t(0));
        }
        choose();
      }
      // Releases every key but keeps the pedal and the last pitch, as the
      // voices do, so tails keep their tuning.
      void reset() { stamps_.fill(0); keys_.fill(0); choose(); }

      void setZonedPitchWheel(float value, int first, int last) {
        for (int channel = std::max(0, first); channel <= std::min(last, kNumMidiChannels - 1); ++channel)
          zoned_[channel] = std::clamp(value, -1.0f, 1.0f);
      }
      void setLocalPitchWheel(float value, int channel) {
        if (channel >= 0 && channel < kNumMidiChannels) local_[channel] = std::clamp(value, -1.0f, 1.0f);
      }
      void setPitchControls(const Value* bend_range, const Value* transpose, const Value* tune) {
        bend_range_ = bend_range; transpose_ = transpose; tune_ = tune;
      }
      void setTuning(const Tuning* tuning) { tuning_ = tuning; }

      bool seen() const { return seen_; }
      bool held() const { return held_; }
      // The chosen key while keys are held; afterwards, the last one chosen.
      int note(int priority) const { return priority == kLowestNote ? lowest_ % 128 : last_ % 128; }
      const Tuning* tuning() const { return tuning_; }
      // Bend on the chosen key's channel, plus Voice Transpose and Voice Tune.
      float offsetSemitones(int priority) const {
        const int channel = (priority == kLowestNote ? lowest_ : last_) / 128;
        const int control = channel >= kNumMidiChannels ? 0 : channel;
        float offset = zoned_[control] * (bend_range_ != nullptr ? bend_range_->value() : 2.0f) +
                       local_[control] * kLocalBendRange;
        if (transpose_ != nullptr) offset += transpose_->value();
        if (tune_ != nullptr) offset += tune_->value();
        return offset;
      }

    private:
      static bool valid(int note, int channel) {
        return note >= 0 && note < 128 && channel >= 0 && channel < kChannels;
      }
      void choose() {
        uint32_t last_stamp = 0, lowest_stamp = 0;
        int last = -1, lowest = -1;
        for (int key = 0; key < 128 * kChannels; ++key) {
          const uint32_t stamp = stamps_[key];
          if (stamp == 0) continue;
          if (stamp > last_stamp) { last_stamp = stamp; last = key; }
          const int note = key % 128;
          if (lowest < 0 || note < lowest % 128 || (note == lowest % 128 && stamp > lowest_stamp)) {
            lowest = key;
            lowest_stamp = stamp;
          }
        }
        held_ = last >= 0;
        if (held_) { last_ = last; lowest_ = lowest; }
      }

      std::array<uint32_t, 128 * kChannels> stamps_ {};
      std::array<uint16_t, 128 * kChannels> keys_ {};
      std::array<bool, kChannels> sustain_ {};
      std::array<float, kNumMidiChannels> zoned_ {}, local_ {};
      uint32_t stamp_ = 0;
      int last_ = 60, lowest_ = 60;  // keys: channel * 128 + note
      bool seen_ = false, held_ = false;
      const Value* bend_range_ = nullptr;
      const Value* transpose_ = nullptr;
      const Value* tune_ = nullptr;
      const Tuning* tuning_ = nullptr;
  };
}
