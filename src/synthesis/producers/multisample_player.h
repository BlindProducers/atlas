// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "multisample_instrument.h"

#include <cstdint>

namespace vital {
  class MultiSampleSlot;

  // Plays one SFZ/SF2 region: sample position, loop, amplitude envelope and
  // choke state. Scalar; a voice runs several at once (layers, release tails).
  class MultiSamplePlayer {
    public:
      struct Start {
        const multisample::Region* region = nullptr;
        int velocity = 127;
        // The played key, for SF2 envelope key scaling.
        int key = 60;
        float random = 0.0f;
        // Samples into the current block before the region starts.
        int offset = 0;
        // Extra gain, e.g. the release-trigger decay.
        float gain = 1.0f;
        // Order of the start among every trigger of the slot, for chokes.
        uint64_t serial = 0;
        bool ignore_note_off = false;
        float sample_rate = 44100.0f;
      };

      void start(const Start& start, const MultiSampleSlot& slot);
      // Note-off at `offset` samples into the block being rendered next.
      void noteOff(int offset);
      // Stops with the region's off mode (another group cut it off).
      void choke(float sample_rate);
      // Fades out within a few milliseconds (voice stolen, all sounds off).
      void fadeOut(float sample_rate);
      void clear() { active_ = false; region_ = nullptr; }

      bool active() const { return active_; }
      bool releasing() const { return stage_ >= kRelease; }
      float level() const { return level_ * fade_; }
      uint64_t serial() const { return serial_; }
      const multisample::Region* region() const { return region_; }

      // Checks for a choke from the slot's group table.
      void checkChoke(const MultiSampleSlot& slot, float sample_rate);

      // Adds `num_samples` frames into `left` / `right`. The playback rate
      // moves linearly from `rate_start` to `rate_end` (frames of the region
      // per output sample); `gain` and the pan gains ramp from their last values.
      void render(float* left, float* right, int num_samples, double rate_start, double rate_end,
                  float gain, float left_gain, float right_gain);

      // The region's pitch, in semitones, relative to its key centre.
      float pitchOffset(float tuned_note) const;
      // Gain from CC modulation (amplitude_cc, volume_cc) and the pan from pan_cc.
      static float ccGain(const multisample::Region& region, const MultiSampleSlot& slot);
      static float ccPan(const multisample::Region& region, const MultiSampleSlot& slot);
      // Balance pan: the centre leaves both channels at unity.
      static void panGains(float pan, float& left, float& right);

      double lastRate() const { return last_rate_; }
      void setLastRate(double rate) { last_rate_ = rate; }

    private:
      enum Stage { kDelay, kAttack, kHold, kDecay, kSustain, kRelease, kDone };

      void enterRelease(float seconds);
      void advanceEnvelope();

      const multisample::Region* region_ = nullptr;
      bool active_ = false;
      bool ignore_note_off_ = false;
      bool looping_ = false;
      double position_ = 0.0;
      double last_rate_ = 0.0;
      int64_t end_ = 0;
      int64_t loop_start_ = 0;
      int64_t loop_end_ = 0;
      int start_delay_ = 0;
      int note_off_at_ = -1;

      Stage stage_ = kDone;
      float level_ = 0.0f;
      int stage_samples_ = 0;
      float attack_step_ = 0.0f;
      float decay_multiplier_ = 1.0f;
      float sustain_ = 1.0f;
      float release_multiplier_ = 1.0f;
      float release_seconds_ = 0.001f;
      float hold_samples_ = 0.0f;
      float sample_rate_ = 44100.0f;

      float fade_ = 1.0f;
      float fade_step_ = 0.0f;

      float note_gain_ = 1.0f;
      float last_gain_ = -1.0f;
      float last_left_ = 1.0f;
      float last_right_ = 1.0f;
      uint64_t serial_ = 0;
  };
} // namespace vital
