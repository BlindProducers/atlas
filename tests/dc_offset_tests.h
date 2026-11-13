// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// DC on held notes: the Analog oscillator's Hard Sync and the voice filters
// on their own, then both in the plugin, with single notes and chords, and
// effects in a slot. `--dc-offsets <preset> [name=value...]...` measures
// presets instead (`init` is the initial patch), and `--dc-offsets effects
// [settings]` every effect with that many random settings.

#include "synth_strings.h"

#include <iomanip>
#include <sstream>

namespace dc_offset_tests {
  using O = vital::SynthOscillator;
  namespace A = vital::analog;

  constexpr int kRate = 48000;
  constexpr int kBlock = 128;
  constexpr double kMaxDcDb = -80.0;

  double toDb(double value) { return 20.0 * std::log10(std::max(value, 1.0e-12)); }

  // Accumulates the mean of a signal under a Blackman-Harris window, which
  // keeps the notes' own partials more than 90 dB out of it.
  struct WindowedMean {
    int length, position = 0;
    double sum[2] {}, weights = 0.0, peak = 0.0;
    explicit WindowedMean(int samples) : length(samples) { }
    void add(double left, double right) {
      const double x = 2.0 * juce::MathConstants<double>::pi * position++ / (length - 1);
      const double w = .35875 - .48829 * std::cos(x) + .14128 * std::cos(2 * x) - .01168 * std::cos(3 * x);
      peak = std::max({ peak, std::abs(left), std::abs(right) });
      sum[0] += w * left;
      sum[1] += w * right;
      weights += w;
    }
    double db() const { return toDb(std::max(std::abs(sum[0]), std::abs(sum[1])) / weights); }
  };

  void report(const std::string& name, double dc_db, bool must_pass = true) {
    std::cout << name << ": DC " << std::fixed << std::setprecision(1) << dc_db << " dB\n";
    if (must_pass)
      expect(dc_db < kMaxDcDb, name + ": DC below " + std::to_string(int(kMaxDcDb)) + " dB");
  }

  // One second settles, the next is measured.
  void testHardSync() {
    for (int waveform : { A::kSine, A::kTriangle, A::kSaw, A::kSquare }) for (float shape : { 0.0f, .6f })
      for (float amount : { .2f, .33f, .45f, .7f, .9f }) {
        analog_oscillator_tests::AnalogFixture f(kRate, waveform, shape);
        f.distortion(O::kSync, amount);
        const std::vector<float> out = f.lane(2 * kRate);
        WindowedMean mean(kRate);
        for (int i = kRate; i < 2 * kRate; ++i)
          mean.add(out[i], out[i]);
        std::ostringstream name;
        name << "Analog oscillator " << waveform << ", shape " << shape << ", Hard Sync " << amount;
        report(name.str(), mean.db());
      }
  }

  // An Analog saw's partials make the models' saturation uneven. Every model
  // but Filter Table, which needs a table. Sample & Hold, which has no DC
  // blocker, is only reported: its aliasing lands near 0 Hz.
  void testFilters() {
    const std::pair<int, const char*> models[] = {
      { vital::constants::kAnalog, "Analog" }, { vital::constants::kDirty, "Dirty" },
      { vital::constants::kLadder, "Ladder" }, { vital::constants::kDigital, "Digital" },
      { vital::constants::kDiode, "Diode" }, { vital::constants::kFormant, "Formant" },
      { vital::constants::kComb, "Comb" }, { vital::constants::kPhase, "Phase" },
      { vital::constants::kReverbFilter, "Reverb" }, { vital::constants::kScreamFilter, "Scream" },
      { vital::constants::kRingModFilter, "Ring Mod" }, { vital::constants::kSampleHoldFilter, "Sample & Hold" },
      { vital::constants::kDistortionFilter, "Distortion" },
      { vital::constants::kBandRejectFilter, "Band Reject" }
    };
    for (const auto& model : models) for (int style : { 0, 1 }) for (float resonance : { .5f, .9f }) {
      analog_oscillator_tests::AnalogFixture source(kRate, A::kSaw);
      vital::FilterModule filter("filter_1");
      vital::Output audio;
      audio.ensureBufferSize(vital::kMaxBufferSize);
      filter.plug(&audio, vital::FilterModule::kAudio);
      filter.init();
      const auto controls = filter.getControls();
      controls.at("filter_1_on")->set(1);
      controls.at("filter_1_model")->set(model.first);
      controls.at("filter_1_style")->set(style);
      controls.at("filter_1_cutoff")->set(60);
      controls.at("filter_1_resonance")->set(resonance);
      controls.at("filter_1_mix")->set(1);
      filter.setSampleRate(kRate);

      WindowedMean mean(kRate);
      for (int b = 0; b < 2 * kRate / kBlock; ++b) {
        source.process(kBlock);
        for (int i = 0; i < kBlock; ++i)
          audio.buffer[i] = source.oscillator->output(O::kRaw)->buffer[i];
        filter.process(kBlock);
        if (b >= kRate / kBlock)
          for (int i = 0; i < kBlock; ++i)
            mean.add(filter.output()->buffer[i][0], filter.output()->buffer[i][1]);
      }
      std::ostringstream name;
      name << model.second << (style ? " 24 dB" : " 12 dB") << " filter, resonance " << resonance;
      report(name.str(), mean.db(), model.first != vital::constants::kSampleHoldFilter);
    }
  }

  SynthPlugin& freshPlugin(std::unique_ptr<SynthPlugin>& plugin) {
    plugin = std::make_unique<SynthPlugin>();
    plugin->setPlayConfigDetails(0, 2, kRate, kBlock);
    plugin->prepareToPlay(kRate, kBlock);
    return *plugin;
  }

  // Holds the notes for half a second to settle, measures the next 2 s, then
  // lets them ring out.
  WindowedMean holdNotes(SynthPlugin& plugin, const std::vector<int>& notes) {
    constexpr int kSettleBlocks = kRate / 2 / kBlock;
    constexpr int kMeasureBlocks = 2 * kRate / kBlock;
    AudioSampleBuffer audio(2, kBlock);
    MidiBuffer midi;
    for (int note : notes)
      midi.addEvent(MidiMessage::noteOn(1, note, .8f), 0);
    WindowedMean mean(kMeasureBlocks * kBlock);
    for (int b = 0; b < kSettleBlocks + kMeasureBlocks; ++b) {
      audio.clear();
      plugin.processBlock(audio, midi);
      midi.clear();
      if (b >= kSettleBlocks)
        for (int i = 0; i < kBlock; ++i)
          mean.add(audio.getSample(0, i), audio.getSample(1, i));
    }
    midi.addEvent(MidiMessage::allNotesOff(1), 0);
    for (int b = 0; b < 2 * kRate / kBlock; ++b) {
      audio.clear();
      plugin.processBlock(audio, midi);
      midi.clear();
    }
    return mean;
  }

  const std::vector<int> kC4 { 60 }, kOctave { 48, 60 }, kChord { 60, 64, 67, 71 };

  void measurePlugin(SynthPlugin& plugin, const std::string& name, bool must_pass) {
    report(name + ", C4", holdNotes(plugin, kC4).db(), must_pass);
    report(name + ", chord", holdNotes(plugin, kChord).db(), must_pass);
  }

  void testPlugin() {
    std::unique_ptr<SynthPlugin> holder;
    auto analogSaw = [](SynthPlugin& plugin) {
      plugin.parameterChanged("osc_1_type", O::kAnalogType);
      plugin.parameterChanged("osc_1_analog_waveform", A::kSaw);
    };
    for (int model : { vital::constants::kAnalog, vital::constants::kLadder }) {
      SynthPlugin& plugin = freshPlugin(holder);
      analogSaw(plugin);
      plugin.parameterChanged("osc_1_destination", vital::constants::kFilter1);
      plugin.parameterChanged("filter_1_on", 1.0f);
      plugin.parameterChanged("filter_1_model", model);
      plugin.parameterChanged("filter_1_resonance", .9f);
      measurePlugin(plugin, model == vital::constants::kAnalog ? "Plugin, Analog filter" : "Plugin, Ladder filter", true);
    }
    for (int waveform : { A::kSaw, A::kSquare }) {
      SynthPlugin& plugin = freshPlugin(holder);
      analogSaw(plugin);
      plugin.parameterChanged("osc_1_analog_waveform", waveform);
      plugin.parameterChanged("osc_1_distortion_type", O::kSync);
      plugin.parameterChanged("osc_1_distortion_amount", .33f);
      measurePlugin(plugin, waveform == A::kSaw ? "Plugin, Analog saw with Hard Sync" :
                                                  "Plugin, Analog square with Hard Sync", true);
    }
  }

  SynthPlugin& pluginWithEffect(std::unique_ptr<SynthPlugin>& holder, int effect) {
    SynthPlugin& plugin = freshPlugin(holder);
    plugin.parameterChanged("fx_slot_1_type", float(effect + 1));
    plugin.parameterChanged("fx_slot_1_enabled", 1.0f);
    return plugin;
  }

  // Effects in a slot on the initial patch, which has no DC of its own. An
  // octave, since a saturated chord's intermodulation lands near 0 Hz.
  void testEffects() {
    namespace C = vital::constants;
    struct Case { const char* name; int effect; std::vector<std::pair<std::string, float>> settings; const std::vector<int>& notes; };
    const Case cases[] = {
      { "Analogue Rotary", C::kAnalogueRotary, {}, kOctave },
      { "Tape, driven", C::kTape, { { "tape_on", 1 }, { "tape_drive_enabled", 1 }, { "tape_drive_amount", 8 } }, kOctave },
      { "Filter FX, Analog driven", C::kFilterFx, { { "filter_fx_model", C::kAnalog }, { "filter_fx_resonance", .9f },
                                                    { "filter_fx_drive", 20 } }, kOctave },
      // Its delay line clips what it writes. Modulation, like the Echo's wow,
      // moves what is left too slowly for the measurement to tell from DC.
      { "Flanger, no modulation", C::kFlanger, { { "flanger_mod_depth", 0 } }, kOctave },
      { "Analogue Echo, no wow", C::kAnalogueEcho, { { "analogueecho_wow", 0 }, { "analogueecho_flutter", 0 } }, kC4 },
    };
    std::unique_ptr<SynthPlugin> holder;
    for (const auto& test : cases) {
      SynthPlugin& plugin = pluginWithEffect(holder, test.effect);
      for (const auto& [name, value] : test.settings)
        plugin.parameterChanged("fx_slot_1_" + name, value);
      report(std::string("Effect slot, ") + test.name, holdNotes(plugin, test.notes).db());
    }
  }

  // Every effect alone with random settings, on the initial patch's chord.
  // Settings that reach the output clamp are left out: clipping makes DC.
  int surveyEffects(int seeds) {
    std::unique_ptr<SynthPlugin> holder;
    for (int effect = 0; effect < vital::constants::kNumEffects; ++effect) {
      if (effect == vital::constants::kRemovedScream) continue;
      const std::string prefix = "fx_slot_1_" + strings::kEffectOrder[effect] + "_";
      double worst = -300.0;
      int worst_seed = -1, clipped = 0;
      for (int seed = 0; seed < seeds; ++seed) {
        SynthPlugin& plugin = pluginWithEffect(holder, effect);
        uint32_t random = 0x9e3779b9u * uint32_t(seed + 1) + uint32_t(effect);
        std::vector<std::pair<std::string, float>> changes;
        for (const auto& control : plugin.getControls()) {
          if (seed == 0 || control.first.rfind(prefix, 0) != 0 || !vital::Parameters::isParameter(control.first))
            continue;
          random = random * 1664525u + 1013904223u;
          const auto& details = vital::Parameters::getDetails(control.first);
          float value = details.min + (details.max - details.min) * float(random >> 8) / float(1 << 24);
          if (details.value_scale == vital::ValueDetails::kIndexed)
            value = std::round(value);
          changes.emplace_back(control.first, value);
        }
        for (const auto& [name, value] : changes)
          plugin.parameterChanged(name, value);
        const WindowedMean mean = holdNotes(plugin, kChord);
        if (mean.peak >= 2.0)
          ++clipped;
        else if (mean.db() > worst) {
          worst = mean.db();
          worst_seed = seed;
        }
      }
      std::cout << strings::kEffectOrder[effect] << ": worst DC " << std::fixed << std::setprecision(1) << worst
                << " dB (seed " << worst_seed << "), " << clipped << " clipped\n";
    }
    return 0;
  }

  int measurePresets(int argc, char** argv) {
    std::unique_ptr<SynthPlugin> holder;
    for (int i = 2; i < argc;) {
      SynthPlugin& plugin = freshPlugin(holder);
      std::string error, name = File(argv[i]).getFileNameWithoutExtension().toStdString();
      if (name != "init")
        expect(plugin.loadFromFile(File(argv[i]), error), std::string("Loads ") + argv[i] + " " + error);
      for (++i; i < argc && String(argv[i]).contains("="); ++i) {
        plugin.parameterChanged(String(argv[i]).upToFirstOccurrenceOf("=", false, false).toStdString(),
                                String(argv[i]).fromFirstOccurrenceOf("=", false, false).getFloatValue());
        name += std::string(" ") + argv[i];
      }
      measurePlugin(plugin, name, false);
    }
    return failures ? 1 : 0;
  }

  int run(int argc, char** argv) {
    juce::ScopedJuceInitialiser_GUI gui;
    if (argc > 2 && std::string(argv[2]) == "effects")
      return surveyEffects(argc > 3 ? std::atoi(argv[3]) : 8);
    if (argc > 2)
      return measurePresets(argc, argv);
    testHardSync();
    testFilters();
    testPlugin();
    testEffects();
    std::cout << "DC offset tests: " << failures << " failures\n";
    return failures ? 1 : 0;
  }
}
