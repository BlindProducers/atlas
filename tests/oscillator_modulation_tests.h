#pragma once

// Oscillators as audio-rate modulation sources (osc_N_output): which controls
// they reach, and that a route follows its source sample by sample within the
// same block, whether it leads into another oscillator or out of the producers.
namespace {
  const vital::Output* oscillatorSource(SynthPlugin& synth, int slot) {
    return synth.getEngine()->getModulationSource(vital::source_types::outputSourceName(slot));
  }

  // Connects an oscillator route with an exact amount, bipolar as it defaults.
  vital::ModulationConnection* connectOscillator(SynthPlugin& synth, int source_slot, const std::string& destination,
                                                 float amount) {
    auto* connection = connect(synth, vital::source_types::outputSourceName(source_slot), destination, amount);
    if (connection != nullptr) {
      const auto prefix = "modulation_" + std::to_string(connection->modulation_processor->index() + 1);
      synth.getControls().at(prefix + "_bipolar")->set(1.0f);
    }
    return connection;
  }

  void testOscillatorSourcesReachOnlyAudioRateControls() {
    SynthPlugin synth;
    for (int slot = 0; slot < vital::kNumOscillators; ++slot)
      expect(oscillatorSource(synth, slot) != nullptr && !oscillatorSource(synth, slot)->isControlRate(),
             "oscillator " + std::to_string(slot + 1) + "'s audio is an audio-rate modulation source");
    expect(vital::ModulationConnection::isModulationSourceDefaultBipolar("osc_2_output") &&
               !vital::ModulationConnection::isModulationSourceDefaultBipolar("osc_5_playhead"),
           "an oscillator's audio defaults to bipolar; playheads keep their default");

    // Oscillator 2 (slot 1) reaches other oscillators' and the voice filters' audio-rate controls.
    for (const char* destination : { "osc_1_transpose", "osc_1_tune", "osc_1_level", "osc_1_phase",
                                     "osc_3_level", "filter_1_cutoff", "filter_2_cutoff", "filter_1_formant_x" })
      expect(synth.acceptsOscillatorModulation(1, destination),
             std::string("Oscillator 2 can modulate ") + destination);
    // Not its own controls, not control-rate controls, not shared (mono) ones, and
    // not LFO rates: LFOs render before the oscillators and are modulators themselves.
    for (const char* destination : { "osc_2_transpose", "osc_2_level", "env_1_attack", "lfo_1_frequency",
                                     "random_1_frequency", "filter_1_resonance", "volume" })
      expect(!synth.acceptsOscillatorModulation(1, destination),
             std::string("Oscillator 2 cannot modulate ") + destination);

    // Everything it can reach is an oscillator or voice filter control, all of
    // which render after the source oscillator.
    StringArray unexpected;
    int accepted = 0;
    for (const auto& destination : synth.getEngine()->getPolyModulationDestinations()) {
      if (!synth.acceptsOscillatorModulation(1, destination.first))
        continue;
      ++accepted;
      const String name(destination.first);
      const bool oscillator = vital::source_types::slotForControl(destination.first) >= 0;
      const bool voice_filter = name.startsWith("filter_1_") || name.startsWith("filter_2_");
      if (!oscillator && !voice_filter)
        unexpected.add(name);
    }
    expect(accepted > 10, "oscillators reach a useful set of audio-rate controls");
    expect(unexpected.isEmpty(), "oscillator routes reach only oscillator and voice filter controls: " +
                                     unexpected.joinIntoString(", ").toStdString());

    // The engine refuses what the editor would not offer.
    synth.connectModulation("osc_2_output", "osc_2_transpose");
    expect(synth.getConnectionIndex("osc_2_output", "osc_2_transpose") < 0, "an oscillator cannot modulate itself");
    synth.connectModulation("osc_2_output", "env_1_attack");
    expect(synth.getConnectionIndex("osc_2_output", "env_1_attack") < 0,
           "an oscillator cannot modulate a control-rate control");
    synth.connectModulation("osc_2_output", "osc_1_transpose");
    expect(synth.getConnectionIndex("osc_2_output", "osc_1_transpose") >= 0,
           "an oscillator can modulate another oscillator's pitch");
  }

  // Each route sample is the same block's source sample, scaled: no block of delay.
  void expectRouteFollowsSource(SynthPlugin& synth, vital::ModulationConnection* connection, int source_slot,
                                const std::string& destination, float amount, const std::string& label) {
    expect(connection != nullptr, label + ": route connected");
    if (connection == nullptr)
      return;
    const vital::Output* source = oscillatorSource(synth, source_slot);
    const vital::Output* route =
        connection->modulation_processor->output(vital::ModulationConnectionProcessor::kModulationOutput);
    const float scale = amount * vital::Parameters::getParameterRange(destination);
    float worst = 0.0f;
    float swing = 0.0f;
    // Sample 0 is overwritten with the voice readout after the block.
    for (int i = 1; i < 128; ++i) {
      for (int lane = 0; lane < vital::poly_float::kSize; ++lane) {
        const float expected = (source->buffer[i][lane] - 0.5f) * scale;
        worst = std::max(worst, std::abs(route->buffer[i][lane] - expected));
        swing = std::max(swing, std::abs(source->buffer[i][lane] - 0.5f));
      }
    }
    expect(swing > 0.1f, label + ": the source oscillator is playing");
    close(worst, 0.0f, label + ": each route sample follows the same block's source sample", 1.0e-4f);
  }

  void testOscillatorRoutesFollowTheirSourceWithinTheBlock() {
    SynthPlugin synth;
    prepare(synth);
    synth.getControls().at("osc_2_on")->set(1.0f);
    synth.getControls().at("osc_2_unison_voices")->set(1.0f);
    // Into another oscillator (the producers render oscillator 2 first, then
    // the route, then oscillator 1) and out of the producers to a filter.
    auto* into_oscillator = connectOscillator(synth, 1, "osc_1_level", 0.5f);
    auto* into_filter = connectOscillator(synth, 1, "filter_1_cutoff", 0.25f);
    render(synth, 128 * 8, true);
    expectRouteFollowsSource(synth, into_oscillator, 1, "osc_1_level", 0.5f, "Oscillator 2 to Oscillator 1 Level");
    expectRouteFollowsSource(synth, into_filter, 1, "filter_1_cutoff", 0.25f, "Oscillator 2 to Filter 1 Cutoff");

    // A loop between two oscillators breaks on the preceding block, like FM, and stays finite.
    auto* back = connectOscillator(synth, 0, "osc_2_level", 0.5f);
    render(synth, 128 * 8);
    expect(back != nullptr, "Oscillator 1 can modulate Oscillator 2 while Oscillator 2 modulates it");
    bool finite = true;
    for (int i = 0; i < 128; ++i)
      finite = finite && std::isfinite(synth.getEngine()->output()->buffer[i][0]);
    expect(finite, "a loop between two oscillators stays finite");

    // Disconnecting hands the route back to the voice graph.
    synth.disconnectModulation("osc_2_output", "osc_1_level");
    render(synth, 128);
    expect(into_oscillator == nullptr || !into_oscillator->modulation_processor->runByProducers(),
           "a disconnected route is no longer run by the producers");
  }

  // An oscillator route must not disturb the timing of other routes into oscillators.
  void testOscillatorRouteKeepsOtherRoutesInPlace() {
    SynthPlugin direct;
    SynthPlugin routed;
    prepare(direct);
    prepare(routed);
    direct.getControls().at("osc_1_level")->set(0.625f);
    routed.getControls().at("osc_1_level")->set(0.0f);
    parameter(routed, "macro_control_1")->setValueNotifyingHost(0.625f);
    connect(routed, "macro_control_1", "osc_1_level");
    for (SynthPlugin* synth : { &direct, &routed }) {
      synth->getControls().at("osc_2_on")->set(1.0f);
      synth->getControls().at("osc_2_level")->set(0.0f);
    }
    // A silent-amount route from oscillator 2 into oscillator 1 and the filter.
    connectOscillator(routed, 1, "osc_1_transpose", 0.0f);
    connectOscillator(routed, 1, "filter_1_cutoff", 0.0f);
    render(direct, 48000);
    render(routed, 48000);
    render(direct, 4096, true);
    render(routed, 4096, true);
    float difference = 0.0f;
    for (int i = 0; i < 128; ++i)
      difference = std::max(difference, std::abs(direct.getEngine()->output()->buffer[i][0] -
                                                routed.getEngine()->output()->buffer[i][0]));
    close(difference, 0.0f, "a macro route into an oscillator keeps its timing beside oscillator routes", 1.0e-5f);
  }

  // Whether the route `source` -> `destination` exists and is plugged into the
  // destination the engine has registered under that name now (the per-voice
  // one, or the shared one for a source that is not per voice).
  bool routeFeeds(SynthPlugin& synth, const std::string& source, const std::string& destination) {
    const int index = synth.getConnectionIndex(source, destination);
    if (index < 0)
      return false;
    const vital::Processor* route = synth.getEngine()->getModulationBank().atIndex(index)->modulation_processor.get();
    for (vital::Processor* target : { synth.getEngine()->getPolyModulationDestination(destination),
                                      synth.getEngine()->getMonoModulationDestination(destination) }) {
      for (int i = 0; target != nullptr && i < target->numInputs(); ++i) {
        const vital::Input* input = target->input(i);
        if (input != nullptr && input->source != nullptr && input->source->owner == route)
          return true;
      }
    }
    return false;
  }

  // Saved routes into an oscillator must reach it once the state is restored,
  // whichever type the oscillator loads as: a route must reach the module of
  // the type the state selects, not the one the slot held before loading.
  void testOscillatorRoutesSurviveStateRestore() {
    struct Case {
      int type;
      const char* type_destination;
    };
    for (const Case& test : { Case { vital::source_types::kWavetable, "osc_2_transpose" },
                              Case { vital::source_types::kNoise, "osc_2_noise_low_cutoff" },
                              Case { vital::source_types::kOpenB3, "osc_2_b3_key_click" } }) {
      const std::string type_name = vital::source_types::describe(test.type).id;
      const std::vector<std::pair<std::string, std::string>> routes = {
        { "osc_1_output", "osc_2_level" }, { "lfo_1", "osc_2_level" }, { "osc_1_playhead", "osc_2_pan" },
        { "lfo_1", test.type_destination } };

      SynthPlugin original;
      prepare(original);
      original.getControls().at("osc_2_on")->set(1.0f);
      original.valueChanged("osc_2_type", static_cast<float>(test.type));
      render(original, 128);
      connectOscillator(original, 0, "osc_2_level", 0.5f);
      connect(original, "lfo_1", "osc_2_level", 0.25f);
      connect(original, "osc_1_playhead", "osc_2_pan", 0.25f);
      connect(original, "lfo_1", test.type_destination, 0.25f);
      MemoryBlock state;
      original.getStateInformation(state);
      TemporaryFile patch(".atp");
      patch.getFile().replaceWithText(LoadSave::stateToJson(&original, original.getCriticalSection()).dump());

      // Reopening a project: a fresh instance restores the host state.
      SynthPlugin restored;
      restored.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
      prepare(restored);
      // Loading the patch into a running instance whose slot holds another type.
      SynthPlugin reloaded;
      prepare(reloaded);
      std::string error;
      expect(reloaded.loadFromFile(patch.getFile(), error), type_name + ": the saved patch loads");

      for (const auto& [source, destination] : routes) {
        const std::string route = source + " -> " + destination + " (Oscillator 2 as " + type_name + ")";
        expect(routeFeeds(restored, source, destination), route + ": the restored state feeds the live control");
        expect(routeFeeds(reloaded, source, destination), route + ": the loaded patch feeds the live control");
      }

      const int index = restored.getConnectionIndex("osc_1_output", "osc_2_level");
      render(restored, 128 * 8, true);
      expectRouteFollowsSource(restored, index >= 0 ? restored.getEngine()->getModulationBank().atIndex(index) : nullptr,
                               0, "osc_2_level", 0.5f, "Oscillator 1 to Oscillator 2 Level restored as " + type_name);
    }
  }

  // Level, pan and the FM/RM amounts are shared by every type, so their routes
  // stay when a slot changes type, and must reach the new type's controls.
  void testSharedRoutesFollowATypeChange() {
    SynthPlugin synth;
    prepare(synth);
    synth.getControls().at("osc_2_on")->set(1.0f);
    connectOscillator(synth, 0, "osc_2_level", 0.5f);
    connect(synth, "lfo_1", "osc_2_level", 0.25f);
    connect(synth, "lfo_1", "osc_2_pan", 0.25f);
    connect(synth, "lfo_1", "osc_2_transpose", 0.25f);
    for (int type : { static_cast<int>(vital::source_types::kNoise), static_cast<int>(vital::source_types::kOpenB3),
                      static_cast<int>(vital::source_types::kWavetable) }) {
      const std::string type_name = vital::source_types::describe(type).id;
      synth.valueChanged("osc_2_type", static_cast<float>(type));
      render(synth, 128);
      for (const auto& [source, destination] : std::vector<std::pair<std::string, std::string>> {
             { "osc_1_output", "osc_2_level" }, { "lfo_1", "osc_2_level" }, { "lfo_1", "osc_2_pan" } })
        expect(routeFeeds(synth, source, destination),
               source + " -> " + destination + " feeds the control after switching Oscillator 2 to " + type_name);
    }
    expect(synth.getConnectionIndex("lfo_1", "osc_2_transpose") < 0,
           "a route into a control the new type lacks is dropped on the switch");
  }

  void testEditorOffersOscillatorSources() {
    SynthPlugin synth;
    SynthEditor editor(synth);
    pumpEditorEvents();
    auto oscillatorItems = [&](const String& destination) {
      std::map<int, String> choices;
      PopupMenu menu = editor.createModulationSourceSubmenu(destination, choices, 1);
      StringArray items;
      bool found = false;
      for (PopupMenu::MenuItemIterator it(menu, false); it.next();) {
        if (it.getItem().text != "Oscillators" || it.getItem().subMenu == nullptr)
          continue;
        found = true;
        for (PopupMenu::MenuItemIterator sub(*it.getItem().subMenu, false); sub.next();)
          items.add(sub.getItem().text);
      }
      return found ? items : StringArray { "<no Oscillators group>" };
    };
    const StringArray for_pitch = oscillatorItems("osc_1_transpose");
    expect(for_pitch.size() == vital::kNumOscillators - 1 && !for_pitch.contains("Oscillator 1") &&
               for_pitch.contains("Oscillator 2") && for_pitch.contains("Oscillator 8"),
           "Oscillator 1 pitch offers every other oscillator: " + for_pitch.joinIntoString(", ").toStdString());
    expect(oscillatorItems("filter_1_cutoff").size() == vital::kNumOscillators,
           "a voice filter's cutoff offers all eight oscillators");
    expect(oscillatorItems("env_1_attack")[0] == "<no Oscillators group>",
           "a control-rate control offers no oscillators");

    editor.assignModulation("osc_2_output", "osc_1_transpose", "raw 50%");
    const int index = synth.getConnectionIndex("osc_2_output", "osc_1_transpose");
    expect(index >= 0, "the editor connects Oscillator 2 to Oscillator 1 pitch");
    if (index >= 0)
      close(synth.getControls().at("modulation_" + std::to_string(index + 1) + "_amount")->value(), 0.5f,
            "the typed raw amount applies");
    editor.assignModulation("osc_1_output", "osc_1_transpose", "raw 50%");
    expect(synth.getConnectionIndex("osc_1_output", "osc_1_transpose") < 0,
           "the editor refuses an oscillator modulating itself");
  }
}
