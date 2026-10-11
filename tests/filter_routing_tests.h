// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "filters_module.h"
#include "producers_module.h"

namespace filter_routing_tests {
  using namespace vital;

  void configureFilter(FilterModule& filter, const std::string& prefix) {
    const auto controls = filter.getControls();
    controls.at(prefix + "_on")->set(1);
    controls.at(prefix + "_model")->set(constants::kDigital);
    controls.at(prefix + "_cutoff")->set(prefix == "filter_1" ? 60 : 84);
    controls.at(prefix + "_resonance")->set(.25f);
    controls.at(prefix + "_mix")->set(1);
  }

  void testCascadeAndDestinations() {
    // Compare the routed audio with two independently wired real filters.
    // Different input tones also verify intentional sources entering stage 2.
    for (int rate : {44100, 96000}) for (int oversampling : {1, 4}) {
      FiltersModule filters;
      FilterModule first("filter_1"), second("filter_2");
      Output in1, in2, cascade;
      cr::Value amplitude(poly_float(.3f, .5f, .8f, 1));
      filters.plug(&amplitude, FiltersModule::kAmplitude);
      for (auto* input : {&in1, &in2, &cascade})
        input->ensureBufferSize(kMaxBufferSize * oversampling);
      filters.plug(&in1, FiltersModule::kFilter1Input);
      filters.plug(&in2, FiltersModule::kFilter2Input);
      first.plug(&in1, FilterModule::kAudio);
      second.plug(&in2, FilterModule::kAudio);
      filters.init(); first.init(); second.init();
      configureFilter(first, "filter_1"); configureFilter(second, "filter_2");
      auto controls = filters.getControls();
      for (auto* reference : {&first, &second})
        for (const auto& control : reference->getControls())
          controls.at(control.first)->set(control.second->value());
      for (SynthModule* module : std::array<SynthModule*, 3>{&filters, &first, &second}) {
        module->setSampleRate(rate);
        module->setOversampleAmount(oversampling);
      }
      int position = 0;
      const int destinations[] {constants::kEffects, constants::kBus1, constants::kBus2,
                                constants::kBus3, constants::kDirectOut};
      // Switch repeatedly on the same processors to expose stale bus contents.
      for (int mode : {0, 1, 2, 0, 2, 1}) for (int bus = 0; bus < 5; ++bus) {
        controls.at("filter_1_filter_input")->set(mode == 2);
        controls.at("filter_2_filter_input")->set(mode == 1);
        controls.at("filter_1_destination")->set(destinations[mode == 1 ? (bus + 1) % 5 : bus]);
        controls.at("filter_2_destination")->set(destinations[mode == 2 ? (bus + 1) % 5 : bus]);
        for (int block : {1, 17, 128}) {
          const int count = block * oversampling;
          for (int i = 0; i < count; ++i, ++position) {
            in1.buffer[i] = poly_float(.1f, .2f, .15f, .05f) * std::sin(position * .031f);
            in2.buffer[i] = poly_float(.07f, .1f, .04f, .02f) * std::sin(position * .17f);
          }
          first.plug(mode == 2 ? &cascade : &in1, FilterModule::kAudio);
          second.plug(mode == 1 ? &cascade : &in2, FilterModule::kAudio);
          if (mode == 2) {
            second.process(count);
            for (int i = 0; i < count; ++i) cascade.buffer[i] = second.output()->buffer[i] + in1.buffer[i];
            first.process(count);
          }
          else {
            first.process(count);
            for (int i = 0; i < count; ++i) cascade.buffer[i] = first.output()->buffer[i] + in2.buffer[i];
            second.process(count);
          }
          tracked_allocations = 0; track_allocations = true;
          filters.process(count);
          track_allocations = false;
          expect(tracked_allocations == 0, "filter routing changes must not allocate during processing");
          float error = 0;
          for (int output = 0; output < 5; ++output) for (int i = 0; i < count; ++i) {
            poly_float expected = 0;
            if (output == bus) {
              if (mode != 1) expected += first.output()->buffer[i];
              if (mode != 2) expected += second.output()->buffer[i];
            }
            for (int lane = 0; lane < poly_float::kSize; ++lane)
              error = std::max(error, std::abs(filters.output(output)->buffer[i][lane] - (expected * amplitude.output()->buffer[0])[lane]));
          }
          expect(error < 1e-6f, "serial/parallel audio must match independent filters on every output bus");
        }
      }
    }
  }

  // Source order of the original test: four oscillators, then the Sample,
  // Granular, Noise and Spectral sources, which now live in slots 5, 6, 8 and 7.
  constexpr int kSourceSlots[8] = { 0, 1, 2, 3, 4, 5, 7, 6 };

  struct Producers : ProducersModule {
    const poly_float* source(int index) {
      return slots_[kSourceSlots[index]]->output(SourceSlotModule::kLevelled)->buffer;
    }
  };

  void testAllSourcesAndBypass() {
    Producers producers;
    FiltersModule filters;
    std::array<vital::Value, ProducersModule::kNumInputs> inputs;
    for (int i = 0; i < ProducersModule::kNumInputs; ++i) producers.plug(&inputs[i], i);
    for (int input : {ProducersModule::kMidi, ProducersModule::kZoneMidi, ProducersModule::kZoneGlideMidi})
      inputs[input].set(60);
    inputs[ProducersModule::kVelocity].set(1);
    inputs[ProducersModule::kNotePressed].set(1);
    inputs[ProducersModule::kActiveVoices].set(1);
    filters.plug(producers.output(ProducersModule::kToFilter1), FiltersModule::kFilter1Input);
    filters.plug(producers.output(ProducersModule::kToFilter2), FiltersModule::kFilter2Input);
    producers.init(); filters.init();
    producers.prepareSelectedSources();
    producers.setSampleRate(48000); filters.setSampleRate(48000);
    producers.setFilter1On(filters.getFilter1OnValue());
    producers.setFilter2On(filters.getFilter2OnValue());
    producers.setFilterInputs(filters.getFilter1InputValue(), filters.getFilter2InputValue());
    const auto fc = filters.getControls(), pc = producers.getControls();
    fc.at("filter_1_mix")->set(0); fc.at("filter_2_mix")->set(0);
    fc.at("filter_1_destination")->set(constants::kBus1);
    fc.at("filter_2_destination")->set(constants::kBus2);
    const std::string names[] {"osc_1", "osc_2", "osc_3", "osc_4", "osc_5", "osc_6", "osc_8", "osc_7"};
    for (const auto& name : names) pc.at(name + "_on")->set(1);
    for (int i = 0; i < kNumOscillators; ++i)
      producers.getWavetable(i)->loadWaveFrame(PredefinedWaveFrames::getWaveFrame(PredefinedWaveFrames::kSaw), 0);
    pc.at("osc_5_sample_loop")->set(1);
    std::vector<float> pcm(48000);
    for (int i = 0; i < pcm.size(); ++i) pcm[i] = .2f * std::sin(i * .07f);
    for (Sample* sample : {producers.getSample(4), producers.getSample(5), producers.getSample(6)})
      sample->loadSample(pcm.data(), pcm.size(), 48000);
    inputs[ProducersModule::kReset].output()->trigger(poly_mask(-1), kVoiceOn, 0);
    for (int i = 0; i < 32; ++i) {
      producers.process(128);
      inputs[ProducersModule::kReset].output()->clearTrigger();
    }
    struct Scenario {
      bool on1, on2, link1, link2;
      // Output masks for sources assigned to Filter 1, Filter 2, and both.
      int outputs[3];
    };
    constexpr int raw = 1, bus1 = 2, bus2 = 4;
    const Scenario cases[] {
      {true, true, false, false, {bus1, bus2, bus1 | bus2}},
      {true, true, false, true, {bus2, bus2, bus2}},
      {true, true, true, false, {bus1, bus1, bus1}},
      {true, true, true, true, {bus1, bus1, bus1}},
      {false, true, false, true, {bus2, bus2, bus2}},
      {true, false, true, false, {bus1, bus1, bus1}},
      {true, false, false, true, {bus1, raw, bus1}},
      {false, true, true, false, {raw, bus2, bus2}},
      {false, false, true, true, {raw, raw, raw}},
      {true, true, false, false, {bus1, bus2, bus1 | bus2}},
    };
    const int destinations[] {constants::kFilter1, constants::kFilter2, constants::kDualFilters,
                              constants::kEffects, constants::kBus1, constants::kBus2,
                              constants::kBus3, constants::kDirectOut};
    for (int source = 0; source < 8; ++source) {
      for (int i = 0; i < 8; ++i) pc.at(names[i] + "_solo")->set(i == source);
      double energy = 0;
      for (const auto& scenario : cases) for (int d = 0; d < 8; ++d) {
        fc.at("filter_1_on")->set(scenario.on1); fc.at("filter_2_on")->set(scenario.on2);
        fc.at("filter_1_filter_input")->set(scenario.link1); fc.at("filter_2_filter_input")->set(scenario.link2);
        pc.at(names[source] + "_destination")->set(destinations[d]);
        producers.process(128); filters.process(128);
        const int expected_mask = d < 3 ? scenario.outputs[d] : 1 << (d - 3);
        float error = 0;
        for (int i = 0; i < 128; ++i) for (int lane = 0; lane < poly_float::kSize; ++lane) {
          const float sample = producers.source(source)[i][lane];
          energy += sample * sample;
          for (int bus = 0; bus < 5; ++bus) {
            const float actual = filters.output(bus)->buffer[i][lane] +
                producers.output(ProducersModule::kRawOut + bus)->buffer[i][lane];
            error = std::max(error, std::abs(actual - ((expected_mask & (1 << bus)) ? sample : 0)));
          }
        }
        expect(error < 1e-6f, names[source] + " must follow routing and bypass without duplicate or leaking audio");
      }
      expect(energy > .001, names[source] + " routing fixture must produce audible source data");
    }
  }

  std::vector<float> renderOscillator(int destination, bool reverse) {
    SynthPlugin plugin;
    plugin.setPlayConfigDetails(0, 2, 48000, 128); plugin.prepareToPlay(48000, 128);
    const auto controls = plugin.getControls();
    controls.at("oversampling")->set(0); plugin.checkOversampling();
    controls.at("osc_1_destination")->set(destination);
    controls.at("osc_1_unison_voices")->set(1); controls.at("osc_1_random_phase")->set(0);
    controls.at("filter_1_on")->set(1); controls.at("filter_2_on")->set(1);
    controls.at("filter_1_filter_input")->set(reverse);
    controls.at("filter_2_filter_input")->set(!reverse);
    controls.at("filter_1_cutoff")->set(60); controls.at("filter_2_cutoff")->set(80);
    AudioSampleBuffer audio(2, 128);
    MidiBuffer midi;
    midi.addEvent(MidiMessage::noteOn(1, 60, .8f), 17);
    midi.addEvent(MidiMessage::noteOn(1, 67, .7f), 51);
    midi.addEvent(MidiMessage::noteOn(1, 72, .6f), 75);
    midi.addEvent(MidiMessage::noteOn(1, 76, .7f), 83);
    midi.addEvent(MidiMessage::noteOn(1, 79, .5f), 105);
    std::vector<float> result;
    for (int block = 0; block < 32; ++block) {
      audio.clear(); plugin.processBlock(audio, midi); midi.clear();
      for (int i = 0; i < 128; ++i) for (int channel = 0; channel < 2; ++channel)
        result.push_back(audio.getSample(channel, i));
    }
    return result;
  }

  Component* withTitle(Component& component, const String& title) {
    if (component.getTitle() == title) return &component;
    for (auto* child : component.getChildren())
      if (auto* found = withTitle(*child, title)) return found;
    return nullptr;
  }

  void select(Component& component, const String& title, const String& text) {
    auto* combo = dynamic_cast<ComboBox*>(withTitle(component, title));
    expect(combo != nullptr, "routing editor must expose " + title.toStdString());
    if (!combo) return;
    for (int i = 0; i < combo->getNumItems(); ++i) if (combo->getItemText(i) == text) {
      combo->setSelectedItemIndex(i, sendNotificationSync); return;
    }
    expect(false, "routing editor must offer " + text.toStdString());
  }

  void testEditorRouting() {
    SynthPlugin plugin;
    const auto controls = plugin.getControls();
    controls.at("filter_1_on")->set(1); controls.at("filter_2_on")->set(1);
    SynthEditor editor(plugin);
    select(editor, "Editor group", "Filters");
    for (int receiving : {1, 2, 1}) {
      select(editor, "Editor element", "Filter " + String(receiving));
      auto* slider = dynamic_cast<Slider*>(withTitle(editor, "Filter " + String(receiving) +
                                                              " input from Filter " + String(3 - receiving)));
      expect(slider != nullptr, "each filter must expose its serial input control");
      if (!slider) continue;
      slider->setValue(1, sendNotificationSync);
      expect(controls.at("filter_" + std::to_string(receiving) + "_filter_input")->value() == 1,
             "editing Filter " + std::to_string(receiving) + " serial input must enable its link");
      expect(controls.at("filter_" + std::to_string(3 - receiving) + "_filter_input")->value() == 0,
             "editing Filter " + std::to_string(receiving) + " serial input must disable the opposite link");
    }
    select(editor, "Editor group", "Mixer and routing");
    select(editor, "Editor element", "Signal routing");
    controls.at("filter_1_filter_input")->set(1); controls.at("filter_2_filter_input")->set(1);
    editor.updateFullGui();
    auto* mode = dynamic_cast<ComboBox*>(withTitle(editor, "Filter routing mode"));
    auto* summary = dynamic_cast<Label*>(withTitle(editor, "Routing summary"));
    expect(mode && mode->getSelectedItemIndex() == 2, "conflicting host links must display the engine's precedence");
    expect(summary && summary->getText().contains("Filter 2 to Filter 1"),
           "serial summary must show the upstream filter feeding the last filter");
    controls.at("filter_1_on")->set(0); controls.at("filter_2_filter_input")->set(0);
    editor.updateFullGui();
    summary = dynamic_cast<Label*>(withTitle(editor, "Routing summary"));
    expect(summary && summary->getText().contains("serial link is inactive"),
           "summary must explain when the receiving filter is bypassed");
  }

  void testEditorFilterDestinations() {
    SynthPlugin plugin;
    const auto controls = plugin.getControls();
    SynthEditor editor(plugin);
    select(editor, "Editor group", "Mixer and routing");
    select(editor, "Editor element", "Signal routing");
    const int destinations[] {constants::kEffects, constants::kBus1, constants::kBus2,
                              constants::kBus3, constants::kDirectOut};
    const String labels[] {"Main effects", "Bus 1", "Bus 2", "Bus 3", "Direct out"};
    for (int filter : {1, 2}) {
      const String title = "Filter " + String(filter) + " destination";
      const std::string id = "filter_" + std::to_string(filter) + "_destination";
      auto* slider = dynamic_cast<Slider*>(withTitle(editor, title));
      expect(slider != nullptr, "Signal routing must expose " + title.toStdString());
      if (!slider) continue;
      expect(slider->isVisible() && !slider->getBounds().isEmpty() && slider->getWantsKeyboardFocus(),
             "filter destination must have visible bounds and accept keyboard focus");
      auto handler = slider->createAccessibilityHandler();
      auto* value = handler ? handler->getValueInterface() : nullptr;
      expect(value != nullptr && !value->isReadOnly(), "filter destination must be editable through accessibility");
      for (int index = 0; index < 5; ++index) {
        if (index > 0)
          expect(slider->keyPressed(KeyPress(KeyPress::rightKey)), "arrows must select the next filter destination");
        expect(controls.at(id)->value() == destinations[index], "filter destination must select " + labels[index].toStdString());
        expect(slider->getTextFromValue(slider->getValue()) == labels[index], "filter destination must display its name");
        if (value)
          expect(value->getCurrentValueAsString() == labels[index], "accessibility must announce the destination name");
      }
      slider->keyPressed(KeyPress(KeyPress::rightKey));
      expect(controls.at(id)->value() == constants::kDirectOut, "filter destinations must stop at Direct out");
      slider->keyPressed(KeyPress(KeyPress::backspaceKey));
      expect(controls.at(id)->value() == constants::kEffects, "reset must restore Main effects");
      if (value) {
        value->setValueAsString("Bus 3");
        expect(controls.at(id)->value() == constants::kBus3, "accessibility edits must select the requested bus");
      }
    }

    // Model changes and bypass must not hide the output controls or overwrite
    // destinations restored by the host or a saved preset.
    for (int model : {constants::kAnalog, constants::kFormant, constants::kComb}) {
      for (int filter : {1, 2}) {
        const std::string prefix = "filter_" + std::to_string(filter) + "_";
        plugin.parameterChanged(prefix + "model", model);
        plugin.parameterChanged(prefix + "on", 0);
        plugin.parameterChanged(prefix + "destination", filter == 1 ? constants::kBus3 : constants::kBus2);
      }
      editor.updateFullGui();
      for (int filter : {1, 2}) {
        auto* slider = dynamic_cast<Slider*>(withTitle(editor, "Filter " + String(filter) + " destination"));
        expect(slider && slider->getTextFromValue(slider->getValue()) == (filter == 1 ? "Bus 3" : "Bus 2"),
               "filter destination must survive model changes, bypass and GUI rebuilds");
      }
    }
  }

  int run() {
    ScopedJuceInitialiser_GUI gui;
    testCascadeAndDestinations();
    testAllSourcesAndBypass();
    testEditorRouting();
    testEditorFilterDestinations();
    for (bool reverse : {false, true}) {
      const auto upstream = renderOscillator(reverse ? constants::kFilter2 : constants::kFilter1, reverse);
      const auto both = renderOscillator(constants::kDualFilters, reverse);
      float error = 0; double energy = 0;
      for (int i = 0; i < both.size(); ++i) {
        error = std::max(error, std::abs(both[i] - upstream[i])); energy += upstream[i] * upstream[i];
      }
      expect(energy > .001, "polyphonic serial routing must render audible notes");
      expect(error < 1e-6f, "Filter 1 and 2 must match entering the serial chain once through the plug-in");
    }
    std::cout << "Filter routing: " << failures << " failures\n";
    return failures ? 1 : 0;
  }
}
