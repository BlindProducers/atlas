// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "common.h"
#include <algorithm>
#include <array>
#include <bitset>

namespace vital {
  // One clock for the instrument, read by all main/bus FX instances. MIDI is
  // scheduled before rendering, so chord notes at one sample trigger only once.
  // Physical keys rearm the sequence; release tails can finish through the gate.
  class TranceGateClock {
    public:
      // VoiceHandler reserves channels 16..23 for Atlas arpeggiator destinations.
      static constexpr int kNoteChannels = kNumMidiChannels + 8;
      void setPlaying(bool playing) { playing_ = playing; }
      bool playing() const { return playing_; }
      void noteOn(int note, int channel, int sample) {
        if (note < 0 || note >= 128 || channel < 0 || channel >= kNoteChannels) return;
        if (held_.none() && trigger_count_ < static_cast<int>(triggers_.size()))
          triggers_[trigger_count_++] = std::max(0, sample);
        held_.set(channel * 128 + note);
      }
      void noteOff(int note, int channel) {
        if (note >= 0 && note < 128 && channel >= 0 && channel < kNoteChannels)
          held_.reset(channel * 128 + note);
      }
      void allNotesOff(int first = 0, int last = kNoteChannels - 1) {
        for (int channel = std::max(0, first); channel <= std::min(last, kNoteChannels - 1); ++channel)
          for (int note = 0; note < 128; ++note) held_.reset(channel * 128 + note);
      }
      void reset() {
        held_.reset(); trigger_count_ = 0; started_ = false; beat_ = 0.0;
        midi_beats_.fill(-1.0);
      }
      void prepare(int samples, double rate, double bps) {
        const double tick = bps / std::max(1.0, rate);
        int trigger = 0;
        for (int sample = 0; sample < samples; ++sample) {
          while (trigger < trigger_count_ && triggers_[trigger] <= sample) {
            started_ = true; beat_ = 0.0; ++trigger;
          }
          midi_beats_[sample] = started_ ? beat_ : -1.0;
          if (started_) beat_ += tick;
        }
        for (int i = trigger; i < trigger_count_; ++i)
          triggers_[i - trigger] = triggers_[i] - samples;
        trigger_count_ -= trigger;
      }
      double midiBeat(int host_sample) const { return midi_beats_[host_sample]; }
    private:
      bool playing_ = false, started_ = false;
      double beat_ = 0.0;
      std::bitset<128 * kNoteChannels> held_;
      std::array<int, kMaxBufferSize * 2> triggers_ {};
      int trigger_count_ = 0;
      std::array<double, kMaxBufferSize> midi_beats_ = [] {
        std::array<double, kMaxBufferSize> beats; beats.fill(-1.0); return beats;
      }();
  };
}
