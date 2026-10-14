// SPDX-License-Identifier: GPL-3.0-or-later
// OpenB3's tonewheel organ (setBfree's Beatrix tonegen.c) split into what the
// instrument shares and what each key does:
//
// - Organ: what every key of one instrument shares. The tonewheels turn
//   continuously on one clock, so two keys that use the same wheel hear it in
//   phase. Key compression lowers the output as more keys go down, and the
//   percussion envelope is single-trigger: it starts when a key goes down
//   with no other key held and runs on through legato playing.
// - Voice: one key. Its contacts connect each wheel to the drawbar buses at
//   the levels tonegen.c's wiring model gives (tapering, foldback and
//   crosstalk), with the drawbars' gains. Closing them makes the key click
//   (tonegen.c's ENV_CLICK); opening them fades the wheels out linearly
//   (ENV_LINEAR). Percussion taps the 4' or 2 2/3' bus through tonegen.c's
//   high pass.
//
// tonegen.c works in 64-sample fragments at the host rate. Here a fragment is
// 64 samples at 48 kHz, so the click, the release and the key compression
// take the same time at any rate.
#pragma once

#include "tonewheel_tables.h"

#include <array>
#include <cstdint>
#include <vector>

namespace atlas::openb3 {
  constexpr double kReferenceRate = 48000.0;
  constexpr int kReferenceFragment = 64;
  // tonegen.c keeps 8 attack envelopes and gives the i-th sounding wheel the
  // (i & 7)-th.
  constexpr int kNumClickEnvelopes = 8;
  // More wheels than any key reaches through its contacts and crosstalk.
  constexpr int kMaxWheelsPerKey = 32;
  // The longest fragment: 64 samples at 48 kHz up to 192 kHz. Above that a
  // fragment stays 256 samples long.
  constexpr int kMaxFragment = 256;

  // The fragment length at a rate: 64 samples at 48 kHz, at most kMaxFragment.
  int fragmentLength(double sample_rate);

  // The organ's settings, read every block.
  struct Registration {
    // 0 (in) to 8 (fully out); modulation may leave them between stops.
    std::array<float, kNumDrawbars> drawbars { { 8.0f, 8.0f, 6.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f } };
    bool percussion = false;
    bool percussion_soft = false;
    bool percussion_fast = true;
    bool percussion_third = true;
    int temperament = kGear60;
  };

  // The longest contact envelope: chatter at Key Click 100% lasts up to about
  // 10 ms, as long as 512 samples allow.
  constexpr int kMaxContactLength = 512;

  // Fills `envelopes` (kNumClickEnvelopes shapes, each the returned length)
  // with the contacts' shapes as a key closes (`closing`) or opens: each
  // envelope runs from 0 to 1 and a wheel moves through its own one.
  // `amount` is Key Click or Release Click:
  // - 50% is tonegen.c's ENV_CLICK model as setBfree sets it: silence (or
  //   full level) until a random contact time, a burst of random contact
  //   noise (osc.attack.click.level 0.5 closing, osc.release.click.level 0.25
  //   opening) of random length (8 to 40 samples at 22.05 kHz closing, 8 to 39
  //   at 48 kHz opening, within a fragment), then the other state, smoothed by
  //   its two-point filter.
  // - Toward 0% each contact still closes at its own moment, but through a
  //   smooth raised-cosine ramp that widens to four fragments (about 5 ms) at
  //   0%, with less noise, so it clicks less and at 0% not at all.
  // - Toward 100% the contacts wear: the noise deepens to full, the bursts
  //   grow up to 8 times longer (to about 10 ms) and the smoothing goes.
  // `state` is a xorshift seed.
  int makeContactEnvelopes(float* envelopes, bool closing, float amount, double sample_rate, int fragment,
                           uint32_t& state);

  class Organ {
   public:
    void prepare(double sample_rate);
    void reset();

    // Once per block, before any voice renders it. `keys_before`: keys down
    // at the end of the previous block; `keys_now`: keys down for this block.
    void beginBlock(int samples, int keys_before, int keys_now);
    // A key went down at `offset` in this block. With no key down before the
    // block, this (or an earlier key in the same block) starts the percussion.
    void keyPressed(int offset);
    void setPercussion(bool soft, bool fast);

    double sampleRate() const { return sample_rate_; }
    int fragmentLength() const { return fragment_; }
    // The organ clock at the block's first sample, in samples.
    double time() const { return time_; }
    int blockSamples() const { return samples_; }
    const float* keyCompression() const { return key_compression_.data(); }
    const float* percussionEnvelope() const { return percussion_.data(); }

    // For hosts that only learn which keys are down from the voices: each key
    // still down at the end of a block checks in; the next block reads the
    // count.
    void checkIn() { ++checked_in_; }
    int takeCheckIns() {
      const int count = checked_in_;
      checked_in_ = 0;
      return count;
    }

   private:
    void fillPercussion(int from);

    double sample_rate_ = kReferenceRate;
    int fragment_ = kReferenceFragment;
    double time_ = 0.0, next_time_ = 0.0;
    int samples_ = 0;
    int keys_before_ = 0;
    int checked_in_ = 0;

    // Key compression: the level chases the table's value for the keys down
    // across each fragment.
    float key_compression_level_ = 1.0f;
    float key_compression_delta_ = 0.0f;
    std::vector<float> key_compression_;

    // Percussion: the envelope's gain, its reset value and per-sample decay.
    bool percussion_soft_ = false, percussion_fast_ = true;
    float percussion_gain_ = 0.0f;
    int trigger_offset_ = 0;
    std::vector<float> percussion_;
  };

  class Voice {
   public:
    bool active() const { return active_; }
    bool held() const { return held_; }
    int key() const { return key_; }

    // Closes the contacts of `key` (0 = C2 .. 60 = C7). `time` is the organ
    // clock at the voice's first sample of sound; the wheels start where
    // they are then. `envelopes` holds kNumClickEnvelopes attack shapes of
    // `length` samples (at most kMaxContactLength). `frequencies` lists the
    // wheels' Hz (index 1-91). A voice still sounding glides to the new contacts.
    void noteOn(int key, double time, const Registration& registration, const float* envelopes, int length,
                const Organ& organ, const double* frequencies);
    // Opens the contacts: through `envelopes` (kNumClickEnvelopes release
    // shapes of `length` samples), or linearly over a fragment (tonegen.c's
    // ENV_LINEAR, OpenB3's default) without them.
    void noteOff(const float* envelopes = nullptr, int length = 0);
    void clear();

    // The voice's pitch against its key's, applied from the next sample.
    void setPitchRatio(double ratio);
    // Follows drawbar and percussion changes over a fragment.
    void setRegistration(const Registration& registration, const double* frequencies);

    // Renders samples [start, end) of the organ's current block into
    // `output`: the wheels through the drawbars, plus the high-passed
    // percussion times the organ's envelope, times key compression.
    void render(const Organ& organ, double* output, int start, int end);

    // The gains tonegen.c's oscGenerateFragment would hold for this key:
    // per wheel, its sum over the drawbar buses and its percussion send.
    int numWheels() const { return num_wheels_; }
    int wheelNumber(int index) const { return wheels_[static_cast<size_t>(index)].wheel; }
    float swellGain(int index) const { return wheels_[static_cast<size_t>(index)].swell_to; }
    float percussionGain(int index) const { return wheels_[static_cast<size_t>(index)].percussion_to; }

   private:
    enum Ramp { kNoRamp, kClickRamp, kReleaseRamp, kLinearRamp };

    struct Wheel {
      int wheel = 0;
      std::array<float, kNumDrawbars> bus {};
      float swell_from = 0.0f, swell_to = 0.0f;
      float percussion_from = 0.0f, percussion_to = 0.0f;
      int envelope = 0;
      double cosine = 1.0, sine = 0.0, step_cosine = 1.0, step_sine = 0.0;
      bool audible = true;
    };

    void targets(const Registration& registration);
    // Freezes every gain where the current ramp has it and starts `ramp`
    // from there; the caller then sets the new targets.
    void startRamp(Ramp ramp);
    float rampValue(const Wheel& wheel) const;
    void syncPhasors(double sample_rate);

    std::array<Wheel, kMaxWheelsPerKey> wheels_ {};
    int num_wheels_ = 0;
    int key_ = -1;
    bool active_ = false, held_ = false;
    Registration registration_;
    const double* frequencies_ = nullptr;
    // The voice's own clock: the organ's, at the voice's pitch.
    double time_ = 0.0;
    double ratio_ = 1.0;
    // The drawbars' overall gain: lower while Normal percussion is on.
    float output_from_ = 1.0f, output_to_ = 1.0f;

    Ramp ramp_ = kNoRamp;
    int ramp_position_ = 0, ramp_length_ = kReferenceFragment;
    std::array<float, kNumClickEnvelopes * kMaxContactLength> envelopes_ {};
    std::array<float, kNumClickEnvelopes * kMaxContactLength> release_envelopes_ {};
    int fragment_ = kReferenceFragment;
    // The percussion high pass's previous input.
    double percussion_previous_ = 0.0;
  };
}
