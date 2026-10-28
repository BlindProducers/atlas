// SPDX-License-Identifier: GPL-3.0-or-later
// Filter Table (the Wavetable filter model): factory tables, kernel design and
// the Atlas filter model's convolution exactness, latency, CPU and integration.
#include "JuceHeader.h"
#include "filter_table/filter_table_kernel.h"
#include "filter_module.h"
#include "table_filter.h"
#include "value.h"
#include "synth_constants.h"
#include "synth_plugin.h"
#include "load_save.h"
#include "wavetable_creator.h"
#include "wavetable.h"

#include <chrono>

#include <cmath>
#include <cstdlib>
#include <limits>
#include <complex>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {
  using namespace atlas::filter_table;
  constexpr double kRate = 48000.0;
  int failures = 0;
  void expect(bool condition, const std::string& message) {
    if (!condition) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
  }
  std::string number(double v) { char t[32]; std::snprintf(t, sizeof(t), "%.2f", v); return t; }
  double db(double v) { return 20.0 * std::log10(std::abs(v) + 1e-12); }
  double db(std::complex<double> v) { return db(std::abs(v)); }

  std::complex<double> response(const std::vector<float>& h, int zero, double hz) {
    std::complex<double> acc = 0;
    const double w = -2.0 * M_PI * hz / kRate;
    for (int t = 0; t < int(h.size()); ++t)
      if (h[t] != 0.0f) acc += double(h[t]) * std::polar(1.0, w * (t - zero));
    return acc;
  }

}

namespace {
  std::vector<float> noise(int samples, uint32_t seed = 0x9E3779B9u) {
    std::vector<float> result(samples);
    for (float& v : result) { seed = seed * 1664525u + 1013904223u; v = 0.3f * (float(seed >> 8) / 8388608.0f - 1.0f); }
    return result;
  }

  // A filter module driven like the voice handler does, with the Table model.
  struct Module {
    vital::FilterModule filter;
    vital::cr::Value keytrack;
    vital::Output audio, reset;
    std::string prefix;
    bool mono;
    Module(bool is_mono, int model = vital::constants::kFilterTable, int oversampling = 1) :
        filter(is_mono ? "filter_fx" : "filter_1"), prefix(is_mono ? "filter_fx" : "filter_1"), mono(is_mono) {
      filter.setMono(mono);
      filter.plug(&keytrack, vital::FilterModule::kKeytrack);
      filter.plug(&audio, vital::FilterModule::kAudio);
      filter.plug(&reset, vital::FilterModule::kReset);
      filter.init();
      filter.setSampleRate(int(kRate));
      filter.setOversampleAmount(oversampling);
      audio.ensureBufferSize(vital::kMaxBufferSize * oversampling);
      auto controls = filter.getControls();
      controls.at(prefix + "_on")->set(1);
      controls.at(prefix + "_model")->set(float(model));
      // The synth allocates a Filter FX table filter when its model is selected.
      if (model == vital::constants::kFilterTable) filter.prepareTableFilter();
      controls.at(prefix + "_style")->set(0);
      controls.at(prefix + "_cutoff")->set(83.2f);     // about 1 kHz
      controls.at(prefix + "_resonance")->set(0.5f);
      controls.at(prefix + "_mix")->set(1);
    }
    void set(const char* suffix, float value) { filter.getControls().at(prefix + "_" + suffix)->set(value); }
    // Lets smoothed controls (cutoff) land and the kernel rebuild.
    void settle() { run(noise(int(kRate) / 2), 128); }
    // Processes `signal` in blocks, returning lane 0 of the output.
    std::vector<float> run(const std::vector<float>& signal, int block = 128) {
      std::vector<float> out(signal.size());
      for (int offset = 0; offset < int(signal.size()); offset += block) {
        const int count = std::min(block, int(signal.size()) - offset);
        for (int i = 0; i < count; ++i) audio.buffer[i] = vital::poly_float(signal[offset + i], 0.5f * signal[offset + i], signal[offset + i], 0.0f);
        filter.process(count);
        for (int i = 0; i < count; ++i) out[offset + i] = filter.output()->buffer[i][0];
      }
      return out;
    }
  };


  std::vector<float> convolve(const std::vector<float>& x, const std::vector<float>& k) {
    std::vector<float> y(x.size(), 0.0f);
    for (int n = 0; n < int(x.size()); ++n) {
      double acc = 0;
      for (int m = 0; m < int(k.size()) && m <= n; ++m) acc += double(k[m]) * x[n - m];
      y[n] = float(acc);
    }
    return y;
  }

  double relativeError(const std::vector<float>& a, const std::vector<float>& b, int from = 0) {
    double num = 0, den = 0;
    for (int i = from; i < int(a.size()); ++i) { num += double(a[i] - b[i]) * (a[i] - b[i]); den += double(b[i]) * b[i]; }
    return 20 * std::log10(std::sqrt(num / std::max(den, 1e-30)) + 1e-12);
  }

  void testVoiceModule() {
    Module module(false);
    module.settle();
    auto* table = module.filter.getTableFilter();
    expect(table != nullptr && table->hasKernel(0) && table->hasKernel(1), "the voice module builds a kernel per voice pair");
    expect(table->kernelSize() == 2048, "voice filters use 2048-tap kernels");
    const std::vector<float> kernel = table->installedKernel(0);
    const auto input = noise(int(kRate) / 2);
    const auto output = module.run(input, 97);
    expect(table->installedKernel(0) == kernel, "a settled kernel stays put while the input runs");
    // The convolver carries state from the settling noise; compare after it has drained.
    const auto reference = convolve(input, kernel);
    const double error = relativeError(output, reference, 2048);
    std::cout << "voice convolution vs direct: " << number(error) << " dB\n";
    expect(error < -80, "partitioned convolution must match direct convolution of the installed kernel");
    const auto& p = table->installedParameters(0);
    expect(std::abs(p.cutoff_hz - 1000.0f) < 15.0f && std::abs(p.resonance - 1.0f) < 1e-4f && p.mode == kMinimum,
           "controls reach the kernel: cutoff " + number(p.cutoff_hz) + " Hz, resonance " + number(p.resonance));
    // Voice filters offer Minimum and Raw only.
    module.set("style", 1);
    module.run(noise(2048), 128);
    expect(table->installedParameters(0).mode == kRaw, "voice style 1 is Raw");
    module.set("style", 3);
    module.run(noise(2048), 128);
    expect(table->installedParameters(0).mode == kRaw && table->getLatencySamples() == 0, "voice filters never add latency");
    // The default table is a gentle low-pass: a 10 kHz sine comes out well below a 200 Hz one.
    module.set("style", 0);
    auto level = [&](double hz) {
      std::vector<float> sine(int(kRate) / 4);
      for (int i = 0; i < int(sine.size()); ++i) sine[i] = 0.3f * std::sin(2 * M_PI * hz * i / kRate);
      const auto out = module.run(sine, 128);
      double sum = 0; for (int i = int(out.size()) / 2; i < int(out.size()); ++i) sum += double(out[i]) * out[i];
      return 10 * std::log10(sum / (out.size() / 2) + 1e-30);
    };
    const double low = level(200), high = level(10000);
    std::cout << "default table: 200 Hz " << number(low) << " dB, 10 kHz " << number(high) << " dB\n";
    expect(low - high > 30, "the default table is a low-pass about Cutoff");
  }

  // A note-on on a voice (the reset trigger) clears that voice's convolution
  // state and rebuilds its kernel at once from the new note's cutoff; the other
  // voice of the pair is untouched.
  void testNoteReset() {
    Module module(false);
    auto* table = module.filter.getTableFilter();
    module.settle();
    // Jump the cutoff: the next block boundary rebuilds (smoothed partway), after
    // which the rate limit holds the kernel for 256 samples.
    // (This fixture processes the prototype module, whose processor order is
    // the construction order, so the filter sees the control buffer of the
    // previous block; the voice copies the synth runs are ordered after their
    // controls. The two blocks here let the jump reach the kernel.)
    module.set("cutoff", 60.0f);
    module.run(noise(256), 128);
    const Parameters held = table->installedParameters(0);
    const Parameters other = table->installedParameters(1);
    expect(std::abs(held.cutoff_hz - 1000.0f) > 20.0f, "the cutoff jump reaches the kernel at the block boundary");
    // Note-on on voice 0 (lanes 0 and 1) with silence at the input.
    const vital::poly_mask voice_mask = vital::poly_float::equal(vital::poly_float(1.0f, 1.0f, 0.0f, 0.0f), 1.0f);
    module.reset.trigger(voice_mask, vital::kVoiceOn, 0);
    const auto out = module.run(std::vector<float>(128, 0.0f), 128);
    module.reset.clearTrigger();
    float voice0 = 0, voice1 = 0;
    for (int i = 0; i < 128; ++i) {
      voice0 = std::max(voice0, std::abs(out[i]));
      voice1 = std::max(voice1, std::abs(module.filter.output()->buffer[i][2]));
    }
    expect(voice0 == 0.0f, "a note-on clears the voice's history and tail: silence in gives silence out");
    expect(voice1 > 1e-4f, "the other voice keeps ringing from its history");
    const Parameters fresh = table->installedParameters(0);
    expect(std::abs(std::log(fresh.cutoff_hz / held.cutoff_hz)) > 0.01f && fresh.cutoff_hz < held.cutoff_hz,
           "a note-on rebuilds the voice's kernel at once from the current cutoff ("
           + number(held.cutoff_hz) + " -> " + number(fresh.cutoff_hz) + " Hz)");
    expect(table->installedParameters(1).cutoff_hz == other.cutoff_hz, "the other voice's kernel is untouched");
    // The rebuilt kernel is in use immediately: an impulse at the note-on
    // returns its first block (the direct head; the tail is empty).
    std::vector<float> impulse(128, 0.0f);
    impulse[0] = 1.0f;
    module.reset.trigger(voice_mask, vital::kVoiceOn, 0);
    const auto response = module.run(impulse, 128);
    module.reset.clearTrigger();
    const std::vector<float> kernel = table->installedKernel(0);
    double error_num = 0, error_den = 0;
    for (int i = 0; i < 128; ++i) { error_num += double(response[i] - kernel[i]) * (response[i] - kernel[i]); error_den += double(kernel[i]) * kernel[i]; }
    expect(error_num < 1e-10 * std::max(error_den, 1e-30), "the first block after a note-on already plays the fresh kernel");
    // The rate limit is lifted once after a note-on, so the next boundary can
    // follow the attack; afterwards it holds again.
    module.run(noise(128), 128);
    const float followed = table->installedParameters(0).cutoff_hz;
    expect(std::abs(std::log(followed / fresh.cutoff_hz)) > 0.01f, "the block boundary after a note-on rebuilds without waiting out the rate limit");
    module.run(noise(128), 128);
    expect(table->installedParameters(0).cutoff_hz == followed, "the rate limit holds again after that");
  }

  void testFilterFxModule() {
    Module module(true);
    auto* table = module.filter.getTableFilter();
    expect(table->kernelSize() == 4096, "Filter FX uses 4096-tap kernels");
    for (int style = 0; style < 4; ++style) {
      module.set("style", float(style));
      std::vector<float> impulse(int(kRate) / 4, 0.0f);
      impulse[1000] = 1.0f;
      module.settle();
      module.run(std::vector<float>(8192, 0.0f), 128);   // drain the settling noise
      const auto out = module.run(impulse, 128);
      const int expected_latency = style == kLinear || style == kOriginal ? 2048 : 0;
      expect(module.filter.getLatencySamples() == expected_latency,
             "Filter FX style " + std::to_string(style) + " reports " + std::to_string(expected_latency) + " samples of latency");
      // The impulse response is the installed kernel starting at the impulse;
      // Linear and Original carry their delay inside the kernel.
      const auto& kernel = table->installedKernel(0);
      std::vector<float> expected(out.size(), 0.0f);
      for (int t = 0; t < int(kernel.size()); ++t)
        if (1000 + t < int(expected.size())) expected[1000 + t] = kernel[t];
      const double error = relativeError(out, expected, 0);
      std::cout << "FX style " << style << " impulse response vs kernel: " << number(error) << " dB\n";
      expect(error < -80, "Filter FX impulse response equals the kernel for style " + std::to_string(style));
      expect(table->installedParameters(0).mode == static_cast<PhaseMode>(style), "Filter FX maps all four styles");
      bool mirrored = true;
      for (int i = 0; i < 128; ++i) mirrored = mirrored && module.filter.output()->buffer[i][0] == module.filter.output()->buffer[i][2];
      expect(mirrored, "Filter FX mirrors the stereo pair into lanes 2 and 3");
    }
  }

  void testModulationAndCrossfade() {
    Module module(false);
    auto* table = module.filter.getTableFilter();
    module.settle();
    const Parameters before = table->installedParameters(0);
    // A small cutoff wiggle below the threshold must not rebuild.
    module.set("cutoff", 83.22f);
    module.run(noise(2048), 128);
    expect(table->installedParameters(0).cutoff_hz == before.cutoff_hz, "sub-threshold cutoff changes keep the kernel");
    // A sweep rebuilds at the rate limit, never faster than every 256 samples, without clicks.
    std::vector<float> sine(int(kRate) / 2);
    for (int i = 0; i < int(sine.size()); ++i) sine[i] = 0.3f * std::sin(2 * M_PI * 220.0 * i / kRate);
    std::vector<float> out(sine.size());
    int rebuilds = 0; float last = before.cutoff_hz;
    for (int offset = 0; offset < int(sine.size()); offset += 128) {
      const int count = std::min(128, int(sine.size()) - offset);
      module.set("cutoff", 60.0f + 50.0f * offset / float(sine.size()));
      for (int i = 0; i < count; ++i) module.audio.buffer[i] = sine[offset + i];
      module.filter.process(count);
      for (int i = 0; i < count; ++i) out[offset + i] = module.filter.output()->buffer[i][0];
      if (table->installedParameters(0).cutoff_hz != last) { ++rebuilds; last = table->installedParameters(0).cutoff_hz; }
    }
    std::cout << "sweep: " << rebuilds << " kernel rebuilds over " << sine.size() << " samples\n";
    expect(rebuilds > 20 && rebuilds <= int(sine.size()) / 256 + 1, "cutoff sweeps rebuild at the rate limit");
    float worst_step = 0; bool finite = true;
    for (int i = 1; i < int(out.size()); ++i) { finite = finite && std::isfinite(out[i]); worst_step = std::max(worst_step, std::abs(out[i] - out[i - 1])); }
    expect(finite && worst_step < 0.2f, "a cutoff sweep stays continuous, worst step " + number(worst_step));
  }

  void testCpuBudget() {
    // Eight voices (four modules of two voices) of Filter Table against Formant, times three.
    auto measure = [&](int model) {
      std::vector<std::unique_ptr<Module>> modules;
      for (int i = 0; i < 4; ++i) { modules.push_back(std::make_unique<Module>(false, model)); modules.back()->settle(); }
      const auto input = noise(int(kRate) * 5);
      double best = 1e9;
      for (int run = 0; run < 3; ++run) {
        const auto start = std::chrono::steady_clock::now();
        for (auto& m : modules) m->run(input, 128);
        best = std::min(best, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
      }
      return best;
    };
    const double table_ms = measure(vital::constants::kFilterTable);
    const double formant_ms = measure(vital::constants::kFormant);
    std::cout << "8 voices, 5 s: Filter Table " << number(table_ms) << " ms, Formant " << number(formant_ms) << " ms\n";
    expect(table_ms <= 3.0 * formant_ms, "Filter Table costs at most three Formant filters per voice");
  }
}

namespace {
  // Two-frame table: a square and a saw, as a filter table for Filter 1.
  void loadTestTable(WavetableCreator& creator, const char* name) {
    std::vector<float> audio(2 * 2048);
    for (int i = 0; i < 2048; ++i) {
      audio[i] = i < 1024 ? 0.8f : -0.8f;
      audio[2048 + i] = 0.8f - 1.6f * i / 2048.0f;
    }
    creator.initFromAudioFile(audio.data(), int(audio.size()), 48000, WavetableCreator::kWavetableSplice,
                              FileSource::kNoInterpolate);
    creator.setName(name);
    creator.render();
  }

  void testPresetsCarryTables() {
    SynthPlugin synth;
    synth.getControls().at("filter_1_model")->set(float(vital::constants::kFilterTable));
    expect(synth.getFilterTableCreator("filter_1", false) == nullptr, "no table exists until one is loaded");
    WavetableCreator* creator = synth.getFilterTableCreator("filter_1", true);
    expect(creator != nullptr, "a filter table creator is created on demand");
    loadTestTable(*creator, "Square Saw");
    // The engine's filter reads the table.
    expect(synth.getEngine() != nullptr, "engine exists");
    const json state = LoadSave::stateToJson(&synth, synth.getCriticalSection());
    const json& settings = state["settings"];
    expect(settings.count("filter_tables") && settings["filter_tables"].count("filter_1"),
           "presets store the filter's table under filter_tables");
    expect(!settings["filter_tables"].count("filter_2"), "filters without a table store nothing");

    SynthPlugin other;
    std::map<std::string, String> save_info;
    LoadSave::jsonToState(&other, save_info, state);
    WavetableCreator* loaded = other.getFilterTableCreator("filter_1", false);
    expect(loaded != nullptr && loaded->getName() == "Square Saw", "the table comes back with the preset");
    expect(other.getFilterTableCreator("filter_2", false) == nullptr, "no stray tables are created");
    // Loading a preset without tables detaches existing ones.
    SynthPlugin plain;
    const json plain_state = LoadSave::stateToJson(&plain, plain.getCriticalSection());
    LoadSave::jsonToState(&other, save_info, plain_state);
    expect(other.getFilterTableCreator("filter_1", false) == nullptr, "a preset without tables clears the filter's table");
    // Init patch clears too.
    synth.initEngine();
    expect(synth.getFilterTableCreator("filter_1", false) == nullptr, "a new patch starts without filter tables");

    // The voice filter actually reads the table: with the square/saw table and a
    // flat-spectrum frame (saw, frame 256) the filter is flat; with the default
    // low-pass it is not.
  }

  void testFxBusPresetsCarryTables() {
    SynthPlugin synth;
    synth.parameterChanged("bus_1_fx_slot_1_type", float(vital::constants::kFilterFx + 1));
    synth.parameterChanged("bus_1_fx_slot_1_filter_fx_model", float(vital::constants::kFilterTable));
    WavetableCreator* creator = synth.getFilterTableCreator("bus_1_fx_slot_1_filter_fx", true);
    loadTestTable(*creator, "Bus Table");
    const json preset = synth.fxBusPreset(1);
    expect(preset["slots"][0].count("filter_table") && preset["slots"][0]["filter_table"].is_object(),
           "FX bus presets carry a Filter FX slot's table");
    String error;
    expect(synth.loadFxBusPreset(2, preset, error), "the bus preset loads into another bus: " + error.toStdString());
    WavetableCreator* copied = synth.getFilterTableCreator("bus_2_fx_slot_1_filter_fx", false);
    expect(copied != nullptr && copied->getName() == "Bus Table", "the table lands in the destination slot");
    // The table's JSON is re-serialised from the loaded creator (its audio
    // source is kept, so the rendering is identical); compare everything else.
    json again = synth.fxBusPreset(2), original = preset;
    const json again_table = again["slots"][0]["filter_table"];
    again["slots"][0].erase("filter_table"); original["slots"][0].erase("filter_table");
    expect(again == original, "the destination bus round-trips the same slot settings");
    expect(again_table.is_object() && again_table["name"] == "Bus Table", "the destination bus keeps the table");
    synth.clearFxBus(2);
    expect(synth.getFilterTableCreator("bus_2_fx_slot_1_filter_fx", false) == nullptr, "clearing a bus drops its tables");
  }
}

namespace {
  // Every factory table imports through Atlas's wavetable loader and filters
  // as designed: "Lowpass Slopes" frame 0 is flat below Cutoff and falls above.
  void testFactoryTables() {
    const juce::File folder(ATLAS_FILTER_TABLE_FACTORY);
    juce::Array<juce::File> files = folder.findChildFiles(juce::File::findFiles, false, "*.wav");
    expect(files.size() >= 10, "the factory folder holds the generated filter tables");
    juce::WavAudioFormat format;
    for (const juce::File& file : files) {
      std::unique_ptr<juce::AudioFormatReader> reader(format.createReaderFor(file.createInputStream().release(), true));
      expect(reader != nullptr && reader->lengthInSamples == 256 * 2048, file.getFileName().toStdString() + " is 256 frames of 2048 samples");
      if (reader == nullptr) continue;
      juce::AudioBuffer<float> audio(1, int(reader->lengthInSamples));
      reader->read(&audio, 0, int(reader->lengthInSamples), 0, true, false);
      vital::Wavetable table(vital::kNumOscillatorWaveFrames);
      WavetableCreator creator(&table);
      creator.init();
      creator.initFromAudioFile(audio.getReadPointer(0), audio.getNumSamples(), int(reader->sampleRate),
                                WavetableCreator::kWavetableSplice, FileSource::kNoInterpolate);
      creator.render();
      expect(table.numFrames() >= 128, "the table renders its frames");
      if (file.getFileNameWithoutExtension() == "Lowpass Slopes") {
        Module module(false);
        module.filter.setTableWavetable(&table);
        module.set("table_frame", 0.0f);
        module.settle();
        auto level = [&](double hz) {
          std::vector<float> sine(int(kRate) / 4);
          for (int i = 0; i < int(sine.size()); ++i) sine[i] = 0.3f * std::sin(2 * M_PI * hz * i / kRate);
          const auto out = module.run(sine, 128);
          double sum = 0; for (int i = int(out.size()) / 2; i < int(out.size()); ++i) sum += double(out[i]) * out[i];
          return 10 * std::log10(sum / (out.size() / 2) + 1e-30);
        };
        const double low = level(200), mid = level(600), high = level(8000);
        std::cout << "Lowpass Slopes frame 0: 200 Hz " << number(low) << " dB, 600 Hz " << number(mid) << " dB, 8 kHz " << number(high) << " dB\n";
        expect(std::abs(low - mid) < 1.0, "the first frame is flat below Cutoff");
        expect(low - high > 15.0 && low - high < 21.0, "the first frame falls about 6 dB per octave above Cutoff (3 octaves)");
        module.set("table_frame", 256.0f);
        module.settle();
        const double steep_high = level(8000), steep_low = level(200);
        std::cout << "Lowpass Slopes frame 256: 200 Hz " << number(steep_low) << " dB, 8 kHz " << number(steep_high) << " dB\n";
        expect(steep_low - steep_high > 60.0, "the last frame is a steep low-pass");
      }
    }
  }
}

int main() {
  juce::ScopedJuceInitialiser_GUI juce;
  testFactoryTables();
  testVoiceModule();
  testNoteReset();
  testFilterFxModule();
  testModulationAndCrossfade();
  testCpuBudget();
  testPresetsCarryTables();
  testFxBusPresetsCarryTables();
  if (failures) return 1;
  std::cout << "Filter Table: factory tables, module convolution, latency, modulation and CPU checks passed.\n";
  return 0;
}
