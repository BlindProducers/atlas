// Source slot regression and behaviour tests.
//
// `--capture <dir>` renders a fixed set of source configurations through the
// plugin and stores them as raw interleaved stereo float files. `--compare
// <dir>` renders the same fixtures and checks them against those files.
// Fixtures are rendered twice in both modes; any fixture that is not
// deterministic on its own machine is reported and excluded from comparison.

#include "JuceHeader.h"
#include "synth_plugin.h"
#include "sound_engine.h"
#include "sample_source.h"
#include "noise_source.h"
#include "analog_waveforms.h"
#include "wavetable_creator.h"
#include "synth_oscillator.h"
#include "source_types.h"
#include "slot_test_ids.h"
#include "smooth_value.h"
#include "source_slot_module.h"
#include "producers_module.h"
#include "load_save.h"
#include "fx_automation_parameter.h"
#include "value_bridge.h"

#include <chrono>
#include <cmath>
#include <iomanip>
#include <thread>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include <algorithm>
#include <vector>

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

  constexpr int kSampleRate = 48000;
  constexpr int kRenderSamples = kSampleRate * 3 / 2;
  constexpr int kNoteOffSample = kSampleRate;


  // Fixed-source parameter names as they were before oscillator slots, mapped to
  // the slot that replaced each source (Sample 5, Granular 6, Spectral 7, Noise 8).
  std::string slotId(const std::string& legacy) {
    static const struct { const char* prefix; int slot; } kLegacy[] = {
      { "sample", 4 }, { "granular", 5 }, { "spectral", 6 }, { "noise", 7 } };
    static const char* kSlotLevel[] = {
      "on", "solo", "destination", "key_zone_start", "key_zone_end", "velocity_zone_start", "velocity_zone_end",
      "level", "pan" };
    for (const auto& source : kLegacy) {
      const std::string prefix = std::string(source.prefix) + "_";
      if (legacy.rfind(prefix, 0) != 0)
        continue;
      const std::string suffix = legacy.substr(prefix.size());
      const std::string slot_prefix = "osc_" + std::to_string(source.slot + 1) + "_";
      if (suffix == "phase")
        return slot_prefix + "playhead";
      for (const char* slot_level : kSlotLevel) {
        if (suffix == slot_level)
          return slot_prefix + suffix;
      }
      return slot_prefix + legacy;
    }
    return legacy;
  }

  void set(SynthPlugin& plugin, const std::string& name, float value) {
    auto& controls = plugin.getControls();
    const auto found = controls.find(name);
    expect(found != controls.end(), "control exists: " + name);
    if (found != controls.end())
      found->second->set(value);
  }

  // The fixtures and their references were written when slots 5-8 defaulted to
  // the Sample, Granular, Spectral and Noise sources and oscillators started at
  // 180 degrees. A new patch is all wavetables at phase 0 now; restore the old one.
  // Phase is smoothed, so it is set without a glide, as if the patch began there.
  void useLegacyDefaults(SynthPlugin& plugin) {
    slot_ids::useLegacySourceTypes(plugin);
    for (int slot = 0; slot < vital::kNumOscillators; ++slot) {
      vital::Value* phase = plugin.getControls().at(vital::source_types::slotPrefix(slot) + "phase");
      if (auto* smooth = dynamic_cast<vital::SmoothValue*>(phase))
        smooth->setHard(0.5f);
      else
        phase->set(0.5f);
    }
  }

  // A deterministic half-second test sample: a decaying chirp with harmonics.
  std::vector<float> testSample() {
    std::vector<float> result(kSampleRate / 2);
    for (size_t i = 0; i < result.size(); ++i) {
      const double t = i / double(kSampleRate);
      const double frequency = 110.0 + 220.0 * t;
      const double phase = 2.0 * M_PI * (110.0 * t + 110.0 * t * t);
      result[i] = float(std::exp(-3.0 * t) * (0.6 * std::sin(phase) + 0.3 * std::sin(2.0 * phase) +
                                              0.1 * std::sin(3.0 * phase + frequency * 0.001)));
    }
    return result;
  }

  void loadTestSample(vital::Sample* sample) {
    expect(sample != nullptr, "sample target exists");
    if (sample == nullptr)
      return;
    const auto data = testSample();
    sample->loadSample(data.data(), static_cast<int>(data.size()), kSampleRate);
    sample->setName("slot test sample");
  }

  struct Fixture {
    std::string name;
    std::function<void(SynthPlugin&)> configure;
  };

  // The current fixed-source configuration, expressed with the parameter names
  // of the engine under test. Keep the sound of every fixture deterministic.
  std::vector<Fixture> fixtures() {
    auto quiet_oscillators = [](SynthPlugin& plugin) {
      for (int i = 1; i <= 4; ++i) {
        set(plugin, "osc_" + std::to_string(i) + "_on", 0.0f);
        set(plugin, "osc_" + std::to_string(i) + "_random_phase", 0.0f);
      }
    };
    return {
      { "osc1_wavetable", [=](SynthPlugin& plugin) {
          quiet_oscillators(plugin);
          set(plugin, "osc_1_on", 1.0f);
          set(plugin, "osc_1_unison_voices", 3.0f);
          set(plugin, "osc_1_unison_detune", 12.0f);
        } },
      { "osc1_analog_sub", [=](SynthPlugin& plugin) {
          quiet_oscillators(plugin);
          set(plugin, "osc_1_on", 1.0f);
          set(plugin, "osc_1_type", 1.0f);
          set(plugin, "osc_1_analog_waveform", 2.0f);
          set(plugin, "osc_1_analog_shape", 0.3f);
          set(plugin, "osc_1_sub_level", 0.5f);
        } },
      { "osc2_fm_from_osc1", [=](SynthPlugin& plugin) {
          quiet_oscillators(plugin);
          set(plugin, "osc_1_on", 1.0f);
          set(plugin, "osc_2_on", 1.0f);
          set(plugin, "osc_2_distortion_type", float(vital::SynthOscillator::kFmOscillatorA));
          set(plugin, "osc_2_distortion_amount", 0.4f);
        } },
      { "osc1_fm_from_sample", [=](SynthPlugin& plugin) {
          quiet_oscillators(plugin);
          loadTestSample(plugin.getSample(4));
          set(plugin, slotId("sample_on"), 1.0f);
          set(plugin, slotId("sample_random_phase"), 0.0f);
          set(plugin, slotId("sample_loop"), 1.0f);
          set(plugin, slotId("sample_level"), 0.0f);
          set(plugin, "osc_1_on", 1.0f);
          set(plugin, "osc_1_distortion_type", float(vital::SynthOscillator::kFmSample));
          set(plugin, "osc_1_distortion_amount", 0.5f);
        } },
      { "osc1_rm_from_sample_osc3_fm_from_granular", [=](SynthPlugin& plugin) {
          quiet_oscillators(plugin);
          loadTestSample(plugin.getSample(4));
          loadTestSample(plugin.getSample(5));
          set(plugin, slotId("sample_on"), 1.0f);
          set(plugin, slotId("sample_random_phase"), 0.0f);
          set(plugin, slotId("sample_loop"), 1.0f);
          set(plugin, slotId("sample_level"), 0.0f);
          set(plugin, slotId("granular_on"), 1.0f);
          set(plugin, slotId("granular_level"), 0.0f);
          set(plugin, slotId("granular_random_position"), 0.0f);
          set(plugin, slotId("granular_random_volume"), 0.0f);
          set(plugin, slotId("granular_random_pan"), 0.0f);
          set(plugin, slotId("granular_random_pitch"), 0.0f);
          set(plugin, slotId("granular_timing_jitter"), 0.0f);
          set(plugin, "osc_1_on", 1.0f);
          set(plugin, "osc_1_distortion_type", float(vital::SynthOscillator::kRmSample));
          set(plugin, "osc_1_distortion_amount", 0.6f);
          set(plugin, "osc_3_on", 1.0f);
          set(plugin, "osc_3_distortion_type", float(vital::SynthOscillator::kFmGranular));
          set(plugin, "osc_3_distortion_amount", 0.3f);
        } },
      { "sample_loop", [=](SynthPlugin& plugin) {
          quiet_oscillators(plugin);
          loadTestSample(plugin.getSample(4));
          set(plugin, slotId("sample_on"), 1.0f);
          set(plugin, slotId("sample_random_phase"), 0.0f);
          set(plugin, slotId("sample_loop"), 1.0f);
          set(plugin, slotId("sample_keytrack"), 1.0f);
          set(plugin, slotId("sample_transpose"), 7.0f);
          set(plugin, slotId("sample_loop_start"), 0.1f);
          set(plugin, slotId("sample_loop_end"), 0.6f);
          set(plugin, slotId("sample_low_cutoff"), 20.0f);
          set(plugin, slotId("sample_pan"), -0.3f);
        } },
      { "granular_grid", [=](SynthPlugin& plugin) {
          quiet_oscillators(plugin);
          loadTestSample(plugin.getSample(5));
          set(plugin, slotId("granular_on"), 1.0f);
          set(plugin, slotId("granular_density"), 30.0f);
          set(plugin, slotId("granular_grain_size"), 80.0f);
          set(plugin, slotId("granular_position"), 0.3f);
          set(plugin, slotId("granular_random_position"), 0.0f);
          set(plugin, slotId("granular_random_volume"), 0.0f);
          set(plugin, slotId("granular_random_pan"), 0.0f);
          set(plugin, slotId("granular_random_pitch"), 0.0f);
          set(plugin, slotId("granular_timing_jitter"), 0.0f);
        } },
      { "spectral_shift", [=](SynthPlugin& plugin) {
          quiet_oscillators(plugin);
          loadTestSample(plugin.getSample(6));
          // Analysis runs on a worker thread; give it time so the render is repeatable.
          std::this_thread::sleep_for(std::chrono::milliseconds(1500));
          set(plugin, slotId("spectral_on"), 1.0f);
          set(plugin, slotId("spectral_shift"), 0.2f);
          set(plugin, slotId("spectral_stretch"), 1.5f);
          set(plugin, slotId("spectral_phase_randomize"), 0.0f);
        } },
      { "noise_filtered", [=](SynthPlugin& plugin) {
          quiet_oscillators(plugin);
          set(plugin, slotId("noise_on"), 1.0f);
          set(plugin, slotId("noise_low_cutoff"), 60.0f);
          set(plugin, slotId("noise_high_cutoff"), 100.0f);
          set(plugin, slotId("noise_width"), 0.5f);
        } },
      { "routing_zones_solo", [=](SynthPlugin& plugin) {
          quiet_oscillators(plugin);
          loadTestSample(plugin.getSample(4));
          set(plugin, "osc_1_on", 1.0f);
          set(plugin, "osc_1_destination", float(vital::constants::kFilter1));
          set(plugin, "osc_2_on", 1.0f);
          set(plugin, "osc_2_destination", float(vital::constants::kBus1));
          set(plugin, "osc_2_key_zone_end", 59.0f); // Excludes the test note.
          set(plugin, slotId("sample_on"), 1.0f);
          set(plugin, slotId("sample_random_phase"), 0.0f);
          set(plugin, slotId("sample_destination"), float(vital::constants::kFilter2));
          set(plugin, "filter_1_on", 1.0f);
          set(plugin, "filter_1_cutoff", 60.0f);
          set(plugin, "filter_2_on", 1.0f);
          set(plugin, "filter_2_cutoff", 90.0f);
        } },
      { "modulation_env_to_sample_start", [=](SynthPlugin& plugin) {
          quiet_oscillators(plugin);
          loadTestSample(plugin.getSample(4));
          set(plugin, slotId("sample_on"), 1.0f);
          set(plugin, slotId("sample_random_phase"), 0.0f);
          set(plugin, slotId("sample_loop"), 1.0f);
          set(plugin, "env_2_attack", 0.5f);
          set(plugin, "env_2_decay", 0.5f);
          plugin.connectModulation("env_2", slotId("sample_start"));
          const int connection = plugin.getConnectionIndex("env_2", slotId("sample_start"));
          expect(connection >= 0, "modulation to sample_start connects");
          if (connection >= 0)
            set(plugin, "modulation_" + std::to_string(connection + 1) + "_amount", 0.5f);
        } },
    };
  }

  std::vector<float> render(const Fixture& fixture) {
    SynthPlugin plugin;
    plugin.setPlayConfigDetails(0, 2, kSampleRate, 128);
    plugin.prepareToPlay(kSampleRate, 128);
    set(plugin, "oversampling", 0.0f);
    for (int chain = 0; chain < 4; ++chain) {
      const auto prefix = chain == 0 ? std::string() : "bus_" + std::to_string(chain) + "_";
      for (int i = 1; i <= 20; ++i)
        set(plugin, prefix + "fx_slot_" + std::to_string(i) + "_type", 0.0f);
    }
    useLegacyDefaults(plugin);
    fixture.configure(plugin);
    plugin.checkOversampling();

    std::vector<float> result(size_t(kRenderSamples) * 2);
    bool note_off_sent = false;
    for (int offset = 0, block = 0; offset < kRenderSamples; ++block) {
      const int count = std::min(block % 2 ? 127 : 17, kRenderSamples - offset);
      juce::AudioBuffer<float> audio(2, count);
      juce::MidiBuffer midi;
      audio.clear();
      if (offset == 0)
        midi.addEvent(juce::MidiMessage::noteOn(1, 60, 0.7f), 11);
      if (!note_off_sent && offset + count > kNoteOffSample) {
        midi.addEvent(juce::MidiMessage::noteOff(1, 60), kNoteOffSample - offset);
        note_off_sent = true;
      }
      plugin.processBlock(audio, midi);
      for (int i = 0; i < count; ++i)
        for (int channel = 0; channel < 2; ++channel)
          result[size_t(offset + i) * 2 + channel] = audio.getSample(channel, i);
      offset += count;
    }
    return result;
  }

  bool identical(const std::vector<float>& a, const std::vector<float>& b, float tolerance, int* first_diff) {
    if (a.size() != b.size())
      return false;
    for (size_t i = 0; i < a.size(); ++i) {
      if (!(std::abs(a[i] - b[i]) <= tolerance)) {
        if (first_diff) *first_diff = static_cast<int>(i / 2);
        return false;
      }
    }
    return true;
  }

  double energy(const std::vector<float>& signal) {
    double sum = 0.0;
    for (float value : signal) sum += double(value) * value;
    return sum;
  }

  juce::File fixtureFile(const juce::File& folder, const Fixture& fixture) {
    return folder.getChildFile(fixture.name + ".f32");
  }

  int capture(const juce::File& folder) {
    folder.createDirectory();
    for (const auto& fixture : fixtures()) {
      const auto first = render(fixture);
      const auto second = render(fixture);
      expect(std::isfinite(energy(first)) && energy(first) > 0.0, fixture.name + " produces audio");
      int diff = 0;
      if (!identical(first, second, 0.0f, &diff)) {
        std::cout << "nondeterministic: " << fixture.name << " (first difference at sample " << diff << ")\n";
        continue;
      }
      std::ofstream file(fixtureFile(folder, fixture).getFullPathName().toStdString(), std::ios::binary);
      file.write(reinterpret_cast<const char*>(first.data()), first.size() * sizeof(float));
      expect(file.good(), "wrote capture for " + fixture.name);
      std::cout << "captured: " << fixture.name << '\n';
    }
    return failures == 0 ? 0 : 1;
  }

  int compare(const juce::File& folder) {
    for (const auto& fixture : fixtures()) {
      const auto file = fixtureFile(folder, fixture);
      if (!file.existsAsFile()) {
        std::cout << "no reference: " << fixture.name << '\n';
        continue;
      }
      juce::MemoryBlock block;
      file.loadFileAsData(block);
      std::vector<float> reference(block.getSize() / sizeof(float));
      std::memcpy(reference.data(), block.getData(), block.getSize());
      const auto rendered = render(fixture);
      int diff = 0;
      const bool same = identical(rendered, reference, 0.0f, &diff);
      expect(same, fixture.name + " matches the reference render (first difference at sample " +
                       std::to_string(diff) + ")");
      std::cout << (same ? "match: " : "DIFFERENT: ") << fixture.name << '\n';
    }
    return failures == 0 ? 0 : 1;
  }

  // ---- Behaviour tests -------------------------------------------------------

  std::vector<float> renderNote(SynthPlugin& plugin, int samples) {
    std::vector<float> result(size_t(samples) * 2);
    for (int offset = 0, block = 0; offset < samples; ++block) {
      const int count = std::min(128, samples - offset);
      juce::AudioBuffer<float> audio(2, count);
      juce::MidiBuffer midi;
      audio.clear();
      if (offset == 0)
        midi.addEvent(juce::MidiMessage::noteOn(1, 60, 0.7f), 0);
      plugin.processBlock(audio, midi);
      for (int i = 0; i < count; ++i)
        for (int channel = 0; channel < 2; ++channel)
          result[size_t(offset + i) * 2 + channel] = audio.getSample(channel, i);
      offset += count;
    }
    juce::AudioBuffer<float> audio(2, 128);
    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
    plugin.processBlock(audio, midi);
    // Let the release tail finish so the next note starts from silence.
    for (int block = 0; block < 2000; ++block) {
      audio.clear();
      juce::MidiBuffer empty;
      plugin.processBlock(audio, empty);
      if (block > 20 && audio.getMagnitude(0, 128) == 0.0f)
        break;
    }
    return result;
  }

  int g_oversampling = 0;

  void preparePlugin(SynthPlugin& plugin) {
    plugin.setPlayConfigDetails(0, 2, kSampleRate, 128);
    plugin.prepareToPlay(kSampleRate, 128);
    set(plugin, "oversampling", float(g_oversampling));
    for (int chain = 0; chain < 4; ++chain) {
      const auto prefix = chain == 0 ? std::string() : "bus_" + std::to_string(chain) + "_";
      for (int i = 1; i <= 20; ++i)
        set(plugin, prefix + "fx_slot_" + std::to_string(i) + "_type", 0.0f);
    }
    for (int i = 1; i <= 8; ++i) {
      set(plugin, "osc_" + std::to_string(i) + "_on", 0.0f);
      set(plugin, "osc_" + std::to_string(i) + "_random_phase", 0.0f);
    }
    useLegacyDefaults(plugin);
    plugin.checkOversampling();
  }

  // Changing a slot's type on the message thread swaps its engine synchronously.
  void setType(SynthPlugin& plugin, int slot, int type) {
    plugin.parameterChanged(vital::source_types::slotPrefix(slot) + "type", float(type));
  }

  // A new patch, and Initialize, make every slot a wavetable oscillator that
  // starts at phase 0 with no phase randomization. Slots 5-8 keep the main
  // effects destination of the sources they replaced.
  void testDefaultPatch() {
    SynthPlugin plugin;
    plugin.setPlayConfigDetails(0, 2, kSampleRate, 128);
    plugin.prepareToPlay(kSampleRate, 128);
    for (bool initialized : { false, true }) {
      if (initialized) {
        setType(plugin, 4, vital::source_types::kSample);
        set(plugin, "osc_1_phase", 0.25f);
        set(plugin, "osc_1_random_phase", 1.0f);
        plugin.loadInitPreset();
      }
      const std::string context = initialized ? " after Initialize" : "";
      for (int slot = 0; slot < vital::kNumOscillators; ++slot) {
        const std::string prefix = vital::source_types::slotPrefix(slot);
        auto* source = plugin.getEngine()->getSourceSlot(slot);
        expect(source != nullptr && source->activeType() == vital::source_types::kWavetable,
               "slot " + std::to_string(slot + 1) + " defaults to a wavetable oscillator" + context);
        expect(plugin.getControls().at(prefix + "phase")->value() == 0.0f &&
               plugin.getControls().at(prefix + "random_phase")->value() == 0.0f,
               "slot " + std::to_string(slot + 1) + " starts at phase 0 without phase randomization" + context);
      }
      for (int slot = vital::source_types::kFirstLegacySourceSlot; slot < vital::kNumOscillators; ++slot)
        expect(plugin.getControls().at(vital::source_types::slotPrefix(slot) + "destination")->value() == 3.0f,
               "slots 5-8 default to the main effects destination like the old sources" + context);
    }
  }

  void testTypeSwitchBuildsOnlyTheActiveEngine() {
    SynthPlugin plugin;
    preparePlugin(plugin);
    auto* engine = plugin.getEngine();
    auto* slot = engine->getSourceSlot(0);
    expect(slot != nullptr && slot->activeType() == vital::source_types::kWavetable, "slot 1 starts as a wavetable");
    expect(engine->getMonoModulationDestination("osc_1_wave_frame") != nullptr,
           "wavetable parameters are modulation destinations while the type is active");
    expect(engine->getMonoModulationDestination("osc_1_sample_start") == nullptr,
           "inactive type parameters are not modulation destinations (their engine does not exist)");
    expect(engine->getMonoModulationDestination("osc_1_granular_density") == nullptr,
           "no granular engine exists for a wavetable slot");

    // Wavetable <-> Analog share one engine.
    setType(plugin, 0, vital::source_types::kAnalog);
    expect(slot->activeType() == vital::source_types::kAnalog, "analog is selected");
    expect(slot->oscillatorModule() != nullptr, "analog keeps the oscillator engine");

    // Sample: the sample engine appears, the wavetable one is gone.
    setType(plugin, 0, vital::source_types::kSample);
    expect(slot->activeType() == vital::source_types::kSample, "sample is selected");
    expect(slot->oscillatorModule() == nullptr, "the oscillator engine is destroyed when another type is chosen");
    expect(engine->getMonoModulationDestination("osc_1_sample_start") != nullptr,
           "sample parameters become modulation destinations");
    expect(engine->getMonoModulationDestination("osc_1_wave_frame") == nullptr,
           "wavetable parameters stop being destinations");

    loadTestSample(plugin.getSample(0));
    set(plugin, "osc_1_on", 1.0f);
    set(plugin, "osc_1_sample_loop", 1.0f);
    const auto sample_audio = renderNote(plugin, kSampleRate / 4);
    expect(energy(sample_audio) > 1.0e-3, "a slot set to Sample plays its sample");

    // Modulate a sample parameter and make sure it reaches the engine.
    expect(plugin.connectModulation("lfo_1", "osc_1_sample_start"), "modulation connects to the active type's parameter");
    const int connection = plugin.getConnectionIndex("lfo_1", "osc_1_sample_start");
    expect(connection >= 0, "connection index exists");
    if (connection >= 0)
      set(plugin, "modulation_" + std::to_string(connection + 1) + "_amount", 0.9f);
    const auto modulated = renderNote(plugin, kSampleRate / 4);
    int diff = 0;
    expect(!identical(sample_audio, modulated, 1.0e-6f, &diff), "modulating a sample parameter changes the sound");

    // Back to wavetable: the route into the sample parameter is dropped, the wavetable engine returns.
    setType(plugin, 0, vital::source_types::kWavetable);
    expect(slot->activeType() == vital::source_types::kWavetable, "wavetable is selected again");
    expect(plugin.getConnectionIndex("lfo_1", "osc_1_sample_start") < 0,
           "routes into a type the slot left are disconnected");
    expect(engine->getMonoModulationDestination("osc_1_sample_start") == nullptr, "sample destinations are gone");
    expect(plugin.getControls().at("osc_1_sample_loop")->value() == 1.0f,
           "a type's settings are kept while it is inactive");
    const auto wavetable_audio = renderNote(plugin, kSampleRate / 4);
    expect(energy(wavetable_audio) > 1.0e-3, "the wavetable engine plays again");
    expect(!identical(wavetable_audio, sample_audio, 1.0e-6f, &diff), "wavetable and sample render differently");

    // Switching an audible slot to Spectral (its engine hard-resets on the first
    // block) must sync the voice copies before touching them.
    set(plugin, "osc_1_on", 1.0f);
    setType(plugin, 0, vital::source_types::kSpectral);
    expect(slot->activeType() == vital::source_types::kSpectral, "spectral is selected");
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    const auto spectral_audio = renderNote(plugin, kSampleRate / 4);
    expect(energy(spectral_audio) > 1.0e-4, "a slot switched to Spectral while on plays");
    setType(plugin, 0, vital::source_types::kGranular);
    expect(energy(renderNote(plugin, kSampleRate / 4)) > 1.0e-4, "a slot switched to Granular while on plays");
    set(plugin, "osc_1_on", 0.0f);

    // All eight slots can be the same type.
    for (int i = 0; i < 8; ++i)
      setType(plugin, i, vital::source_types::kNoise);
    for (int i = 0; i < 8; ++i)
      expect(engine->getSourceSlot(i)->activeType() == vital::source_types::kNoise, "every slot can be Noise");
    set(plugin, "osc_8_on", 1.0f);
    expect(energy(renderNote(plugin, kSampleRate / 8)) > 1.0e-4, "slot 8 plays as noise");
  }

  void testLegacyPresetMigration() {
    json settings = {
      { "sample_on", 1.0f }, { "sample_start", 0.3f }, { "sample_destination", 1.0f }, { "sample_solo", 0.0f },
      { "sample_key_zone_end", 100.0f }, { "granular_on", 1.0f }, { "granular_density", 50.0f },
      { "spectral_shift", 0.25f }, { "noise_on", 1.0f }, { "noise_width", 0.4f },
      { "spectral_gate_mix", 0.5f }, { "spectral_morph_amount", 0.1f }, { "osc_1_spectral_morph_amount", 0.2f },
      { "arp_1_destination_sample", 0.0f }, { "arp_2_destination_noise", 1.0f },
      { "sample", { { "name", "legacy" } } }, { "granular_sample", { { "name", "grains" } } },
    };
    json modulations = json::array({
      { { "source", "env_2" }, { "destination", "sample_start" } },
      { { "source", "sample_phase" }, { "destination", "osc_1_level" } },
      { { "source", "granular_phase" }, { "destination", "noise_width" } },
      { { "source", "lfo_1" }, { "destination", "spectral_gate_mix" } },
    });
    LoadSave::migrateFixedSourcesToSlots(settings, modulations);
    auto has = [&](const char* key) { return settings.count(key) > 0; };
    expect(!has("sample_on") && settings.value("osc_5_on", 0.0f) == 1.0f, "sample_on becomes slot 5 on");
    expect(settings.value("osc_5_sample_start", 0.0f) == 0.3f, "sample_start becomes osc_5_sample_start");
    expect(settings.value("osc_5_destination", 0.0f) == 1.0f, "sample destination becomes slot 5 destination");
    expect(settings.value("osc_5_key_zone_end", 0.0f) == 100.0f, "zones move to the slot");
    expect(settings.value("osc_5_type", -1.0f) == float(vital::source_types::kSample), "slot 5 is typed Sample");
    expect(settings.value("osc_6_granular_density", 0.0f) == 50.0f && settings.value("osc_6_type", -1.0f) == float(vital::source_types::kGranular),
           "granular moves to slot 6");
    expect(settings.value("osc_7_spectral_shift", 0.0f) == 0.25f && settings.value("osc_7_type", -1.0f) == float(vital::source_types::kSpectral),
           "spectral oscillator moves to slot 7");
    expect(settings.value("osc_8_noise_width", 0.0f) == 0.4f && settings.value("osc_8_on", 0.0f) == 1.0f,
           "noise moves to slot 8");
    expect(settings.value("spectral_gate_mix", 0.0f) == 0.5f && !has("osc_7_spectral_gate_mix"),
           "spectral FX parameters are not oscillator parameters and stay put");
    expect(settings.value("osc_1_spectral_morph_amount", 0.0f) == 0.2f, "oscillator spectral morph stays with its oscillator");
    expect(!has("arp_1_destination_sample") && settings.value("arp_1_destination_osc_5", 1.0f) == 0.0f,
           "arp destinations follow their source into the slot");
    expect(settings.value("arp_2_destination_osc_8", 0.0f) == 1.0f, "noise arp destination moves to slot 8");
    expect(!has("sample") && settings.count("osc_5_sample") && settings["osc_5_sample"]["name"] == "legacy",
           "the sample data moves to slot 5");
    expect(settings.count("osc_6_sample") && settings["osc_6_sample"]["name"] == "grains", "granular sample moves to slot 6");
    expect(modulations[0]["destination"] == "osc_5_sample_start", "modulation destinations are renamed");
    expect(modulations[1]["source"] == "osc_5_playhead", "sample phase source becomes the slot playhead");
    expect(modulations[2]["source"] == "osc_6_playhead" && modulations[2]["destination"] == "osc_8_noise_width",
           "granular phase and noise destinations are renamed");
    expect(modulations[3]["destination"] == "spectral_gate_mix", "FX destinations are untouched");

    // Migrating twice is harmless.
    json again_settings = settings, again_modulations = modulations;
    LoadSave::migrateFixedSourcesToSlots(again_settings, again_modulations);
    expect(again_settings == settings && again_modulations == modulations, "migration is idempotent");

    // Loading an old-format preset through the plugin applies it to the slots.
    SynthPlugin plugin;
    preparePlugin(plugin);
    json state = { { "synth_version", "2.1.0" }, { "preset_name", "legacy" }, { "author", "" },
                   { "settings", {
                     { "sample_on", 1.0f }, { "sample_start", 0.3f }, { "sample_loop", 1.0f },
                     { "noise_on", 1.0f }, { "noise_width", 0.4f },
                     { "modulations", json::array({ { { "source", "env_2" }, { "destination", "sample_start" } },
                                                    { { "source", "sample_phase" }, { "destination", "osc_1_level" } } }) },
                   } } };
    const std::string text = state.dump();
    plugin.setStateInformation(text.data(), int(text.size()));
    auto& controls = plugin.getControls();
    expect(controls.at("osc_5_on")->value() == 1.0f && controls.at("osc_5_sample_start")->value() == 0.3f,
           "loaded slot 5 settings");
    expect(plugin.getEngine()->getSourceSlot(4)->activeType() == vital::source_types::kSample, "slot 5 engine is Sample");
    expect(plugin.getEngine()->getSourceSlot(7)->activeType() == vital::source_types::kNoise && controls.at("osc_8_on")->value() == 1.0f,
           "slot 8 engine is Noise and on");
    expect(plugin.getConnectionIndex("env_2", "osc_5_sample_start") >= 0, "legacy modulation reconnects to the slot");
    expect(plugin.getConnectionIndex("osc_5_playhead", "osc_1_level") >= 0, "legacy playhead source reconnects");
  }

  void testHostParametersAndAutomationProxies() {
    SynthPlugin plugin;
    preparePlugin(plugin);
    int direct = 0, proxies = 0;
    bool has_legacy_sample_start = false, has_osc1_sample_start = false, has_osc1_level = false, has_osc_proxy = false;
    for (auto* parameter : plugin.getParameters()) {
      if (auto* bridge = dynamic_cast<ValueBridge*>(parameter)) {
        ++direct;
        const auto id = bridge->getParameterId();
        has_legacy_sample_start |= id == "osc_5_sample_start";
        has_osc1_sample_start |= id == "osc_1_sample_start";
        has_osc1_level |= id == "osc_1_level";
      }
      else if (dynamic_cast<FxAutomationParameter*>(parameter)) {
        ++proxies;
        has_osc_proxy |= parameter->getName(64) == "Osc 1 Param 1";
      }
    }
    expect(has_legacy_sample_start, "the old sample parameters stay host-visible through slot 5");
    expect(!has_osc1_sample_start, "new per-slot type parameters are not individual host parameters");
    expect(has_osc1_level, "existing oscillator parameters stay host-visible");
    expect(has_osc_proxy, "oscillator automation proxies exist");
    expect(proxies == SynthPlugin::kNumFxAutomationChains * SynthPlugin::kFxAutomationSlotsPerChain +
                          vital::kNumOscillators * SynthPlugin::kOscAutomationSlotsPerOscillator,
           "proxy count is FX chains plus eight oscillators");
    std::cout << "host parameters: " << direct << " direct, " << proxies << " proxies\n";

    expect(plugin.hasAutomationProxy("osc_1_sample_start"), "new type parameters can be assigned to a proxy");
    expect(!plugin.hasAutomationProxy("osc_1_level"), "directly exposed parameters need no proxy");
    expect(!plugin.hasAutomationProxy("osc_5_sample_start"), "legacy-aliased parameters need no proxy");
    expect(plugin.hasAutomationProxy("osc_5_wave_frame"), "a slot's other-type parameters use its proxies");
    expect(plugin.assignFxParameterToAutomationSlot("osc_1_sample_start") == 0, "assigning takes the first free proxy");
    expect(plugin.fxAutomationSlotNameForParameter("osc_1_sample_start") == "Osc 1 Param 1", "proxy is named for the oscillator");
    FxAutomationParameter* proxy = nullptr;
    for (auto* parameter : plugin.getParameters()) {
      if (auto* candidate = dynamic_cast<FxAutomationParameter*>(parameter)) {
        if (candidate->getName(64) == "Osc 1 Param 1")
          proxy = candidate;
      }
    }
    expect(proxy != nullptr, "found the proxy");
    if (proxy != nullptr) {
      proxy->setValueNotifyingHost(0.75f);
      close(plugin.getControls().at("osc_1_sample_start")->value(), 0.75f, "host automation of the proxy reaches the slot control");
    }
    expect(plugin.removeFxParameterAutomation("osc_1_sample_start"), "assignment can be removed");
  }

  void testStateRoundTripKeepsSlotTypesAndSamples() {
    SynthPlugin source;
    preparePlugin(source);
    setType(source, 1, vital::source_types::kSample);
    loadTestSample(source.getSample(1));
    source.getSample(1)->setName("round trip");
    set(source, "osc_2_sample_start", 0.4f);
    set(source, "osc_2_on", 1.0f);
    setType(source, 2, vital::source_types::kNoise);
    set(source, "osc_3_noise_width", 0.7f);
    juce::MemoryBlock state;
    source.getStateInformation(state);

    SynthPlugin restored;
    preparePlugin(restored);
    restored.setStateInformation(state.getData(), int(state.getSize()));
    expect(restored.getEngine()->getSourceSlot(1)->activeType() == vital::source_types::kSample, "slot 2 restores as Sample");
    expect(restored.getEngine()->getSourceSlot(2)->activeType() == vital::source_types::kNoise, "slot 3 restores as Noise");
    close(restored.getControls().at("osc_2_sample_start")->value(), 0.4f, "slot 2 sample start restores");
    close(restored.getControls().at("osc_3_noise_width")->value(), 0.7f, "slot 3 noise width restores");
    expect(restored.getSample(1)->getName() == "round trip", "slot 2 sample restores with its name");
    expect(restored.getSample(1)->originalLength() == source.getSample(1)->originalLength(), "slot 2 sample data restores");
  }

  // Visiting other types and coming back must leave a type sounding exactly as before.
  void testReturningToATypeSoundsIdentical() {
    SynthPlugin plugin;
    preparePlugin(plugin);
    set(plugin, "osc_1_on", 1.0f);
    set(plugin, "osc_1_unison_voices", 3.0f);
    set(plugin, "osc_1_unison_detune", 12.0f);
    set(plugin, "osc_1_spectral_morph_type", 0.0f);
    loadTestSample(plugin.getSample(0));
    set(plugin, "osc_1_sample_loop", 1.0f);
    set(plugin, "osc_1_sample_random_phase", 0.0f);
    set(plugin, "osc_1_granular_random_position", 0.0f);
    set(plugin, "osc_1_granular_random_volume", 0.0f);
    set(plugin, "osc_1_granular_random_pan", 0.0f);
    set(plugin, "osc_1_granular_random_pitch", 0.0f);
    set(plugin, "osc_1_granular_timing_jitter", 0.0f);

    const auto wavetable_before = renderNote(plugin, kSampleRate / 4);
    setType(plugin, 0, vital::source_types::kSample);
    const auto sample_before = renderNote(plugin, kSampleRate / 4);
    expect(energy(wavetable_before) > 1.0e-3 && energy(sample_before) > 1.0e-3, "both types play");

    for (int type : { vital::source_types::kSpectral, vital::source_types::kGranular, vital::source_types::kNoise,
                      vital::source_types::kAnalog, vital::source_types::kWavetable }) {
      setType(plugin, 0, type);
      if (type == vital::source_types::kSpectral)
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
      renderNote(plugin, kSampleRate / 8);
    }
    int diff = 0;
    const auto wavetable_after = renderNote(plugin, kSampleRate / 4);
    expect(identical(wavetable_after, wavetable_before, 0.0f, &diff),
           "wavetable sounds identical after visiting every other type (first difference at sample " +
               std::to_string(diff) + ")");
    setType(plugin, 0, vital::source_types::kSample);
    const auto sample_after = renderNote(plugin, kSampleRate / 4);
    expect(identical(sample_after, sample_before, 0.0f, &diff),
           "sample sounds identical after visiting every other type (first difference at sample " +
               std::to_string(diff) + ")");
    // A plain re-render of the same type is itself repeatable.
    const auto sample_again = renderNote(plugin, kSampleRate / 4);
    expect(identical(sample_again, sample_after, 0.0f, &diff), "re-rendering the same type is deterministic");
  }

  // Compares a fresh plugin that goes straight to a type with one that detours
  // through another type first (voice state is not repeatable across notes, so
  // every comparison starts from a fresh plugin).
  // Compares a fresh plugin that goes straight to a type with one that detours
  // through another type first (voice state is not repeatable across notes, so
  // every comparison starts from a fresh plugin).
  double brightness(const std::vector<float>& x) {
    double hf = 0.0, total = 0.0;
    for (size_t i = 2; i < x.size(); i += 2) {
      const double d = x[i] - x[i - 2];
      hf += d * d;
      total += double(x[i]) * x[i];
    }
    return total > 0.0 ? hf / total : 0.0;
  }

  void diagnoseFirstNote() {
    for (int type : { vital::source_types::kWavetable, vital::source_types::kSample }) {
      SynthPlugin plugin;
      preparePlugin(plugin);
      loadTestSample(plugin.getSample(0));
      set(plugin, "osc_1_sample_loop", 1.0f);
      set(plugin, "osc_1_sample_random_phase", 0.0f);
      set(plugin, "osc_1_on", 1.0f);
      setType(plugin, 0, type);
      const auto first = renderNote(plugin, kSampleRate / 8);
      const auto second = renderNote(plugin, kSampleRate / 8);
      const auto third = renderNote(plugin, kSampleRate / 8);
      std::cout << "type " << type << " brightness note1 " << brightness(first) << " note2 " << brightness(second)
                << " note3 " << brightness(third) << " energy " << energy(first) << " " << energy(second) << '\n';
      std::cout << "  first samples note1:";
      for (int i = 0; i < 12; i += 2) std::cout << ' ' << first[i];
      std::cout << "\n  first samples note2:";
      for (int i = 0; i < 12; i += 2) std::cout << ' ' << second[i];
      std::cout << '\n';
    }
  }

  void testDetoursLeaveTypesUnchanged() {
    for (int oversampling : { 0, 2 }) {
    g_oversampling = oversampling;
    for (int target : { vital::source_types::kWavetable, vital::source_types::kSample }) {
      for (int detour : { vital::source_types::kAnalog, vital::source_types::kSample, vital::source_types::kNoise,
                          vital::source_types::kGranular, vital::source_types::kSpectral }) {
        if (detour == target) continue;
        for (bool play_detour : { false, true }) {
          auto make = [&](bool take_detour) {
            auto plugin = std::make_unique<SynthPlugin>();
            preparePlugin(*plugin);
            loadTestSample(plugin->getSample(0));
            set(*plugin, "osc_1_sample_loop", 1.0f);
            set(*plugin, "osc_1_sample_random_phase", 0.0f);
            set(*plugin, "osc_1_on", 1.0f);
            if (take_detour) {
              setType(*plugin, 0, detour);
              if (detour == vital::source_types::kSpectral)
                std::this_thread::sleep_for(std::chrono::milliseconds(1500));
              if (play_detour)
                renderNote(*plugin, kSampleRate / 16);
            }
            setType(*plugin, 0, target);
            return plugin;
          };
          auto straight = make(false);
          auto detoured = make(true);
          const auto a = renderNote(*straight, kSampleRate / 8);
          const auto b = renderNote(*detoured, kSampleRate / 8);
          int diff = 0;
          const bool same = identical(a, b, 0.0f, &diff);
          double max_diff = 0.0;
          for (size_t i = 0; i < a.size(); ++i) max_diff = std::max(max_diff, double(std::abs(a[i] - b[i])));
          // A note played before changes later notes' attack (voice state), so only
          // silent detours compare bit-exactly; played ones must at least stay close in level.
          if (!play_detour)
            expect(same, "a type sounds identical after a detour through type " + std::to_string(detour) +
                             " (target " + std::to_string(target) + ", oversampling " + std::to_string(oversampling) +
                             ", first difference at sample " + std::to_string(diff) + ", max " + std::to_string(max_diff) + ")");
          else
            expect(max_diff < 0.3, "a played detour does not change the type's sound beyond the attack (max " +
                                       std::to_string(max_diff) + ")");
        }
      }
    }
    }
    g_oversampling = 0;
  }


  // Phase 2: every type reads the slot's shared distortion controls, so a
  // Sample, Granular, Spectral or Noise slot can be ring modulated by any other
  // slot, and the pitched sample types can be frequency modulated.
  void testSlotModulationBetweenTypes() {
    using vital::SynthOscillator;
    // Static, so the lambdas below can use it without capturing it: MSVC asks
    // for a capture where Clang does not.
    static constexpr int kSlot = 4;  // Oscillator 5
    // Renders a fresh plugin: slot 5 plays the test sample as `type`, slots
    // 1-3 are wavetable modulators kept out of the mix by a key zone the note
    // misses (a level of 0 would still leak while its smoother settles), then
    // `configure` sets the distortion.
    auto renderWith = [](int type, int samples, const std::function<void(SynthPlugin&)>& configure) {
      SynthPlugin plugin;
      preparePlugin(plugin);
      setType(plugin, kSlot, type);
      loadTestSample(plugin.getSample(kSlot));
      set(plugin, "osc_5_on", 1.0f);
      set(plugin, "osc_5_sample_loop", 1.0f);
      for (int i = 1; i <= 3; ++i) {
        set(plugin, "osc_" + std::to_string(i) + "_on", 1.0f);
        set(plugin, "osc_" + std::to_string(i) + "_key_zone_start", 0.0f);
        set(plugin, "osc_" + std::to_string(i) + "_key_zone_end", 12.0f);
      }
      configure(plugin);
      return renderNote(plugin, samples);
    };
    auto distortion = [](int type, float amount, bool modulator_on = true) {
      return [=](SynthPlugin& plugin) {
        set(plugin, "osc_5_distortion_type", float(type));
        set(plugin, "osc_5_distortion_amount", amount);
        set(plugin, "osc_1_on", modulator_on ? 1.0f : 0.0f);
      };
    };
    auto bounded = [](const std::vector<float>& audio) {
      for (float value : audio) {
        if (!std::isfinite(value) || std::abs(value) >= 4.0f)
          return false;
      }
      return true;
    };
    const int quarter = kSampleRate / 4;
    const int eighth = kSampleRate / 8;
    int diff = 0;

    expect(vital::ProducersModule::getModulationIndex(kSlot, 0) == 0, "position 0 of slot 5 reads slot 1");
    const auto baseline = renderWith(vital::source_types::kSample, quarter, distortion(SynthOscillator::kNone, 0.5f));
    expect(energy(baseline) > 1.0e-3, "the sample slot plays");
    expect(identical(baseline, renderWith(vital::source_types::kSample, quarter,
                                          distortion(SynthOscillator::kNone, 0.5f)), 0.0f, &diff),
           "fresh plugins render the sample slot identically");

    const auto ring_modulated = renderWith(vital::source_types::kSample, quarter,
                                           distortion(SynthOscillator::kRmOscillatorA, 1.0f));
    expect(!identical(baseline, ring_modulated, 1.0e-6f, &diff), "ring modulation from a wavetable slot changes a sample slot");
    expect(energy(renderWith(vital::source_types::kSample, quarter,
                             distortion(SynthOscillator::kRmOscillatorA, 1.0f, false))) < 1.0e-9,
           "full ring modulation by a silent slot mutes the sample slot");
    expect(identical(baseline, renderWith(vital::source_types::kSample, quarter,
                                          distortion(SynthOscillator::kRmOscillatorA, 0.0f)), 0.0f, &diff),
           "ring modulation at zero amount is bit-exact with none");
    expect(identical(baseline, renderWith(vital::source_types::kSample, quarter,
                                          distortion(SynthOscillator::kFmOscillatorA, 0.0f)), 0.0f, &diff),
           "FM at zero amount is bit-exact with none");
    const auto frequency_modulated = renderWith(vital::source_types::kSample, quarter,
                                                distortion(SynthOscillator::kFmOscillatorA, 0.5f));
    expect(!identical(baseline, frequency_modulated, 1.0e-6f, &diff), "FM from a wavetable slot changes a sample slot");
    expect(energy(frequency_modulated) > 1.0e-3 && bounded(frequency_modulated), "an FM'd sample plays, finite and bounded");
    // Through-zero FM at full depth runs the sample backwards part of the time.
    const auto deep = renderWith(vital::source_types::kSample, quarter, distortion(SynthOscillator::kFmOscillatorA, 1.0f));
    expect(energy(deep) > 1.0e-4 && bounded(deep), "deep through-zero FM plays, finite and bounded");
    // One-shot (no loop) playback with deep FM must not read outside the region either.
    const auto one_shot = renderWith(vital::source_types::kSample, quarter, [&](SynthPlugin& plugin) {
      distortion(SynthOscillator::kFmOscillatorA, 1.0f)(plugin);
      set(plugin, "osc_5_sample_loop", 0.0f);
    });
    expect(bounded(one_shot), "deep FM on a one-shot sample stays finite and bounded");
    // The half-second one-shot ends at the average rate of 1x; once finished it
    // must not back into its tail on the modulator's troughs.
    const auto one_shot_long = renderWith(vital::source_types::kSample, kSampleRate * 3 / 2, [&](SynthPlugin& plugin) {
      distortion(SynthOscillator::kFmOscillatorA, 1.0f)(plugin);
      set(plugin, "osc_5_sample_loop", 0.0f);
    });
    expect(energy(std::vector<float>(one_shot_long.end() - kSampleRate / 4 * 2, one_shot_long.end())) < 1.0e-9,
           "a finished one-shot sample stays silent under deep FM");
    // A warp a sample cannot do reads as none.
    expect(identical(baseline, renderWith(vital::source_types::kSample, quarter,
                                          distortion(SynthOscillator::kBend, 0.7f)), 0.0f, &diff),
           "warps read as none on a sample slot");
    // The secondary pair works too and both can run together.
    expect(energy(renderWith(vital::source_types::kSample, quarter, [&](SynthPlugin& plugin) {
      distortion(SynthOscillator::kFmOscillatorA, 0.5f)(plugin);
      set(plugin, "osc_5_secondary_distortion_type", float(SynthOscillator::kRmOscillatorB));
      set(plugin, "osc_5_secondary_distortion_amount", 1.0f);
      set(plugin, "osc_2_on", 0.0f);
    })) < 1.0e-9, "the secondary distortion ring modulates by slot 2 while the primary FMs");

    // Granular: FM and RM are live as well.
    const auto granular_baseline = renderWith(vital::source_types::kGranular, quarter,
                                              distortion(SynthOscillator::kNone, 0.5f));
    expect(energy(granular_baseline) > 1.0e-4, "the granular slot plays");
    expect(identical(granular_baseline, renderWith(vital::source_types::kGranular, quarter,
                                                   distortion(SynthOscillator::kNone, 0.5f)), 0.0f, &diff),
           "fresh plugins render the granular slot identically");
    const auto granular_fm = renderWith(vital::source_types::kGranular, quarter,
                                        distortion(SynthOscillator::kFmOscillatorA, 0.5f));
    expect(!identical(granular_baseline, granular_fm, 1.0e-6f, &diff) && bounded(granular_fm), "FM changes a granular slot");
    expect(energy(renderWith(vital::source_types::kGranular, quarter,
                             distortion(SynthOscillator::kRmOscillatorA, 1.0f, false))) < 1.0e-9,
           "full ring modulation by a silent slot mutes granular");

    // Noise: ring modulation only; FM reads as none.
    expect(energy(renderWith(vital::source_types::kNoise, eighth, distortion(SynthOscillator::kNone, 0.5f))) > 1.0e-4,
           "the noise slot plays");
    expect(energy(renderWith(vital::source_types::kNoise, eighth,
                             distortion(SynthOscillator::kRmOscillatorA, 1.0f, false))) < 1.0e-9,
           "full ring modulation by a silent slot mutes noise");
    expect(energy(renderWith(vital::source_types::kNoise, eighth,
                             distortion(SynthOscillator::kFmOscillatorA, 1.0f, false))) > 1.0e-4,
           "FM on a noise slot is ignored, the slot still plays");
    expect(vital::source_types::supportsRm(vital::source_types::kSpectral) &&
           !vital::source_types::supportsFm(vital::source_types::kSpectral) &&
           vital::source_types::supportsFm(vital::source_types::kGranular) &&
           vital::source_types::supportsRm(vital::source_types::kNoise) &&
           !vital::source_types::supportsFm(vital::source_types::kNoise),
           "type registry reports slot modulation support");

    // Render order and routes on one plugin across type changes.
    SynthPlugin plugin;
    preparePlugin(plugin);
    setType(plugin, kSlot, vital::source_types::kNoise);
    auto* slot = plugin.getEngine()->getSourceSlot(kSlot);
    set(plugin, "osc_5_distortion_type", float(SynthOscillator::kFmOscillatorA));
    expect(slot->modulatorPosition(false) < 0,
           "a noise slot with FM selected does not depend on the modulator for render order");
    set(plugin, "osc_5_distortion_type", float(SynthOscillator::kRmOscillatorA));
    expect(slot->modulatorPosition(false) == 0, "a ring modulated noise slot renders after its modulator");
    set(plugin, "osc_5_secondary_distortion_type", float(SynthOscillator::kRmOscillatorC));
    expect(slot->modulatorPosition(true) == 2, "the secondary distortion orders rendering too");

    expect(plugin.connectModulation("lfo_1", "osc_5_distortion_amount"), "distortion amount is a destination on a noise slot");
    setType(plugin, kSlot, vital::source_types::kSample);
    expect(plugin.getConnectionIndex("lfo_1", "osc_5_distortion_amount") >= 0,
           "the route into the shared distortion amount survives Noise -> Sample");
    expect(plugin.getEngine()->getMonoModulationDestination("osc_5_distortion_amount") != nullptr,
           "the sample engine exposes the shared distortion amount");
    setType(plugin, kSlot, vital::source_types::kWavetable);
    expect(plugin.getConnectionIndex("lfo_1", "osc_5_distortion_amount") >= 0, "the route survives Sample -> Wavetable");
    setType(plugin, kSlot, vital::source_types::kSpectral);
    expect(plugin.getConnectionIndex("lfo_1", "osc_5_distortion_amount") >= 0, "the route survives Wavetable -> Spectral");
    expect(plugin.getEngine()->getMonoModulationDestination("osc_5_distortion_amount") != nullptr,
           "the spectral engine exposes the shared distortion amount");
  }


  // Level and pan belong to the slot: a muted oscillator stays muted whatever
  // type it is switched to, and legacy per-type levels land on the slot.
  void testLevelAndPanAreSlotControls() {
    SynthPlugin plugin;
    preparePlugin(plugin);
    auto& controls = plugin.getControls();
    expect(controls.count("osc_5_sample_level") == 0 && controls.count("osc_8_noise_pan") == 0 &&
           controls.count("osc_6_granular_level") == 0 && controls.count("osc_7_spectral_pan") == 0,
           "the per-type level and pan controls are gone");
    expect(!vital::Parameters::isParameter("osc_5_sample_level") && vital::Parameters::isParameter("sample_level"),
           "per-type level parameters are gone, the legacy host alias stays");
    expect(SynthPlugin::legacyAliasTarget("sample_level") == "osc_5_level" &&
           SynthPlugin::legacyAliasTarget("noise_pan") == "osc_8_pan",
           "legacy level and pan host parameters drive the slot controls");

    const int eighth = kSampleRate / 8;
    set(plugin, "osc_1_on", 1.0f);
    set(plugin, "osc_1_level", 0.0f);
    renderNote(plugin, eighth);  // The level smoother fades from its default; let it settle.
    expect(energy(renderNote(plugin, eighth)) < 1.0e-9, "a wavetable slot at level 0 is silent");
    setType(plugin, 0, vital::source_types::kNoise);
    expect(energy(renderNote(plugin, eighth)) < 1.0e-9, "the slot stays muted when switched to Noise");
    setType(plugin, 0, vital::source_types::kSample);
    loadTestSample(plugin.getSample(0));
    set(plugin, "osc_1_sample_loop", 1.0f);
    expect(energy(renderNote(plugin, eighth)) < 1.0e-9, "the slot stays muted when switched to Sample");
    set(plugin, "osc_1_level", 0.5f);
    const auto centred = renderNote(plugin, eighth);
    expect(energy(centred) > 1.0e-4, "raising the slot level is heard by the Sample type");
    set(plugin, "osc_1_pan", -1.0f);
    const auto panned = renderNote(plugin, eighth);
    double left = 0.0, right = 0.0;
    for (size_t i = 0; i < panned.size(); i += 2) {
      left += double(panned[i]) * panned[i];
      right += double(panned[i + 1]) * panned[i + 1];
    }
    expect(left > 1.0e-4 && right < 1.0e-9, "the slot pan pans the Sample type hard left");
    set(plugin, "osc_1_pan", 0.0f);
    setType(plugin, 0, vital::source_types::kGranular);
    expect(energy(renderNote(plugin, eighth)) > 1.0e-4, "Granular reads the slot level");
    set(plugin, "osc_1_level", 0.0f);
    renderNote(plugin, eighth);
    expect(energy(renderNote(plugin, eighth)) < 1.0e-9, "Granular is muted by the slot level");
    setType(plugin, 0, vital::source_types::kNoise);
    set(plugin, "osc_1_level", 0.5f);
    expect(energy(renderNote(plugin, eighth)) > 1.0e-4, "Noise reads the slot level");

    // Routes into level survive type changes.
    expect(plugin.connectModulation("lfo_1", "osc_1_level"), "level is a destination on a Noise slot");
    setType(plugin, 0, vital::source_types::kSpectral);
    expect(plugin.getConnectionIndex("lfo_1", "osc_1_level") >= 0 &&
           plugin.getEngine()->getMonoModulationDestination("osc_1_level") != nullptr,
           "the level route survives Noise -> Spectral");
    setType(plugin, 0, vital::source_types::kWavetable);
    expect(plugin.getConnectionIndex("lfo_1", "osc_1_level") >= 0, "the level route survives Spectral -> Wavetable");

    // Legacy presets: sample_level/pan become the slot's level/pan.
    json settings = { { "sample_level", 0.25f }, { "sample_pan", 0.5f }, { "noise_level", 0.1f } };
    json modulations = json::array({ { { "source", "lfo_1" }, { "destination", "granular_level" } } });
    LoadSave::migrateFixedSourcesToSlots(settings, modulations);
    expect(settings.value("osc_5_level", -1.0f) == 0.25f && settings.value("osc_5_pan", -1.0f) == 0.5f &&
           settings.value("osc_8_level", -1.0f) == 0.1f && settings.count("osc_5_sample_level") == 0,
           "legacy level and pan move to the slot controls");
    expect(modulations[0]["destination"] == "osc_6_level", "legacy level routes move to the slot level");

    // Early slot-era saves: the active type's per-type level moves, the others are dropped.
    json slot_settings = { { "osc_6_type", float(vital::source_types::kGranular) }, { "osc_6_granular_level", 0.1f },
                           { "osc_6_sample_level", 0.9f }, { "osc_6_sample_pan", -0.4f },
                           { "osc_7_spectral_pan", 0.3f }, { "osc_1_noise_level", 0.2f } };
    json slot_modulations = json::array({ { { "source", "lfo_1" }, { "destination", "osc_6_granular_level" } },
                                          { { "source", "lfo_2" }, { "destination", "osc_6_sample_level" } } });
    LoadSave::migrateSlotTypeLevels(slot_settings, slot_modulations);
    expect(slot_settings.value("osc_6_level", -1.0f) == 0.1f && slot_settings.count("osc_6_granular_level") == 0 &&
           slot_settings.count("osc_6_sample_level") == 0 && slot_settings.count("osc_6_sample_pan") == 0,
           "the active type's level moves to the slot and inactive copies are dropped");
    expect(slot_settings.value("osc_7_pan", -1.0f) == 0.3f &&
           slot_settings.value("osc_7_type", -1.0f) == float(vital::source_types::kSpectral),
           "a save without slot 7's type keeps the Spectral source, so its spectral pan moves");
    expect(slot_settings.count("osc_1_level") == 0 && slot_settings.count("osc_1_noise_level") == 0,
           "a wavetable slot drops a stray noise level");
    expect(slot_modulations.size() == 1 && slot_modulations[0]["destination"] == "osc_6_level",
           "routes into the active type's level move, the others are dropped");
  }


  // The plugin's state as JSON (SynthBase::saveToJson is protected).
  json pluginState(SynthPlugin& plugin) {
    juce::MemoryBlock block;
    plugin.getStateInformation(block);
    return json::parse(block.toString().toStdString());
  }

  void testFactoryPresetsLoad();

  // Phase 3: a slot renders its wavetable only while it plays one. The creator
  // keeps the table's state for the slot's return to Wavetable or Analog.
  void testWavetablesRenderOnlyForWavetableSlots() {
    SynthPlugin plugin;
    preparePlugin(plugin);
    for (int slot = 0; slot < 8; ++slot) {
      expect(plugin.getWavetable(slot)->numFrames() == 1, "an initialised slot holds a one-frame default table");
      expect(plugin.isWavetableRenderPending(slot) == (slot >= 4),
             "slots 5-8 (not wavetable types) hold their render back, slots 1-4 are rendered");
    }
    WavetableCreator* creator = plugin.getWavetableCreator(0);
    creator->initPredefinedWaves();
    creator->render();
    const int frames = plugin.getWavetable(0)->numFrames();
    expect(frames > 1, "the predefined wave set renders several frames");
    json table = creator->stateToJson();
    json state = pluginState(plugin);
    state["settings"]["wavetables"][0] = table;
    state["settings"]["wavetables"][4] = table;
    const std::string text = state.dump();
    plugin.setStateInformation(text.data(), int(text.size()));
    expect(plugin.getWavetable(0)->numFrames() == frames, "a wavetable slot renders its loaded table");
    expect(plugin.getWavetable(4)->numFrames() == 1 && plugin.isWavetableRenderPending(4),
           "a Sample slot keeps its loaded table unrendered");
    expect(plugin.getWavetableCreator(4)->getName() == plugin.getWavetableCreator(0)->getName(),
           "the unrendered creator still carries the table's state");
    setType(plugin, 4, vital::source_types::kWavetable);
    expect(plugin.getWavetable(4)->numFrames() == frames && !plugin.isWavetableRenderPending(4),
           "switching the slot to Wavetable renders the table it was holding");
    set(plugin, "osc_5_on", 1.0f);
    expect(energy(renderNote(plugin, kSampleRate / 8)) > 1.0e-4, "the slot plays its rendered table");
    setType(plugin, 4, vital::source_types::kNoise);
    expect(plugin.getWavetable(4)->numFrames() == 1 && plugin.isWavetableRenderPending(4),
           "switching away from the wavetable group drops the table to one frame");
    setType(plugin, 4, vital::source_types::kAnalog);
    expect(plugin.getWavetable(4)->numFrames() == frames, "Analog uses the wavetable group and renders it again");
    json saved = pluginState(plugin);
    expect(saved["settings"]["wavetables"][4]["name"] == table["name"], "the table is saved whatever the slot's type");
  }


  // The secondary warp amounts spread across unison voices like the primary ones.
  void testSecondaryWarpSpread() {
    auto renderWith = [](float voices, const std::function<void(SynthPlugin&)>& configure) {
      SynthPlugin plugin;
      preparePlugin(plugin);
      set(plugin, "osc_1_on", 1.0f);
      set(plugin, "osc_1_unison_voices", voices);
      set(plugin, "osc_1_unison_detune", 0.0f);
      set(plugin, "osc_1_spectral_unison", 1.0f);  // Morph spreads apply per unison voice only with spectral unison.
      set(plugin, "osc_1_secondary_distortion_type", float(vital::SynthOscillator::kBend));
      set(plugin, "osc_1_secondary_distortion_amount", 0.6f);
      set(plugin, "osc_1_secondary_spectral_morph_type", float(vital::SynthOscillator::kHarmonicScale));
      set(plugin, "osc_1_secondary_spectral_morph_amount", 0.4f);
      configure(plugin);
      return renderNote(plugin, kSampleRate / 8);
    };
    int diff = 0;
    const auto unspread = renderWith(4.0f, [](SynthPlugin&) {});
    expect(identical(unspread, renderWith(4.0f, [](SynthPlugin&) {}), 0.0f, &diff), "fresh plugins render identically");
    const auto distortion_spread = renderWith(4.0f, [](SynthPlugin& plugin) {
      set(plugin, "osc_1_secondary_distortion_spread", 0.3f);
    });
    expect(!identical(unspread, distortion_spread, 1.0e-6f, &diff), "secondary distortion spread changes unison voices");
    const auto morph_spread = renderWith(4.0f, [](SynthPlugin& plugin) {
      set(plugin, "osc_1_secondary_spectral_morph_spread", 0.3f);
    });
    expect(!identical(unspread, morph_spread, 1.0e-6f, &diff), "secondary frequency morph spread changes unison voices");
    const auto mono = renderWith(1.0f, [](SynthPlugin&) {});
    const auto mono_spread = renderWith(1.0f, [](SynthPlugin& plugin) {
      set(plugin, "osc_1_secondary_distortion_spread", 0.3f);
      set(plugin, "osc_1_secondary_spectral_morph_spread", 0.3f);
    });
    expect(identical(mono, mono_spread, 0.0f, &diff), "with one unison voice the secondary spreads change nothing");
    expect(vital::Parameters::isParameter("osc_5_secondary_distortion_spread") &&
           vital::Parameters::isParameter("osc_1_secondary_spectral_morph_spread"),
           "the secondary spread parameters exist on every slot");
  }

  int behaviour() {
    testDetoursLeaveTypesUnchanged();
    testDefaultPatch();
    testTypeSwitchBuildsOnlyTheActiveEngine();
    testLegacyPresetMigration();
    testHostParametersAndAutomationProxies();
    testStateRoundTripKeepsSlotTypesAndSamples();
    testSlotModulationBetweenTypes();
    testLevelAndPanAreSlotControls();
    testWavetablesRenderOnlyForWavetableSlots();
    testFactoryPresetsLoad();
    testSecondaryWarpSpread();
    if (failures == 0)
      std::cout << "Atlas source slot tests passed\n";
    return failures == 0 ? 0 : 1;
  }
}
namespace {
  // Loads a preset and reports how the output behaves around a note for a few
  // oscillator switch variants: energy per quarter second while held, then after release.
  int probePreset(const juce::File& file) {
    const auto text = file.loadFileAsString().toStdString();
    struct Variant { const char* name; std::function<void(SynthPlugin&)> apply; };
    const Variant variants[] = {
      { "as saved", [](SynthPlugin&) { } },
      { "osc 3 on", [](SynthPlugin& p) { set(p, "osc_3_on", 1.0f); } },
      { "osc 3 alone", [](SynthPlugin& p) { for (int i = 1; i <= 8; ++i) set(p, "osc_" + std::to_string(i) + "_on", i == 3 ? 1.0f : 0.0f); } },
      { "all oscillators off", [](SynthPlugin& p) { for (int i = 1; i <= 8; ++i) set(p, "osc_" + std::to_string(i) + "_on", 0.0f); } },
    };
    {
      // Live edits while a note is held: toggle oscillator 3, change its type, switch it off.
      SynthPlugin plugin;
      plugin.setPlayConfigDetails(0, 2, kSampleRate, 128);
      plugin.prepareToPlay(kSampleRate, 128);
      plugin.setStateInformation(text.data(), int(text.size()));
      plugin.checkOversampling();
      auto render = [&](int blocks, bool on_event, bool off_event) {
        double e = 0.0;
        for (int b = 0; b < blocks; ++b) {
          juce::AudioBuffer<float> audio(2, 128);
          juce::MidiBuffer midi;
          audio.clear();
          if (b == 0 && on_event) midi.addEvent(juce::MidiMessage::noteOn(1, 60, 0.8f), 0);
          if (b == 0 && off_event) midi.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
          plugin.processBlock(audio, midi);
          for (int i = 0; i < 128; ++i) e += double(audio.getSample(0, i)) * audio.getSample(0, i);
        }
        return e;
      };
      std::cout << "live: note on " << render(40, true, false);
      plugin.parameterChanged("osc_3_on", 1.0f); std::cout << " | osc3 on " << render(40, false, false);
      plugin.parameterChanged("osc_3_type", 5.0f); std::cout << " | osc3 noise " << render(40, false, false);
      plugin.parameterChanged("osc_3_type", 0.0f); std::cout << " | osc3 wavetable " << render(40, false, false);
      plugin.parameterChanged("osc_3_on", 0.0f); std::cout << " | osc3 off " << render(40, false, false);
      std::cout << " | released " << render(200, false, true) << " | later " << render(200, false, false) << '\n';
      for (int i = 1; i <= 8; ++i) plugin.parameterChanged("osc_" + std::to_string(i) + "_on", 0.0f);
      std::cout << "live: all off, new note " << render(100, true, false) << " | released " << render(200, false, true) << '\n';
    }
    for (int experiment = 0; experiment < 3; ++experiment) {
      SynthPlugin plugin;
      plugin.setPlayConfigDetails(0, 2, kSampleRate, 128);
      plugin.prepareToPlay(kSampleRate, 128);
      plugin.setStateInformation(text.data(), int(text.size()));
      for (int i = 1; i <= 8; ++i) plugin.parameterChanged("osc_" + std::to_string(i) + "_on", 0.0f);
      plugin.parameterChanged("env_1_sustain", 1.0f); // hold the tone so switch effects are visible
      plugin.checkOversampling();
      auto render = [&](int blocks, bool on_event) {
        double e = 0.0;
        for (int b = 0; b < blocks; ++b) {
          juce::AudioBuffer<float> audio(2, 128);
          juce::MidiBuffer midi;
          audio.clear();
          if (b == 0 && on_event) midi.addEvent(juce::MidiMessage::noteOn(1, 60, 0.8f), 0);
          plugin.processBlock(audio, midi);
          for (int i = 0; i < 128; ++i) e += double(audio.getSample(0, i)) * audio.getSample(0, i);
        }
        return e;
      };
      if (experiment == 2) plugin.parameterChanged("osc_3_on", 1.0f);
      std::cout << "exp " << experiment << ": note on (osc3 " << (experiment == 2 ? "on" : "off") << ") " << render(40, true);
      if (experiment != 2) { plugin.parameterChanged("osc_3_on", 1.0f); std::cout << " | osc3 on " << render(40, false); }
      if (experiment == 1) { plugin.parameterChanged("osc_3_type", 5.0f); render(20, false); plugin.parameterChanged("osc_3_type", 0.0f); std::cout << " | after type round trip " << render(40, false); }
      plugin.parameterChanged("osc_3_on", 0.0f);
      std::cout << " | osc3 off " << render(40, false) << " " << render(40, false) << '\n';
    }
    for (const auto& variant : variants) {
      SynthPlugin plugin;
      plugin.setPlayConfigDetails(0, 2, kSampleRate, 128);
      plugin.prepareToPlay(kSampleRate, 128);
      plugin.setStateInformation(text.data(), int(text.size()));
      variant.apply(plugin);
      plugin.checkOversampling();
      std::cout << variant.name << ": types";
      for (int i = 0; i < 8; ++i)
        std::cout << ' ' << plugin.getEngine()->getSourceSlot(i)->activeType() << (plugin.getControls().at(vital::source_types::slotPrefix(i) + "on")->value() >= 0.5f ? "+" : "-");
      std::cout << "\n  energy per 0.25 s:";
      const int quarter = kSampleRate / 4;
      bool note_on = true;
      for (int segment = 0; segment < 12; ++segment) {
        if (segment == 4) note_on = false;
        double e = 0.0;
        for (int offset = 0; offset < quarter; offset += 128) {
          juce::AudioBuffer<float> audio(2, 128);
          juce::MidiBuffer midi;
          audio.clear();
          if (segment == 0 && offset == 0) midi.addEvent(juce::MidiMessage::noteOn(1, 60, 0.8f), 0);
          if (segment == 4 && offset == 0) midi.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
          plugin.processBlock(audio, midi);
          for (int i = 0; i < 128; ++i) e += double(audio.getSample(0, i)) * audio.getSample(0, i);
        }
        std::cout << ' ' << (segment == 4 ? "| " : "") << std::fixed << std::setprecision(4) << e;
      }
      std::cout << '\n';
    }
    return 0;
  }
}

namespace {
  // Loads preset B while a note from preset A is held, then releases: reports
  // the output energy per 0.25 s so a stuck or orphaned sound shows up.
  int probePresetSwitch(const juce::File& first, const juce::File& second) {
    for (int mode = 0; mode < 3; ++mode) {
      SynthPlugin plugin;
      plugin.setPlayConfigDetails(0, 2, kSampleRate, 128);
      plugin.prepareToPlay(kSampleRate, 128);
      std::string error;
      plugin.loadFromFile(first, error);
      const char* names[] = { "load file while held", "host state while held", "init patch while held" };
      std::cout << names[mode] << ": ";
      const int quarter = kSampleRate / 4;
      for (int segment = 0; segment < 14; ++segment) {
        double e = 0.0;
        for (int offset = 0; offset < quarter; offset += 128) {
          juce::AudioBuffer<float> audio(2, 128);
          juce::MidiBuffer midi;
          audio.clear();
          if (segment == 0 && offset == 0) midi.addEvent(juce::MidiMessage::noteOn(1, 60, 0.8f), 0);
          if (segment == 2 && offset == 0) {
            if (mode == 0) plugin.loadFromFile(second, error);
            else if (mode == 1) {
              const auto text = second.loadFileAsString().toStdString();
              plugin.setStateInformation(text.data(), int(text.size()));
            }
            else plugin.loadInitPreset();
          }
          if (segment == 4 && offset == 0) midi.addEvent(juce::MidiMessage::noteOn(1, 64, 0.8f), 0);
          if (segment == 6 && offset == 0) { midi.addEvent(juce::MidiMessage::noteOff(1, 60), 0); midi.addEvent(juce::MidiMessage::noteOff(1, 64), 0); }
          plugin.processBlock(audio, midi);
          for (int i = 0; i < 128; ++i) e += double(audio.getSample(0, i)) * audio.getSample(0, i);
        }
        std::cout << (segment == 2 ? "|switch| " : segment == 4 ? "|note2| " : segment == 6 ? "|off| " : "") << std::fixed << std::setprecision(3) << e << ' ';
      }
      std::cout << '\n';
    }
    return 0;
  }
}

namespace {
  // Two plugins follow the same timeline (load A, run, load B, run, release, play
  // two notes); only one of them holds notes across the switch to B. Any difference
  // in the later note comes from switching presets while notes were held.
  int probeSwitchState(const juce::File& first, const juce::File& second) {
    auto make = [&](bool hold) {
      auto plugin = std::make_unique<SynthPlugin>();
      plugin->setPlayConfigDetails(0, 2, kSampleRate, 128);
      plugin->prepareToPlay(kSampleRate, 128);
      std::string error;
      if (!plugin->loadFromFile(first, error)) std::cout << "first load failed: " << error << '\n';
      juce::AudioBuffer<float> audio(2, 128);
      juce::MidiBuffer midi;
      // Both variants play the same notes; only the held one still holds them at the switch.
      midi.addEvent(juce::MidiMessage::noteOn(1, 60, 0.8f), 0);
      midi.addEvent(juce::MidiMessage::noteOn(1, 67, 0.8f), 0);
      for (int b = 0; b < 40; ++b) { audio.clear(); plugin->processBlock(audio, midi); midi.clear(); }
      if (!hold) {
        midi.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
        midi.addEvent(juce::MidiMessage::noteOff(1, 67), 0);
        for (int b = 0; b < 400; ++b) { audio.clear(); plugin->processBlock(audio, midi); midi.clear(); }
      }
      else {
        for (int b = 0; b < 400; ++b) { audio.clear(); plugin->processBlock(audio, midi); }
      }
      if (!plugin->loadFromFile(second, error)) std::cout << "second load failed: " << error << '\n';
      double after_switch = 0.0;
      for (int b = 0; b < 40; ++b) {
        audio.clear(); plugin->processBlock(audio, midi);
        for (int i = 0; i < 128; ++i) after_switch += double(audio.getSample(0, i)) * audio.getSample(0, i);
      }
      midi.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
      midi.addEvent(juce::MidiMessage::noteOff(1, 67), 0);
      double after_release = 0.0;
      for (int b = 0; b < 400; ++b) {
        audio.clear(); plugin->processBlock(audio, midi); midi.clear();
        if (b >= 200) for (int i = 0; i < 128; ++i) after_release += double(audio.getSample(0, i)) * audio.getSample(0, i);
      }
      std::cout << (hold ? "held" : "silent") << " switch: energy right after switch " << after_switch
                << ", 0.5-1.0 s after release " << after_release << '\n';
      return plugin;
    };
    auto play = [](SynthPlugin& plugin, int note, int blocks) {
      std::vector<float> out;
      juce::AudioBuffer<float> audio(2, 128);
      for (int b = 0; b < blocks; ++b) {
        juce::MidiBuffer midi;
        audio.clear();
        if (b == 0) midi.addEvent(juce::MidiMessage::noteOn(1, note, 0.8f), 0);
        if (b == blocks / 2) midi.addEvent(juce::MidiMessage::noteOff(1, note), 0);
        plugin.processBlock(audio, midi);
        for (int i = 0; i < 128; ++i) out.push_back(audio.getSample(0, i));
      }
      for (int b = 0; b < 400; ++b) { juce::MidiBuffer midi; audio.clear(); plugin.processBlock(audio, midi); }
      return out;
    };
    auto silent = make(false);
    auto held = make(true);
    // Same engine state? Compare controls, routes and slot types.
    int control_diffs = 0;
    for (const auto& control : silent->getControls()) {
      const auto other = held->getControls().find(control.first);
      if (other != held->getControls().end() && other->second->value() != control.second->value()) {
        if (control_diffs < 8) std::cout << "  control differs: " << control.first << " " << control.second->value() << " vs " << other->second->value() << '\n';
        ++control_diffs;
      }
    }
    std::cout << "controls differing: " << control_diffs << ", routes " << silent->getModulationConnections().size()
              << " vs " << held->getModulationConnections().size() << ", slot types";
    for (int i = 0; i < 8; ++i)
      std::cout << ' ' << silent->getEngine()->getSourceSlot(i)->activeType() << '/' << held->getEngine()->getSourceSlot(i)->activeType();
    std::cout << '\n';
    play(*silent, 60, 60); play(*held, 60, 60);
    const auto a = play(*silent, 60, 60);
    const auto b = play(*held, 60, 60);
    int diff = 0;
    double max_diff = 0.0;
    for (size_t i = 0; i < a.size(); ++i) max_diff = std::max(max_diff, double(std::abs(a[i] - b[i])));
    std::cout << "later note, silent-switch vs held-switch: " << (identical(a, b, 0.0f, &diff) ? "identical" : "DIFFERENT")
              << " first diff " << diff << " max " << max_diff << " energies " << energy(a) << " vs " << energy(b) << '\n';
    return 0;
  }
}

namespace {
  // Factory presets that mix oscillator types. Their samples are synthesised
  // here so the presets stay self-contained (samples embed in the preset).
  namespace material {
    std::vector<float> pluck(float frequency, float seconds) {
      // Karplus-Strong: a noise burst through a lowpassed delay line.
      std::vector<float> out(static_cast<size_t>(seconds * kSampleRate));
      const int period = std::max(2, int(kSampleRate / frequency));
      std::vector<float> line(static_cast<size_t>(period));
      uint32_t seed = 12345u;
      for (float& v : line) {
        seed = seed * 1664525u + 1013904223u;
        v = (seed >> 8) / float(1 << 24) * 2.0f - 1.0f;
      }
      size_t index = 0;
      for (size_t i = 0; i < out.size(); ++i) {
        const float current = line[index];
        const float next = line[(index + 1) % line.size()];
        line[index] = 0.996f * 0.5f * (current + next);
        out[i] = current * 0.8f;
        index = (index + 1) % line.size();
      }
      return out;
    }

    std::vector<float> metal(float seconds) {
      // Inharmonic partials with their own decays: a struck bar.
      std::vector<float> out(static_cast<size_t>(seconds * kSampleRate));
      const float ratios[] = { 1.0f, 2.76f, 5.4f, 8.93f, 13.34f };
      const float gains[] = { 0.5f, 0.35f, 0.25f, 0.15f, 0.08f };
      for (size_t i = 0; i < out.size(); ++i) {
        const double t = i / double(kSampleRate);
        double v = 0.0;
        for (int k = 0; k < 5; ++k)
          v += gains[k] * std::sin(2.0 * M_PI * 220.0 * ratios[k] * t) * std::exp(-t * (2.0 + 1.5 * k));
        out[i] = float(v);
      }
      return out;
    }

    std::vector<float> choir(float seconds) {
      // Slowly drifting detuned saws through two moving formant bands.
      std::vector<float> out(static_cast<size_t>(seconds * kSampleRate));
      double phases[3] = { 0.0, 0.33, 0.66 };
      const double freqs[3] = { 220.0, 220.0 * 1.003, 220.0 * 0.997 };
      double f1 = 0.0, f2 = 0.0, b1 = 0.0, b2 = 0.0;
      for (size_t i = 0; i < out.size(); ++i) {
        const double t = i / double(kSampleRate);
        double saw = 0.0;
        for (int k = 0; k < 3; ++k) {
          phases[k] += freqs[k] / kSampleRate;
          phases[k] -= std::floor(phases[k]);
          saw += (phases[k] * 2.0 - 1.0) / 3.0;
        }
        // Two resonant one-pole pairs sweeping between "oo" and "ah".
        const double sweep = 0.5 + 0.5 * std::sin(2.0 * M_PI * 0.2 * t);
        const double c1 = 2.0 * M_PI * (400.0 + 350.0 * sweep) / kSampleRate;
        const double c2 = 2.0 * M_PI * (900.0 + 500.0 * sweep) / kSampleRate;
        b1 += c1 * (saw - f1); f1 += c1 * (b1 - f1);
        b2 += c2 * (saw - f2); f2 += c2 * (b2 - f2);
        const double env = std::min(1.0, t * 4.0) * std::min(1.0, (seconds - t) * 4.0);
        out[i] = float(0.5 * (f1 + 0.7 * f2) * env);
      }
      return out;
    }
  }

  void loadMaterial(SynthPlugin& plugin, int slot, const std::vector<float>& audio, const char* name) {
    vital::Sample* sample = plugin.getSample(slot);
    sample->loadSample(audio.data(), int(audio.size()), kSampleRate);
    sample->setName(name);
  }

  struct FactoryPreset {
    const char* name;
    const char* style;
    const char* comments;
    std::function<void(SynthPlugin&)> configure;
  };

  std::vector<FactoryPreset> factoryPresets() {
    using namespace vital::source_types;
    using O = vital::SynthOscillator;
    auto id = [](int slot, const char* suffix) { return slotPrefix(slot) + suffix; };
    return {
      { "Granular Over Analog", "Pad",
        "Oscillator 1 is an Analog saw; Oscillator 6 is a Granular cloud of a plucked string, frequency "
        "modulated by Oscillator 1 (Distortion mode: FM from Oscillator 1).",
        [=](SynthPlugin& plugin) {
          setType(plugin, 0, kAnalog);
          set(plugin, id(0, "on"), 1.0f);
          set(plugin, id(0, "level"), 0.4f);
          set(plugin, id(0, "transpose"), -12.0f);
          loadMaterial(plugin, 5, material::pluck(110.0f, 1.2f), "Factory Pluck");
          set(plugin, id(5, "on"), 1.0f);
          set(plugin, id(5, "level"), 0.6f);
          set(plugin, id(5, "granular_density"), 36.0f);
          set(plugin, id(5, "granular_grain_size"), 140.0f);
          set(plugin, id(5, "granular_random_position"), 0.15f);
          set(plugin, id(5, "distortion_type"), float(O::kFmOscillatorA));
          set(plugin, id(5, "distortion_amount"), 0.2f);
          set(plugin, "env_1_release", 1.2f);
        } },
      { "Ring Mod Noise Bells", "Keys",
        "Oscillator 1 is an Analog sine an octave up; Oscillator 8 is Air noise ring modulated by it "
        "(Distortion mode: Ring Mod from Oscillator 1), so the noise rings at the note's pitch.",
        [=](SynthPlugin& plugin) {
          setType(plugin, 0, kAnalog);
          set(plugin, id(0, "on"), 1.0f);
          set(plugin, id(0, "analog_waveform"), float(vital::analog::kSine));
          set(plugin, id(0, "level"), 0.35f);
          set(plugin, id(0, "transpose"), 12.0f);
          set(plugin, id(7, "on"), 1.0f);
          set(plugin, id(7, "level"), 0.7f);
          set(plugin, id(7, "noise_type"), float(vital::NoiseSource::kAir));
          set(plugin, id(7, "noise_low_cutoff"), 52.0f);
          set(plugin, id(7, "noise_high_cutoff"), 112.0f);
          set(plugin, id(7, "distortion_type"), float(O::kRmOscillatorA));
          set(plugin, id(7, "distortion_amount"), 1.0f);
          set(plugin, "env_1_release", 1.6f);
        } },
      { "Twin Samples Split", "Keys",
        "Two Sample oscillators: Oscillator 5 plays a plucked string below middle C, Oscillator 2 (switched to "
        "Sample) plays a struck bar from middle C up. Any oscillator can be a sampler.",
        [=](SynthPlugin& plugin) {
          set(plugin, id(0, "on"), 0.0f);
          loadMaterial(plugin, 4, material::pluck(110.0f, 1.2f), "Factory Pluck");
          set(plugin, id(4, "on"), 1.0f);
          set(plugin, id(4, "level"), 0.7f);
          set(plugin, id(4, "sample_loop"), 0.0f);
          set(plugin, id(4, "key_zone_end"), 59.0f);
          setType(plugin, 1, kSample);
          loadMaterial(plugin, 1, material::metal(1.5f), "Factory Bar");
          set(plugin, id(1, "on"), 1.0f);
          set(plugin, id(1, "level"), 0.6f);
          set(plugin, id(1, "sample_loop"), 0.0f);
          set(plugin, id(1, "sample_root_key"), 57.0f);
          set(plugin, id(1, "key_zone_start"), 60.0f);
          set(plugin, "env_1_release", 0.8f);
        } },
      { "Spectral Freeze Pad", "Pad",
        "Oscillator 7 freezes a spectral snapshot of a formant choir and plays it at any pitch; Oscillator 1 "
        "adds an Analog saw an octave below.",
        [=](SynthPlugin& plugin) {
          setType(plugin, 0, kAnalog);
          set(plugin, id(0, "on"), 1.0f);
          set(plugin, id(0, "level"), 0.25f);
          set(plugin, id(0, "transpose"), -12.0f);
          loadMaterial(plugin, 6, material::choir(2.5f), "Factory Choir");
          set(plugin, id(6, "on"), 1.0f);
          set(plugin, id(6, "level"), 0.65f);
          set(plugin, id(6, "spectral_freeze"), 1.0f);
          set(plugin, id(6, "spectral_position"), 0.4f);
          set(plugin, "env_1_attack", 0.6f);
          set(plugin, "env_1_release", 2.0f);
        } },
      { "Eight Ways", "Experimental",
        "Every oscillator type at once: Wavetable, Analog, Sample, Granular, Spectral and Noise across the "
        "eight slots, with Oscillator 7's bar ring modulated by Oscillator 2.",
        [=](SynthPlugin& plugin) {
          set(plugin, id(0, "on"), 1.0f);
          set(plugin, id(0, "level"), 0.3f);
          setType(plugin, 1, kAnalog);
          set(plugin, id(1, "on"), 1.0f);
          set(plugin, id(1, "level"), 0.3f);
          set(plugin, id(1, "analog_waveform"), float(vital::analog::kTriangle));
          set(plugin, id(1, "transpose"), 12.0f);
          setType(plugin, 2, kSample);
          loadMaterial(plugin, 2, material::pluck(110.0f, 1.2f), "Factory Pluck");
          set(plugin, id(2, "on"), 1.0f);
          set(plugin, id(2, "level"), 0.4f);
          set(plugin, id(2, "sample_loop"), 0.0f);
          setType(plugin, 3, kGranular);
          loadMaterial(plugin, 3, material::choir(2.5f), "Factory Choir");
          set(plugin, id(3, "on"), 1.0f);
          set(plugin, id(3, "level"), 0.35f);
          set(plugin, id(3, "granular_density"), 20.0f);
          set(plugin, id(3, "granular_grain_size"), 200.0f);
          setType(plugin, 4, kSpectral);
          loadMaterial(plugin, 4, material::choir(2.5f), "Factory Choir");
          set(plugin, id(4, "on"), 1.0f);
          set(plugin, id(4, "level"), 0.3f);
          set(plugin, id(4, "spectral_freeze"), 1.0f);
          setType(plugin, 5, kNoise);
          set(plugin, id(5, "on"), 1.0f);
          set(plugin, id(5, "level"), 0.2f);
          set(plugin, id(5, "noise_type"), float(vital::NoiseSource::kPink));
          set(plugin, id(5, "noise_high_cutoff"), 90.0f);
          setType(plugin, 6, kSample);
          loadMaterial(plugin, 6, material::metal(1.5f), "Factory Bar");
          set(plugin, id(6, "on"), 1.0f);
          set(plugin, id(6, "level"), 0.4f);
          set(plugin, id(6, "sample_loop"), 0.0f);
          set(plugin, id(6, "distortion_type"), float(O::kRmOscillatorB));
          set(plugin, id(6, "distortion_amount"), 0.8f);
          set(plugin, id(7, "on"), 1.0f);
          set(plugin, id(7, "level"), 0.15f);
          set(plugin, id(7, "noise_type"), float(vital::NoiseSource::kAir));
          set(plugin, id(7, "noise_low_cutoff"), 96.0f);
          set(plugin, "env_1_release", 1.0f);
        } },
    };
  }

  int writePresets(const juce::File& folder) {
    folder.createDirectory();
    for (const FactoryPreset& preset : factoryPresets()) {
      SynthPlugin plugin;
      preparePlugin(plugin);
      // preparePlugin switches every slot off; presets start from the defaults they
      // were written with, which included full phase randomization.
      for (int slot = 0; slot < 8; ++slot) {
        set(plugin, vital::source_types::slotPrefix(slot) + "on", slot == 0 ? 1.0f : 0.0f);
        set(plugin, vital::source_types::slotPrefix(slot) + "random_phase", 1.0f);
      }
      set(plugin, "oversampling", vital::Parameters::getDetails("oversampling").default_value);
      preset.configure(plugin);
      plugin.setPresetName(preset.name);
      plugin.setAuthor("Blind Producers");
      plugin.setStyle(preset.style);
      plugin.setComments(preset.comments);
      // Every preset must make sound before it ships.
      const auto audio = renderNote(plugin, kSampleRate / 2);
      expect(energy(audio) > 1.0e-3, std::string("preset plays: ") + preset.name);
      json state = pluginState(plugin);
      const juce::File file = folder.getChildFile(juce::String(preset.name) + ".atp");
      expect(file.replaceWithText(juce::String(state.dump())), std::string("preset written: ") + preset.name);
      std::cout << "wrote " << file.getFullPathName() << " (" << file.getSize() / 1024 << " KB)\n";
    }
    return failures == 0 ? 0 : 1;
  }

  // Each shipped preset loads back with its types and sounds.
  void testFactoryPresetsLoad() {
    for (const FactoryPreset& preset : factoryPresets()) {
      SynthPlugin source;
      preparePlugin(source);
      preset.configure(source);
      source.setPresetName(preset.name);
      const std::string text = pluginState(source).dump();
      SynthPlugin plugin;
      preparePlugin(plugin);
      plugin.setStateInformation(text.data(), int(text.size()));
      for (int slot = 0; slot < 8; ++slot) {
        expect(plugin.getEngine()->getSourceSlot(slot)->activeType() == source.getEngine()->getSourceSlot(slot)->activeType(),
               std::string(preset.name) + ": slot types reload");
      }
      if (std::string(preset.name) == "Spectral Freeze Pad" || std::string(preset.name) == "Eight Ways")
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
      expect(energy(renderNote(plugin, kSampleRate / 4)) > 1.0e-4, std::string(preset.name) + ": reloaded preset plays");
    }
  }
}

namespace {
  // Wall time of rendering `seconds` of a held note, best of three fresh plugins.
  double renderMs(const std::function<void(SynthPlugin&)>& configure, double seconds) {
    double best = 1.0e9;
    for (int run = 0; run < 3; ++run) {
      SynthPlugin plugin;
      preparePlugin(plugin);
      configure(plugin);
      const int samples = int(seconds * kSampleRate);
      juce::AudioBuffer<float> audio(2, 128);
      const auto start = std::chrono::steady_clock::now();
      for (int offset = 0; offset < samples; offset += 128) {
        juce::MidiBuffer midi;
        audio.clear();
        if (offset == 0)
          midi.addEvent(juce::MidiMessage::noteOn(1, 60, 0.7f), 0);
        plugin.processBlock(audio, midi);
      }
      best = std::min(best, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    }
    return best;
  }

  // Per-type render cost. The bounds are loose: they catch an inactive type
  // or an off slot that still costs, not small regressions.
  int cpu() {
    const double seconds = 2.0;
    auto slot_on = [](int slot, int type) {
      return [=](SynthPlugin& plugin) {
        setType(plugin, slot, type);
        if (vital::source_types::describe(type).uses_sample) {
          loadTestSample(plugin.getSample(slot));
          set(plugin, vital::source_types::slotPrefix(slot) + "sample_loop", 1.0f);
        }
        set(plugin, vital::source_types::slotPrefix(slot) + "on", 1.0f);
        if (type == vital::source_types::kSpectral)
          std::this_thread::sleep_for(std::chrono::milliseconds(1500));  // background analysis
      };
    };
    // Wall-clock bounds are at the mercy of whatever else the machine runs,
    // so a failed measurement is repeated: a real hidden cost fails every
    // attempt, a busy moment does not.
    struct Check { bool passed; const char* name; };
    std::vector<Check> checks;
    constexpr int kAttempts = 3;
    for (int attempt = 1; attempt <= kAttempts; ++attempt) {
      const double all_off = renderMs([](SynthPlugin&) {}, seconds);
      std::cout << std::fixed << std::setprecision(1) << "attempt " << attempt << "\nall slots off: " << all_off
                << " ms per " << seconds << " s\n";
      double per_type[vital::source_types::kNumTypes];
      for (int type = 0; type < vital::source_types::kNumTypes; ++type) {
        per_type[type] = renderMs(slot_on(0, type), seconds);
        std::cout << "one " << vital::source_types::describe(type).id << " slot: " << per_type[type] << " ms\n";
      }
      const double eight_noise = renderMs([&](SynthPlugin& plugin) {
        for (int slot = 0; slot < 8; ++slot) slot_on(slot, vital::source_types::kNoise)(plugin);
      }, seconds);
      const double eight_wavetable = renderMs([&](SynthPlugin& plugin) {
        for (int slot = 0; slot < 8; ++slot) slot_on(slot, vital::source_types::kWavetable)(plugin);
      }, seconds);
      const double eight_sample = renderMs([&](SynthPlugin& plugin) {
        for (int slot = 0; slot < 8; ++slot) slot_on(slot, vital::source_types::kSample)(plugin);
      }, seconds);
      std::cout << "eight noise slots: " << eight_noise << " ms\neight wavetable slots: " << eight_wavetable
                << " ms\neight sample slots: " << eight_sample << " ms\n";

      const double wavetable = per_type[vital::source_types::kWavetable];
      // Cheap types add well under a millisecond per slot, which timing noise
      // swamps, so the per-slot term has a floor: the check is for a slot that
      // secretly runs an engine it should not, not for small regressions.
      auto marginal = [&](double total) { return std::max(0.0, total - all_off); };
      auto budget = [&](double one_slot) { return 8.0 * (marginal(one_slot) + 0.5) * 2.0 + 2.0; };
      checks = {
        { all_off < wavetable, "eight off slots (with eight inactive types each) cost less than one playing wavetable" },
        { marginal(eight_noise) < budget(per_type[vital::source_types::kNoise]),
          "eight noise slots scale linearly: no hidden per-slot cost" },
        { marginal(eight_wavetable) < budget(wavetable), "eight wavetable slots scale linearly" },
        { marginal(eight_sample) < budget(per_type[vital::source_types::kSample]), "eight sample slots scale linearly" },
      };
      if (std::all_of(checks.begin(), checks.end(), [](const Check& check) { return check.passed; }))
        break;
      if (attempt < kAttempts)
        std::cout << "a bound was missed; measuring again\n";
    }
    for (const Check& check : checks)
      expect(check.passed, check.name);
    if (failures == 0)
      std::cout << "Atlas source slot CPU tests passed\n";
    return failures == 0 ? 0 : 1;
  }
}

int main(int argc, char** argv) {
  juce::ScopedJuceInitialiser_GUI juce;
  if (argc > 3 && juce::String(argv[1]) == "--probe-switch-state")
    return probeSwitchState(juce::File(argv[2]), juce::File(argv[3]));
  if (argc > 3 && juce::String(argv[1]) == "--probe-switch")
    return probePresetSwitch(juce::File(argv[2]), juce::File(argv[3]));
  if (argc > 2 && juce::String(argv[1]) == "--probe")
    return probePreset(juce::File(argv[2]));
  if (argc > 2 && juce::String(argv[1]) == "--capture")
    return capture(juce::File(argv[2]));
  if (argc > 2 && juce::String(argv[1]) == "--compare")
    return compare(juce::File(argv[2]));
  if (argc > 2 && juce::String(argv[1]) == "--write-presets")
    return writePresets(juce::File(juce::String(argv[2])));
  if (argc > 1 && juce::String(argv[1]) == "--cpu")
    return cpu();
  if (argc > 1 && juce::String(argv[1]) == "--behaviour")
    return behaviour();
  if (argc > 1) {
    std::cerr << "usage: AtlasSourceSlotTests [--behaviour | --cpu | --write-presets <dir> | --capture <dir> | --compare <dir>]\n";
    return 2;
  }
  return behaviour();
}
