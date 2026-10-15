#pragma once

// Regression tests for the problems the 2026-10-14 audit confirmed: voice
// allocation with arpeggiator tails, MIDI learn, state restore and the host's
// view of changes. Included into macro_modulation_tests.cpp.

#include "load_save.h"
#include "../src/synthesis/utilities/value_switch.h"

namespace {
  bool hasMapping(const std::vector<std::pair<int, std::string>>& mappings, int controller, const std::string& name) {
    for (const auto& mapping : mappings)
      if (mapping.first == controller && mapping.second == name)
        return true;
    return false;
  }

  // Points Atlas.config at a temporary copy of the user's, so these tests
  // never write the real one.
  struct TemporaryConfig {
    TemporaryFile file { ".config" };
    explicit TemporaryConfig(json contents = LoadSave::getConfigJson()) {
      if (!contents.is_object())
        contents = json::object();
      file.getFile().replaceWithText(contents.dump());
      LoadSave::setConfigFileForTesting(file.getFile());
    }
    ~TemporaryConfig() { LoadSave::setConfigFileForTesting({}); }
    String text() const { return file.getFile().loadFileAsString(); }
  };

  // Each arpeggiator note-off ends its own note. It used to release every
  // voice of that pitch again, restarting tails that were already fading, so
  // they rang many times their Release and filled the voice pool.
  void testArpeggiatorTailsEndAfterTheirRelease() {
    SynthPlugin synth;
    prepare(synth);
    auto& controls = synth.getControls();
    for (int slot = 1; slot <= vital::kNumOscillators; ++slot)
      controls.at("osc_" + std::to_string(slot) + "_on")->set(slot == 1 ? 1.0f : 0.0f);
    controls.at("env_1_release")->set(std::pow(0.5f, 0.25f));  // half a second
    controls.at("arp_1_on")->set(1.0f);
    controls.at("arp_1_time_mode")->set(1.0f);
    controls.at("arp_1_free_rate")->set(8.0f);  // eight steps a second, all one pitch
    MidiBuffer hold;
    hold.addEvent(MidiMessage::noteOn(1, 60, 1.0f), 0);
    renderMidi(synth, hold);
    MidiBuffer empty;
    int most = 0;
    for (int i = 0; i < 1500; ++i) {  // four seconds
      renderMidi(synth, empty);
      most = std::max(most, synth.getEngine()->getNumActiveVoices());
    }
    // About 8 notes a second each ringing half a second, against the whole
    // pool of 34 when every note-off restarted the older tails.
    expect(most <= 10, "arpeggiator tails end after their Release (" + std::to_string(most) + " voices at most)");

    controls.at("arp_1_on")->set(0.0f);
    MidiBuffer release;
    release.addEvent(MidiMessage::noteOff(1, 60), 0);
    renderMidi(synth, release);
    int blocks = 0;
    while (synth.getEngine()->getNumActiveVoices() > 0 && blocks < 3000) {
      renderMidi(synth, empty);
      ++blocks;
    }
    expect(blocks * 128 < 48000, "the last tail ends within a second of the arpeggiator stopping (" +
                                     std::to_string(blocks * 128 / 48) + " ms)");
  }

  // Arpeggiator voices sit outside the polyphony limit and their tails can
  // fill the whole pool. A key on a slot the arpeggiator does not play must
  // still sound, and a mono trill there (Kill override, legato off) used to
  // hand a voice that did not exist to the audio thread.
  void testKeyboardNotesSurviveAFullVoicePool() {
    SynthPlugin synth;
    prepare(synth);
    auto& controls = synth.getControls();
    for (int slot = 1; slot <= vital::kNumOscillators; ++slot)
      controls.at("osc_" + std::to_string(slot) + "_on")->set(slot <= 2 ? 1.0f : 0.0f);
    controls.at("polyphony")->set(1.0f);
    controls.at("env_1_release")->set(std::pow(16.0f, 0.25f));  // sixteen seconds
    controls.at("arp_1_on")->set(1.0f);
    controls.at("arp_1_time_mode")->set(1.0f);
    controls.at("arp_1_free_rate")->set(16.0f);
    controls.at("arp_1_destination_osc_2")->set(0.0f);  // oscillator 2 plays from the keyboard
    MidiBuffer key_a;
    key_a.addEvent(MidiMessage::noteOn(1, 52, 1.0f), 0);
    renderMidi(synth, key_a);
    MidiBuffer empty;
    for (int i = 0; i < 2000 && synth.getEngine()->getNumActiveVoices() < 34; ++i)
      renderMidi(synth, empty);
    expect(synth.getEngine()->getNumActiveVoices() == 34, "arpeggiator tails fill the voice pool");

    MidiBuffer key_b;
    key_b.addEvent(MidiMessage::noteOn(1, 55, 1.0f), 0);
    renderMidi(synth, key_b);
    expect(synth.getEngine()->getNumPressedNotes() == 2, "a key pressed with the pool full is played");
    MidiBuffer release_b;
    release_b.addEvent(MidiMessage::noteOff(1, 55), 0);
    renderMidi(synth, release_b);
    for (int i = 0; i < 10; ++i)
      renderMidi(synth, empty);
    expect(synth.getEngine()->getNumPressedNotes() == 1, "releasing the trill's upper key returns to the held one");
    // The upper key's voice fades out (Kill) and the next arpeggiator step takes it.
    expect(synth.getEngine()->getNumActiveVoices() >= 33, "the pool stays in use and nothing breaks");
  }

  // A MIDI-learn mapping in Atlas.config, which every Atlas build shares,
  // could name a parameter that is no longer a control (the fixed Sample,
  // Granular, Spectral and Noise sources became slots 5-8). Its controller
  // then crashed the audio thread; a malformed entry made the plugin throw.
  void testStaleMidiLearnMappingsAreTranslatedOrDropped() {
    json data = LoadSave::getConfigJson();
    if (!data.is_object())
      data = json::object();
    data["midi_learn"] = json::array({
      { { "source", 21 }, { "destinations", json::array({ { { "destination", "sample_level" } } }) } },
      { { "source", 22 }, { "destinations", json::array({ { { "destination", "osc_5_sample_level" } } }) } },
      { { "source", "twenty" }, { "destinations", 7 } },
      { { "source", 23 }, { "destinations", json::array({ { { "destination", 5 } },
                                                          { { "destination", "osc_1_tune" } } }) } } });
    TemporaryConfig config(data);
    {
      SynthPlugin synth;  // reads the config while it is built
      prepare(synth);
      const auto mappings = synth.midiLearnMappings();
      expect(hasMapping(mappings, 21, "osc_5_level"), "a fixed-source mapping drives its slot control");
      expect(hasMapping(mappings, 23, "osc_1_tune"), "valid mappings load");
      expect(mappings.size() == 2, "mappings to nothing are dropped (" + std::to_string(mappings.size()) + " left)");

      MidiBuffer controllers;
      controllers.addEvent(MidiMessage::controllerEvent(1, 21, 127), 0);
      controllers.addEvent(MidiMessage::controllerEvent(1, 22, 64), 1);
      renderMidi(synth, controllers);
      close(synth.getControls().at("osc_5_level")->value(), vital::Parameters::getDetails("osc_5_level").max,
            "the translated mapping follows its controller");
      synth.valueChanged("osc_5_sample_level", 1.0f);
      synth.valueChanged("sample_level", 1.0f);
      expect(synth.getControls().count("osc_5_sample_level") == 0 && synth.getControls().count("sample_level") == 0,
             "a name that is no control changes nothing and is never added");
    }
  }

  // Completing a MIDI learn happens on the audio thread, which must not touch
  // files: the mapping is handed over and follows the controller at once; the
  // message thread adds it to the map and saves the config.
  void testMidiLearnSavesOffTheAudioThread() {
    json data = LoadSave::getConfigJson();
    if (data.is_object())
      data.erase("midi_learn");
    TemporaryConfig config(data);
    {
      SynthPlugin synth;
      prepare(synth);
      const String before = config.text();
      synth.armMidiLearn("osc_1_tune");
      MidiBuffer up;
      up.addEvent(MidiMessage::controllerEvent(1, 16, 127), 0);
      renderMidi(synth, up);
      expect(config.text() == before, "the audio thread writes no file when a learn completes");
      const auto& tune = vital::Parameters::getDetails("osc_1_tune");
      close(synth.getControls().at("osc_1_tune")->value(), tune.max, "the learned control follows at once");
      MidiBuffer down;
      down.addEvent(MidiMessage::controllerEvent(1, 16, 0), 0);
      renderMidi(synth, down);
      close(synth.getControls().at("osc_1_tune")->value(), tune.min, "and keeps following before it is saved");

      expect(hasMapping(synth.midiLearnMappings(), 16, "osc_1_tune"), "the message thread adds the mapping");
      expect(config.text().contains("osc_1_tune"), "and saves it");
      synth.clearMidiLearn("osc_1_tune");
      expect(synth.midiLearnMappings().empty() && !config.text().contains("osc_1_tune"), "removing it saves too");

      synth.armMidiLearn("not_a_parameter");
      expect(synth.midiLearnArmedName().empty(), "only a control can be learned");
    }
  }

  // Restoring a patch builds each slot's saved type before connecting its
  // routes. A route into the type's own parameter used to be dropped (and
  // lost on the next save), and one into the slot's level was plugged into
  // the module that was then replaced, so it did nothing.
  void testRestoredRoutesReachTheSlotType() {
    auto plugged = [](SynthPlugin& synth, const std::string& destination) {
      vital::ValueSwitch* modulation = synth.getEngine()->getMonoModulationSwitch(destination);
      return modulation != nullptr && modulation->value() >= 0.5f;
    };
    juce::MemoryBlock state;
    {
      SynthPlugin synth;
      prepare(synth);
      synth.valueChanged("osc_1_type", float(vital::source_types::kGranular));
      connect(synth, "lfo_1", "osc_1_granular_density", 0.9f);
      connect(synth, "lfo_1", "osc_1_level", 0.9f);
      synth.getStateInformation(state);

      // The same in the editor: the slot changes type under a level route.
      synth.valueChanged("osc_1_type", float(vital::source_types::kSample));
      expect(synth.getConnectionIndex("lfo_1", "osc_1_level") >= 0, "a level route stays across a type change");
      expect(plugged(synth, "osc_1_level"), "and reaches the new type");
    }
    SynthPlugin restored;
    prepare(restored);
    restored.setStateInformation(state.getData(), int(state.getSize()));
    expect(restored.getConnectionIndex("lfo_1", "osc_1_granular_density") >= 0,
           "a route into the type's own parameter survives a restore");
    expect(plugged(restored, "osc_1_granular_density"), "and is connected");
    expect(restored.getConnectionIndex("lfo_1", "osc_1_level") >= 0, "a route into the slot's level survives");
    expect(plugged(restored, "osc_1_level"), "and is connected to the restored type");
    juce::MemoryBlock saved;
    restored.getStateInformation(saved);
    expect(saved.toString().contains("osc_1_granular_density"), "the next save keeps the route");
  }

  // A host restoring state (a preset, undo) pauses processing, and note-offs
  // sent meanwhile never arrive. Held notes are released, so neither a note
  // nor an arpeggio hangs on into the new patch.
  void testHostStateRestoreReleasesHeldNotes() {
    for (bool arp : { false, true }) {
      SynthPlugin synth;
      prepare(synth);
      auto& controls = synth.getControls();
      controls.at("env_1_release")->set(std::pow(0.2f, 0.25f));
      if (arp) {
        controls.at("arp_1_on")->set(1.0f);
        controls.at("arp_1_time_mode")->set(1.0f);
        controls.at("arp_1_free_rate")->set(8.0f);
      }
      juce::MemoryBlock state;
      synth.getStateInformation(state);
      MidiBuffer on;
      on.addEvent(MidiMessage::noteOn(1, 60, 1.0f), 0);
      renderMidi(synth, on);
      MidiBuffer empty;
      for (int i = 0; i < 40; ++i)
        renderMidi(synth, empty);
      expect(synth.getEngine()->getNumActiveVoices() > 0, "the note plays");
      synth.setStateInformation(state.getData(), int(state.getSize()));  // its note-off is lost
      for (int i = 0; i < 1500; ++i)
        renderMidi(synth, empty);
      expect(synth.getEngine()->getNumActiveVoices() == 0,
             std::string(arp ? "an arpeggio" : "a note") + " held across a host restore ends");
    }
  }

  struct StateChangeCounter : public AudioProcessorListener {
    int changes = 0;
    void audioProcessorParameterChanged(AudioProcessor*, int, float) override { }
    void audioProcessorChanged(AudioProcessor*, const ChangeDetails& details) override {
      if (details.nonParameterStateChanged)
        ++changes;
    }
  };

  // Preset loads and edits the host has no parameter for (effects, routes,
  // wavetables) never told the host the project changed, so it did not offer
  // to save them. A state the host restores is not such a change.
  void testUserEditsMarkTheHostProjectChanged() {
    SynthPlugin synth;
    prepare(synth);
    juce::MemoryBlock state;
    synth.getStateInformation(state);
    TemporaryFile preset(".atp");
    preset.getFile().replaceWithText(state.toString());
    StateChangeCounter counter;
    synth.addListener(&counter);
    auto changes = [&](const std::function<void()>& action) {
      counter.changes = 0;
      action();
      MessageManager::getInstance()->runDispatchLoopUntil(40);
      return counter.changes;
    };
    expect(changes([&] { synth.setStateInformation(state.getData(), int(state.getSize())); }) == 0,
           "a host restore is not a change to save");
    expect(changes([&] { synth.parameterChanged("fx_slot_1_enabled", 0.0f); }) == 1,
           "an edit to a control the host does not see marks the project (once)");
    expect(changes([&] { synth.connectModulation("lfo_1", "osc_1_level"); }) == 1, "a new route marks it");
    expect(changes([&] {
      std::string error;
      synth.loadFromFile(preset.getFile(), error);
    }) == 1, "a preset load marks it");
    expect(changes([&] { synth.parameterChanged("osc_1_level", 0.4f); }) == 0,
           "a host parameter needs no mark: the host saw the edit");
    synth.removeListener(&counter);
  }

  // The editor's first timer tick rebuilt every row it had just built, since
  // the glide setting it compares with was never recorded.
  void testEditorFirstTickKeepsItsRows() {
    SynthPlugin synth;
    SynthEditor editor(synth);
    std::vector<Component*> rows;
    std::function<void(Component&)> collect = [&](Component& component) {
      if (component.getTitle().isNotEmpty())
        rows.push_back(&component);
      for (auto* child : component.getChildren())
        collect(*child);
    };
    collect(editor);
    const auto before = rows;
    editor.pollStateNow();
    rows.clear();
    collect(editor);
    expect(!before.empty() && rows == before, "the first timer tick keeps the rows the editor opened with");
  }

  int runReleaseHardeningTests() {
    testArpeggiatorTailsEndAfterTheirRelease();
    testKeyboardNotesSurviveAFullVoicePool();
    testStaleMidiLearnMappingsAreTranslatedOrDropped();
    testMidiLearnSavesOffTheAudioThread();
    testRestoredRoutesReachTheSlotType();
    testHostStateRestoreReleasesHeldNotes();
    testUserEditsMarkTheHostProjectChanged();
    testEditorFirstTickKeepsItsRows();
    if (failures == 0)
      std::cout << "Atlas release hardening tests passed\n";
    return failures == 0 ? 0 : 1;
  }
}
