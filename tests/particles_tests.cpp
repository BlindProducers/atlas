// Particles effect tests: the engine against the granular processor driven
// directly, its latency at several rates, every mode and quality, triggers
// from notes and from Trigger, the parameters in every slot and bus, and the
// editor's rows.

#include "JuceHeader.h"
#include "synth_plugin.h"
#include "synth_editor.h"
#include "sound_engine.h"
#include "particles_module.h"
#include "particles/particles_engine.h"
#include "synth_strings.h"

#include <cmath>
#include <cstdint>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

namespace {
  namespace particles = atlas::particles;
  int failures = 0;

  void expect(bool condition, const std::string& message) {
    if (!condition) {
      std::cerr << "FAIL: " << message << '\n';
      ++failures;
    }
  }

  int finish(const char* name) {
    if (failures == 0)
      std::cout << name << ": all tests passed\n";
    return failures == 0 ? 0 : 1;
  }

  bool finite(const std::vector<float>& signal) {
    for (float value : signal) {
      if (!std::isfinite(value))
        return false;
    }
    return true;
  }

  double rms(const std::vector<float>& signal, size_t start, size_t end) {
    end = std::min(end, signal.size());
    double sum = 0.0;
    for (size_t i = start; i < end; ++i)
      sum += double(signal[i]) * signal[i];
    return end > start ? std::sqrt(sum / double(end - start)) : 0.0;
  }

  // A deterministic stereo signal: two detuned saws and some noise.
  float testSignal(int index, int channel) {
    const float phase_a = std::fmod(index * (220.0f + 3.0f * channel) / 32000.0f, 1.0f);
    const float phase_b = std::fmod(index * 331.0f / 32000.0f, 1.0f);
    uint32_t hash = static_cast<uint32_t>(index) * 2654435761u + static_cast<uint32_t>(channel) * 40503u;
    hash ^= hash >> 13;
    const float noise = static_cast<float>(hash & 0xffff) / 65536.0f - 0.5f;
    return 0.3f * (phase_a - 0.5f) + 0.2f * (phase_b - 0.5f) + 0.05f * noise;
  }

  particles::Parameters parametersFor(int mode, int quality, int block) {
    particles::Parameters p;
    p.mode = mode;
    p.quality = quality;
    p.position = 0.3f + 0.2f * std::sin(block * 0.01f);
    p.size = 0.6f;
    p.pitch = block % 400 < 200 ? 0.0f : 7.0f;
    p.density = 0.8f;
    p.texture = 0.6f;
    p.dry_wet = 0.8f;
    p.stereo_spread = 0.5f;
    p.feedback = 0.3f;
    p.reverb = 0.3f;
    p.freeze = block % 1000 > 700;
    return p;
  }

  // At 32 kHz the engine is the processor, block for block, behind one block
  // of delay: its output matches the processor driven directly, sample for
  // sample, in every mode and quality.
  int engine() {
    constexpr int kBlocks = 1500;
    constexpr int kSize = particles::kBlockSize;
    // The engine's 16-bit output scale (particles_engine.cpp).
    constexpr float kOutputScale = 2.8284271f / 32768.0f;
    for (int mode = 0; mode < particles::kNumModes; ++mode) {
      for (int quality = 0; quality < particles::kNumQualities; ++quality) {
        std::vector<int16_t> input(static_cast<size_t>(kBlocks * kSize * 2));
        std::vector<particles::Parameters> parameters;
        std::vector<bool> triggers;
        for (int i = 0; i < kBlocks * kSize; ++i) {
          for (int ch = 0; ch < 2; ++ch)
            input[static_cast<size_t>(2 * i + ch)] = static_cast<int16_t>(std::lround(testSignal(i, ch) * 32768.0f));
        }
        for (int block = 0; block < kBlocks; ++block) {
          parameters.push_back(parametersFor(mode, quality, block));
          triggers.push_back(block % 97 == 3);
        }
        particles::seedRandom(0x21);
        const std::vector<int16_t> reference = particles::renderReference(input, parameters, triggers);
        particles::seedRandom(0x21);

        particles::Engine engine;
        engine.setParameters(parameters.front());
        engine.prepare(particles::kNativeRate);
        expect(engine.latencySamples() == kSize, "the engine waits one block at 32 kHz");
        std::vector<float> left(kSize), right(kSize), out_left(kSize), out_right(kSize);
        std::vector<int16_t> rendered;
        for (int block = 0; block < kBlocks + 1; ++block) {
          const int index = std::min(block, kBlocks - 1);
          engine.setParameters(parameters[static_cast<size_t>(index)]);
          if (block < kBlocks && triggers[static_cast<size_t>(block)]) engine.trigger();
          for (int i = 0; i < kSize; ++i) {
            const size_t at = static_cast<size_t>(2 * (block * kSize + i));
            left[static_cast<size_t>(i)] = block < kBlocks ? input[at] / 32768.0f : 0.0f;
            right[static_cast<size_t>(i)] = block < kBlocks ? input[at + 1] / 32768.0f : 0.0f;
          }
          engine.process(left.data(), right.data(), out_left.data(), out_right.data(), kSize);
          if (block == 0) continue;
          for (int i = 0; i < kSize; ++i) {
            rendered.push_back(static_cast<int16_t>(std::lround(out_left[static_cast<size_t>(i)] / kOutputScale)));
            rendered.push_back(static_cast<int16_t>(std::lround(out_right[static_cast<size_t>(i)] / kOutputScale)));
          }
        }
        size_t mismatches = 0, first = 0;
        for (size_t i = 0; i < reference.size(); ++i) {
          if (reference[i] != rendered[i]) {
            if (mismatches == 0) first = i;
            ++mismatches;
          }
        }
        const std::string name = strings::kParticlesModeNames[mode] + ", " + strings::kParticlesQualityNames[quality];
        expect(mismatches == 0, name + ": the engine matches the processor (" + std::to_string(mismatches) +
                                " samples differ, the first at " + std::to_string(first) + ")");
        double energy = 0.0;
        for (int16_t sample : reference) energy += double(sample) * sample;
        expect(energy > 0.0, name + ": the processor makes sound");
      }
    }
    return finish("engine");
  }

  // A fully dry impulse peaks at the latency the engine reports, at every
  // rate Atlas processes at, and a dry tone leaves at the level it came in.
  int latency() {
    for (int rate : { 32000, 44100, 48000, 88200, 96000, 176400, 192000, 384000 }) {
      particles::Parameters p;
      p.mode = particles::kGranular;
      p.dry_wet = 0.0f;
      const std::string at = " at " + std::to_string(rate) + " Hz";
      int latency = 0;
      {
        particles::Engine engine;
        engine.setParameters(p);
        engine.prepare(rate);
        latency = engine.latencySamples();
        expect(latency > 0 && latency <= vital::ParticlesModule::kMaxLatencySamples,
               "latency" + at + " fits the shells' reserve");
        // Let the dry/wet smoothing settle first.
        const int settle = rate / 10;
        const int length = settle + latency + rate / 50;
        std::vector<float> in(static_cast<size_t>(length), 0.0f), out_left(in.size()), out_right(in.size());
        in[static_cast<size_t>(settle)] = 0.5f;
        engine.process(in.data(), in.data(), out_left.data(), out_right.data(), length);
        int peak = 0;
        for (int i = 1; i < length; ++i) {
          if (std::abs(out_left[static_cast<size_t>(i)]) > std::abs(out_left[static_cast<size_t>(peak)]))
            peak = i;
        }
        expect(peak - settle == latency, "dry impulse" + at + " arrives after " + std::to_string(latency) +
                                         " samples (arrived after " + std::to_string(peak - settle) + ")");
        expect(finite(out_left) && finite(out_right), "output finite" + at);
      }
      {
        particles::Engine engine;
        engine.setParameters(p);
        engine.prepare(rate);
        const int length = rate / 2;
        std::vector<float> in(static_cast<size_t>(length)), out_left(in.size()), out_right(in.size());
        for (int i = 0; i < length; ++i)
          in[static_cast<size_t>(i)] = 0.25f * std::sin(2.0f * 3.14159265f * 440.0f * i / rate);
        engine.process(in.data(), in.data(), out_left.data(), out_right.data(), length);
        const double gain = rms(out_left, static_cast<size_t>(length / 2), in.size()) /
                            rms(in, static_cast<size_t>(length / 2 - latency), in.size() - static_cast<size_t>(latency));
        expect(std::abs(20.0 * std::log10(gain)) < 0.3, "the dry signal leaves at the level it came in" + at +
               " (" + std::to_string(20.0 * std::log10(gain)) + " dB)");
      }
    }
    return finish("latency");
  }

  std::vector<float> renderEngine(particles::Engine& engine, int rate, int samples, bool silent_input) {
    std::vector<float> in_left(static_cast<size_t>(samples)), in_right(in_left.size());
    std::vector<float> out_left(in_left.size()), out_right(in_left.size());
    for (int i = 0; i < samples; ++i) {
      const int native = static_cast<int>(int64_t(i) * particles::kNativeRate / rate);
      in_left[static_cast<size_t>(i)] = silent_input ? 0.0f : testSignal(native, 0);
      in_right[static_cast<size_t>(i)] = silent_input ? 0.0f : testSignal(native, 1);
    }
    engine.process(in_left.data(), in_right.data(), out_left.data(), out_right.data(), samples);
    return out_left;
  }

  // Every mode and quality sounds from a signal and stays finite; frozen, the
  // sound goes on after the input stops; changing quality restarts cleanly.
  int modes() {
    constexpr int kRate = 48000;
    for (int mode = 0; mode < particles::kNumModes; ++mode) {
      for (int quality = 0; quality < particles::kNumQualities; ++quality) {
        const std::string name = strings::kParticlesModeNames[mode] + ", " + strings::kParticlesQualityNames[quality];
        particles::Engine engine;
        particles::Parameters p;
        p.mode = mode;
        p.quality = quality;
        p.dry_wet = 1.0f;
        p.density = 0.9f;
        p.size = 0.4f;
        engine.setParameters(p);
        engine.prepare(kRate);
        const auto sounding = renderEngine(engine, kRate, kRate, false);
        expect(finite(sounding), name + ": finite");
        expect(rms(sounding, kRate / 2, sounding.size()) > 1.0e-3, name + ": wet output sounds");
        p.freeze = true;
        engine.setParameters(p);
        renderEngine(engine, kRate, kRate / 4, false);
        const auto frozen = renderEngine(engine, kRate, kRate, true);
        expect(finite(frozen), name + ": frozen output finite");
        expect(rms(frozen, kRate / 2, frozen.size()) > 1.0e-4, name + ": frozen buffer keeps playing");
        p.quality = (quality + 1) % particles::kNumQualities;
        p.freeze = false;
        engine.setParameters(p);
        const auto changed = renderEngine(engine, kRate, kRate, false);
        expect(finite(changed), name + ": finite after a quality change");
      }
    }
    return finish("modes");
  }

  // ---- Through the plugin --------------------------------------------------

  void setControl(SynthPlugin& plugin, const std::string& name, float value) {
    auto& controls = plugin.getControls();
    auto found = controls.find(name);
    expect(found != controls.end(), "control exists: " + name);
    if (found != controls.end())
      found->second->set(value);
  }

  // Oscillator 1 into the main chain, Particles in slot 1.
  void preparePlayback(SynthPlugin& plugin, int rate) {
    plugin.setPlayConfigDetails(0, 2, rate, 128);
    plugin.prepareToPlay(rate, 128);
    setControl(plugin, "oversampling", 0.0f);
    for (int chain = 0; chain < 4; ++chain) {
      const auto prefix = chain == 0 ? std::string() : "bus_" + std::to_string(chain) + "_";
      for (int i = 1; i <= vital::constants::kMaxEffectSlots; ++i)
        setControl(plugin, prefix + "fx_slot_" + std::to_string(i) + "_type", 0.0f);
    }
    plugin.parameterChanged("fx_slot_1_type", vital::constants::kParticles + 1);
    plugin.parameterChanged("fx_slot_1_enabled", 1.0f);
    setControl(plugin, "osc_1_destination", static_cast<float>(vital::constants::kEffects));
    setControl(plugin, "env_1_attack", 0.0f);
    setControl(plugin, "env_1_sustain", 1.0f);
    setControl(plugin, "env_1_release", 0.0f);
    plugin.checkOversampling();
  }

  struct MidiEvent {
    int sample;
    juce::MidiMessage message;
  };

  std::vector<float> render(SynthPlugin& plugin, int samples, const std::vector<MidiEvent>& events,
                            const std::function<void(int)>& before_block = {}) {
    std::vector<float> result(static_cast<size_t>(samples));
    for (int offset = 0; offset < samples;) {
      const int count = std::min(128, samples - offset);
      if (before_block) before_block(offset);
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

  // With Density at its centre no grains play, until a note-on or Trigger
  // rising through one half fires one. A note is recorded and frozen, then
  // the oscillator is switched off so grains are all that can sound.
  int trigger() {
    constexpr int kRate = 48000;
    const std::string p = "fx_slot_1_particles_";
    const int note_off = kRate, window = kRate + kRate / 2, end = window + kRate;
    const auto run = [&](bool note, bool control) {
      SynthPlugin plugin;
      preparePlayback(plugin, kRate);
      setControl(plugin, p + "mix", 1.0f);
      setControl(plugin, p + "density", 0.0f);
      setControl(plugin, p + "size", 0.8f);
      setControl(plugin, p + "position", 0.3f);
      std::vector<MidiEvent> events { { 0, juce::MidiMessage::noteOn(1, 60, 1.0f) },
                                      { note_off, juce::MidiMessage::noteOff(1, 60) } };
      if (note) {
        events.push_back({ window, juce::MidiMessage::noteOn(1, 72, 1.0f) });
        events.push_back({ window + 64, juce::MidiMessage::noteOff(1, 72) });
      }
      const auto audio = render(plugin, end, events, [&](int offset) {
        setControl(plugin, p + "freeze", offset >= note_off ? 1.0f : 0.0f);
        setControl(plugin, "osc_1_on", offset >= note_off ? 0.0f : 1.0f);
        setControl(plugin, p + "trigger", control && offset >= window ? 1.0f : 0.0f);
      });
      expect(finite(audio), "trigger test output finite");
      return rms(audio, static_cast<size_t>(window), static_cast<size_t>(end));
    };
    const double untriggered = run(false, false);
    expect(untriggered < 1.0e-4, "no grains without a trigger (" + std::to_string(untriggered) + ")");
    const double by_note = run(true, false);
    expect(by_note > 1.0e-3, "a note-on fires a grain (" + std::to_string(by_note) + ")");
    const double by_control = run(false, true);
    expect(by_control > 1.0e-3, "Trigger fires a grain (" + std::to_string(by_control) + ")");
    return finish("trigger");
  }

  // Every Particles control exists in every slot of every chain, is new in
  // this version, and every slider (and Freeze) is a modulation destination;
  // Mode and Quality are plain menus. Values survive saving and loading.
  int parameters() {
    SynthPlugin synth;
    const char* modulated[] = { "input_gain", "position", "size", "pitch", "density", "texture", "mix", "spread",
                                "feedback", "reverb", "freeze", "trigger" };
    const char* menus[] = { "mode", "quality" };
    for (int chain = 0; chain <= vital::kNumBuses; ++chain) {
      const std::string chain_prefix = chain == 0 ? "" : "bus_" + std::to_string(chain) + "_";
      for (int slot = 1; slot <= vital::constants::kMaxEffectSlots; ++slot) {
        const std::string prefix = chain_prefix + "fx_slot_" + std::to_string(slot) + "_particles_";
        for (const char* name : modulated) {
          const std::string id = prefix + name;
          expect(synth.getControls().count(id) == 1 && vital::Parameters::getDetails(id).version_added == 0x020129,
                 "Particles parameter present with its version: " + id);
          synth.prepareModulationDestination(id);
          expect(synth.getEngine()->getMonoModulationDestination(id) != nullptr,
                 "Particles control is a modulation destination: " + id);
        }
        for (const char* name : menus)
          expect(synth.getControls().count(prefix + name) == 1, "Particles menu present: " + prefix + name);
      }
    }
    expect(vital::Parameters::getDetails("effect_chain_slot_42").default_value == vital::constants::kParticles,
           "the legacy chain order knows Particles");
    expect(std::string(strings::kEffectOrder[vital::constants::kParticles]) == "particles" &&
           strings::kEffectSlotTypeNames[vital::constants::kParticles + 1] == "Particles",
           "the effect is named Particles");

    synth.parameterChanged("fx_slot_3_type", vital::constants::kParticles + 1);
    const std::pair<const char*, float> values[] = { { "mode", 2.0f }, { "quality", 3.0f }, { "pitch", -7.0f },
                                                     { "density", -0.4f }, { "mix", 0.25f }, { "freeze", 1.0f } };
    for (const auto& value : values)
      synth.parameterChanged(std::string("fx_slot_3_particles_") + value.first, value.second);
    juce::MemoryBlock state;
    synth.getStateInformation(state);
    SynthPlugin restored;
    restored.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    for (const auto& value : values) {
      const std::string id = std::string("fx_slot_3_particles_") + value.first;
      expect(std::abs(restored.getControls().at(id)->value() - value.second) < 1.0e-5f,
             "Particles state survives reload: " + id);
    }
    return finish("parameters");
  }

  template <typename Predicate>
  void pumpUntil(Predicate done, int timeout_ms = 5000) {
    const uint32 start = juce::Time::getMillisecondCounter();
    while (!done() && juce::Time::getMillisecondCounter() - start < static_cast<uint32>(timeout_ms))
      juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
    juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
  }

  // The slot shows every control in order, labelled and read out in display
  // units, each slider a destination the editor's Modulate item offers.
  int editorTest() {
    SynthPlugin synth;
    synth.parameterChanged("fx_slot_1_type", vital::constants::kParticles + 1);
    SynthEditor editor(synth);
    editor.updateFullGui();
    editor.addToDesktop(0);
    editor.setVisible(true);
    editor.toFront(true);
    const String prefix = "fx_slot_1_particles_";
    pumpUntil([&] { return editor.controlForParameter(prefix + "mode") != nullptr; });
    auto* mode = dynamic_cast<ComboBox*>(editor.controlForParameter(prefix + "mode"));
    expect(mode != nullptr, "Mode is a menu");
    if (mode != nullptr) {
      StringArray items;
      for (int i = 0; i < mode->getNumItems(); ++i)
        items.add(mode->getItemText(i));
      expect(items == StringArray { "Granular", "Pitch shifter and time stretcher", "Looping delay", "Spectral" },
             "Mode offers the four modes: " + items.joinIntoString(", ").toStdString());
    }
    expect(dynamic_cast<ComboBox*>(editor.controlForParameter(prefix + "quality")) != nullptr, "Quality is a menu");

    const auto value_text = [&](const char* suffix) {
      auto* control = editor.controlForParameter(prefix + suffix);
      auto* handler = control ? control->getAccessibilityHandler() : nullptr;
      auto* value = handler ? handler->getValueInterface() : nullptr;
      return value ? value->getCurrentValueAsString() : String("<missing>");
    };
    struct Row { const char* suffix; const char* text; };
    for (const Row row : { Row { "input_gain", "0 dB" }, Row { "position", "0%" }, Row { "size", "50%" },
                           Row { "pitch", "0 semitones" }, Row { "density", "30%" }, Row { "texture", "50%" },
                           Row { "mix", "50%" }, Row { "spread", "0%" }, Row { "feedback", "0%" },
                           Row { "reverb", "0%" }, Row { "trigger", "Off" } }) {
      expect(value_text(row.suffix) == row.text, std::string("Particles ") + row.suffix + " reads " + row.text +
             " (reads " + value_text(row.suffix).toStdString() + ")");
    }

    const char* order[] = { "mode", "quality", "input_gain", "position", "size", "pitch", "density", "texture", "mix",
                            "spread", "feedback", "reverb", "freeze", "trigger" };
    int previous_y = -1;
    for (const char* suffix : order) {
      auto* control = editor.controlForParameter(prefix + suffix);
      expect(control != nullptr, std::string("Particles row present: ") + suffix);
      if (control == nullptr) continue;
      expect(control->getTitle().isNotEmpty() && control->getDescription().isNotEmpty(),
             std::string("Particles row labelled and described: ") + suffix);
      expect(control->getWantsKeyboardFocus(), std::string("Particles row takes keyboard focus: ") + suffix);
      const int y = control->getScreenY();
      expect(y > previous_y, std::string("Particles rows in order at ") + suffix);
      previous_y = y;
      const bool menu = String(suffix) == "mode" || String(suffix) == "quality";
      if (String(suffix) == "trigger")
        expect(dynamic_cast<Slider*>(control) != nullptr && dynamic_cast<Button*>(control) == nullptr,
               "Trigger is an Off/On slider, not a toggle");
      // The editor offers Modulate where the engine has a destination.
      const bool destination = synth.getEngine()->getMonoModulationDestination((prefix + suffix).toStdString()) != nullptr;
      expect(destination != menu, std::string("Particles ") + suffix + (menu ? " is not" : " is") +
                                  " offered for modulation");
    }
    if (auto* density = editor.controlForParameter(prefix + "density"))
      expect(density->getDescription().contains("steady rate"), "Density's description explains its sides");
    return finish("editor");
  }

  // Not a ctest: how long one Particles slot takes per mode.
  int cpu() {
    constexpr int kRate = 48000;
    for (int mode = 0; mode < particles::kNumModes; ++mode) {
      particles::Engine engine;
      particles::Parameters p;
      p.mode = mode;
      p.density = 0.9f;
      p.dry_wet = 1.0f;
      p.reverb = 0.5f;
      engine.setParameters(p);
      engine.prepare(kRate);
      renderEngine(engine, kRate, kRate / 10, false);
      const double start = juce::Time::getMillisecondCounterHiRes();
      renderEngine(engine, kRate, kRate * 10, false);
      const double elapsed = juce::Time::getMillisecondCounterHiRes() - start;
      std::cout << strings::kParticlesModeNames[mode] << ": 10 s in " << elapsed << " ms ("
                << elapsed / 100.0 << "% of real time)\n";
    }
    return 0;
  }
} // namespace

int main(int argc, char** argv) {
  juce::ScopedJuceInitialiser_GUI juce;
  const juce::String flag = argc > 1 ? juce::String(argv[1]) : juce::String();
  if (flag == "--engine")
    return engine();
  if (flag == "--latency")
    return latency();
  if (flag == "--modes")
    return modes();
  if (flag == "--trigger")
    return trigger();
  if (flag == "--parameters")
    return parameters();
  if (flag == "--editor")
    return editorTest();
  if (flag == "--cpu")
    return cpu();
  std::cerr << "usage: AtlasParticlesTests --engine | --latency | --modes | --trigger | --parameters | --editor | --cpu\n";
  return 2;
}
