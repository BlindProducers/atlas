#include "JuceHeader.h"
#include "scream_module.h"
#include "reorderable_effect_chain.h"
#ifndef JucePlugin_Name
#define JucePlugin_Name "BP Scream"
#endif
#include "../third_party/scream/Source/PluginProcessor.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <new>

namespace {
  // Per thread: the effect chain's preparation worker allocates by design,
  // off the audio thread, and must not count against processing.
  thread_local std::atomic<bool> tracking { false };
  thread_local std::atomic<size_t> allocations { 0 };
}
void* operator new(std::size_t size) {
  if (tracking.load()) ++allocations;
  if (void* p = std::malloc(size)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {
  int failures = 0;
  void expect(bool ok, const char* message) {
    if (!ok) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
  }
  using Parameters = std::array<float, 7>;
  constexpr const char* atlas_ids[] = { "cutoff", "scream", "resonance", "input_gain", "wet",
                                       "output_gain", "gate_release" };
  constexpr const char* source_ids[] = { "cutoff", "scream", "resonance", "inputGain", "wet",
                                        "outputGain", "gateRelease" };

  bool compare(vital::SynthModule& module, vital::Value& midi_note, int rate, int factor,
               int block, Parameters p, bool changes, const std::string& prefix = "") {
    std::array<BPScreamProcessor, vital::poly_float::kSize / 2> source;
    for (auto& processor : source) processor.prepareToPlay(rate * factor, block);
    juce::AudioBuffer<float> reference(2, block);
    std::vector<vital::poly_float> input(block);
    uint32_t random = 0xabcdef;
    auto next = [&] { random = random * 1664525u + 1013904223u; return float(random >> 8) / 16777216.0f; };
    const auto controls = module.getControls();
    bool autogain = true;
    bool keytracking = false;
    midi_note.set(-1.0f);
    int note = -1;
    for (int iteration = 0; iteration < 180; ++iteration) {
      juce::MidiBuffer midi;
      if (changes && iteration % 19 == 18) {
        for (int i = 0; i < 6; ++i) p[i] = next();
        p[6] = 5.0f + 495.0f * next();
        autogain = !autogain;
        keytracking = !keytracking;
        note = 12 + int(next() * 85);
        midi_note.set(float(note));
        midi.addEvent(juce::MidiMessage::noteOn(1, note, 0.8f), 0);
      }
      if (iteration % 19 == 0 && note >= 0)
        midi.addEvent(juce::MidiMessage::noteOff(1, note), 0);

      for (int i = 0; i < 7; ++i)
        controls.at(prefix + "scream_" + atlas_ids[i])->set(p[i]);
      controls.at(prefix + "scream_autogain")->set(autogain ? 1.0f : 0.0f);
      controls.at(prefix + "scream_keytracking")->set(keytracking ? 1.0f : 0.0f);
      for (int i = 0; i < block; ++i) {
        for (int lane = 0; lane < vital::poly_float::kSize; ++lane) {
          // Silence, asymmetric stereo transients, bass/sweeps and overdrive.
          float x = 0.4f * (next() - 0.5f) + 0.6f * std::sin((iteration * block + i) * (lane + 1) * 0.013f);
          if (iteration % 47 >= 40) x = 0;
          else if (iteration % 31 >= 27) x *= 0.001f;
          else if (iteration % 17 == 3) x *= 3.0f;
          input[i].set(lane, x);
        }
      }

      allocations.store(0); tracking.store(true);
      module.processWithInput(input.data(), block);
      tracking.store(false);
      expect(allocations.load() == 0, "processing/parameter changes must not allocate");
      for (int pair = 0; pair < vital::poly_float::kSize / 2; ++pair) {
        auto& processor = source[pair];
        for (int i = 0; i < 7; ++i)
          processor.getAPVTS().getRawParameterValue(source_ids[i])->store(p[i]);
        processor.autogainOn.store(autogain);
        processor.midiKeytracking.store(keytracking);
        for (int ch = 0; ch < 2; ++ch)
          for (int i = 0; i < block; ++i)
            reference.setSample(ch, i, input[i][pair * 2 + ch]);
        processor.processBlock(reference, midi);
        for (int ch = 0; ch < 2; ++ch) {
          for (int i = 0; i < block; ++i) {
            float actual = module.output()->buffer[i][pair * 2 + ch];
            float expected = reference.getSample(ch, i);
            if (!std::isfinite(actual) || std::memcmp(&actual, &expected, sizeof(float)) != 0) {
              std::cerr << "Mismatch rate=" << rate * factor << " block=" << block << " iteration="
                        << iteration << " lane=" << pair * 2 + ch << " sample=" << i << " Atlas="
                        << actual << " source=" << expected << '\n';
              return false;
            }
          }
        }
      }
    }
    return true;
  }
}

int main() {
  juce::ScopedJuceInitialiser_GUI juce;
  const Parameters defaults { 1.0f, 0.5f, 0.5f, 0.75f, 1.0f, 0.75f, 30.0f };
  for (int rate : { 44100, 48000, 96000 }) {
    for (int block : { 1, 7, 31, 64, 128 }) {
      vital::Value note(-1.0f);
      vital::ScreamModule module("", note.output());
      module.init(); module.setSampleRate(rate);
      expect(compare(module, note, rate, 1, block, defaults, true), "bit-exact BP Scream processor parity");
      module.hardReset();
      Parameters extreme { 0.0f, 1.0f, 1.0f, 1.0f, 0.0f, 1.0f, 500.0f };
      expect(compare(module, note, rate, 1, block, extreme, false), "reset/dry/extreme source parity");
      module.enable(false); module.enable(true);
      expect(compare(module, note, rate, 1, block, defaults, false), "bypass/re-enable resets source state");
    }
  }
  for (int factor : { 2, 4, 8 }) {
    vital::Value note(-1.0f);
    vital::ScreamModule module("", note.output());
    module.init(); module.setSampleRate(48000); module.setOversampleAmount(factor);
    expect(compare(module, note, 48000, factor, 128 * factor, defaults, true),
           "oversampled blocks preserve original DSP at the chain sample rate");
  }
  {
    vital::Value tempo(2.0f), keytrack(0.0f), note(-1.0f);
    vital::ReorderableEffectChain chain(tempo.output(), keytrack.output(), "bus_3_", nullptr, note.output());
    chain.init(); chain.setSampleRate(48000);
    const std::string prefix = "bus_3_fx_slot_20_";
    chain.getControls().at(prefix + "type")->set(vital::constants::kScream + 1);
    chain.getControls().at(prefix + "enabled")->set(1.0f);
    chain.prepareSelectedEffects(); // As the plugin does before playback.
    expect(compare(chain, note, 48000, 1, 128, defaults, true, prefix), "last bus slot routing and keytracking parity");
  }
  if (failures) return 1;
  std::cout << "Scream: bit-exact full BP Scream processor parity, all controls, keytracking, stereo, "
               "rates, blocks, oversampling, reset, bypass and allocation checks passed.\n";
}
