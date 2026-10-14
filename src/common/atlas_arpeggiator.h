#pragma once

#include "JuceHeader.h"

#include <array>
#include <atomic>
#include <bitset>
#include <limits>
#include <vector>

class AtlasArpeggiator {
  public:
    enum TimeMode {
      kSync = 0,
      kFree
    };

    enum Style {
      kUp = 0,
      kDown,
      kUpDown,
      kDownUp,
      kUpAndDown,
      kDownAndUp,
      kConverge,
      kDiverge,
      kConvergeDiverge,
      kPinkyUp,
      kPinkyUpDown,
      kThumbUp,
      kThumbUpDown,
      kRandom,
      kRandomOther,
      kRandomOnce,
      kChordTrigger,
      kNumStyles
    };

    enum Retrigger {
      kRetriggerOff = 0,
      kRetriggerNote,
      kRetriggerBeat,
      kNumRetriggerModes
    };

    static constexpr int kDefaultRetriggerDivision = 4;

    enum PlayOrder {
      kPlayOrderPitch = 0,
      kPlayOrderAsPlayed,
      kNumPlayOrders
    };

    struct SyncRate {
      const char* name;
      double quarter_notes;
    };

    struct HeldNote {
      int note = 0;
      int velocity = 100;
      int channel = 1;
      uint64_t order = 0;
    };

    struct Settings {
      bool enabled = false;
      int time_mode = kSync;
      int sync_rate = 10;
      float free_rate = 5.0f;
      int style = kUp;
      // The order Style walks through: low to high, or the order keys were pressed.
      int play_order = kPlayOrderPitch;
      float gate = 80.0f;
      float random_gate = 0.0f;
      int repeats = 1;
      int offset = 0;
      int distance = 12;
      int steps = 1;
      int velocity = 100;
      float velocity_decay = 0.0f;
      float random_velocity = 0.0f;
      bool hold = false;
      int retrigger = kRetriggerNote;
      // Index into retriggerDivisions(): how often Beat retrigger restarts the pattern.
      int retrigger_division = kDefaultRetriggerDivision;
      int channel = 0;
    };

    void prepare(double sample_rate, int64 random_seed = 0x41544c4153415250ll);
    void setSampleRate(double sample_rate);
    void clear();
    void process(const juce::MidiBuffer& input, juce::MidiBuffer& output, int num_samples,
                 double bpm, double ppq_position, bool has_ppq_position, const Settings& settings);

    static const std::vector<SyncRate>& syncRates();
    static const std::vector<SyncRate>& retriggerDivisions();

    // MIDI panic: stop generated notes at sample_offset and forget held keys.
    void panic(juce::MidiBuffer& output, int sample_offset) {
      stopAllGeneratedNotes(output, sample_offset);
      held_notes_.clear();
      for (auto& channel : physically_down_)
        channel.reset();
      resetSequence();
      was_idle_ = true;
    }

  private:
    struct ActiveNote {
      int note = 0;
      int channel = 1;
      int samples_remaining = 0;
    };

    int consumeInputMidi(const juce::MidiBuffer& input, juce::MidiBuffer& output, const Settings& settings);
    void handleNoteOn(const juce::MidiMessage& message);
    void handleNoteOff(const juce::MidiMessage& message, const Settings& settings);
    void addDueNoteOffs(juce::MidiBuffer& output, int num_samples);
    void addInternalClockSteps(juce::MidiBuffer& output, int num_samples, double step_samples,
                               double gate_samples, double block_start_beat, double quarters_per_sample,
                               double division_quarters, const Settings& settings);
    void addBeatClockSteps(juce::MidiBuffer& output, int num_samples, double rate_quarters,
                           double gate_samples, double block_start_beat, double quarters_per_sample,
                           double division_quarters, const Settings& settings);
    void restartPatternAtDivision(int64 division_index);
    void triggerStep(juce::MidiBuffer& output, int sample_offset, int gate_samples, int block_samples,
                     const Settings& settings);
    const std::vector<HeldNote>& sortedHeldNotes();
    const std::vector<int>& buildPatternNotes(const std::vector<HeldNote>& source_notes, const Settings& settings);
    int choosePatternNote(const std::vector<int>& pattern, const Settings& settings);
    int outputChannel(const std::vector<HeldNote>& source_notes, const Settings& settings) const;
    int velocityForStep(const std::vector<HeldNote>& source_notes, const Settings& settings);
    void resetSequence(double samples_until_next_step = 0.0);
    void stopAllGeneratedNotes(juce::MidiBuffer& output, int sample_offset);
    void addGeneratedNote(juce::MidiBuffer& output, int note, int channel, int velocity,
                          int sample_offset, int gate_samples, int block_samples);
    void rebuildRandomOncePattern(const std::vector<int>& pattern);
    void updateChordFingerprint();

    std::vector<HeldNote> held_notes_;
    std::vector<ActiveNote> active_notes_;
    std::vector<int> random_once_pattern_;
    std::vector<HeldNote> sorted_notes_;
    std::vector<HeldNote> sorted_by_order_;
    std::vector<int> pattern_notes_;
    std::vector<int> scratch_notes_;
    std::vector<int> scratch_notes_2_;
    juce::Random random_;

    double sample_rate_ = 44100.0;
    double samples_until_next_step_ = 0.0;
    int pattern_index_ = 0;
    int repeat_index_ = 0;
    int last_random_note_ = -1;
    uint64_t next_held_order_ = 1;
    uint64_t chord_fingerprint_ = 0;
    bool was_idle_ = true;
    int64 random_seed_ = 0x41544c4153415250ll;
    double last_transport_end_ppq_ = 0.0;
    bool have_transport_position_ = false;
    // Beat position (in quarter notes) used when the host transport is stopped.
    // It starts at the first note and follows the host position while it plays.
    double beat_position_ = 0.0;
    // Division of the beat grid the last step fell in, for Beat retrigger.
    static constexpr int64 kNoDivision = std::numeric_limits<int64>::min();
    int64 last_division_index_ = kNoDivision;
    // Keys physically down per MIDI channel, so releasing Hold drops latched notes.
    std::array<std::bitset<128>, 16> physically_down_;
    bool last_hold_ = false;
};
