// Braids oscillator tests: every synthesis mode through the plugin, pitch at
// several sample rates, strikes on note-on, random phase, slot FM, host state
// and the editor's mode menu and mode parameter group.

#include "JuceHeader.h"
#include "synth_plugin.h"
#include "synth_editor.h"
#include "sound_engine.h"
#include "source_types.h"
#include "source_slot_module.h"
#include "braids_engine.h"
#include "braids_source.h"
#include "synth_strings.h"

#include <cmath>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

namespace {
  int failures = 0;

  void expect(bool condition, const std::string& message) {
    if (!condition) {
      std::cerr << "FAIL: " << message << '\n';
      ++failures;
    }
  }

  void close(double actual, double expected, const std::string& message, double tolerance) {
    expect(std::isfinite(actual) && std::abs(actual - expected) <= tolerance,
           message + " (actual=" + std::to_string(actual) + ", expected=" + std::to_string(expected) + ")");
  }

  int finish(const char* name) {
    if (failures == 0)
      std::cout << name << ": all tests passed\n";
    return failures == 0 ? 0 : 1;
  }

  struct MidiEvent {
    int sample;
    juce::MidiMessage message;
  };

  void setControl(SynthPlugin& plugin, const std::string& name, float value) {
    auto& controls = plugin.getControls();
    auto found = controls.find(name);
    expect(found != controls.end(), "control exists: " + name);
    if (found != controls.end())
      found->second->set(value);
  }

  float control(SynthPlugin& plugin, const std::string& name) {
    auto& controls = plugin.getControls();
    auto found = controls.find(name);
    return found == controls.end() ? -1.0f : found->second->value();
  }

  // Slot 1 is a Braids oscillator straight to the effects (no filter), every
  // effect and other oscillator off, Env 1 open with a near-zero release.
  void preparePlayback(SynthPlugin& plugin, int rate, int model) {
    plugin.setPlayConfigDetails(0, 2, rate, 128);
    plugin.prepareToPlay(rate, 128);
    setControl(plugin, "oversampling", 0.0f);
    for (int chain = 0; chain < 4; ++chain) {
      const auto prefix = chain == 0 ? std::string() : "bus_" + std::to_string(chain) + "_";
      for (int i = 1; i <= 20; ++i)
        setControl(plugin, prefix + "fx_slot_" + std::to_string(i) + "_type", 0.0f);
    }
    for (int i = 1; i <= 8; ++i)
      setControl(plugin, "osc_" + std::to_string(i) + "_on", 0.0f);
    plugin.parameterChanged("osc_1_type", static_cast<float>(vital::source_types::kBraids));
    setControl(plugin, "osc_1_on", 1.0f);
    setControl(plugin, "osc_1_destination", static_cast<float>(vital::constants::kEffects));
    setControl(plugin, "osc_1_braids_model", static_cast<float>(model));
    setControl(plugin, "env_1_attack", 0.0f);
    setControl(plugin, "env_1_sustain", 1.0f);
    setControl(plugin, "env_1_release", 0.0f);
    plugin.checkOversampling();
    expect(plugin.getEngine()->getSourceSlot(0)->activeType() == vital::source_types::kBraids,
           "slot 1 runs the Braids type");
  }

  // Renders `samples` frames (left channel) with MIDI events at given samples.
  std::vector<float> render(SynthPlugin& plugin, int samples, const std::vector<MidiEvent>& events) {
    std::vector<float> result(static_cast<size_t>(samples));
    for (int offset = 0; offset < samples;) {
      const int count = std::min(128, samples - offset);
      juce::AudioBuffer<float> audio(2, count);
      audio.clear();
      juce::MidiBuffer midi;
      for (const MidiEvent& event : events) {
        if (event.sample >= offset && event.sample < offset + count)
          midi.addEvent(event.message, event.sample - offset);
      }
      plugin.processBlock(audio, midi);
      for (int i = 0; i < count; ++i)
        result[static_cast<size_t>(offset + i)] = audio.getSample(0, i);
      offset += count;
    }
    return result;
  }

  double rms(const std::vector<float>& signal, int start, int end) {
    double sum = 0.0;
    end = std::min<int>(end, static_cast<int>(signal.size()));
    for (int i = start; i < end; ++i)
      sum += double(signal[static_cast<size_t>(i)]) * signal[static_cast<size_t>(i)];
    return end > start ? std::sqrt(sum / (end - start)) : 0.0;
  }

  double frequency(const std::vector<float>& signal, int start, int end, int rate) {
    int first = -1;
    int last = -1;
    int crossings = 0;
    for (int i = start + 1; i < end; ++i) {
      if (signal[static_cast<size_t>(i - 1)] <= 0.0f && signal[static_cast<size_t>(i)] > 0.0f) {
        if (first < 0)
          first = i;
        last = i;
        crossings++;
      }
    }
    return crossings > 1 ? (crossings - 1) * double(rate) / (last - first) : 0.0;
  }

  bool finite(const std::vector<float>& signal) {
    for (float value : signal) {
      if (!std::isfinite(value))
        return false;
    }
    return true;
  }

  constexpr int kRate = 48000;

  // Every mode sounds, stays finite and bounded, and goes quiet after note-off.
  int models() {
    for (int model = 0; model < vital::braids_engine::kNumModels; ++model) {
      SynthPlugin plugin;
      preparePlayback(plugin, kRate, model);
      const auto audio = render(plugin, kRate / 2, { { 0, juce::MidiMessage::noteOn(1, 57, 1.0f) },
                                                     { kRate / 4, juce::MidiMessage::noteOff(1, 57) } });
      const std::string name = std::to_string(model) + " " + strings::kBraidsModelNames[model];
      expect(finite(audio), name + " renders finite audio");
      expect(rms(audio, 0, kRate / 4) > 1.0e-3, name + " is audible (rms " + std::to_string(rms(audio, 0, kRate / 4)) + ")");
      float peak = 0.0f;
      for (float value : audio)
        peak = std::max(peak, std::abs(value));
      expect(peak < 4.0f, name + " stays bounded (peak " + std::to_string(peak) + ")");
      expect(rms(audio, kRate / 4 + kRate / 20, kRate / 2) < 1.0e-4, name + " stops after note-off");
    }
    return finish("models");
  }

  // The morph mode (a plain triangle here) tracks the keyboard at every host
  // rate and with oversampling.
  int pitch() {
    struct Case { int rate; int oversampling; int note; float transpose; double expected; };
    const Case cases[] = {
      { 48000, 0, 69, 0.0f, 440.0 },
      { 44100, 0, 69, 0.0f, 440.0 },
      { 96000, 0, 69, 0.0f, 440.0 },
      { 48000, 1, 69, 0.0f, 440.0 },
      { 48000, 0, 57, 0.0f, 220.0 },
      { 48000, 0, 57, 12.0f, 440.0 },
      { 44100, 0, 81, 0.0f, 880.0 },
    };
    for (const Case& test : cases) {
      SynthPlugin plugin;
      preparePlayback(plugin, test.rate, 1);
      setControl(plugin, "oversampling", static_cast<float>(test.oversampling));
      setControl(plugin, "osc_1_braids_timbre", 0.0f);  // A plain triangle.
      setControl(plugin, "osc_1_braids_color", 0.0f);
      setControl(plugin, "osc_1_braids_transpose", test.transpose);
      plugin.checkOversampling();
      const auto audio = render(plugin, test.rate / 2, { { 0, juce::MidiMessage::noteOn(1, test.note, 1.0f) } });
      close(frequency(audio, test.rate / 10, test.rate / 2, test.rate), test.expected,
            "note " + std::to_string(test.note) + " at " + std::to_string(test.rate) + " Hz, oversampling " +
                std::to_string(test.oversampling) + ", transpose " + std::to_string(test.transpose),
            test.expected * 0.005);
    }
    return finish("pitch");
  }

  // Plucked string: every note-on plucks again, so a repeated note sounds
  // again after the first has decayed.
  int strike() {
    SynthPlugin plugin;
    preparePlayback(plugin, kRate, 28);
    setControl(plugin, "osc_1_braids_timbre", 0.0f);  // Shortest decay.
    const int second = kRate;
    const auto audio = render(plugin, kRate * 3 / 2, { { 0, juce::MidiMessage::noteOn(1, 60, 1.0f) },
                                                       { second - 2000, juce::MidiMessage::noteOff(1, 60) },
                                                       { second, juce::MidiMessage::noteOn(1, 60, 1.0f) } });
    const double attack = rms(audio, 0, kRate / 20);
    const double decayed = rms(audio, second - 6000, second - 3000);
    const double again = rms(audio, second, second + kRate / 20);
    expect(attack > 0.02, "the pluck sounds (" + std::to_string(attack) + ")");
    expect(decayed < attack * 0.1, "the string decays while held (" + std::to_string(decayed) + ")");
    expect(again > attack * 0.5, "a new note plucks again (" + std::to_string(again) + ")");
    return finish("strike");
  }

  // Phase Randomization: off, every note from silence starts the same way; on,
  // notes start at different points of the cycle, in analog and digital modes,
  // and still play in tune.
  int randomPhase() {
    // Morph with Timbre and Color at 0 is a plain triangle, so its pitch can be
    // read from zero crossings.
    struct Mode { int model; const char* name; bool triangle; };
    for (const Mode mode : { Mode { 1, "Morph", true }, Mode { 0, "CS-80 sawtooth", false },
                             Mode { 2, "Saw/square", false }, Mode { 25, "FM", false },
                             Mode { 37, "Wavetables", false } }) {
      // Notes from silence, one at a time. The engine can give each one another
      // voice lane, and it renders idle lanes too, so notes play on fresh lanes
      // and on lanes that have run before. The first note only warms up the
      // plugin's parameter smoothing and is not compared.
      constexpr int kNotes = 6;
      auto notes = [&](float amount) {
        SynthPlugin plugin;
        preparePlayback(plugin, kRate, mode.model);
        setControl(plugin, "polyphony", 1.0f);
        setControl(plugin, "osc_1_braids_random_phase", amount);
        if (mode.triangle) {
          setControl(plugin, "osc_1_braids_timbre", 0.0f);
          setControl(plugin, "osc_1_braids_color", 0.0f);
        }
        const int spacing = 128 * 100;
        const int length = 128 * 40;
        std::vector<MidiEvent> events;
        for (int note = 0; note < kNotes; ++note) {
          events.push_back({ note * spacing, juce::MidiMessage::noteOn(1, 57, 1.0f) });
          events.push_back({ note * spacing + length, juce::MidiMessage::noteOff(1, 57) });
        }
        const auto audio = render(plugin, kNotes * spacing, events);
        expect(finite(audio), std::string(mode.name) + " renders finite audio");
        std::vector<std::vector<float>> result;
        for (int note = 1; note < kNotes; ++note) {
          const auto start = audio.begin() + note * spacing;
          result.emplace_back(start, start + length);
          expect(rms(audio, (note - 1) * spacing + length + kRate / 20, note * spacing) < 1.0e-4,
                 std::string(mode.name) + " is silent before note " + std::to_string(note + 1));
        }
        return result;
      };
      // Mean absolute difference, relative to the first note's mean level.
      auto difference = [](const std::vector<float>& a, const std::vector<float>& b) {
        double sum = 0.0;
        double level = 0.0;
        for (size_t i = 0; i < a.size(); ++i) {
          sum += std::abs(double(a[i]) - b[i]);
          level += std::abs(double(a[i]));
        }
        return level > 0.0 ? sum / level : 0.0;
      };

      const auto fixed = notes(0.0f);
      for (size_t note = 1; note < fixed.size(); ++note) {
        const double same = difference(fixed[0], fixed[note]);
        expect(same < 1.0e-6, std::string(mode.name) + ": at 0% every note starts the same way (difference " +
                                  std::to_string(same) + ")");
      }
      // Two random phases can fall close together, or half a cycle apart in a
      // mode whose wave repeats every half cycle, so look across several notes.
      const auto random = notes(1.0f);
      double changed = 0.0;
      for (size_t note = 1; note < random.size(); ++note)
        changed = std::max(changed, difference(random[0], random[note]));
      expect(changed > 0.1, std::string(mode.name) + ": at 100% notes start at different phases (difference " +
                                std::to_string(changed) + ")");
      if (mode.triangle) {
        for (const auto& note : random) {
          close(frequency(note, kRate / 20, static_cast<int>(note.size()), kRate), 220.0,
                std::string(mode.name) + ": a random start keeps the pitch", 220.0 * 0.01);
        }
      }
    }

    // Every mode at the lowest pitch, on fresh voices: a start past an analog
    // edge must not divide by a zero phase increment.
    for (int model = 0; model < vital::braids_engine::kNumModels; ++model) {
      SynthPlugin plugin;
      preparePlayback(plugin, kRate, model);
      setControl(plugin, "osc_1_braids_random_phase", 1.0f);
      setControl(plugin, "osc_1_braids_transpose", -48.0f);
      std::vector<MidiEvent> events;
      for (int note = 0; note < 8; ++note)
        events.push_back({ 0, juce::MidiMessage::noteOn(1, note, 1.0f) });
      const auto audio = render(plugin, kRate / 8, events);
      expect(finite(audio), std::to_string(model) + " " + strings::kBraidsModelNames[model] +
                                " renders finite audio at the lowest pitch with random phase");
    }
    return finish("random phase");
  }

  // Ring and frequency modulation from another slot reach the Braids type.
  int slotModulation() {
    auto render_with = [](int distortion_type, float amount) {
      SynthPlugin plugin;
      preparePlayback(plugin, kRate, 0);
      plugin.parameterChanged("osc_2_type", static_cast<float>(vital::source_types::kWavetable));
      setControl(plugin, "osc_2_on", 1.0f);
      setControl(plugin, "osc_2_level", 0.0f);
      setControl(plugin, "osc_2_transpose", 7.0f);
      setControl(plugin, "osc_1_distortion_type", static_cast<float>(distortion_type));
      setControl(plugin, "osc_1_distortion_amount", amount);
      plugin.checkOversampling();
      return render(plugin, kRate / 4, { { 0, juce::MidiMessage::noteOn(1, 60, 1.0f) } });
    };
    const auto dry = render_with(vital::SynthOscillator::kNone, 0.0f);
    double difference_fm = 0.0;
    double difference_rm = 0.0;
    const auto fm = render_with(vital::SynthOscillator::kFmOscillatorA, 0.8f);
    const auto rm = render_with(vital::SynthOscillator::kRmOscillatorA, 1.0f);
    for (size_t i = 0; i < dry.size(); ++i) {
      difference_fm += std::abs(fm[i] - dry[i]);
      difference_rm += std::abs(rm[i] - dry[i]);
    }
    expect(finite(fm) && finite(rm), "modulated renders are finite");
    expect(difference_fm > 1.0, "FM from oscillator 2 changes the sound");
    expect(difference_rm > 1.0, "RM from oscillator 2 changes the sound");
    expect(vital::source_types::supportsFm(vital::source_types::kBraids) &&
           vital::source_types::supportsRm(vital::source_types::kBraids), "Braids accepts slot FM and RM");
    return finish("slot modulation");
  }

  // The resampler passes a 96 kHz stream through unchanged at 96 kHz and has
  // unity gain at DC at every rate.
  int resampler() {
    for (double rate : { 44100.0, 48000.0, 88200.0, 96000.0, 192000.0, 384000.0 }) {
      auto kernel = vital::BraidsSource::kernelFor(rate);
      expect(kernel == vital::BraidsSource::kernelFor(rate), "kernels are shared");
      for (int phase = 0; phase < vital::BraidsSource::kKernelPhases; phase += 37) {
        double sum = 0.0;
        for (int tap = 0; tap < vital::BraidsSource::kTaps; ++tap)
          sum += kernel->table[static_cast<size_t>(phase + 1 + tap * vital::BraidsSource::kKernelPhases)];
        close(sum, 1.0, "DC gain at " + std::to_string(rate) + " Hz phase " + std::to_string(phase), 0.01);
      }
    }
    auto native = vital::BraidsSource::kernelFor(96000.0);
    for (int tap = 1; tap <= vital::BraidsSource::kTaps; ++tap) {
      const float value = native->table[static_cast<size_t>(tap * vital::BraidsSource::kKernelPhases)];
      close(value, tap == vital::BraidsSource::kHalfTaps ? 1.0 : 0.0, "96 kHz passes samples through, tap " +
            std::to_string(tap), 1.0e-6);
    }
    return finish("resampler");
  }

  // Parameters exist on every slot, survive host state, and old states load
  // with the defaults.
  int parameters() {
    juce::MemoryBlock state;
    {
      SynthPlugin plugin;
      preparePlayback(plugin, kRate, 0);
      for (int slot = 1; slot <= vital::kNumOscillators; ++slot) {
        const std::string prefix = "osc_" + std::to_string(slot) + "_braids_";
        for (const char* suffix : { "model", "timbre", "color", "resolution", "sample_rate", "transpose", "tune",
                                    "random_phase" })
          expect(plugin.getControls().count(prefix + suffix) == 1, "control exists: " + prefix + suffix);
      }
      expect(control(plugin, "osc_2_braids_resolution") == vital::braids_engine::kFullResolution &&
             control(plugin, "osc_2_braids_sample_rate") == vital::braids_engine::kFullSampleRate &&
             control(plugin, "osc_2_braids_timbre") == 0.5f, "defaults are clean 16 bit, 96 kHz, centred");
      expect(control(plugin, "osc_2_braids_random_phase") == 0.0f, "random phase is off by default");
      setControl(plugin, "osc_1_braids_model", 36.0f);
      setControl(plugin, "osc_1_braids_timbre", 0.25f);
      setControl(plugin, "osc_1_braids_color", 0.75f);
      setControl(plugin, "osc_1_braids_resolution", 3.0f);
      setControl(plugin, "osc_1_braids_sample_rate", 2.0f);
      setControl(plugin, "osc_1_braids_random_phase", 0.6f);
      plugin.getStateInformation(state);
    }
    {
      SynthPlugin plugin;
      preparePlayback(plugin, kRate, 0);
      plugin.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
      plugin.checkOversampling();
      expect(plugin.getEngine()->getSourceSlot(0)->activeType() == vital::source_types::kBraids,
             "the restored slot runs Braids");
      expect(control(plugin, "osc_1_braids_model") == 36.0f, "the mode is restored");
      expect(control(plugin, "osc_1_braids_timbre") == 0.25f && control(plugin, "osc_1_braids_color") == 0.75f,
             "Timbre and Color are restored");
      expect(control(plugin, "osc_1_braids_resolution") == 3.0f &&
             control(plugin, "osc_1_braids_sample_rate") == 2.0f, "bit depth and sample rate are restored");
      close(control(plugin, "osc_1_braids_random_phase"), 0.6, "random phase is restored", 1.0e-6);
      const auto audio = render(plugin, kRate / 4, { { 0, juce::MidiMessage::noteOn(1, 60, 1.0f) } });
      expect(finite(audio) && rms(audio, 0, kRate / 4) > 1.0e-3, "the restored snare plays");
    }
    return finish("parameters");
  }

  // ---- Editor ---------------------------------------------------------------------

  Component* findTitled(Component& parent, const std::function<bool(const String&)>& match) {
    if (match(parent.getTitle()))
      return &parent;
    for (auto* child : parent.getChildren()) {
      if (auto* found = findTitled(*child, match))
        return found;
    }
    return nullptr;
  }

  Component* titledExactly(Component& parent, const String& title) {
    return findTitled(parent, [&title](const String& text) { return text == title; });
  }

  Component* oscillatorSection(Component& parent) {
    if (parent.getProperties()["SectionName"].toString() == "Oscillator 1")
      return &parent;
    for (auto* child : parent.getChildren()) {
      if (auto* found = oscillatorSection(*child))
        return found;
    }
    return nullptr;
  }

  template <typename Predicate>
  void pumpUntil(Predicate done, int timeout_ms = 5000) {
    const uint32 start = juce::Time::getMillisecondCounter();
    while (!done() && juce::Time::getMillisecondCounter() - start < static_cast<uint32>(timeout_ms))
      juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
    juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
  }

  int editorTest() {
    SynthPlugin synth;
    synth.parameterChanged("osc_1_type", static_cast<float>(vital::source_types::kBraids));
    SynthEditor editor(synth);
    editor.updateFullGui();
    pumpUntil([] { return false; }, 200);

    auto section = [&] { return oscillatorSection(editor); };
    expect(section() != nullptr, "Oscillator 1 section exists");
    if (section() == nullptr)
      return finish("editor");

    auto* menu = dynamic_cast<ComboBox*>(titledExactly(*section(), "1 Macro Synthesis Mode"));
    expect(menu != nullptr, "the synthesis mode is a menu");
    if (menu != nullptr) {
      expect(menu->getNumItems() == vital::braids_engine::kNumModels, "the menu lists every mode");
      expect(menu->getItemText(0) == "CS-80 sawtooth" && menu->getItemText(28) == "Plucked string" &&
             menu->getItemText(47) == "Question mark", "modes read by name");
      expect(menu->getWantsKeyboardFocus() && menu->getDescription().isNotEmpty(), "the menu is accessible");
    }
    expect(titledExactly(*section(), "Macro controls") != nullptr, "a Braids controls group");
    expect(titledExactly(*section(), "1 Macro Bit Depth") != nullptr &&
           titledExactly(*section(), "1 Macro Sample Rate") != nullptr, "bit depth and sample rate rows");
    expect(titledExactly(*section(), "CS-80 sawtooth parameters") != nullptr, "a group named after the mode");
    auto* timbre = titledExactly(*section(), "1 Notch width (Timbre)");
    expect(timbre != nullptr, "Timbre is named after what it does");
    expect(titledExactly(*section(), "1 Notch shape (Color)") != nullptr, "Color is named after what it does");
    if (timbre != nullptr)
      expect(timbre->getDescription().contains("notch width") && timbre->getDescription().contains("CS-80 sawtooth"),
             "Timbre's description names the mode");
    expect(findTitled(*section(), [](const String& t) { return t.contains("Multi-sample") || t.contains("Noise Type") ||
                                                               t.contains("Wave Frame"); }) == nullptr,
           "other types' rows are hidden");
    expect(findTitled(*section(), [](const String& t) { return t.startsWith("1 Macro Transpose"); }) != nullptr,
           "transpose sits with pitch");
    if (auto* pitch = titledExactly(*section(), "Pitch and unison")) {
      auto* tune = titledExactly(*pitch, "1 Macro Tune");
      auto* random_phase = titledExactly(*pitch, "1 Macro Phase Randomization");
      expect(random_phase != nullptr, "random phase sits with pitch");
      if (random_phase != nullptr) {
        expect(random_phase->getWantsKeyboardFocus() && random_phase->getDescription().contains("starts its cycle"),
               "random phase is accessible and described");
        if (auto* handler = random_phase->getAccessibilityHandler()) {
          if (auto* value = handler->getValueInterface())
            expect(value->getCurrentValueAsString() == "0%", "random phase reads as a percentage (" +
                                                                 value->getCurrentValueAsString().toStdString() + ")");
        }
      }
      if (tune != nullptr && random_phase != nullptr)
        expect(random_phase->getScreenY() > tune->getScreenY(), "random phase follows Tune");
    } else {
      expect(false, "a Pitch and unison group");
    }
    expect(findTitled(*section(), [](const String& t) { return t.startsWith("Oscillator 1") &&
                                                               t.contains("braids"); }) != nullptr,
           "the section title names the type");

    if (menu != nullptr) {
      menu->setSelectedItemIndex(28, juce::sendNotificationSync);
      pumpUntil([&] { return section() != nullptr && titledExactly(*section(), "Plucked string parameters") != nullptr; });
      expect(section() != nullptr && titledExactly(*section(), "Plucked string parameters") != nullptr,
             "choosing a mode renames its group");
      expect(section() != nullptr && titledExactly(*section(), "1 Decay (Timbre)") != nullptr &&
             titledExactly(*section(), "1 Plucking position (Color)") != nullptr, "the rows follow the mode");
      expect(section() != nullptr && titledExactly(*section(), "CS-80 sawtooth parameters") == nullptr,
             "the old mode's group is gone");
      expect(synth.getControls()["osc_1_braids_model"]->value() == 28.0f, "the menu sets the mode");
      auto* focused = Component::getCurrentlyFocusedComponent();
      expect(focused == nullptr || focused->getTitle() == "1 Macro Synthesis Mode",
             "focus stays on the mode menu after the rebuild");
    }
    return finish("editor");
  }
  // Not a ctest: how long eight-note chords take to render, per mode family.
  int cpu() {
    for (int model : { 0, 9, 14, 28, 31, 37, 44 }) {
      SynthPlugin plugin;
      preparePlayback(plugin, kRate, model);
      std::vector<MidiEvent> events;
      for (int i = 0; i < 8; ++i)
        events.push_back({ 0, juce::MidiMessage::noteOn(1, 48 + i * 3, 1.0f) });
      render(plugin, 128, events);
      const double start = juce::Time::getMillisecondCounterHiRes();
      render(plugin, kRate * 10, {});
      const double elapsed = juce::Time::getMillisecondCounterHiRes() - start;
      std::cout << strings::kBraidsModelNames[model] << ": 8 voices, 10 s in " << elapsed << " ms ("
                << elapsed / 100.0 << "% of real time)\n";
    }
    return 0;
  }
} // namespace

int main(int argc, char** argv) {
  juce::ScopedJuceInitialiser_GUI juce;
  const juce::String flag = argc > 1 ? juce::String(argv[1]) : juce::String();
  if (flag == "--models")
    return models();
  if (flag == "--pitch")
    return pitch();
  if (flag == "--strike")
    return strike();
  if (flag == "--random-phase")
    return randomPhase();
  if (flag == "--slot-modulation")
    return slotModulation();
  if (flag == "--resampler")
    return resampler();
  if (flag == "--parameters")
    return parameters();
  if (flag == "--editor")
    return editorTest();
  if (flag == "--cpu")
    return cpu();
  std::cerr << "usage: AtlasBraidsTests --models | --pitch | --strike | --random-phase | --slot-modulation | --resampler | "
               "--parameters | --editor\n";
  return 2;
}
