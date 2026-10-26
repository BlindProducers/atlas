#include "JuceHeader.h"
#include "slot_test_ids.h"
#include "synth_plugin.h"
#include "synth_editor.h"
#include "sound_engine.h"
#include "modulation_connection_processor.h"
#include "value_bridge.h"
#include "sample_source.h"
#include "source_types.h"
#include "random_lfo.h"
#include "synth_lfo.h"
#include "synth_strings.h"
#include "file_source.h"
#include "line_generator.h"
#include "load_save.h"
#include "community_library.h"
#include "credential_store.h"
#include "phase_modifier.h"
#include "wave_source.h"
#include "wavetable_creator.h"
#include "wavetable_edit_model.h"

#include <array>
#include <cmath>
#include <iostream>

#if JUCE_MAC
bool cpuMeterNativeAccessibilityMatches(juce::AccessibilityHandler& handler);
#endif

namespace {
  int failures = 0;

  void expect(bool condition, const std::string& message) {
    if (!condition) {
      std::cerr << "FAIL: " << message << '\n';
      ++failures;
    }
  }

  void close(float actual, float expected, const std::string& message, float tolerance = 1.0e-4f) {
    expect(std::isfinite(actual) && std::abs(actual - expected) <= tolerance,
           message + " (actual=" + std::to_string(actual) + ", expected=" + std::to_string(expected) + ")");
  }

  ValueBridge* parameter(SynthPlugin& synth, const std::string& name) {
    for (auto* candidate : synth.getParameters()) {
      auto* bridge = dynamic_cast<ValueBridge*>(candidate);
      if (bridge && bridge->getParameterId().toStdString() == name)
        return bridge;
    }
    return nullptr;
  }

  Component* titled(Component& parent, const String& title) {
    if (parent.getTitle() == title)
      return &parent;
    for (auto* child : parent.getChildren()) {
      if (auto* found = titled(*child, title))
        return found;
    }
    return nullptr;
  }

  void prepare(SynthPlugin& synth, int oversampling = 0) {
    synth.setRateAndBufferSizeDetails(48000.0, 128);
    synth.prepareToPlay(48000.0, 128);
    auto& controls = synth.getControls();
    controls.at("oversampling")->set(oversampling);
    synth.checkOversampling();
    controls.at("osc_1_random_phase")->set(0.0f);
    // The comparisons were calibrated with the oscillator starting at 180 degrees.
    controls.at("osc_1_phase")->set(0.5f);
    controls.at("osc_1_unison_voices")->set(1.0f);
    controls.at("env_1_attack")->set(0.001f);
    controls.at("env_1_decay")->set(0.001f);
    controls.at("env_1_sustain")->set(1.0f);
  }

  void render(SynthPlugin& synth, int samples, bool note = false) {
    while (samples > 0) {
      const int block = std::min(128, samples);
      AudioBuffer<float> audio(2, block);
      MidiBuffer midi;
      if (note)
        midi.addEvent(MidiMessage::noteOn(1, 48, 1.0f), 0);
      synth.processBlock(audio, midi);
      note = false;
      samples -= block;
    }
  }

  vital::ModulationConnection* connect(SynthPlugin& synth, const std::string& source,
                                       const std::string& destination, float amount = 1.0f) {
    expect(synth.connectModulation(source, destination), "create " + source + " -> " + destination);
    const int index = synth.getConnectionIndex(source, destination);
    auto* connection = index >= 0 ? synth.getEngine()->getModulationBank().atIndex(index) : nullptr;
    if (connection) {
      const auto prefix = "modulation_" + std::to_string(connection->modulation_processor->index() + 1);
      synth.getControls().at(prefix + "_amount")->set(amount);
      synth.getControls().at(prefix + "_bipolar")->set(0.0f);
    }
    render(synth, 1);
    return connection;
  }


  void renderMidi(SynthPlugin& synth, MidiBuffer& midi, int block = 128) {
    AudioBuffer<float> audio(2, block);
    synth.processBlock(audio, midi);
  }

  // A synced LFO on pitch, notes played at unrelated times. Trigger restarts
  // with every note; Sync follows the song with the transport running and,
  // with it stopped, restarts on the first note played from silence.
  struct TestPlayHead : public AudioPlayHead {
    bool playing = false; double bpm = 120.0; double ppq = 7.25;
    Optional<PositionInfo> getPosition() const override {
      PositionInfo info;
      info.setIsPlaying(playing); info.setBpm(bpm); info.setPpqPosition(ppq);
      info.setTimeInSeconds(ppq * 60.0 / bpm);
      return info;
    }
  };

  std::vector<std::vector<double>> lfoPitchCurves(int sync_type, bool playing, const std::vector<double>& starts,
                                                  double length, double* origin_after_overlap = nullptr) {
    SynthPlugin synth;
    TestPlayHead head;
    head.playing = playing;
    prepare(synth);
    synth.setPlayHead(&head);
    auto& controls = synth.getControls();
    controls.at("env_1_release")->set(0.05f);
    controls.at("lfo_1_sync")->set(1.0f);        // tempo
    controls.at("lfo_1_tempo")->set(8.0f);       // 1/4
    controls.at("lfo_1_sync_type")->set(float(sync_type));
    synth.connectModulation("lfo_1", "osc_1_transpose");
    const int connection = synth.getConnectionIndex("lfo_1", "osc_1_transpose");
    controls.at("modulation_" + std::to_string(connection + 1) + "_amount")->set(12.0f / 96.0f);
    controls.at("modulation_" + std::to_string(connection + 1) + "_bipolar")->set(1.0f);
    std::vector<float> audio;
    double end = 0.0;
    for (double start : starts) end = std::max(end, start + length + 0.3);
    const int total = int(end * 48000);
    for (int rendered = 0; rendered < total; rendered += 128) {
      MidiBuffer midi;
      for (size_t n = 0; n < starts.size(); ++n) {
        const int on = int(starts[n] * 48000), off = int((starts[n] + length) * 48000);
        if (off >= rendered && off < rendered + 128) midi.addEvent(MidiMessage::noteOff(1, 60 + int(n) * 7), off - rendered);
        if (on >= rendered && on < rendered + 128) midi.addEvent(MidiMessage::noteOn(1, 60 + int(n) * 7, 0.8f), on - rendered);
      }
      AudioBuffer<float> buffer(2, 128);
      synth.processBlock(buffer, midi);
      for (int i = 0; i < 128; ++i) audio.push_back(synth.getEngine()->output()->buffer[i][0]);
      if (playing) head.ppq += 128.0 / 48000.0 * head.bpm / 60.0;
      if (origin_after_overlap && starts.size() > 1 && rendered <= int(starts[1] * 48000) &&
          int(starts[1] * 48000) < rendered + 128)
        *origin_after_overlap = synth.getEngine()->voiceClockOrigin();
    }
    std::vector<std::vector<double>> curves;
    for (size_t n = 0; n < starts.size(); ++n) {
      std::vector<double> curve;
      const int on = int(starts[n] * 48000);
      const double base = 261.6256 * std::pow(2.0, int(n) * 7 / 12.0);
      for (int ms = 0; ms < 400; ms += 25) {
        const int from = on + ms * 48 + 48, to = from + 960;
        int crossings = 0, first = -1, last = -1;
        for (int i = from + 1; i < to && i < int(audio.size()); ++i)
          if (audio[size_t(i - 1)] <= 0.0f && audio[size_t(i)] > 0.0f) { ++crossings; if (first < 0) first = i; last = i; }
        const double hz = crossings > 1 ? 48000.0 * (crossings - 1) / std::max(1, last - first) : 0.0;
        curve.push_back(hz > 0.0 ? 12.0 * std::log2(hz / base) : 99.0);
      }
      curves.push_back(curve);
    }
    return curves;
  }

  double curveDifference(const std::vector<double>& a, const std::vector<double>& b) {
    double worst = 0.0;
    for (size_t i = 0; i < a.size(); ++i) worst = std::max(worst, std::abs(a[i] - b[i]));
    return worst;
  }

  void testSyncedLfoNotesWithTransportStoppedAndPlaying() {
    const std::vector<double> starts { 0.37, 1.81 };
    auto trigger_stopped = lfoPitchCurves(0, false, starts, 0.45);
    expect(curveDifference(trigger_stopped[0], trigger_stopped[1]) < 0.3,
           "Trigger restarts the LFO with every note while the transport is stopped");
    expect(trigger_stopped[0].front() < -4.5, "the pitch LFO starts at the bottom of its cycle");
    auto trigger_playing = lfoPitchCurves(0, true, starts, 0.45);
    expect(curveDifference(trigger_playing[0], trigger_playing[1]) < 0.3,
           "Trigger restarts the LFO with every note while the transport runs");
    auto sync_stopped = lfoPitchCurves(1, false, starts, 0.45);
    expect(curveDifference(sync_stopped[0], sync_stopped[1]) < 0.3,
           "with the transport stopped, a synced LFO starts with the first note played from silence");
    expect(curveDifference(sync_stopped[0], trigger_stopped[0]) < 0.3,
           "that first note hears the LFO from the start of its cycle");
    auto sync_playing = lfoPitchCurves(1, true, starts, 0.45);
    expect(curveDifference(sync_playing[0], sync_playing[1]) > 2.0,
           "with the transport running, a synced LFO follows the song position, not the note");
    double origin = -1.0;
    lfoPitchCurves(1, false, { 0.37, 0.61 }, 0.6, &origin);
    expect(std::abs(origin - 0.37) < 0.01,
           "a note played while another sounds joins the running clock instead of restarting it");
  }

  // Free Running keeps one LFO phase through every note and the gaps between
  // them, transport or not: notes two cycles apart hear the same curve, notes
  // two and a half apart hear it half a cycle out.
  void testFreeRunningLfoJoinsNotesWherever() {
    for (bool playing : { false, true }) {
      const std::string transport = playing ? " with the transport running" : " with the transport stopped";
      auto whole = lfoPitchCurves(vital::SynthLfo::kFreeRunning, playing, { 0.37, 1.37 }, 0.45);
      expect(curveDifference(whole[0], whole[1]) < 0.3,
             "a free-running LFO is where it would be two cycles on" + transport);
      auto half = lfoPitchCurves(vital::SynthLfo::kFreeRunning, playing, { 0.37, 1.62 }, 0.45);
      expect(curveDifference(half[0], half[1]) > 4.0,
             "a free-running LFO does not restart with a note" + transport);
    }
  }

  // An LFO or random source on an effect keeps moving after the last voice
  // has released when it runs freely; Trigger holds its last value.
  void testFreeRunningSourcesOutliveTheirVoices() {
    for (const std::string source : { "lfo_1", "random_1" }) {
      for (bool free_running : { false, true }) {
        SynthPlugin synth;
        prepare(synth);
        auto& controls = synth.getControls();
        controls.at("env_1_release")->set(0.05f);
        controls.at(source + "_frequency")->set(2.0f);
        controls.at(source + "_sync")->set(0.0f);   // seconds
        const float mode = source == "lfo_1" ? float(vital::SynthLfo::kFreeRunning)
                                             : float(vital::RandomLfo::kFreeRunning);
        controls.at(source + "_sync_type")->set(free_running ? mode : 0.0f);
        auto* route = connect(synth, source, "delay_dry_wet");
        expect(route != nullptr, "connect " + source + " to an effect");
        if (route == nullptr)
          continue;

        MidiBuffer on;
        on.addEvent(MidiMessage::noteOn(1, 60, 0.8f), 0);
        renderMidi(synth, on);
        for (int block = 0; block < 40; ++block) {
          MidiBuffer none;
          renderMidi(synth, none);
        }
        MidiBuffer off;
        off.addEvent(MidiMessage::noteOff(1, 60), 0);
        renderMidi(synth, off);
        for (int block = 0; block < 75; ++block) {
          MidiBuffer none;
          renderMidi(synth, none);
        }
        expect(synth.getEngine()->getNumActiveVoices() == 0, source + ": the voice has released");

        float lowest = 1.0e9f, highest = -1.0e9f;
        for (int block = 0; block < 375; ++block) {
          MidiBuffer none;
          renderMidi(synth, none);
          const float value = route->modulation_processor->output()->buffer[0][0];
          lowest = std::min(lowest, value);
          highest = std::max(highest, value);
        }
        if (free_running)
          expect(highest - lowest > 0.2f, source + ": a free-running source keeps moving after its voice ends");
        else
          expect(highest - lowest < 1.0e-4f, source + ": a triggered source holds after its voice ends");
      }
    }
  }

  // As in a preset made on a Mac: LFO 5 holds 0.5, then 1, then 0 across two
  // bars and pushes LFO 3's Tempo (1/8) up by that much of a division. As on
  // the arm64 build, LFO 3 runs at 1/16 only while the push is 1. x86 builds
  // used to round 0.5 up too and ran at 1/16 for seven of the eight beats.
  void testLfoTempoModulatedByAnotherLfo() {
    SynthPlugin synth;
    prepare(synth);
    auto& controls = synth.getControls();
    controls.at("lfo_3_sync")->set(1.0f);        // tempo
    controls.at("lfo_3_tempo")->set(9.0f);       // 1/8
    controls.at("lfo_5_sync")->set(1.0f);
    controls.at("lfo_5_tempo")->set(5.0f);       // 2/1: one cycle every two bars
    synth.getLfoSource(4)->jsonToState({
      { "num_points", 4 },
      { "points", { 0.0f, 0.5f, 0.75f, 0.0f, 0.875f, 1.0f, 1.0f, 0.5f } },
      { "powers", { 20.0f, 20.0f, 20.0f, 0.0f } },   // held steps
      { "smooth", false }
    });
    connect(synth, "lfo_5", "lfo_3_tempo", 1.0f / 12.0f);   // full scale is one division
    connect(synth, "lfo_3", "osc_1_wave_frame");            // an LFO runs only while it modulates something

    const float beats_per_second = controls.at("beats_per_minute")->value();
    const float eighth = vital::constants::kSyncedFrequencyRatios[9] * beats_per_second;
    const float sixteenth = vital::constants::kSyncedFrequencyRatios[10] * beats_per_second;
    const int blocks = static_cast<int>(8.0f / beats_per_second * 48000.0f) / 128;
    int at_eighth = 0, at_sixteenth = 0;
    for (int block = 0; block < blocks; ++block) {
      MidiBuffer midi;
      if (block == 0)
        midi.addEvent(MidiMessage::noteOn(1, 48, 1.0f), 0);
      renderMidi(synth, midi);
      const float frequency = synth.getStatusOutput("lfo_3_frequency")->value()[0];
      at_eighth += std::abs(frequency - eighth) < 1.0e-3f ? 1 : 0;
      at_sixteenth += std::abs(frequency - sixteenth) < 1.0e-3f ? 1 : 0;
    }
    expect(at_eighth + at_sixteenth == blocks, "LFO 3 runs at 1/8 or 1/16 throughout");
    close(at_sixteenth / static_cast<float>(blocks), 0.125f,
          "LFO 3 runs at 1/16 only for the beat LFO 5 holds 1", 0.02f);
  }

  // An LFO's Simple view plays a standard shape bent by Morph in place of its
  // custom shape, with only its own rate and Trigger Mode.
  void testSimpleLfoShapes() {
    using vital::SynthLfo;
    auto near = [](float a, float b) { return std::abs(a - b) < 1.0e-3f; };
    expect(near(SynthLfo::simpleShapeValue(SynthLfo::kSimpleSine, 0.0f, 0.0f), 0.0f) &&
           near(SynthLfo::simpleShapeValue(SynthLfo::kSimpleSine, 0.5f, 0.0f), 1.0f) &&
           near(SynthLfo::simpleShapeValue(SynthLfo::kSimpleSine, 0.25f, 0.0f), 0.5f),
           "Sine starts at the bottom and peaks halfway, as the custom Sin does");
    expect(near(SynthLfo::simpleShapeValue(SynthLfo::kSimpleSine, 0.125f, -1.0f), 0.25f),
           "Sine at Morph -100% is a triangle");
    expect(SynthLfo::simpleShapeValue(SynthLfo::kSimpleSine, 0.125f, 1.0f) > 0.0f &&
           SynthLfo::simpleShapeValue(SynthLfo::kSimpleSine, 0.375f, 1.0f) > 0.95f,
           "Sine at Morph 100% is nearly square");
    expect(near(SynthLfo::simpleShapeValue(SynthLfo::kSimpleTriangle, 0.25f, 0.0f), 0.5f) &&
           near(SynthLfo::simpleShapeValue(SynthLfo::kSimpleTriangle, 0.5f, 0.0f), 1.0f),
           "Triangle rises to its peak halfway");
    expect(near(SynthLfo::simpleShapeValue(SynthLfo::kSimpleTriangle, 0.5f, 1.0f), 0.5f / 0.99f),
           "Triangle at Morph 100% rises all cycle, as a saw");
    expect(near(SynthLfo::simpleShapeValue(SynthLfo::kSimpleSawUp, 0.3f, 0.0f), 0.3f) &&
           near(SynthLfo::simpleShapeValue(SynthLfo::kSimpleSawDown, 0.3f, 0.0f), 0.7f),
           "the saws ramp straight at Morph 0");
    expect(SynthLfo::simpleShapeValue(SynthLfo::kSimpleSawUp, 0.3f, 0.5f) > 0.3f &&
           SynthLfo::simpleShapeValue(SynthLfo::kSimpleSawUp, 0.3f, -0.5f) < 0.3f,
           "Morph curves the saws");
    expect(SynthLfo::simpleShapeValue(SynthLfo::kSimpleSquare, 0.49f, 0.0f) == 1.0f &&
           SynthLfo::simpleShapeValue(SynthLfo::kSimpleSquare, 0.51f, 0.0f) == 0.0f &&
           SynthLfo::simpleShapeValue(SynthLfo::kSimpleSquare, 0.6f, 0.5f) == 1.0f,
           "Square is high for the first half, and Morph widens it");

    // The default custom shape is a triangle; the Simple view's square only
    // ever sits at its two levels. Free Running runs it without a note.
    for (bool simple : { false, true }) {
      SynthPlugin synth;
      prepare(synth);
      auto& controls = synth.getControls();
      controls.at("lfo_1_sync")->set(0.0f);
      controls.at("lfo_1_frequency")->set(2.0f);
      controls.at("lfo_1_sync_type")->set(float(SynthLfo::kFreeRunning));
      controls.at("lfo_1_simple_trigger")->set(float(SynthLfo::kSimpleFreeRunning));
      controls.at("lfo_1_simple_shape")->set(float(SynthLfo::kSimpleSquare));
      controls.at("lfo_1_view")->set(simple ? float(SynthLfo::kSimpleView) : float(SynthLfo::kCustomView));
      // The Simple view leaves the custom view's 10x Rate alone.
      controls.at("lfo_1_rate_x10")->set(simple ? 1.0f : 0.0f);
      auto* route = connect(synth, "lfo_1", "delay_dry_wet");
      if (route == nullptr)
        continue;
      std::vector<float> values;
      for (int block = 0; block < 750; ++block) {
        MidiBuffer none;
        renderMidi(synth, none);
        values.push_back(route->modulation_processor->output()->buffer[0][0]);
      }
      const auto [lowest, highest] = std::minmax_element(values.begin(), values.end());
      const float range = *highest - *lowest;
      int between = 0, rises = 0;
      for (size_t i = 0; i < values.size(); ++i) {
        const float level = range > 0.0f ? (values[i] - *lowest) / range : 0.0f;
        between += level > 0.1f && level < 0.9f;
        rises += i > 0 && values[i] > values[i - 1] + 0.5f * range;
      }
      const std::string view = simple ? "the Simple view" : "the Custom view";
      expect(range > 0.2f, view + " moves its route");
      if (simple) {
        expect(between < 5, "the Simple view plays its square, not the custom triangle");
        // Frequency 2 is 4 Hz: about eight cycles in two seconds, not eighty.
        expect(rises >= 7 && rises <= 9, "the Simple view runs at its rate without 10x Rate");
      }
      else
        expect(between > int(values.size()) / 2, "the Custom view plays its own triangle");
    }
  }

  void testSameSampleNoteOnOffDoesNotStick() {
    SynthPlugin synth;
    prepare(synth);
    MidiBuffer midi;
    midi.addEvent(MidiMessage::noteOn(1, 60, 1.0f), 10);
    midi.addEvent(MidiMessage::noteOff(1, 60), 10);
    renderMidi(synth, midi);
    MidiBuffer empty;
    renderMidi(synth, empty);
    expect(synth.getEngine()->getNumPressedNotes() == 0,
           "A zero-length host note (on then off at one sample) must not stay pressed");
  }

  // A wheel on a parameter outside the voices (an effect's mix, the volume)
  // used to move it only while a note played: the route read the wheel from
  // a voice, and stayed where the last voice left it once the notes ended.
  void testWheelsMoveGlobalParametersWithoutNotes() {
    SynthPlugin synth;
    prepare(synth);
    auto& controls = synth.getControls();
    const auto settled = [&](const std::string& destination) {
      render(synth, 24000);
      return synth.getEngine()->getMonoModulations().at(destination)->buffer[0][0];
    };
    const auto range = [](const std::string& name) {
      const auto& details = vital::Parameters::getDetails(name);
      return details.max - details.min;
    };

    const float volume = controls.at("volume")->value();
    connect(synth, "mod_wheel", "volume", 0.5f);
    MidiBuffer wheel_up;
    wheel_up.addEvent(MidiMessage::controllerEvent(1, 1, 127), 0);
    renderMidi(synth, wheel_up);
    close(settled("volume"), volume + 0.5f * range("volume"),
          "the mod wheel moves a global parameter with no note playing", 0.01f * range("volume"));

    // After a note has ended there is no voice to read the wheel from.
    render(synth, 4800, true);
    MidiBuffer release;
    release.addEvent(MidiMessage::noteOff(1, 48), 0);
    renderMidi(synth, release);
    render(synth, 96000);
    expect(synth.getEngine()->getNumActiveVoices() == 0, "the note has ended");
    MidiBuffer wheel_down;
    wheel_down.addEvent(MidiMessage::controllerEvent(1, 1, 0), 0);
    renderMidi(synth, wheel_down);
    close(settled("volume"), volume, "the mod wheel keeps moving a global parameter after the notes end",
          0.01f * range("volume"));

    const float routing = controls.at("stereo_routing")->value();
    connect(synth, "pitch_wheel", "stereo_routing", 0.5f);
    MidiBuffer bend;
    bend.addEvent(MidiMessage::pitchWheel(1, 16383), 0);
    renderMidi(synth, bend);
    close(settled("stereo_routing"), routing + 0.5f * range("stereo_routing"),
          "the pitch wheel moves a global parameter with no note playing", 0.01f * range("stereo_routing"));

    // A per-voice parameter still takes each voice's own wheel.
    auto* per_voice = connect(synth, "mod_wheel", "filter_1_cutoff");
    expect(per_voice != nullptr && per_voice->modulation_processor->isPolyphonicModulation(),
           "the mod wheel stays a per-voice source for a per-voice parameter");
  }

  void testArpeggiatorEnableAndDisableReleaseNotes() {
    SynthPlugin synth;
    prepare(synth);
    auto& controls = synth.getControls();

    // Key pressed with the arp off, released after it is switched on.
    MidiBuffer midi;
    midi.addEvent(MidiMessage::noteOn(1, 60, 1.0f), 0);
    renderMidi(synth, midi);
    controls.at("arp_1_on")->set(1.0f);
    MidiBuffer release;
    release.addEvent(MidiMessage::noteOff(1, 60), 0);
    renderMidi(synth, release);
    MidiBuffer empty;
    for (int i = 0; i < 4; ++i)
      renderMidi(synth, empty);
    expect(synth.getEngine()->getNumPressedNotes() == 0,
           "Releasing a key held from before the arpeggiator was enabled must release its voice");

    // Arp playing a held key, then switched off while its step note sounds.
    controls.at("arp_1_free_rate")->set(20.0f);
    controls.at("arp_1_time_mode")->set(1.0f);
    MidiBuffer hold;
    hold.addEvent(MidiMessage::noteOn(1, 64, 1.0f), 0);
    renderMidi(synth, hold);
    for (int i = 0; i < 4; ++i)
      renderMidi(synth, empty);
    controls.at("arp_1_on")->set(0.0f);
    MidiBuffer off;
    off.addEvent(MidiMessage::noteOff(1, 64), 0);
    renderMidi(synth, off);
    for (int i = 0; i < 4; ++i)
      renderMidi(synth, empty);
    expect(synth.getEngine()->getNumPressedNotes() == 0,
           "Switching the arpeggiator off must release the step note it was playing");
  }

  // Each oscillator slot has its own arpeggiator, and every note one plays
  // takes a voice. With all eight destinations on by default, a step used to
  // start eight voices however few slots sounded, each rendering every slot,
  // and arpeggiated patches ran out of CPU.
  void testArpeggiatorVoicesFollowSoundingSlots() {
    SynthPlugin synth;
    prepare(synth);
    auto& controls = synth.getControls();
    for (int slot = 1; slot <= vital::kNumOscillators; ++slot)
      controls.at("osc_" + std::to_string(slot) + "_on")->set(slot <= 2 ? 1.0f : 0.0f);
    controls.at("env_1_release")->set(std::pow(0.01f, 0.25f));
    controls.at("arp_1_on")->set(1.0f);
    controls.at("arp_1_time_mode")->set(1.0f);
    controls.at("arp_1_free_rate")->set(2.0f);  // a step every half second
    MidiBuffer hold;
    hold.addEvent(MidiMessage::noteOn(1, 60, 1.0f), 0);
    renderMidi(synth, hold);
    MidiBuffer empty;
    renderMidi(synth, empty);
    expect(synth.getEngine()->getNumActiveVoices() == 2, "An arpeggiator step takes one voice per sounding slot");

    controls.at("osc_2_on")->set(0.0f);
    for (int i = 0; i < 40; ++i)
      renderMidi(synth, empty);
    expect(synth.getEngine()->getNumActiveVoices() == 1,
           "Switching a slot off stops the note its arpeggiator was playing");

    MidiBuffer release;
    release.addEvent(MidiMessage::noteOff(1, 60), 0);
    renderMidi(synth, release);
    for (int i = 0; i < 200; ++i)
      renderMidi(synth, empty);
    expect(synth.getEngine()->getNumActiveVoices() == 0, "Arpeggiator voices end after the key is released");
  }

  // A step's voices each play one slot. Switching the arpeggiator off while
  // they rang out used to give each of them every slot at once, so the tails
  // jumped in level.
  void testArpeggiatorTailsKeepTheirSlot() {
    SynthPlugin synth;
    prepare(synth);
    auto& controls = synth.getControls();
    for (int slot = 1; slot <= vital::kNumOscillators; ++slot)
      controls.at("osc_" + std::to_string(slot) + "_on")->set(slot <= 4 ? 1.0f : 0.0f);
    controls.at("env_1_release")->set(1.0f);  // one second
    controls.at("arp_1_on")->set(1.0f);
    controls.at("arp_1_time_mode")->set(1.0f);
    controls.at("arp_1_free_rate")->set(2.0f);
    controls.at("arp_1_gate")->set(10.0f);  // the step's note ends after 50 ms
    // The engine's output, as these tests set up no host channels.
    auto blockPeak = [&synth](MidiBuffer midi) {
      AudioBuffer<float> audio(2, 128);
      synth.processBlock(audio, midi);
      float peak = 0.0f;
      for (int i = 0; i < 128; ++i)
        for (int lane = 0; lane < 2; ++lane)
          peak = std::max(peak, std::abs(synth.getEngine()->output()->buffer[i][lane]));
      return peak;
    };
    MidiBuffer hold;
    hold.addEvent(MidiMessage::noteOn(1, 60, 1.0f), 0);
    blockPeak(hold);
    MidiBuffer release;
    release.addEvent(MidiMessage::noteOff(1, 60), 0);
    blockPeak(release);
    for (int i = 0; i < 30; ++i)
      blockPeak({});
    const float ringing = blockPeak({});
    controls.at("arp_1_on")->set(0.0f);
    float after = 0.0f;
    for (int i = 0; i < 4; ++i)
      after = std::max(after, blockPeak({}));
    // The engine clamps its output to +-2.1, so a NaN reads as 2.1 here; the
    // meter before the clamp shows it.
    const float meter = synth.getStatusOutput("peak_meter")->value()[0];
    expect(ringing > 0.01f, "The arpeggiator's step rings out (peak " + std::to_string(ringing) + ")");
    expect(after <= ringing * 1.05f && std::isfinite(meter),
           "Switching the arpeggiator off leaves its ringing notes as they were (peak " +
           std::to_string(ringing) + " then " + std::to_string(after) + ", meter " + std::to_string(meter) + ")");
  }


  void testGranularGridFollowsPluginClock() {
    SynthPlugin synth;
    prepare(synth);
    slot_ids::useLegacySourceTypes(synth);
    auto& controls = synth.getControls();
    std::vector<float> ones(48000, 0.5f);
    synth.getSample(slot_ids::kGranularSlot)->loadSample(ones.data(), static_cast<int>(ones.size()), 48000);
    controls.at("osc_1_on")->set(0.0f);
    controls.at(slot_ids::id("granular_on"))->set(1.0f);
    controls.at("beats_per_minute")->set(2.0f);  // 120 BPM
    controls.at(slot_ids::id("granular_timing_mode"))->set(1.0f);
    controls.at(slot_ids::id("granular_grid_rate"))->set(4.0f);  // 1/16 = 6000 samples at 48 kHz
    controls.at(slot_ids::id("granular_grain_size"))->set(20.0f);

    std::vector<float> left;
    MidiBuffer note, empty;
    note.addEvent(MidiMessage::noteOn(1, 60, 1.0f), 0);
    for (int block = 0; block < 110; ++block) {
      AudioBuffer<float> audio(2, 128);
      synth.processBlock(audio, block == 0 ? note : empty);
      for (int i = 0; i < 128; ++i)
        left.push_back(synth.getEngine()->output()->buffer[i][0]);
    }
    const auto peak = [&](int from, int to) {
      float value = 0.0f;
      for (int i = from; i < to; ++i)
        value = std::max(value, std::abs(left[static_cast<size_t>(i)]));
      return value;
    };
    // Grains at the grid lines 6000 and 12000 (20 ms = 960 samples long);
    // nothing between them.
    const float on_grid = std::min(peak(6000, 7000), peak(12000, 13000));
    const float between = peak(8000, 11500);
    expect(on_grid > 0.01f && between < on_grid * 0.05f,
           "Granular grid timing must follow the plugin's host clock (on grid=" + std::to_string(on_grid) +
           ", between=" + std::to_string(between) + ")");
  }

  void testEditorMacrosUpdateIndependently() {
    SynthPlugin synth;
    prepare(synth);
    synth.getControls().at("osc_1_level")->set(0.0f);
    std::array<vital::ModulationConnection*, vital::kNumMacros> routes {};
    for (int i = 0; i < vital::kNumMacros; ++i)
      routes[i] = connect(synth, "macro_control_" + std::to_string(i + 1),
                          "osc_1_level", 1.0f / vital::kNumMacros);
    render(synth, 48000, true);
    SynthEditor editor(synth);
    for (int i = 1; i <= vital::kNumMacros; ++i) {
      const std::string id = "macro_control_" + std::to_string(i);
      auto* slider = dynamic_cast<Slider*>(titled(editor, synth.getMacroName(i - 1)));
      expect(slider != nullptr, "editor exposes " + id);
      if (!slider)
        continue;
      slider->keyPressed(KeyPress(KeyPress::upKey));
      close(parameter(synth, id)->getValue(), 0.01f, "keyboard immediately updates " + id);
      auto handler = slider->createAccessibilityHandler();
      handler->getValueInterface()->setValue(0.4);
      close(parameter(synth, id)->getValue(), 0.4f, "accessibility immediately updates " + id);
      render(synth, 128);
      close(synth.getEngine()->getModulationSource(id)->buffer[0][0], 0.4f,
            "next audio block reads " + id);
      auto* route = routes[i - 1];
      if (route) {
        expect(route->modulation_processor->output()->buffer[127][0] > 0.0f,
               id + " starts changing oscillator level in the next block");
        render(synth, 4800);
        close(route->modulation_processor->output()->buffer[127][0], 0.4f / vital::kNumMacros,
              id + " reaches its target without touching another macro");
        for (int previous = 0; previous < i - 1; ++previous)
          close(routes[previous]->modulation_processor->output()->buffer[127][0],
                0.4f / vital::kNumMacros, "moving " + id + " preserves previous macros");
      }
    }
  }

  void testMacroLevelMatchesDirectLevel() {
    for (int oversampling : { 0, 1, 2, 3 }) {
      SynthPlugin direct;
      SynthPlugin routed;
      prepare(direct, oversampling);
      prepare(routed, oversampling);
      direct.getControls().at("osc_1_level")->set(0.625f);
      routed.getControls().at("osc_1_level")->set(0.0f);
      parameter(routed, "macro_control_1")->setValueNotifyingHost(0.625f);
      connect(routed, "macro_control_1", "osc_1_level");
      render(direct, 48000);
      render(routed, 48000);
      render(direct, 4096, true);
      render(routed, 4096, true);
      float difference = 0.0f;
      for (int i = 0; i < 128; ++i)
        difference = std::max(difference, std::abs(direct.getEngine()->output()->buffer[i][0] -
                                                  routed.getEngine()->output()->buffer[i][0]));
      close(difference, 0.0f, "stationary macro matches direct gain at oversampling " +
            std::to_string(oversampling), 1.0e-5f);

      // Voices have already been created and rendered. A stale per-voice
      // routing flag used to leave all but sample zero at the old gain.
      direct.getControls().at("osc_1_level")->set(0.25f);
      parameter(routed, "macro_control_1")->setValueNotifyingHost(0.25f);
      render(direct, 48000);
      render(routed, 48000);
      difference = 0.0f;
      for (int i = 0; i < 128; ++i)
        difference = std::max(difference, std::abs(direct.getEngine()->output()->buffer[i][0] -
                                                  routed.getEngine()->output()->buffer[i][0]));
      close(difference, 0.0f, "moving macro matches direct gain at oversampling " +
            std::to_string(oversampling), 1.0e-5f);
    }
  }

  void testExistingVoiceReceivesRoutingConfiguration() {
    vital::ModulationConnectionProcessor original(0);
    original.init();
    vital::cr::Value source(0.625f);
    vital::cr::Value amount(1.0f);
    vital::cr::Value power(0.0f);
    original.plug(&source, vital::ModulationConnectionProcessor::kModulationInput);
    original.plug(&amount, vital::ModulationConnectionProcessor::kModulationAmount);
    original.plug(&power, vital::ModulationConnectionProcessor::kModulationPower);
    std::unique_ptr<vital::Processor> existing_voice(original.clone());

    original.setSourceSmoothing(true);
    original.setControlRate(false);
    original.setDestinationScale(1.0f);
    for (int i = 0; i < 128; ++i)
      original.output()->buffer[i] = -9.0f;
    existing_voice->process(128);
    for (int i = 0; i < 128; ++i) {
      const float value = existing_voice->output()->buffer[i][0];
      expect(value >= 0.0f && value <= 0.6251f,
             "an existing voice fills every audio-rate macro sample, including " + std::to_string(i));
    }
    close(existing_voice->output()->buffer[127][0], 0.625f, "existing voice uses new smoothing setting");

    // Reusing a modulation slot for another macro with the same amount and
    // destination range must reset each voice's smoothing history as well.
    source.set(0.125f);
    original.setSourceSmoothing(true);
    existing_voice->process(128);
    close(existing_voice->output()->buffer[127][0], 0.125f, "slot reuse resets existing voice history");

    original.setControlRate(true);
    source.set(0.75f);
    existing_voice->process(128);
    const float smoothed = existing_voice->output()->buffer[0][0];
    expect(smoothed > 0.125f && smoothed < 0.75f,
           "control-rate macro destinations also use the updated smoothing setting");
    original.setSourceSmoothing(false);
    existing_voice->process(128);
    close(existing_voice->output()->buffer[0][0], 0.75f,
          "reusing a macro slot for an unsmoothed source turns smoothing off");
  }

  void testOversamplingChangesReachPlayingVoices() {
    SynthPlugin synth;
    prepare(synth);
    auto* quality = parameter(synth, "oversampling");
    expect(quality != nullptr, "Host exposes the oversampling setting");
    if (!quality)
      return;

    for (int sample_rate : { 44100, 48000, 96000 }) {
      synth.setRateAndBufferSizeDetails(sample_rate, 128);
      synth.prepareToPlay(sample_rate, 128);
      synth.getEngine()->allSoundsOff();
      render(synth, 128, true);
      for (int index : { 3, 0, 2, 1 }) {
        quality->setValueNotifyingHost(quality->convertToPluginValue(static_cast<float>(index)));
        render(synth, 128);
        int expected = 1 << index;
        if (sample_rate >= 88200)
          expected = std::max(1, expected / 2);
        const auto* source = synth.getEngine()->getModulationSource("macro_control_1");
        expect(source->owner->getOversampleAmount() == expected,
               "Host quality change applies at the next block at " + std::to_string(sample_rate) + " Hz");
        expect(synth.getEngine()->getNumPressedNotes() == 1,
               "Changing oversampling must preserve the held note");
        bool finite = true;
        for (int i = 0; i < 128; ++i)
          finite &= std::isfinite(synth.getEngine()->output()->buffer[i][0]);
        expect(finite, "Live oversampling transitions must produce finite audio");
      }
    }

    // The editor uses the same host parameter path; verify its real keyboard
    // callback rather than calling the engine's checkOversampling helper.
    SynthEditor editor(synth);
    auto* slider = dynamic_cast<Slider*>(titled(editor, "Oversampling"));
    expect(slider != nullptr, "Editor exposes oversampling");
    if (slider) {
      slider->keyPressed(KeyPress(KeyPress::homeKey));
      render(synth, 128);
      expect(synth.getEngine()->getOversamplingAmount() == 8,
             "Editor quality change must reach DSP without another control being moved");
    }

    MemoryBlock state;
    synth.getStateInformation(state);
    SynthPlugin restored;
    restored.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    restored.setRateAndBufferSizeDetails(48000.0, 128);
    restored.prepareToPlay(48000.0, 128);
    expect(restored.getEngine()->getModulationSource("macro_control_1")->owner->getOversampleAmount() == 8,
           "Preparing restored state must apply the saved oversampling at the new sample rate");
  }

  void testCpuMeterBelongsToItsInstance() {
    SynthPlugin active;
    SynthPlugin idle;
    prepare(active);
    prepare(idle);
    close(active.getCpuUsagePercent(), 0.0f, "CPU meter starts at zero");
    for (int block : { 64, 128, 512, 256 }) {
      AudioBuffer<float> audio(2, block);
      MidiBuffer midi;
      midi.addEvent(MidiMessage::noteOn(1, 60, 0.8f), 0);
      active.processBlock(audio, midi);
      const double load = active.getCpuUsagePercent();
      expect(std::isfinite(load) && load > 0.0 && load <= 100.0,
             "CPU meter measures actual variable-size host blocks");
      close(idle.getCpuUsagePercent(), 0.0f, "CPU readings are independent for each instance");
    }
    AudioBuffer<float> empty(2, 0);
    MidiBuffer midi;
    active.processBlock(empty, midi);
    expect(std::isfinite(active.getCpuUsagePercent()), "Zero-length blocks do not corrupt the CPU meter");
    {
      SynthEditor editor(active);
      auto* meter = titled(editor, "CPU Usage");
      auto* gain = titled(editor, "Global Gain");
      expect(meter != nullptr && gain != nullptr, "Globals exposes CPU Usage and Global Gain");
      if (meter != nullptr && gain != nullptr) {
        const auto& order = editor.getKeyboardFocusOrder();
        const auto position = std::find(order.begin(), order.end(), meter);
        expect(position != order.end() && std::next(position) != order.end() && *std::next(position) == gain,
               "CPU meter is immediately before Global Gain in keyboard order");
        const auto meter_bounds = editor.getLocalArea(meter, meter->getLocalBounds());
        const auto gain_bounds = editor.getLocalArea(gain, gain->getLocalBounds());
        expect(meter_bounds.getY() < gain_bounds.getY() && gain_bounds.getY() - meter_bounds.getBottom() < 48,
               "CPU meter is visually above Global Gain");
        const String snapshot_directory = SystemStats::getEnvironmentVariable("ATLAS_UI_SNAPSHOT_DIR", {});
        if (snapshot_directory.isNotEmpty()) {
          File directory(snapshot_directory);
          expect(directory.createDirectory().wasOk(), "Create optional UI snapshot directory");
          auto* group = meter->getParentComponent();
          auto snapshot = group->createComponentSnapshot(group->getLocalBounds());
          FileOutputStream output(directory.getChildFile("globals.png"));
          output.setPosition(0);
          output.truncate();
          PNGImageFormat png;
          expect(output.openedOk() && png.writeImageToStream(snapshot, output), "Render Globals for visual review");
        }
        auto handler = meter->createAccessibilityHandler();
        auto* value = handler->getValueInterface();
#if JUCE_WINDOWS
        // NVDA beeps at every change of a progress bar, so Windows gets a quiet readout.
        expect(value == nullptr && handler->getRole() == AccessibilityRole::staticText,
               "On Windows the CPU meter is a plain readout, not a progress bar");
        render(active, 128, true);
        const double load = active.getCpuUsagePercent();
        expect(load > 0.0 && handler->getTitle() == "CPU Usage, " + String(load, 1) + " percent",
               "The CPU readout's name gives the current percentage");
        expect(handler->getDescription().isEmpty(), "The CPU readout has no generic description");
        active.releaseResources();
        expect(handler->getTitle() == "CPU Usage, 0.0 percent",
               "The CPU readout's name updates to zero when processing stops");
        prepare(active);
        render(active, 128, true);
        expect(handler->getTitle() != "CPU Usage, 0.0 percent",
               "The CPU readout's name updates when processing resumes");
#else
        expect(value != nullptr && value->isReadOnly() && value->getCurrentValueAsString().endsWith("%"),
               "CPU percentage is exposed as a read-only screen-reader value");
#endif
        if (value != nullptr) {
          render(active, 128, true);
          const double before = value->getCurrentValue();
          expect(before > 0.0 && handler->getRole() == AccessibilityRole::progressBar,
                 "CPU load remains a live indicator");
          expect(handler->getTitle() == "CPU Usage, " + String(before, 1) + " percent",
                 "CPU indicator's spoken name includes the current percentage");
          expect(handler->getDescription().isEmpty(),
                 "A generic description must not replace the CPU percentage in VoiceOver's label");
#if JUCE_MAC
          expect(cpuMeterNativeAccessibilityMatches(*handler),
                 "Native macOS CPU indicator exposes its percentage and remains read-only");
#endif
          value->setValue(99.0);
          value->setValueAsString("99%");
          close(value->getCurrentValue(), before, "CPU reading cannot be edited");
          active.releaseResources();
          expect(handler->getTitle() == "CPU Usage, 0.0 percent",
                 "CPU indicator's spoken name updates to zero when processing stops");
#if JUCE_MAC
          expect(cpuMeterNativeAccessibilityMatches(*handler),
                 "Native macOS CPU percentage updates after processing stops");
#endif
          prepare(active);
          render(active, 128, true);
          expect(value->getCurrentValue() > 0.0 && handler->getTitle() != "CPU Usage, 0.0 percent",
                 "CPU indicator's spoken name updates when processing resumes");
        }
      }
      const auto& order = editor.getKeyboardFocusOrder();
      int previous = -1;
      for (const char* stage : { "Tempo Sync", "Delay", "Attack", "Attack Power", "Hold", "Decay",
                                 "Decay Power", "Sustain", "Release", "Release Power" }) {
        auto* control = titled(editor, String("Envelope 1 ") + stage);
        const auto found = std::find(order.begin(), order.end(), control);
        expect(control != nullptr && found != order.end(), std::string("Envelope exposes ") + stage);
        if (found != order.end()) {
          const int current = static_cast<int>(std::distance(order.begin(), found));
          expect(current > previous, "Envelope parameters follow their stage order");
          previous = current;
        }
      }
    }
    Thread::sleep(1100);
    close(active.getCpuUsagePercent(), 0.0f, "CPU meter clears when the host stops calling the instance");
    active.releaseResources();
    close(active.getCpuUsagePercent(), 0.0f, "CPU meter resets when processing resources are released");
    active.prepareToPlay(96000.0, 256);
    close(active.getCpuUsagePercent(), 0.0f, "CPU meter resets when the host sample rate changes");
  }

  void testSharedSliderKeyboardNavigation() {
    ParameterSlider slider;
    slider.setRange(-100.0, 100.0);
    slider.setValue(0.0, dontSendNotification);
    slider.keyPressed(KeyPress(KeyPress::rightKey));
    close(slider.getValue(), 2.0f, "Auxiliary sliders use one-percent arrow changes");
    slider.keyPressed(KeyPress(KeyPress::leftKey, ModifierKeys::shiftModifier, 0));
    close(slider.getValue(), 1.8f, "Auxiliary sliders support fine adjustment");
    slider.keyPressed(KeyPress(KeyPress::pageDownKey));
    close(slider.getValue(), -18.2f, "Auxiliary sliders use ten-percent page changes");
    slider.keyPressed(KeyPress(KeyPress::homeKey));
    close(slider.getValue(), 100.0f, "Home preserves the Delay maximum convention");
    slider.keyPressed(KeyPress(KeyPress::endKey));
    close(slider.getValue(), -100.0f, "End preserves the Delay minimum convention");
    slider.setRange(0.0, 3.0, 1.0);
    slider.setValue(0.0, dontSendNotification);
    slider.keyPressed(KeyPress(KeyPress::rightKey, ModifierKeys::shiftModifier, 0));
    close(slider.getValue(), 1.0f, "Indexed choices retain whole steps with Shift");
  }

  void testHostTailIncludesSustainedFeedback() {
    SynthPlugin synth;
    prepare(synth);
    synth.getControls().at("delay_on")->set(1.0f);
    synth.getControls().at("delay_feedback")->set(1.0f);
    synth.getControls().at("delay_dry_wet")->set(1.0f);
    expect(std::isinf(synth.getTailLengthSeconds()) && synth.getTailLengthSeconds() > 0.0,
           "Unity feedback requires an unbounded positive host tail, not silence or a fixed timeout");
    synth.getControls().at("env_1_release")->set(2.0f); // Sixteen-second release.
    expect(synth.getTailLengthSeconds() >= 16.0,
           "Host tail must also cover the amplitude envelope's release");
  }
}

#include "editor_accessibility_tests.h"
#include "editor_speech_tests.h"
#include "oscillator_modulation_tests.h"
#include "arpeggiator_tests.h"
#include "release_hardening_tests.h"
#include "undo_tests.h"
#include "community_library_tests.h"
#include "window_repaint_tests.h"
#include "text_field_reading_tests.h"
#include "modulation_amount_slider_tests.h"
#include "focus_events_tests.h"
#include "library_layout_tests.h"

int main(int argc, char** argv) {
  ScopedJuceInitialiser_GUI juce;
  // API tokens go to a store in memory: no test reaches the real keychain or
  // Credential Manager. --credential-store alone tries the real one.
  atlas::community::setCredentialStoreForTesting(std::make_shared<atlas::credentials::MemoryStore>());
  if (argc > 1 && String(argv[1]) == "--credential-store")
    return runCredentialStoreTests();
  if (argc > 1 && String(argv[1]) == "--editor-rebuild-focus") {
    testEditorRebuildFocus();
    testEffectRemovalFocus();
    if (failures == 0)
      std::cout << "Atlas editor rebuild focus tests passed\n";
    return failures == 0 ? 0 : 1;
  }
  if (argc > 1 && String(argv[1]) == "--preset-refresh-probe") {
    // Timing only: a refresh of the user's real library.
    SynthPlugin synth;
    SynthEditor editor(synth);
    editor.updateFullGui();
    for (int pass = 0; pass < 2; ++pass) {
      const auto start = Time::getMillisecondCounterHiRes();
      editor.refreshPresetLibrary();
      while (editor.isPresetLibraryBusy() && Time::getMillisecondCounterHiRes() - start < 120000.0)
        MessageManager::getInstance()->runDispatchLoopUntil(20);
      std::cout << "refresh " << pass + 1 << ": " << editor.presetLibrarySize() << " presets in "
                << int(Time::getMillisecondCounterHiRes() - start) << " ms\n";
    }
    return 0;
  }
  if (argc > 2 && String(argv[1]) == "--preset-load-probe") {
    // Timing only: how long loading a preset and refreshing the editor take.
    SynthPlugin synth;
    SynthEditor editor(synth);
    editor.updateFullGui();
    Array<File> files;
    File(String(argv[2])).findChildFiles(files, File::findFiles, true, "*.vital;*.atp");
    for (int i = 0; i < jmin(8, files.size()); ++i) {
      const File& file = files.getReference(i * jmax(1, files.size() / 8));
      std::string error;
      const auto start = Time::getMillisecondCounterHiRes();
      synth.loadFromFile(file, error);
      const auto loaded = Time::getMillisecondCounterHiRes();
      editor.updateFullGui();
      const auto refreshed = Time::getMillisecondCounterHiRes();
      std::cout << file.getFileName() << " (" << file.getSize() / 1024 << " KB): load " << int(loaded - start)
                << " ms, editor refresh " << int(refreshed - loaded) << " ms\n";
    }
    return 0;
  }
  if (argc > 1 && String(argv[1]) == "--arpeggiator") {
    testArpNoteOffKeepsGate();
    testArpOffStartsAtTheNote();
    testArpBeatRetriggerFollowsTransport();
    testArpBeatRetriggerWithTransportStopped();
    testArpPlayOrder();
    testArpLoop();
    testArpOctave();
    testArpRetriggerDivisionParameter();
    testArpChordUsesVoicesOnlyForOscillatorsThatAreOn();
    testArpRateFollowsModulation();
    if (failures == 0)
      std::cout << "Atlas arpeggiator tests passed\n";
    return failures == 0 ? 0 : 1;
  }
  if (argc > 2 && String(argv[1]) == "--state-dump") {
    // Diagnostics only: writes a fresh instance's saved state to a file.
    SynthPlugin synth;
    synth.setPlayConfigDetails(0, 2, 48000, 512);
    synth.prepareToPlay(48000, 512);
    MemoryBlock state;
    // With a second file: restore that state first (an older save, say).
    if (argc > 3 && File(String(argv[3])).loadFileAsData(state))
      synth.setStateInformation(state.getData(), int(state.getSize()));
    state.reset();
    synth.getStateInformation(state);
    File(String(argv[2])).replaceWithData(state.getData(), state.getSize());
    std::cout << "state bytes=" << state.getSize() << std::endl;
    return 0;
  }
  if (argc > 1 && String(argv[1]) == "--undo-timing") {
    // Timing only: what recording and undoing costs the message thread, on
    // the default patch or, with a preset file, on that preset.
    const auto ms = [](auto&& function) {
      const auto start = Time::getMillisecondCounterHiRes();
      function();
      return Time::getMillisecondCounterHiRes() - start;
    };
    SynthPlugin synth;
    synth.setPlayConfigDetails(0, 2, 48000, 512);
    synth.prepareToPlay(48000, 512);
    SynthEditor editor(synth);
    editor.updateFullGui();
    auto& history = synth.undoHistory();
    if (argc > 2) {
      std::string error;
      const File preset = File(String(argv[2]));
      std::cout << "load " << preset.getFileName() << " ms=" << ms([&] { synth.loadFromFile(preset, error); })
                << std::endl;
      std::cout << "record the load ms=" << ms([&] { history.commitPendingEdits(); }) << std::endl;
    }
    std::cout << "full snapshot ms=" << ms([&] { history.rebaseline(); }) << std::endl;
    auto* bridge = parameter(synth, "filter_1_cutoff");
    constexpr int kEdits = 50;
    double record = 0.0;
    for (int i = 0; i < kEdits; ++i) {
      bridge->beginChangeGesture();
      bridge->setValueNotifyingHost(i % 2 == 0 ? 0.25f : 0.75f);
      bridge->endChangeGesture();
      record += ms([&] { history.commitPendingEdits(); });
    }
    std::cout << "record one value change ms=" << record / kEdits << std::endl;
    std::cout << "undo a value change ms=" << ms([&] { history.undo(); }) << std::endl;
    if (argc > 2) {
      while (history.numUndoSteps() > 1)
        history.undo();
      std::cout << "undo the preset load ms=" << ms([&] { history.undo(); }) << std::endl;
      std::cout << "editor refresh after it ms=" << ms([&] { editor.updateFullGui(); }) << std::endl;
      std::cout << "redo the preset load ms=" << ms([&] { history.redo(); }) << std::endl;
    }
    return 0;
  }
  if (argc > 1 && String(argv[1]) == "--editor-timing") {
    // Timing only: what opening and running the editor costs the message thread.
    const auto ms = [](auto&& function) {
      const auto start = Time::getMillisecondCounterHiRes();
      function();
      return Time::getMillisecondCounterHiRes() - start;
    };
    std::unique_ptr<SynthPlugin> synth;
    std::cout << "plugin constructed ms=" << ms([&] { synth = std::make_unique<SynthPlugin>(); }) << std::endl;
    synth->setPlayConfigDetails(0, 2, 48000, 512);
    std::cout << "prepareToPlay ms=" << ms([&] { synth->prepareToPlay(48000, 512); }) << std::endl;
    for (int pass = 0; pass < 2; ++pass) {
      std::unique_ptr<SynthEditor> editor;
      std::cout << "editor constructed ms=" << ms([&] { editor = std::make_unique<SynthEditor>(*synth); }) << std::endl;
      // The 20 Hz timer's first tick lands right after the window appears.
      std::cout << "first timer tick ms=" << ms([&] { editor->pollStateNow(); }) << std::endl;
      std::cout << "editor shown ms=" << ms([&] {
        editor->addToDesktop(ComponentPeer::windowHasTitleBar);
        editor->setVisible(true);
        MessageManager::getInstance()->runDispatchLoopUntil(10);
      }) << std::endl;
      MessageManager::getInstance()->runDispatchLoopUntil(500);
      constexpr int kTicks = 100;
      std::cout << "timer tick ms=" << ms([&] { for (int i = 0; i < kTicks; ++i) editor->pollStateNow(); }) / kTicks
                << std::endl;
      std::cout << "full refresh ms=" << ms([&] { editor->updateFullGui(); }) << std::endl;
      std::cout << "editor closed ms=" << ms([&] {
        synth->editorBeingDeleted(editor.get());
        editor.reset();
      }) << std::endl;
    }
    std::cout << "plugin destroyed ms=" << ms([&] { synth.reset(); }) << std::endl;
    return 0;
  }
  if (argc > 1 && String(argv[1]) == "--release-hardening")
    return runReleaseHardeningTests();
  if (argc > 1 && String(argv[1]) == "--window-repaint")
    return runWindowRepaintTests();
  if (argc > 1 && String(argv[1]) == "--text-field-reading")
    return runTextFieldReadingTests();
  if (argc > 1 && String(argv[1]) == "--modulation-amount-slider")
    return runModulationAmountSliderTests();
  if (argc > 1 && String(argv[1]) == "--focus-events")
    return runFocusEventTests();
  if (argc > 1 && String(argv[1]) == "--first-window-timing") {
    // Timing only: the cost of the process's first window, before Atlas draws.
    firstWindowTiming();
    return 0;
  }
  if (argc > 1 && String(argv[1]) == "--community-probe") {
    // By hand, not a test: fetches the live library's list (or ATLAS_COMMUNITY_URL's) and prints it.
    std::vector<atlas::community::RemotePreset> presets;
    const String error = atlas::community::fetchAllPresets(atlas::community::libraryUrl(), presets);
    if (error.isNotEmpty()) {
      std::cout << "Community library " << atlas::community::libraryHost() << ": " << error << '\n';
      return 1;
    }
    std::cout << "Community library " << atlas::community::libraryHost() << ": " << presets.size() << " patches\n";
    for (const auto& preset : presets) {
      std::cout << "  " << preset.name << " by " << preset.author << ", shared by " << preset.uploader
                << " (" << preset.style_label << "; " << preset.tags.joinIntoString(", ") << ") "
                << preset.file_size << " bytes, sha256 " << preset.sha256
                << (preset.preview.exists() ? ", preview " + atlas::community::durationText(preset.preview.duration_seconds)
                                            : String()) << '\n';
    }
    std::vector<atlas::community::RemoteBank> banks;
    const String bank_error = atlas::community::fetchAllBanks(atlas::community::libraryUrl(), banks);
    if (bank_error.isNotEmpty()) {
      std::cout << "Community library banks: " << bank_error << '\n';
      return 1;
    }
    std::cout << "Community library " << atlas::community::libraryHost() << ": " << banks.size() << " banks\n";
    for (const auto& bank : banks) {
      std::cout << "  " << bank.name << " shared by " << bank.uploader << " (" << bank.genre << "; "
                << bank.tags.joinIntoString(", ") << ") " << atlas::community::bankContentsText(bank) << ", "
                << bank.file_size << " bytes, sha256 " << bank.sha256 << (bank.preview.exists() ? ", preview" : "") << '\n';
    }
    return 0;
  }
  // By hand, not in ctest: migrates a copy of an Atlas folder as if its
  // config said `layout` and `default_bank`, and prints the log. A
  // favourites file (its paths already pointing into the copy) is rewritten
  // as the user's would be, and every favourite that misses its preset after
  // the moves is listed; an empty default bank leaves the config without one.
  //   AtlasMacroTests --library-migration <folder> <layout> [default bank] [favourites]
  if (argc > 3 && String(argv[1]) == "--library-migration") {
    const File folder { String(argv[2]) };
    TemporaryFile config { ".config" };
    TemporaryFile favourites { ".favorites" };
    json contents = json::object();
    contents["data_directory"] = folder.getFullPathName().toStdString();
    contents["library_layout"] = String(argv[3]).getIntValue();
    if (argc > 4 && String(argv[4]).isNotEmpty())
      contents["default_bank"] = String(argv[4]).toStdString();
    const File start_favourites = argc > 5 ? File(String(argv[5])) : File();
    if (start_favourites.existsAsFile())
      favourites.getFile().replaceWithText(start_favourites.loadFileAsString());
    config.getFile().replaceWithText(contents.dump());
    LoadSave::setConfigFileForTesting(config.getFile());
    LoadSave::setFavoritesFileForTesting(favourites.getFile());
    if (LoadSave::getDataDirectory() != folder) {
      std::cerr << "not a folder: " << folder.getFullPathName() << "\n";
      return 2;
    }
    const auto report = atlas::library::migrate(folder);
    std::cout << report.moves << " moves, " << report.problems.size() << " problems, default bank "
              << report.default_bank << "\n";
    for (const auto& line : report.lines)
      std::cout << "  " << line << "\n";
    for (const auto& problem : report.problems)
      std::cout << "  Problem: " << problem << "\n";
    int missing = 0;
    if (start_favourites.existsAsFile()) {
      const json after = LoadSave::getFavoritesJson();
      for (const auto& entry : after.items()) {
        if (!File(String(entry.key())).existsAsFile()) {
          ++missing;
          std::cout << "  Favourite without its preset: " << entry.key() << "\n";
        }
      }
      std::cout << after.size() << " favourites, " << report.favourites << " rewritten, " << missing
                << " without their preset\n";
    }
    LoadSave::setConfigFileForTesting({});
    LoadSave::setFavoritesFileForTesting({});
    return report.problems.isEmpty() && missing == 0 ? 0 : 1;
  }
  if (argc > 1 && String(argv[1]) == "--library-layout") {
    testLocateFindsLibraryBankKindAndCategory();
    testBanksAndKindFoldersAreListed();
    testMigrationFromTheFirstLayout();
    testMigrationFromTheSecondLayout();
    testStrayBankIsAdopted();
#if JUCE_WINDOWS
    testWindowsNamesAndCase();
#endif
    testBankArchivesRoundTrip();
    testPresetBrowsersFilterAndLoad();
    if (failures == 0)
      std::cout << "Atlas library layout tests passed\n";
    return failures == 0 ? 0 : 1;
  }
  if (argc > 1 && String(argv[1]) == "--community-library") {
    // What is said must not depend on the user's own speech settings.
    atlas::speech::useDefaultSettingsForTesting();
    return runCommunityLibraryTests();
  }
  if (argc > 1 && String(argv[1]) == "--editor-undo") {
    // What is said must not depend on the user's own speech settings.
    atlas::speech::useDefaultSettingsForTesting();
    return runUndoTests();
  }
  if (argc > 1 && String(argv[1]) == "--oscillator-modulation") {
    testOscillatorSourcesReachOnlyAudioRateControls();
    testOscillatorRoutesFollowTheirSourceWithinTheBlock();
    testOscillatorRouteKeepsOtherRoutesInPlace();
    testOscillatorRoutesSurviveStateRestore();
    testSharedRoutesFollowATypeChange();
    testEditorOffersOscillatorSources();
    if (failures == 0)
      std::cout << "Atlas oscillator modulation tests passed\n";
    return failures == 0 ? 0 : 1;
  }
  if (argc > 1 && String(argv[1]) == "--editor-focus") {
    // What is said must not depend on the user's own speech settings.
    atlas::speech::useDefaultSettingsForTesting();
    testSectionKeysStepThroughEffectGroups();
    testOsWindowsGiveFocusBack();
    testOrphanedFocusGoesToTheSectionHeading();
    testContextMenuArrowsLeaveTheControl();
    testPresetTrashKeepsBankAndFocus();
    if (failures == 0)
      std::cout << "Atlas editor focus tests passed\n";
    return failures == 0 ? 0 : 1;
  }
  if (argc > 1 && String(argv[1]) == "--editor-speech") {
    testKeyboardEditorsAnnounceNavigation();
    testListsSayTheSelectedRow();
    testTogglesSayTheirState();
    testUiaNotificationOnJuceElement();
    testAnnouncementsReachNvda();
    if (failures == 0)
      std::cout << "Atlas editor speech tests passed\n";
    return failures == 0 ? 0 : 1;
  }
  if (argc > 1 && String(argv[1]) == "--editor-accessibility") {
    testPresetGroupWorksWithoutKeyboardFocus();
    testWavetableEditorReturnsFocusToItsOscillator();
    testFilterFxModelControlsAndValues();
    testGranularSizeModeNamesBothChoices();
    testLevelsAndAmountsReadAsPercent();
    testSlotTypeRowsFollowTheType();
    testArpeggiatorRatesOfferOnlyFreeRunningSources();
    testWavetableEditingOnASlotSwitchedToWavetable();
    testSwitchingChoiceKeepsItsControl();
    testAnalogueCompressorRows();
    testMaimRows();
    testDriveDynamicsRows();
    testAnalogSwitchKeepsItsControl();
    testLfoViewTabs();
    testEffectMoveKeepsFocusAcrossTimerTick();
    testSectionKeysStepThroughEffectSlots();
    testShiftMOpensEveryContextMenu();
    testSaveFormClosesWithEscape();
    testSaveFormSavesOnReturn();
    testOscillatorRowGroups();
    testOscillatorRowsNameTheirOscillatorNumber();
    testArpeggiatorDestinationsAreAGroup();
    testWavetableEditorLayersAndInterpolation();
    testPresetAutoloadAndRefresh();
    testEditorStateIsPerInstance();
    testConvolutionPredelayRows();
    testEqualizerBandGroups();
    testCompressorBandGroups();
    testWindowRefocusFindsTheRebuiltControl();
    testTypedValueReturnsToOscillatorRows();
    testFractionalModulationAmountOnTranspose();
    testModulationPromptsKeepFocus();
    testFxBusControlsAndFocus();
    testEffectSlotContextMenu();
    testModulationSlotTitlesDescribeOneRoute();
    testFullGuiRefreshRebuildsEffectRows();
    testWavetableFilterRows();
    testWavetableFilterLoadKeepsFocus();
    if (failures == 0)
      std::cout << "Atlas editor accessibility tests passed\n";
    return failures == 0 ? 0 : 1;
  }
  testCpuMeterBelongsToItsInstance();
  testSharedSliderKeyboardNavigation();
  testExistingVoiceReceivesRoutingConfiguration();
  testOversamplingChangesReachPlayingVoices();
  testHostTailIncludesSustainedFeedback();
  testEditorMacrosUpdateIndependently();
  testMacroLevelMatchesDirectLevel();
  testSameSampleNoteOnOffDoesNotStick();
  testWheelsMoveGlobalParametersWithoutNotes();
  testArpeggiatorEnableAndDisableReleaseNotes();
  testArpeggiatorVoicesFollowSoundingSlots();
  testArpeggiatorTailsKeepTheirSlot();
  testGranularGridFollowsPluginClock();
  testSyncedLfoNotesWithTransportStoppedAndPlaying();
  testFreeRunningLfoJoinsNotesWherever();
  testFreeRunningSourcesOutliveTheirVoices();
  testLfoTempoModulatedByAnotherLfo();
  testSimpleLfoShapes();
  if (failures == 0)
    std::cout << "Atlas macro tests passed\n";
  return failures == 0 ? 0 : 1;
}
