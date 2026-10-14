#pragma once

// AtlasArpeggiator run on its own, split at input events the way
// SynthBase::buildScheduledMidiEvents feeds it.

#include "atlas_arpeggiator.h"

#include <map>
#include <vector>

namespace {
  struct ArpEvent {
    int sample = 0;
    MidiMessage message;
  };

  constexpr double kArpTestSampleRate = 48000.0;
  constexpr double kArpTestBpm = 120.0;
  constexpr double kArpTestSamplesPerQuarter = kArpTestSampleRate * 60.0 / kArpTestBpm;

  int arpSyncRateIndex(const char* name) {
    const auto& rates = AtlasArpeggiator::syncRates();
    for (int i = 0; i < static_cast<int>(rates.size()); ++i) {
      if (String(rates[static_cast<size_t>(i)].name) == name)
        return i;
    }
    return -1;
  }

  int arpRetriggerDivisionIndex(const char* name) {
    const auto& divisions = AtlasArpeggiator::retriggerDivisions();
    for (int i = 0; i < static_cast<int>(divisions.size()); ++i) {
      if (String(divisions[static_cast<size_t>(i)].name) == name)
        return i;
    }
    return -1;
  }

  // Runs the arp for total_samples in blocks, with input events at absolute
  // sample positions. ppq_start < 0 means the host transport is stopped.
  std::vector<ArpEvent> runArpeggiator(const AtlasArpeggiator::Settings& settings,
                                       const std::multimap<int, MidiMessage>& input,
                                       int total_samples, double ppq_start = -1.0, int block_size = 512) {
    AtlasArpeggiator arp;
    arp.prepare(kArpTestSampleRate);
    std::vector<ArpEvent> events;
    const bool playing = ppq_start >= 0.0;
    const double quarters_per_sample = kArpTestBpm / (60.0 * kArpTestSampleRate);

    for (int block_start = 0; block_start < total_samples; block_start += block_size) {
      const int block_end = std::min(total_samples, block_start + block_size);
      int cursor = block_start;
      while (cursor < block_end) {
        auto next = input.lower_bound(cursor + 1);
        const int segment_end = next != input.end() && next->first < block_end ? next->first : block_end;
        MidiBuffer segment_input;
        const auto range = input.equal_range(cursor);
        for (auto it = range.first; it != range.second; ++it)
          segment_input.addEvent(it->second, 0);

        MidiBuffer output;
        arp.process(segment_input, output, segment_end - cursor, kArpTestBpm,
                    playing ? ppq_start + cursor * quarters_per_sample : 0.0, playing, settings);
        for (const auto metadata : output)
          events.push_back({ cursor + metadata.samplePosition, metadata.getMessage() });
        cursor = segment_end;
      }
    }
    return events;
  }

  std::vector<ArpEvent> arpNoteOns(const std::vector<ArpEvent>& events) {
    std::vector<ArpEvent> note_ons;
    for (const auto& event : events) {
      if (event.message.isNoteOn())
        note_ons.push_back(event);
    }
    return note_ons;
  }

  AtlasArpeggiator::Settings arpTestSettings() {
    AtlasArpeggiator::Settings settings;
    settings.enabled = true;
    settings.time_mode = AtlasArpeggiator::kSync;
    settings.sync_rate = arpSyncRateIndex("1/8");
    settings.style = AtlasArpeggiator::kUp;
    settings.gate = 80.0f;
    settings.velocity = 100;
    return settings;
  }

  std::multimap<int, MidiMessage> arpChord(int sample) {
    return {
      { sample, MidiMessage::noteOn(1, 60, 1.0f) },
      { sample, MidiMessage::noteOn(1, 64, 1.0f) },
      { sample, MidiMessage::noteOn(1, 67, 1.0f) }
    };
  }

  void expectArpSteps(const std::vector<ArpEvent>& note_ons, const std::vector<int>& notes,
                      const std::vector<int>& samples, const std::string& message) {
    bool matches = note_ons.size() >= notes.size();
    std::string found;
    for (size_t i = 0; i < note_ons.size() && i < notes.size() + 2; ++i)
      found += " " + std::to_string(note_ons[i].message.getNoteNumber()) + "@" + std::to_string(note_ons[i].sample);
    for (size_t i = 0; matches && i < notes.size(); ++i) {
      matches = note_ons[i].message.getNoteNumber() == notes[i] &&
                std::abs(note_ons[i].sample - samples[i]) <= 1;
    }
    expect(matches, message + " (found" + found + ")");
  }

  void testArpNoteOffKeepsGate() {
    for (int retrigger = 0; retrigger < AtlasArpeggiator::kNumRetriggerModes; ++retrigger) {
      auto settings = arpTestSettings();
      settings.retrigger = retrigger;
      const int step = static_cast<int>(kArpTestSamplesPerQuarter * 0.5);
      const int gate = static_cast<int>(step * 0.8);
      auto input = arpChord(0);
      // Release the chord's notes one by one while the first step sounds.
      input.insert({ 1000, MidiMessage::noteOff(1, 64) });
      input.insert({ 2000, MidiMessage::noteOff(1, 60) });
      const auto events = runArpeggiator(settings, input, step * 3);

      int first_off = -1;
      for (const auto& event : events) {
        if (event.message.isNoteOff() && event.message.getNoteNumber() == 60) {
          first_off = event.sample;
          break;
        }
      }
      expect(std::abs(first_off - gate) <= 1,
             "Releasing keys must not cut the sounding arp step short (retrigger " + std::to_string(retrigger) +
             ", note-off at " + std::to_string(first_off) + ", gate " + std::to_string(gate) + ")");
      const auto note_ons = arpNoteOns(events);
      expect(note_ons.size() >= 2 && note_ons[1].message.getNoteNumber() == 67,
             "After releasing keys the arp plays only the notes still held");
    }

    // Releasing Hold drops the latched notes without cutting the step either.
    AtlasArpeggiator arp;
    arp.prepare(kArpTestSampleRate);
    auto settings = arpTestSettings();
    settings.hold = true;
    MidiBuffer chord;
    chord.addEvent(MidiMessage::noteOn(1, 60, 1.0f), 0);
    chord.addEvent(MidiMessage::noteOff(1, 60), 0);
    MidiBuffer output;
    arp.process(chord, output, 512, kArpTestBpm, 0.0, false, settings);
    settings.hold = false;
    MidiBuffer released;
    arp.process(MidiBuffer(), released, 512, kArpTestBpm, 0.0, false, settings);
    bool cut = false;
    for (const auto metadata : released)
      cut = cut || metadata.getMessage().isNoteOff();
    expect(!cut, "Releasing Hold must let the sounding arp step finish its gate");
  }

  void testArpOffStartsAtTheNote() {
    auto settings = arpTestSettings();
    settings.retrigger = AtlasArpeggiator::kRetriggerOff;
    const int step = static_cast<int>(kArpTestSamplesPerQuarter * 0.5);
    // Transport playing from 0.3 quarters: Off ignores the host grid.
    const auto note_ons = arpNoteOns(runArpeggiator(settings, arpChord(0), step * 5, 0.3));
    expectArpSteps(note_ons, { 60, 64, 67, 60, 64 }, { 0, step, 2 * step, 3 * step, 4 * step },
                   "Off retrigger runs the pattern from the first note without restarting");
  }

  void testArpBeatRetriggerFollowsTransport() {
    auto settings = arpTestSettings();
    settings.retrigger = AtlasArpeggiator::kRetriggerBeat;
    settings.retrigger_division = arpRetriggerDivisionIndex("1/4");
    const double quarter = kArpTestSamplesPerQuarter;
    // Transport at 0.3 quarters: steps land on the host's 1/8 grid (0.5, 1.0,
    // 1.5 ...) and the pattern restarts on every quarter note.
    const auto note_ons = arpNoteOns(runArpeggiator(settings, arpChord(0), static_cast<int>(quarter * 3.2), 0.3));
    auto at = [quarter](double ppq) { return static_cast<int>(std::round((ppq - 0.3) * quarter)); };
    expectArpSteps(note_ons, { 60, 60, 64, 60, 64, 60 },
                   { at(0.5), at(1.0), at(1.5), at(2.0), at(2.5), at(3.0) },
                   "Beat retrigger restarts the pattern on each quarter of the host transport");

    // One bar division with a 1/4 dotted rate: the grid restarts on the bar.
    settings.retrigger_division = arpRetriggerDivisionIndex("1 Bar");
    settings.sync_rate = arpSyncRateIndex("1/4 Dotted");
    const auto dotted = arpNoteOns(runArpeggiator(settings, arpChord(0), static_cast<int>(quarter * 4.6), 3.9));
    auto at_bar = [quarter](double ppq) { return static_cast<int>(std::round((ppq - 3.9) * quarter)); };
    expectArpSteps(dotted, { 60, 64, 67 }, { at_bar(4.0), at_bar(5.5), at_bar(7.0) },
                   "Beat retrigger starts the pattern on the bar line");
    expect(dotted.size() >= 4 && dotted[3].message.getNoteNumber() == 60 &&
           std::abs(dotted[3].sample - at_bar(8.0)) <= 1,
           "A rate that does not divide the bar restarts its grid on the next bar");
  }

  void testArpBeatRetriggerWithTransportStopped() {
    auto settings = arpTestSettings();
    settings.retrigger = AtlasArpeggiator::kRetriggerBeat;
    settings.retrigger_division = arpRetriggerDivisionIndex("1/4");
    const int step = static_cast<int>(kArpTestSamplesPerQuarter * 0.5);
    const auto note_ons = arpNoteOns(runArpeggiator(settings, arpChord(100), 100 + step * 5));
    expectArpSteps(note_ons, { 60, 64, 60, 64, 60 },
                   { 100, 100 + step, 100 + 2 * step, 100 + 3 * step, 100 + 4 * step },
                   "With the transport stopped, Beat retrigger counts divisions from the first note");

    // Free rate: 3 Hz steps with a quarter (0.5 s) division. The step clock
    // restarts on each division boundary.
    settings.time_mode = AtlasArpeggiator::kFree;
    settings.free_rate = 3.0f;
    const auto free_note_ons = arpNoteOns(runArpeggiator(settings, arpChord(0), 50000));
    expectArpSteps(free_note_ons, { 60, 64, 60, 64, 60 }, { 0, 16000, 24000, 40000, 48000 },
                   "Free rate Beat retrigger restarts the step clock on each division");
  }

  void testArpPlayOrder() {
    const int step = static_cast<int>(kArpTestSamplesPerQuarter * 0.5);
    const std::vector<int> times { 0, step, 2 * step, 3 * step, 4 * step, 5 * step };
    // Pressed G, then C, then E in the same block.
    const std::multimap<int, MidiMessage> played {
      { 0, MidiMessage::noteOn(1, 67, 1.0f) },
      { 0, MidiMessage::noteOn(1, 60, 1.0f) },
      { 0, MidiMessage::noteOn(1, 64, 1.0f) }
    };
    auto settings = arpTestSettings();
    expectArpSteps(arpNoteOns(runArpeggiator(settings, played, step * 4)), { 60, 64, 67, 60 },
                   { times[0], times[1], times[2], times[3] }, "Pitch play order walks low to high");

    settings.play_order = AtlasArpeggiator::kPlayOrderAsPlayed;
    expectArpSteps(arpNoteOns(runArpeggiator(settings, played, step * 4)), { 67, 60, 64, 67 },
                   { times[0], times[1], times[2], times[3] }, "As Played starts with the first key pressed");

    settings.style = AtlasArpeggiator::kDown;
    expectArpSteps(arpNoteOns(runArpeggiator(settings, played, step * 3)), { 64, 60, 67 },
                   { times[0], times[1], times[2] }, "As Played with Down walks back from the last key pressed");

    settings.style = AtlasArpeggiator::kUp;
    settings.steps = 2;
    settings.distance = 12;
    expectArpSteps(arpNoteOns(runArpeggiator(settings, played, step * 6)), { 67, 60, 64, 79, 72, 76 }, times,
                   "As Played repeats the pressed order in each transposed step");

    // A key added later joins the end of the order.
    settings.steps = 1;
    auto added = played;
    added.insert({ step * 3 + 100, MidiMessage::noteOn(1, 62, 1.0f) });
    settings.retrigger = AtlasArpeggiator::kRetriggerOff;
    // The fifth step is pattern index 4, which wraps to the first key of the four.
    expectArpSteps(arpNoteOns(runArpeggiator(settings, added, step * 5)), { 67, 60, 64, 67, 67 },
                   { times[0], times[1], times[2], times[3], times[4] }, "As Played keeps its order as a key is added");
    const auto with_added = arpNoteOns(runArpeggiator(settings, added, step * 8));
    expect(with_added.size() >= 8 && with_added[7].message.getNoteNumber() == 62,
           "A key pressed later plays after the keys pressed before it");
  }

  void testArpLoop() {
    const int step = static_cast<int>(kArpTestSamplesPerQuarter * 0.5);
    auto settings = arpTestSettings();
    expect(settings.loop == AtlasArpeggiator::kLoopInfinite, "Loop defaults to infinite");
    expect(arpNoteOns(runArpeggiator(settings, arpChord(0), step * 10)).size() == 10,
           "Infinite loop keeps playing the pattern");

    // Loop value 1 plays the pattern twice, then the arp falls silent.
    settings.loop = 1;
    const auto twice = arpNoteOns(runArpeggiator(settings, arpChord(0), step * 10));
    expect(twice.size() == 6, "Loop 2 plays a three-note pattern twice (found " +
                              std::to_string(twice.size()) + " notes)");
    expectArpSteps(twice, { 60, 64, 67, 60, 64, 67 }, { 0, step, 2 * step, 3 * step, 4 * step, 5 * step },
                   "Loop 2 plays the whole pattern each time");

    // A pass includes every repeat of each note.
    settings.loop = 0;
    settings.repeats = 2;
    expect(arpNoteOns(runArpeggiator(settings, arpChord(0), step * 10)).size() == 6,
           "Loop counts a pass with every repeat");
    settings.repeats = 1;

    // Up Down plays 60 64 67 64 in one pass.
    settings.style = AtlasArpeggiator::kUpDown;
    const auto up_down = arpNoteOns(runArpeggiator(settings, arpChord(0), step * 10));
    expectArpSteps(up_down, { 60, 64, 67, 64 }, { 0, step, 2 * step, 3 * step }, "Loop 1 plays one Up Down pass");
    expect(up_down.size() == 4, "Loop 1 stops after one Up Down pass");

    settings.style = AtlasArpeggiator::kChordTrigger;
    settings.loop = 2;
    expect(arpNoteOns(runArpeggiator(settings, arpChord(0), step * 10)).size() == 9,
           "Chord trigger plays the chord once per loop");
    settings.style = AtlasArpeggiator::kUp;

    // A new note under Note retrigger restarts the counted pattern.
    settings.loop = 0;
    auto replayed = arpChord(0);
    replayed.insert({ step * 5, MidiMessage::noteOn(1, 72, 1.0f) });
    const auto restarted = arpNoteOns(runArpeggiator(settings, replayed, step * 12));
    expect(restarted.size() == 7, "Note retrigger restarts a finished loop (found " +
                                  std::to_string(restarted.size()) + " notes)");

    // Beat retrigger restarts it on each division: one pass per bar.
    settings.retrigger = AtlasArpeggiator::kRetriggerBeat;
    settings.retrigger_division = arpRetriggerDivisionIndex("1 Bar");
    const auto per_bar = arpNoteOns(runArpeggiator(settings, arpChord(0), step * 16, 0.0));
    expectArpSteps(per_bar, { 60, 64, 67, 60, 64, 67 }, { 0, step, 2 * step, 8 * step, 9 * step, 10 * step },
                   "Beat retrigger plays the looped pattern again on each division");
    expect(per_bar.size() == 6, "Beat retrigger with Loop 1 plays one pass per bar");

    // Off: the pattern restarts after every key is released and pressed again.
    settings.retrigger = AtlasArpeggiator::kRetriggerOff;
    auto again = arpChord(0);
    for (int note : { 60, 64, 67 })
      again.insert({ step * 5, MidiMessage::noteOff(1, note) });
    for (const auto& event : arpChord(step * 6))
      again.insert(event);
    expect(arpNoteOns(runArpeggiator(settings, again, step * 12)).size() == 6,
           "Off retrigger restarts a finished loop on a new chord after release");
  }

  void testArpRetriggerDivisionParameter() {
    SynthPlugin synth;
    auto& controls = synth.getControls();
    for (const char* name : { "arp_1_retrigger_division", "arp_2_retrigger_division" }) {
      const auto found = controls.find(name);
      expect(found != controls.end(), std::string(name) + " exists");
      if (found != controls.end())
        close(found->second->value(), static_cast<float>(AtlasArpeggiator::kDefaultRetriggerDivision),
              std::string(name) + " defaults to one bar");
    }
    expect(static_cast<int>(AtlasArpeggiator::retriggerDivisions().size()) ==
           static_cast<int>(vital::Parameters::getDetails("arp_1_retrigger_division").max) + 1,
           "Every retrigger division has a parameter value");
    for (size_t i = 0; i < AtlasArpeggiator::retriggerDivisions().size(); ++i) {
      expect(strings::kArpRetriggerDivisionNames[i] == AtlasArpeggiator::retriggerDivisions()[i].name,
             "Retrigger division names match the arpeggiator's divisions");
    }
    expect(String(AtlasArpeggiator::retriggerDivisions()[AtlasArpeggiator::kDefaultRetriggerDivision].name) == "1 Bar",
           "The default retrigger division is one bar");
    for (const char* name : { "arp_1_play_order", "arp_2_play_order" }) {
      const auto found = controls.find(name);
      expect(found != controls.end(), std::string(name) + " exists");
      if (found != controls.end())
        close(found->second->value(), static_cast<float>(AtlasArpeggiator::kPlayOrderPitch),
              std::string(name) + " defaults to pitch order");
    }
    expect(static_cast<int>(vital::Parameters::getDetails("arp_1_play_order").max) + 1 ==
           AtlasArpeggiator::kNumPlayOrders, "Every play order has a parameter value");
    for (const char* name : { "arp_1_loop", "arp_2_loop" }) {
      const auto found = controls.find(name);
      expect(found != controls.end(), std::string(name) + " exists");
      if (found != controls.end())
        close(found->second->value(), static_cast<float>(AtlasArpeggiator::kLoopInfinite),
              std::string(name) + " defaults to infinite");
    }
    expect(static_cast<int>(vital::Parameters::getDetails("arp_1_loop").max) == AtlasArpeggiator::kLoopInfinite,
           "The last loop value is infinite");
    expect(strings::kArpLoopNames[0] == "1" && strings::kArpLoopNames[AtlasArpeggiator::kLoopInfinite] == "Infinite",
           "Loop values read from 1 to Infinite");
  }
}
