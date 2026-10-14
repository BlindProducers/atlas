#include "atlas_arpeggiator.h"

#include <algorithm>
#include <cmath>

namespace {
  template <typename T>
  void rotateLeft(std::vector<T>& values, int amount) {
    if (values.empty())
      return;

    amount %= static_cast<int>(values.size());
    if (amount < 0)
      amount += static_cast<int>(values.size());
    std::rotate(values.begin(), values.begin() + amount, values.end());
  }

}

void AtlasArpeggiator::prepare(double sample_rate, int64 random_seed) {
  static constexpr size_t kMaxHeldNotes = 128;
  static constexpr size_t kMaxPatternNotes = 1024;
  held_notes_.reserve(kMaxHeldNotes);
  active_notes_.reserve(kMaxHeldNotes);
  random_once_pattern_.reserve(kMaxPatternNotes);
  sorted_notes_.reserve(kMaxHeldNotes);
  sorted_by_order_.reserve(kMaxHeldNotes);
  pattern_notes_.reserve(kMaxPatternNotes);
  scratch_notes_.reserve(kMaxPatternNotes);
  scratch_notes_2_.reserve(kMaxPatternNotes);
  random_seed_ = random_seed;
  setSampleRate(sample_rate);
  clear();
}

void AtlasArpeggiator::setSampleRate(double sample_rate) {
  sample_rate_ = sample_rate > 0.0 ? sample_rate : 44100.0;
}

void AtlasArpeggiator::clear() {
  samples_until_next_step_ = 0.0;
  resetSequence();
  held_notes_.clear();
  active_notes_.clear();
  for (auto& channel : physically_down_)
    channel.reset();
  last_hold_ = false;
  random_.setSeed(random_seed_);
  chord_fingerprint_ = 0;
  was_idle_ = true;
  have_transport_position_ = false;
}

void AtlasArpeggiator::process(const MidiBuffer& input, MidiBuffer& output, int num_samples,
                               double bpm, double ppq_position, bool has_ppq_position,
                               const Settings& settings) {
  if (has_ppq_position) {
    const double quarters_per_sample = bpm / (60.0 * sample_rate_);
    if (have_transport_position_ && std::abs(ppq_position - last_transport_end_ppq_) >
                                    quarters_per_sample * 1.5) {
      stopAllGeneratedNotes(output, 0);
      resetSequence();
      random_.setSeed(random_seed_);
    }
    last_transport_end_ppq_ = ppq_position + quarters_per_sample * num_samples;
    have_transport_position_ = true;
  }
  else
    have_transport_position_ = false;

  const int first_note_sample = consumeInputMidi(input, output, settings);
  addDueNoteOffs(output, num_samples);

  if (!settings.enabled) {
    // A disabled arp receives no input, so anything it holds would be stale
    // (and replayed as phantom notes) by the time it is enabled again.
    stopAllGeneratedNotes(output, 0);
    held_notes_.clear();
    for (auto& channel : physically_down_)
      channel.reset();
    last_hold_ = settings.hold;
    was_idle_ = true;
    return;
  }

  if (last_hold_ && !settings.hold) {
    held_notes_.erase(std::remove_if(held_notes_.begin(), held_notes_.end(), [this](const HeldNote& held) {
      return !physically_down_[static_cast<size_t>(jlimit(1, 16, held.channel) - 1)][static_cast<size_t>(held.note & 127)];
    }), held_notes_.end());
    const uint64_t previous_fingerprint = chord_fingerprint_;
    updateChordFingerprint();
    // Dropping latched notes changes the pattern, but the step already
    // sounding keeps its full gate.
    if (previous_fingerprint != chord_fingerprint_)
      random_once_pattern_.clear();
  }
  last_hold_ = settings.hold;

  const bool has_held_notes = !sortedHeldNotes().empty();
  if (!has_held_notes) {
    resetSequence();
    was_idle_ = true;
    return;
  }

  const double quarters_per_sample = jlimit(1.0, 999.0, bpm) / (60.0 * sample_rate_);
  if (was_idle_) {
    const int start_sample = first_note_sample >= 0 ? first_note_sample : 0;
    if (settings.retrigger == kRetriggerOff)
      resetSequence(start_sample);
    // With the transport stopped, the beat grid starts at the first note.
    beat_position_ = -start_sample * quarters_per_sample;
    last_division_index_ = kNoDivision;
  }
  was_idle_ = false;

  const double block_start_beat = has_ppq_position ? ppq_position : beat_position_;
  beat_position_ = block_start_beat + quarters_per_sample * num_samples;

  double division_quarters = 0.0;
  if (settings.retrigger == kRetriggerBeat) {
    const int division_index = jlimit(0, static_cast<int>(retriggerDivisions().size()) - 1,
                                      settings.retrigger_division);
    division_quarters = retriggerDivisions()[static_cast<size_t>(division_index)].quarter_notes;
  }

  const int rate_index = jlimit(0, static_cast<int>(syncRates().size()) - 1, settings.sync_rate);
  const double rate_quarters = syncRates()[static_cast<size_t>(rate_index)].quarter_notes;
  const double gate_ratio = jlimit(0.01, 1.0, static_cast<double>(settings.gate) / 100.0);

  if (settings.time_mode == kSync) {
    const double step_samples = rate_quarters / quarters_per_sample;
    if (division_quarters > 0.0) {
      addBeatClockSteps(output, num_samples, rate_quarters, step_samples * gate_ratio,
                        block_start_beat, quarters_per_sample, division_quarters, settings);
    }
    else {
      addInternalClockSteps(output, num_samples, step_samples, step_samples * gate_ratio,
                            block_start_beat, quarters_per_sample, 0.0, settings);
    }
  }
  else {
    const double hz = jlimit(0.05, 100.0, static_cast<double>(settings.free_rate));
    const double step_samples = sample_rate_ / hz;
    addInternalClockSteps(output, num_samples, step_samples, step_samples * gate_ratio,
                          block_start_beat, quarters_per_sample, division_quarters, settings);
  }
}

const std::vector<AtlasArpeggiator::SyncRate>& AtlasArpeggiator::syncRates() {
  static const std::vector<SyncRate> rates {
    { "8 Bars", 32.0 }, { "4 Bars", 16.0 }, { "2 Bars", 8.0 }, { "1 Bar", 4.0 },
    { "1/2", 2.0 }, { "1/2 Dotted", 3.0 }, { "1/2 Triplet", 4.0 / 3.0 },
    { "1/4", 1.0 }, { "1/4 Dotted", 1.5 }, { "1/4 Triplet", 2.0 / 3.0 },
    { "1/8", 0.5 }, { "1/8 Dotted", 0.75 }, { "1/8 Triplet", 1.0 / 3.0 },
    { "1/16", 0.25 }, { "1/16 Dotted", 0.375 }, { "1/16 Triplet", 1.0 / 6.0 },
    { "1/32", 0.125 }, { "1/32 Dotted", 0.1875 }, { "1/32 Triplet", 1.0 / 12.0 },
    { "1/64", 0.0625 }, { "1/64 Dotted", 0.09375 }, { "1/64 Triplet", 1.0 / 24.0 },
    { "1/128", 0.03125 }
  };
  return rates;
}

const std::vector<AtlasArpeggiator::SyncRate>& AtlasArpeggiator::retriggerDivisions() {
  static const std::vector<SyncRate> divisions {
    { "1/16", 0.25 }, { "1/8", 0.5 }, { "1/4", 1.0 }, { "1/2", 2.0 }, { "1 Bar", 4.0 },
    { "2 Bars", 8.0 }, { "4 Bars", 16.0 }, { "8 Bars", 32.0 }, { "16 Bars", 64.0 }
  };
  return divisions;
}

int AtlasArpeggiator::consumeInputMidi(const MidiBuffer& input, MidiBuffer& output, const Settings& settings) {
  bool saw_note_on = false;
  bool chord_changed = false;
  int retrigger_sample = 0;
  int first_note_sample = -1;
  uint64_t previous_fingerprint = chord_fingerprint_;

  for (const auto metadata : input) {
    const MidiMessage message = metadata.getMessage();
    if (message.isNoteOn()) {
      handleNoteOn(message);
      saw_note_on = true;
      retrigger_sample = metadata.samplePosition;
      if (first_note_sample < 0)
        first_note_sample = metadata.samplePosition;
    }
    else if (message.isNoteOff()) {
      handleNoteOff(message, settings);
    }
    else {
      output.addEvent(message, metadata.samplePosition);
    }

    if (message.isNoteOnOrOff()) {
      updateChordFingerprint();
      if (previous_fingerprint != chord_fingerprint_) {
        chord_changed = true;
        random_once_pattern_.clear();
        previous_fingerprint = chord_fingerprint_;
      }
    }
  }

  // A changed chord only cuts the sounding step when a new note restarts the
  // sequence. Otherwise (a released key, or Off and Beat retrigger) the step
  // plays out its gate and the next step uses the new chord.
  if (saw_note_on && settings.retrigger == kRetriggerNote && chord_changed) {
    stopAllGeneratedNotes(output, retrigger_sample);
    resetSequence(retrigger_sample);
  }

  return first_note_sample;
}

void AtlasArpeggiator::handleNoteOn(const MidiMessage& message) {
  const int note = message.getNoteNumber();
  const int channel = message.getChannel();
  const int velocity = jlimit(1, 127, static_cast<int>(message.getVelocity() * 127.0f));
  physically_down_[static_cast<size_t>(jlimit(1, 16, channel) - 1)][static_cast<size_t>(note & 127)] = true;

  auto existing = std::find_if(held_notes_.begin(), held_notes_.end(), [note, channel](const HeldNote& held) {
    return held.note == note && held.channel == channel;
  });
  if (existing != held_notes_.end()) {
    existing->velocity = velocity;
    existing->order = next_held_order_++;
  }
  else {
    held_notes_.push_back({ note, velocity, channel, next_held_order_++ });
  }
}

void AtlasArpeggiator::handleNoteOff(const MidiMessage& message, const Settings& settings) {
  const int note = message.getNoteNumber();
  const int channel = message.getChannel();
  physically_down_[static_cast<size_t>(jlimit(1, 16, channel) - 1)][static_cast<size_t>(note & 127)] = false;
  if (settings.hold)
    return;

  held_notes_.erase(std::remove_if(held_notes_.begin(), held_notes_.end(), [note, channel](const HeldNote& held) {
    return held.note == note && held.channel == channel;
  }), held_notes_.end());
}

void AtlasArpeggiator::addDueNoteOffs(MidiBuffer& output, int num_samples) {
  for (auto& active : active_notes_) {
    active.samples_remaining -= num_samples;
    if (active.samples_remaining <= 0)
      output.addEvent(MidiMessage::noteOff(active.channel, active.note),
                      jlimit(0, num_samples - 1, num_samples + active.samples_remaining));
  }

  active_notes_.erase(std::remove_if(active_notes_.begin(), active_notes_.end(), [](const ActiveNote& active) {
    return active.samples_remaining <= 0;
  }), active_notes_.end());
}

void AtlasArpeggiator::addInternalClockSteps(MidiBuffer& output, int num_samples, double step_samples,
                                             double gate_samples, double block_start_beat,
                                             double quarters_per_sample, double division_quarters,
                                             const Settings& settings) {
  static constexpr double kToleranceSamples = 0.25;
  step_samples = jmax(1.0, step_samples);
  const int max_gate_samples = jmax(1, static_cast<int>(std::floor(step_samples)) - 1);
  const int clamped_gate_samples = jlimit(1, max_gate_samples, static_cast<int>(std::round(gate_samples)));
  while (true) {
    double event = samples_until_next_step_;
    int64 event_division = kNoDivision;
    if (division_quarters > 0.0) {
      const double step_beat = block_start_beat + event * quarters_per_sample;
      event_division = static_cast<int64>(std::floor((step_beat + kToleranceSamples * quarters_per_sample) /
                                                     division_quarters));
      if (last_division_index_ != kNoDivision && event_division > last_division_index_) {
        // Beat retrigger: the first boundary crossed restarts the step clock,
        // so the pattern begins exactly on the division.
        const int64 boundary_division = last_division_index_ + 1;
        const double boundary = (static_cast<double>(boundary_division) * division_quarters - block_start_beat) /
                                quarters_per_sample;
        if (boundary >= -kToleranceSamples) {
          event = jmax(0.0, boundary);
          event_division = boundary_division;
        }
      }
    }
    if (event >= static_cast<double>(num_samples))
      break;

    if (event_division != kNoDivision && event_division != last_division_index_)
      restartPatternAtDivision(event_division);
    const int offset = jlimit(0, num_samples - 1, static_cast<int>(std::round(event)));
    triggerStep(output, offset, clamped_gate_samples, num_samples, settings);
    samples_until_next_step_ = event + step_samples;
  }
  samples_until_next_step_ -= static_cast<double>(num_samples);
}

void AtlasArpeggiator::addBeatClockSteps(MidiBuffer& output, int num_samples, double rate_quarters,
                                         double gate_samples, double block_start_beat,
                                         double quarters_per_sample, double division_quarters,
                                         const Settings& settings) {
  // Steps sit on the beat grid (the host's while its transport plays) and the
  // grid restarts on every division boundary, so a rate that does not divide
  // the division still puts the first step of the pattern on the boundary.
  const double tolerance = quarters_per_sample * 0.25;
  const double block_end = block_start_beat + quarters_per_sample * static_cast<double>(num_samples);
  const double step_samples = rate_quarters / quarters_per_sample;
  const int max_gate_samples = jmax(1, static_cast<int>(std::floor(step_samples)) - 1);
  const int clamped_gate_samples = jlimit(1, max_gate_samples, static_cast<int>(std::round(gate_samples)));

  double position = block_start_beat;
  while (true) {
    const double division_start = std::floor((position + tolerance) / division_quarters) * division_quarters;
    double step = division_start + std::ceil((position - division_start - tolerance) / rate_quarters) * rate_quarters;
    const double next_division = division_start + division_quarters;
    if (step > next_division - tolerance)
      step = next_division;
    if (step >= block_end - tolerance)
      break;

    const int64 step_division = static_cast<int64>(std::floor((step + tolerance) / division_quarters));
    if (step_division != last_division_index_)
      restartPatternAtDivision(step_division);
    const int offset = jlimit(0, num_samples - 1,
                              static_cast<int>(std::round((step - block_start_beat) / quarters_per_sample)));
    triggerStep(output, offset, clamped_gate_samples, num_samples, settings);
    position = step + 2.0 * tolerance;
  }
}

void AtlasArpeggiator::restartPatternAtDivision(int64 division_index) {
  last_division_index_ = division_index;
  pattern_index_ = 0;
  repeat_index_ = 0;
  loop_steps_ = 0;
  last_random_note_ = -1;
}

void AtlasArpeggiator::triggerStep(MidiBuffer& output, int sample_offset, int gate_samples, int block_samples,
                                   const Settings& settings) {
  const auto& source_notes = sortedHeldNotes();
  if (source_notes.empty()) {
    random_once_pattern_.clear();
    return;
  }

  const int style = jlimit(0, kNumStyles - 1, settings.style);
  const std::vector<int>* pattern = nullptr;
  int steps_per_loop = 1;
  if (style != kChordTrigger) {
    pattern = &buildPatternNotes(source_notes, settings);
    if (pattern->empty())
      return;
    // Random styles ignore Repeats; the others play each note Repeats times.
    const bool repeats_apply = style != kRandom && style != kRandomOther;
    steps_per_loop = static_cast<int>(pattern->size()) * (repeats_apply ? jmax(1, settings.repeats) : 1);
  }

  // Loop: once the pattern has played the set number of times the arp stays
  // silent until a retrigger, or a new chord after release, restarts it.
  if (settings.loop >= 0 && settings.loop < kLoopInfinite &&
      loop_steps_ >= (settings.loop + 1) * steps_per_loop)
    return;
  ++loop_steps_;

  const int channel = outputChannel(source_notes, settings);
  const float random_gate_scale = 1.0f - random_.nextFloat() *
      jlimit(0.0f, 1.0f, settings.random_gate);
  gate_samples = jmax(1, roundToInt(gate_samples * random_gate_scale));
  if (style == kChordTrigger) {
    const int velocity = velocityForStep(source_notes, settings);
    std::vector<int>& chord_notes = pattern_notes_;
    chord_notes.clear();
    for (const auto& held : source_notes)
      chord_notes.push_back(held.note);
    std::sort(chord_notes.begin(), chord_notes.end());
    chord_notes.erase(std::unique(chord_notes.begin(), chord_notes.end()), chord_notes.end());
    for (int note : chord_notes)
      addGeneratedNote(output, note, channel, velocity, sample_offset, gate_samples, block_samples);
    return;
  }

  addGeneratedNote(output, choosePatternNote(*pattern, settings), channel, velocityForStep(source_notes, settings),
                   sample_offset, gate_samples, block_samples);
}

const std::vector<AtlasArpeggiator::HeldNote>& AtlasArpeggiator::sortedHeldNotes() {
  sorted_notes_.assign(held_notes_.begin(), held_notes_.end());
  std::sort(sorted_notes_.begin(), sorted_notes_.end(), [](const HeldNote& a, const HeldNote& b) {
    return a.note == b.note ? a.order < b.order : a.note < b.note;
  });
  return sorted_notes_;
}

const std::vector<int>& AtlasArpeggiator::buildPatternNotes(const std::vector<HeldNote>& source_notes,
                                                            const Settings& settings) {
  pattern_notes_.clear();
  if (settings.play_order == kPlayOrderAsPlayed) {
    // Keys in the order they were pressed, each step's transposed copy after
    // the last. Styles then treat this order as "up".
    sorted_by_order_.assign(source_notes.begin(), source_notes.end());
    std::sort(sorted_by_order_.begin(), sorted_by_order_.end(), [](const HeldNote& a, const HeldNote& b) {
      return a.order < b.order;
    });
    std::bitset<128> used;
    for (int step = 0; step < jmax(1, settings.steps); ++step) {
      for (const auto& held : sorted_by_order_) {
        const int note = held.note + settings.distance * step;
        if (isPositiveAndBelow(note, 128) && !used[static_cast<size_t>(note)]) {
          used[static_cast<size_t>(note)] = true;
          pattern_notes_.push_back(note);
        }
      }
    }
  }
  else {
    for (int step = 0; step < jmax(1, settings.steps); ++step) {
      for (const auto& held : source_notes) {
        const int note = held.note + settings.distance * step;
        if (isPositiveAndBelow(note, 128))
          pattern_notes_.push_back(note);
      }
    }
    std::sort(pattern_notes_.begin(), pattern_notes_.end());
    pattern_notes_.erase(std::unique(pattern_notes_.begin(), pattern_notes_.end()), pattern_notes_.end());
  }
  if (pattern_notes_.empty())
    return pattern_notes_;

  auto appendReverseExcludingEnds = [](std::vector<int>& target, const std::vector<int>& source) {
    if (source.size() <= 2)
      return;
    for (auto it = source.rbegin() + 1; it != source.rend() - 1; ++it)
      target.push_back(*it);
  };
  auto writeConverged = [this](const std::vector<int>& source, bool diverging) {
    scratch_notes_.clear();
    if (!diverging) {
      int low = 0;
      int high = static_cast<int>(source.size()) - 1;
      while (low <= high) {
        scratch_notes_.push_back(source[static_cast<size_t>(low++)]);
        if (low <= high)
          scratch_notes_.push_back(source[static_cast<size_t>(high--)]);
      }
    }
    else {
      int low = (static_cast<int>(source.size()) - 1) / 2;
      int high = low + 1;
      while (low >= 0 || high < static_cast<int>(source.size())) {
        if (low >= 0)
          scratch_notes_.push_back(source[static_cast<size_t>(low--)]);
        if (high < static_cast<int>(source.size()))
          scratch_notes_.push_back(source[static_cast<size_t>(high++)]);
      }
    }
    pattern_notes_.assign(scratch_notes_.begin(), scratch_notes_.end());
  };
  auto writeAnchored = [this, &appendReverseExcludingEnds](bool high_anchor, bool up_down) {
    // With a single note there is nothing to alternate with the anchor.
    if (pattern_notes_.size() < 2)
      return;
    const int anchor = high_anchor ? pattern_notes_.back() : pattern_notes_.front();
    scratch_notes_.assign(pattern_notes_.begin(), pattern_notes_.end());
    if (high_anchor)
      scratch_notes_.pop_back();
    else
      scratch_notes_.erase(scratch_notes_.begin());
    if (up_down) {
      scratch_notes_2_.assign(scratch_notes_.begin(), scratch_notes_.end());
      appendReverseExcludingEnds(scratch_notes_, scratch_notes_2_);
    }
    pattern_notes_.clear();
    for (int note : scratch_notes_) {
      pattern_notes_.push_back(anchor);
      pattern_notes_.push_back(note);
    }
  };

  switch (jlimit(0, kNumStyles - 1, settings.style)) {
    case kUp: break;
    case kDown: std::reverse(pattern_notes_.begin(), pattern_notes_.end()); break;
    case kUpDown: {
      scratch_notes_.assign(pattern_notes_.begin(), pattern_notes_.end());
      appendReverseExcludingEnds(pattern_notes_, scratch_notes_);
      break;
    }
    case kDownUp: {
      std::reverse(pattern_notes_.begin(), pattern_notes_.end());
      scratch_notes_.assign(pattern_notes_.begin(), pattern_notes_.end());
      appendReverseExcludingEnds(pattern_notes_, scratch_notes_);
      break;
    }
    case kUpAndDown: {
      scratch_notes_.assign(pattern_notes_.rbegin(), pattern_notes_.rend());
      pattern_notes_.insert(pattern_notes_.end(), scratch_notes_.begin(), scratch_notes_.end());
      break;
    }
    case kDownAndUp: {
      std::reverse(pattern_notes_.begin(), pattern_notes_.end());
      scratch_notes_.assign(pattern_notes_.rbegin(), pattern_notes_.rend());
      pattern_notes_.insert(pattern_notes_.end(), scratch_notes_.begin(), scratch_notes_.end());
      break;
    }
    case kConverge: writeConverged(pattern_notes_, false); break;
    case kDiverge: writeConverged(pattern_notes_, true); break;
    case kConvergeDiverge: {
      writeConverged(pattern_notes_, false);
      scratch_notes_.assign(pattern_notes_.begin(), pattern_notes_.end());
      appendReverseExcludingEnds(pattern_notes_, scratch_notes_);
      break;
    }
    case kPinkyUp: writeAnchored(true, false); break;
    case kPinkyUpDown: writeAnchored(true, true); break;
    case kThumbUp: writeAnchored(false, false); break;
    case kThumbUpDown: writeAnchored(false, true); break;
    default: break;
  }

  rotateLeft(pattern_notes_, settings.offset);
  return pattern_notes_;
}

int AtlasArpeggiator::choosePatternNote(const std::vector<int>& pattern, const Settings& settings) {
  const int style = jlimit(0, kNumStyles - 1, settings.style);
  if (style == kRandom || style == kRandomOther) {
    int note = pattern[static_cast<size_t>(random_.nextInt(static_cast<int>(pattern.size())))];
    if (style == kRandomOther && pattern.size() > 1) {
      for (int attempt = 0; attempt < 8 && note == last_random_note_; ++attempt)
        note = pattern[static_cast<size_t>(random_.nextInt(static_cast<int>(pattern.size())))];
    }
    last_random_note_ = note;
    return note;
  }

  if (style == kRandomOnce) {
    if (random_once_pattern_.empty())
      rebuildRandomOncePattern(pattern);
    if (!random_once_pattern_.empty()) {
      const int repeats = jmax(1, settings.repeats);
      const int note = random_once_pattern_[static_cast<size_t>(pattern_index_ %
          static_cast<int>(random_once_pattern_.size()))];
      if (++repeat_index_ >= repeats) {
        repeat_index_ = 0;
        ++pattern_index_;
      }
      return note;
    }
  }

  const int repeats = jmax(1, settings.repeats);
  const int note = pattern[static_cast<size_t>(pattern_index_ % static_cast<int>(pattern.size()))];
  if (++repeat_index_ >= repeats) {
    repeat_index_ = 0;
    ++pattern_index_;
  }
  return note;
}

int AtlasArpeggiator::outputChannel(const std::vector<HeldNote>& source_notes, const Settings& settings) const {
  if (settings.channel >= 1 && settings.channel <= 16)
    return settings.channel;
  return source_notes.empty() ? 1 : source_notes.front().channel;
}

int AtlasArpeggiator::velocityForStep(const std::vector<HeldNote>& source_notes, const Settings& settings) {
  const float decay = settings.velocity_decay / 100.0f;
  const float multiplier = std::pow(jlimit(0.0f, 1.0f, 1.0f - decay), static_cast<float>(jmax(0, pattern_index_)));
  const float source_scale = source_notes.empty() ? 1.0f : static_cast<float>(source_notes.front().velocity) / 127.0f;
  const float random_amount = jlimit(0.0f, 1.0f, settings.random_velocity);
  const float random_scale = 1.0f - random_.nextFloat() * random_amount;
  return jlimit(1, 127, roundToInt(static_cast<float>(settings.velocity) * source_scale * multiplier * random_scale));
}

void AtlasArpeggiator::resetSequence(double samples_until_next_step) {
  samples_until_next_step_ = jmax(0.0, samples_until_next_step);
  pattern_index_ = 0;
  repeat_index_ = 0;
  loop_steps_ = 0;
  last_random_note_ = -1;
  last_division_index_ = kNoDivision;
  random_once_pattern_.clear();
}

void AtlasArpeggiator::stopAllGeneratedNotes(MidiBuffer& output, int sample_offset) {
  for (const auto& active : active_notes_)
    output.addEvent(MidiMessage::noteOff(active.channel, active.note), sample_offset);
  active_notes_.clear();
}

void AtlasArpeggiator::addGeneratedNote(MidiBuffer& output, int note, int channel, int velocity,
                                        int sample_offset, int gate_samples, int block_samples) {
  for (auto& active : active_notes_) {
    if (active.note == note && active.channel == channel && active.samples_remaining > 0) {
      output.addEvent(MidiMessage::noteOff(channel, note), sample_offset);
      active.samples_remaining = 0;
    }
  }
  active_notes_.erase(std::remove_if(active_notes_.begin(), active_notes_.end(), [](const ActiveNote& active) {
    return active.samples_remaining <= 0;
  }), active_notes_.end());

  output.addEvent(MidiMessage::noteOn(channel, note, static_cast<uint8>(velocity)), sample_offset);
  const int note_off_offset = sample_offset + gate_samples;
  if (note_off_offset < block_samples)
    output.addEvent(MidiMessage::noteOff(channel, note), note_off_offset);
  else
    active_notes_.push_back({ note, channel, note_off_offset - block_samples });
}

void AtlasArpeggiator::rebuildRandomOncePattern(const std::vector<int>& pattern) {
  random_once_pattern_ = pattern;
  for (int i = static_cast<int>(random_once_pattern_.size()) - 1; i > 0; --i)
    std::swap(random_once_pattern_[static_cast<size_t>(i)],
              random_once_pattern_[static_cast<size_t>(random_.nextInt(i + 1))]);
}

void AtlasArpeggiator::updateChordFingerprint() {
  uint64_t fingerprint = 1469598103934665603ull;
  for (const auto& held : held_notes_) {
    fingerprint ^= static_cast<uint64_t>((held.channel << 8) | held.note);
    fingerprint *= 1099511628211ull;
  }
  chord_fingerprint_ = fingerprint;
}
