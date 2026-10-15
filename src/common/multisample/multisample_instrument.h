// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace vital {
namespace multisample {

  constexpr int kNumKeys = 128;
  constexpr int kNumCcs = 128;

  // Decoded audio for one sample file, rounded to 16 bits. Shared between the
  // regions that play it and between instruments built from the same files.
  struct SampleData {
    std::vector<int16_t> channels[2];
    int num_channels = 1;
    int64_t frames = 0;
    double sample_rate = 44100.0;
    // Loop and root key stored in the file itself (WAV smpl chunk).
    bool has_loop = false;
    int64_t loop_start = 0;
    int64_t loop_end = 0;
    int root_key = -1;

    const int16_t* channel(int index) const {
      return channels[index < num_channels ? index : 0].data();
    }
    size_t memoryBytes() const {
      return (channels[0].size() + channels[1].size()) * sizeof(int16_t);
    }
  };

  enum class LoopMode : uint8_t { kNoLoop, kOneShot, kContinuous, kSustain };
  enum class Trigger : uint8_t { kAttack, kRelease, kFirst, kLegato, kCc };
  enum class OffMode : uint8_t { kFast, kNormal, kTime };

  // A CC range a region needs to sound (locc/hicc).
  struct CcRange {
    int cc = 0;
    float low = 0.0f;
    float high = 127.0f;
  };

  // A per-CC modulation; `amount` is in the unit of the target, scaled by
  // the CC through `curve` (an index into Instrument::curve_tables), or
  // linearly over 0..127 when it has none.
  struct CcMod {
    int cc = 0;
    float amount = 0.0f;
    int curve = -1;
  };

  // DAHDSR amplitude envelope in seconds; start and sustain are 0..1.
  struct Envelope {
    float delay = 0.0f;
    float start = 0.0f;
    float attack = 0.0f;
    float hold = 0.0f;
    float decay = 0.0f;
    float sustain = 1.0f;
    float release = 0.001f;
  };

  struct EnvelopeCcMods {
    std::vector<CcMod> delay;
    std::vector<CcMod> start;
    std::vector<CcMod> attack;
    std::vector<CcMod> hold;
    std::vector<CcMod> decay;
    std::vector<CcMod> sustain;
    std::vector<CcMod> release;

    bool empty() const {
      return delay.empty() && start.empty() && attack.empty() && hold.empty() &&
             decay.empty() && sustain.empty() && release.empty();
    }
  };

  struct Region {
    // Sample data. `left`/`right` point into `sample` (SFZ) or the SF2 pool
    // (`pool`), and `data_frames` bounds every read.
    std::string sample_path;
    // sample=*silence: plays nothing but still chokes other groups.
    bool silent = false;
    std::shared_ptr<const SampleData> sample;
    std::shared_ptr<const std::vector<int16_t>> pool;
    const int16_t* left = nullptr;
    const int16_t* right = nullptr;
    int64_t data_frames = 0;
    double sample_rate = 44100.0;

    // Playback range in frames of the data; end is exclusive.
    int64_t offset = 0;
    int64_t end = -1;
    float offset_random = 0.0f;
    LoopMode loop_mode = LoopMode::kNoLoop;
    int64_t loop_start = 0;
    int64_t loop_end = 0;
    bool loop_mode_set = false;
    bool loop_start_set = false;
    bool loop_end_set = false;
    // The loop plays forward and back (DecentSampler loopMode="pingpong").
    bool loop_alternate = false;

    // Selection.
    int lokey = 0;
    int hikey = 127;
    int lovel = 1;
    int hivel = 127;
    float lorand = 0.0f;
    float hirand = 1.0f;
    int seq_length = 1;
    int seq_position = 1;
    std::vector<CcRange> cc_gates;
    int sw_last = -1;
    int sw_lokey = -1;
    int sw_hikey = -1;
    Trigger trigger = Trigger::kAttack;
    int on_cc = -1;
    float on_low = 0.0f;
    float on_high = 127.0f;

    // Choke groups.
    int64_t group = 0;
    int64_t off_by = 0;
    OffMode off_mode = OffMode::kFast;
    float off_time = 0.006f;
    // Dense indices into the instrument's group table, -1 for none.
    int group_slot = -1;
    int off_by_slot = -1;

    // Pitch: playback key centre, cents per key, and tuning in cents.
    float pitch_keycenter = 60.0f;
    bool keycenter_from_sample = false;
    float pitch_keytrack = 100.0f;
    float tune_cents = 0.0f;
    int transpose = 0;

    // Level: linear gain (volume and amplitude), pan and stereo width -1..1.
    float volume_db = 0.0f;
    float amplitude = 1.0f;
    float pan = 0.0f;
    float width = 1.0f;
    float amp_veltrack = 1.0f;
    std::vector<std::pair<int, float>> velocity_points;
    std::array<float, 128> velocity_gain {};
    float rt_decay = 0.0f;
    float delay = 0.0f;

    std::vector<CcMod> amplitude_cc;
    std::vector<CcMod> volume_cc;
    std::vector<CcMod> pan_cc;
    Envelope ampeg;
    EnvelopeCcMods ampeg_cc;
    // ampeg_dynamic: the envelope's CC modulation follows the CCs while the
    // note plays (a release lengthens and shortens with the pedal).
    bool ampeg_dynamic = false;
    // SF2 keynumToVolEnvHold / Decay in timecents per key from key 60.
    float hold_keytrack = 0.0f;
    float decay_keytrack = 0.0f;

    // Linear gain from volume and amplitude, before velocity.
    float baseGain() const;
    // Resolves the velocity curve; called once the region is complete.
    void buildVelocityCurve();
  };

  struct PresetName {
    int bank = 0;
    int program = 0;
    std::string name;
    // Index of the preset header in the SF2 file.
    int index = 0;
  };

  // A control the instrument's author put on its panel (a DecentSampler knob,
  // menu or button). Changing `value` and parsing again applies its bindings.
  struct Control {
    enum class Kind : uint8_t { kKnob, kMenu, kButton };
    Kind kind = Kind::kKnob;
    // Position among the file's controls, in document order: the key under
    // which presets store the value.
    int index = 0;
    std::string label;
    // Knobs: the range, and whole numbers only for type="integer".
    double minimum = 0.0;
    double maximum = 1.0;
    bool integer = false;
    // Menus count options from 1, buttons count states from 0.
    std::vector<std::string> choices;
    double default_value = 0.0;
    double value = 0.0;
  };

  // The playable result of parsing an SFZ or SF2 instrument. Immutable after
  // build(); the audio thread reads it through a shared pointer.
  class Instrument {
    public:
      std::string name;
      std::string format;
      std::string source_path;
      std::vector<Region> regions;

      std::array<std::vector<int>, kNumKeys> attack_by_key;
      std::array<std::vector<int>, kNumKeys> release_by_key;
      std::array<std::vector<int>, kNumCcs> cc_triggered;
      std::array<float, kNumCcs> cc_defaults {};
      // SFZ <curve> tables (and built-in curves in use): 128 points, CC value to 0..1.
      std::vector<std::array<float, kNumCcs>> curve_tables;
      std::map<int, std::string> cc_labels;
      // Controls whose bindings change the sound here, in panel order.
      std::vector<Control> controls;
      int sw_default = -1;
      int sw_lokey = 128;
      int sw_hikey = -1;
      int num_group_slots = 0;
      bool uses_cc_state = false;

      // SF2 only: every preset in the file and the one this instrument plays.
      std::vector<PresetName> preset_names;
      int preset_index = -1;

      std::vector<std::string> warnings;
      std::vector<std::string> missing_files;
      size_t memory_bytes = 0;

      // Builds the lookup tables. Call once every region is complete.
      void build();
      bool isKeySwitch(int key) const { return key >= sw_lokey && key <= sw_hikey; }
  };

  // SFZ note names: c4 = 60, c#4 / db4 = 61. Returns -1 when the text is not a note.
  int parseNoteName(const std::string& text);
  // A key number or note name; `fallback` when it is neither.
  int parseKey(const std::string& text, int fallback);
} // namespace multisample
} // namespace vital
