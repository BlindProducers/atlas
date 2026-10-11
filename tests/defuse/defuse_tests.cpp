#include "JuceHeader.h"
#include "defuse_module.h"
#include "reorderable_effect_chain.h"
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <new>

namespace { std::atomic<bool> tracking { false }; std::atomic<size_t> allocations { 0 }; }
void* operator new(std::size_t size) {
  if (tracking) ++allocations;
  if (void* p = std::malloc(size)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t s) { return ::operator new(s); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {
  int failures = 0;
  const char* names[] = { "stages", "frequency", "resonance", "spread", "spread_style", "automation_precision" };
  void expect(bool value, const char* text) { if (!value) { ++failures; std::cerr << "FAIL: " << text << '\n'; } }
  template<class T> void write(std::ostream& f, T v) { f.write(reinterpret_cast<const char*>(&v), sizeof(v)); }
  using Settings = std::array<float, 6>;
  void set(vital::SynthModule& module, const Settings& values, const std::string& prefix = "defuse_") {
    auto controls = module.getControls();
    for (int i = 0; i < 6; ++i) controls.at(prefix + names[i])->set(values[i]);
  }
  void process(vital::SynthModule& module, const vital::poly_float* input, int n) {
    allocations = 0; tracking = true;
    module.processWithInput(input, n);
    tracking = false;
    expect(allocations == 0, "audio processing must not allocate");
  }
}

int main(int argc, char** argv) {
  juce::ScopedJuceInitialiser_GUI juce;
  // Basic physics, state, oversampling and routing checks run without Rust.
  for (int rate : { 44100, 48000, 96000 }) for (int factor : { 1, 2, 4, 8 }) {
    vital::DefuseModule dsp;
    dsp.init(); dsp.setSampleRate(rate); dsp.setOversampleAmount(factor);
    std::array<vital::poly_float, 128> input;
    for (auto& frame : input) frame = vital::poly_float(0.125f);
    process(dsp, input.data(), 128);
    expect(dsp.getLatencySamples() == 0, "Defuse must add no algorithmic latency");
    for (int i = 0; i < 128; ++i) expect(dsp.output()->buffer[i][0] == 0.125f, "default zero stages must be transparent");
    set(dsp, { 40, 1400, .5f, 2, 0, 1 }); dsp.hardReset();
    double energy = 0;
    for (int block = 0; block < 512; ++block) {
      for (auto& frame : input) frame = vital::poly_float(0);
      if (block == 0) input[0].set(0, 1.0f);
      process(dsp, input.data(), 128);
      for (int i = 0; i < 128; ++i) {
        auto frame = dsp.output()->buffer[i];
        energy += double(frame[0]) * frame[0];
        expect(frame[1] == 0 && frame[2] == 0 && frame[3] == 0, "filter state must not leak across SIMD lanes");
      }
    }
    expect(std::abs(energy - 1.0) < 0.002, "all-pass cascade must preserve impulse energy");
    // A structural parameter change clears a ringing cascade immediately.
    set(dsp, { 41, 1400, .5f, 2, 0, 1 });
    process(dsp, input.data(), 128);
    expect(dsp.output()->buffer[0][0] == 0, "changing stages must clear state");
  }
  // The 20 Hz floor must cover manual/host/modulated values and frequencies
  // produced by Spread. Checking only the slider range would leave subsonics.
  for (int style : { 0, 1 }) {
    vital::DefuseModule floor, spread, below;
    for (auto* dsp : { &floor, &spread, &below }) { dsp->init(); dsp->setSampleRate(96000); }
    set(floor, { 1, 20, .5f, 0, float(style), 1 });
    set(spread, { 1, 20, .5f, 5, float(style), 1 });
    set(below, { 1, 1, .5f, 0, float(style), 1 });
    std::array<vital::poly_float, 128> input {};
    for (int block = 0; block < 64; ++block) {
      input[0] = vital::poly_float(block == 0 ? .5f : 0.0f);
      for (auto* dsp : { &floor, &spread, &below }) process(*dsp, input.data(), 128);
      for (int i = 0; i < 128; ++i) {
        expect(floor.output()->buffer[i][0] == spread.output()->buffer[i][0], "Spread must not produce stages below 20 Hz");
        expect(floor.output()->buffer[i][0] == below.output()->buffer[i][0], "host/modulated frequency must stop at 20 Hz in DSP");
      }
    }
  }
  for (int bus = 0; bus <= vital::kNumBuses; ++bus) {
    vital::Value bpm(2), note(60);
    const std::string cp = bus == 0 ? "" : "bus_" + std::to_string(bus) + "_";
    vital::ReorderableEffectChain chain(bpm.output(), note.output(), cp);
    chain.init(); chain.setSampleRate(48000);
    const auto controls = chain.getControls();
    std::array<vital::poly_float, 128> input {};
    input[0] = vital::poly_float(1);
    for (int slot : { 1, 20 }) {
      const std::string sp = cp + "fx_slot_" + std::to_string(slot) + "_";
      controls.at(sp + "type")->set(vital::constants::kDefuse + 1);
      set(chain, { float(slot), float(slot * 700), .5f, .4f, 0, 1 }, sp + "defuse_");
    }
    vital::DefuseModule first, last;
    first.init(); last.init(); first.setSampleRate(48000); last.setSampleRate(48000);
    set(first, { 1, 700, .5f, .4f, 0, 1 }); set(last, { 20, 14000, .5f, .4f, 0, 1 });
    process(first, input.data(), 128); process(last, first.output()->buffer, 128);
    process(chain, input.data(), 128);
    for (int i = 0; i < 128; ++i)
      expect(chain.output()->buffer[i][0] == last.output()->buffer[i][0], "first and last slots on every bus must have independent Defuse DSP");
    controls.at(cp + "fx_slot_1_enabled")->set(0);
    controls.at(cp + "fx_slot_20_type")->set(0);
    process(chain, input.data(), 128);
    for (int i = 0; i < 128; ++i)
      expect(chain.output()->buffer[i][0] == input[i][0], "slot removal and bypass must return dry audio");
  }

  if (argc > 1) {
    const auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("atlas-defuse-reference", "", false);
    dir.createDirectory();
    const auto request = dir.getChildFile("request.bin"), reference = dir.getChildFile("reference.bin");
    std::ofstream file(request.getFullPathName().toStdString(), std::ios::binary);
    std::vector<float> actual;
    std::vector<size_t> case_ends;
    constexpr uint32_t cases = 48;
    write(file, cases);
    for (uint32_t test = 0; test < cases; ++test) {
      const int rate = std::array<int, 4> { 44100, 48000, 96000, 192000 }[test % 4];
      vital::DefuseModule dsp; dsp.init(); dsp.setSampleRate(rate);
      const auto controls = dsp.getControls();
      Settings settings { std::array<float, 6> { 0, 1, 12, 40, 128, 512 }[(test / 4) % 6],
          test >= 24 ? 20.0f : 200.0f, test >= 24 ? 30.0f : .5f, test >= 24 ? -5.0f : 0.0f,
          float((test / 8) % 2), std::array<float, 3> { 1, .5f, 0 }[(test / 4) % 3] };
      write(file, float(rate));
      constexpr uint32_t packets = 260;
      write(file, packets);
      uint32_t rng = 17;
      int cursor = 0;
      for (uint32_t packet = 0; packet < packets; ++packet) {
        const int count = std::array<int, 5> { 1, 17, 63, 64, 127 }[packet % 5];
        const bool reset = packet == 0 || packet == 170;
        if (packet == 16) { settings[1] = 1700; settings[2] = .73f; settings[3] = -2.31f; }
        if (packet == 65) { settings[0] = 13; settings[4] = 1 - settings[4]; settings[1] = 430; }
        if (packet == 100) { settings[0] = 40; settings[4] = 1 - settings[4]; }
        if (packet == 120) { settings[1] = 20000; settings[2] = .01f; settings[3] = 5; }
        write(file, uint32_t(reset)); write(file, uint32_t(count));
        for (int i = 0; i < 6; ++i) { write(file, settings[i]); controls.at(std::string("defuse_") + names[i])->set(settings[i]); }
        write(file, 0.0f); // Diopser reference always processes wet; Atlas owns slot bypass.
        if (reset) dsp.hardReset();
        std::array<vital::poly_float, 128> input {};
        for (int sample = 0; sample < count; ++sample) {
          for (int lane = 0; lane < 2; ++lane) {
            rng = rng * 1664525u + 1013904223u;
            float value = (int(rng >> 8) - 8388608) * (0.05f / 8388608.0f);
            if (cursor == 0 && sample == 0) value = lane == 0 ? .75f : -.25f;
            if (packet >= 175) value = 0;
            input[sample].set(lane, value); input[sample].set(lane + 2, value);
            write(file, value);
          }
        }
        process(dsp, input.data(), count);
        for (int sample = 0; sample < count; ++sample) for (int lane = 0; lane < 2; ++lane) {
          actual.push_back(dsp.output()->buffer[sample][lane]);
          expect(dsp.output()->buffer[sample][lane] == dsp.output()->buffer[sample][lane + 2], "both stereo pairs must match");
        }
        cursor += count;
      }
      case_ends.push_back(actual.size());
    }
    file.close();
    juce::ChildProcess oracle;
    expect(oracle.start(juce::StringArray { argv[1], request.getFullPathName(), reference.getFullPathName() }), "reference must start");
    expect(oracle.waitForProcessToFinish(60000) && oracle.getExitCode() == 0, "reference must finish successfully");
    std::ifstream output(reference.getFullPathName().toStdString(), std::ios::binary);
    double max_error = 0; size_t mismatches = 0, index = 0;
    for (size_t test = 0; test < case_ends.size(); ++test) {
      double case_error = 0;
      for (; index < case_ends[test]; ++index) {
        float expected = 0; output.read(reinterpret_cast<char*>(&expected), sizeof(expected));
        expect(output.good(), "reference must have every output sample");
        const double error = std::abs(double(actual[index]) - expected);
        if (std::memcmp(&actual[index], &expected, sizeof(float)) != 0) ++mismatches;
        expect(std::isfinite(actual[index]) && std::isfinite(expected), "reference and port must stay finite");
        case_error = std::max(case_error, error);
      }
      if (case_error > 0) std::cout << "Reference case " << test << " max error " << case_error << '\n';
      max_error = std::max(max_error, case_error);
    }
    std::cout << "Diopser reference: " << cases << " cases, " << actual.size() << " samples, "
              << mismatches << " unequal, max error " << max_error << '\n';
    expect(mismatches == 0, "Defuse must match the original Rust DSP sample for sample on this platform");
    dir.deleteRecursively();
  }
  std::cout << "Defuse: " << failures << " failures\n";
  return failures ? 1 : 0;
}
