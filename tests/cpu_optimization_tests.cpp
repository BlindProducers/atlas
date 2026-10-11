#include "JuceHeader.h"
#include "tape_module.h"
#include "granular_source.h"
#include "spectral_compressor_module.h"
#include "spectral_balance_module.h"
#include "reorderable_effect_chain.h"
#include "synth_plugin.h"
#include "memory.h"

#include <chrono>
#include <fstream>
#include <iostream>

namespace {
using Signal = std::vector<vital::poly_float>;
int failures = 0;
void expect(bool ok, const char* message) {
  if (!ok) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}

Signal signal(int count) {
  Signal result(count);
  for (int i = 0; i < count; ++i)
    for (int lane = 0; lane < vital::poly_float::kSize; ++lane)
      result[i].set(lane, .25f * std::sin(.071f * i * (lane + 1)));
  return result;
}

// Optional captures support comparing the same deterministic renders before
// and after a DSP optimization, without platform-specific golden audio files.
void record(const std::string& name, const Signal& result, const juce::File& folder) {
  double energy = 0;
  for (auto frame : result) for (int lane = 0; lane < vital::poly_float::kSize; ++lane) {
    expect(std::isfinite(frame[lane]), "rendered samples must be finite");
    energy += double(frame[lane]) * frame[lane];
  }
  expect(energy > 0, "audible fixture must produce audio");
  if (folder != juce::File()) {
    folder.createDirectory();
    std::ofstream file(folder.getChildFile(name + ".f32").getFullPathName().toStdString(), std::ios::binary);
    file.write(reinterpret_cast<const char*>(result.data()), result.size() * sizeof(vital::poly_float));
    expect(file.good(), "audio capture must be written");
  }
}

template<class Function> void measured(const std::string& name, Function function) {
  auto start = std::chrono::steady_clock::now();
  function();
  std::cout << name << " ms=" << std::chrono::duration<double, std::milli>(
      std::chrono::steady_clock::now() - start).count() << '\n';
}

void tape(std::shared_ptr<vital::TapeNoiseLibrary> library, int rate, bool automate,
          const juce::File& folder) {
  vital::TapeModule module("", library);
  module.init(); module.setSampleRate(rate);
  auto controls = module.getControls();
  auto set = [&](const char* id, float value) { controls.at(std::string("tape_") + id)->set(value); };
  set("drive_enabled", 1); set("motion_enabled", 1); set("noise_enabled", 1); set("noise_source", 1);
  set("dropouts", .7f); set("dropout_density", 1); set("noise_level", -25);
  auto input = signal(96000);
  Signal result(input.size());
  measured("tape_" + std::to_string(rate) + (automate ? "_automated" : "_steady"), [&] {
    for (int offset = 0, block = 0; offset < int(input.size()); ++block) {
      int count = std::min(block % 2 ? 127 : 17, int(input.size()) - offset);
      if (automate && block % 97 == 0) {
        const bool odd = (block / 97) % 2;
        set("drive_amount", odd ? 8 : 2); set("drive_output", odd ? -5 : 0);
        set("drive_tone", odd ? .8f : .2f); set("noise_tone", odd ? 80 : -70);
        set("wow", odd ? .7f : .1f); set("flutter_rate", odd ? 12 : 3);
        set("noise_pitch", odd ? 7 : -12); set("noise_source", odd ? 2 : 1);
        set("global_hp_enabled", odd); set("noise_lp_enabled", odd);
        set("motion_enabled", odd); set("drive_enabled", !odd);
      }
      module.processWithInput(input.data() + offset, count);
      std::copy_n(module.output()->buffer, count, result.data() + offset);
      offset += count;
    }
  });
  record("tape_" + std::to_string(rate) + (automate ? "_automated" : "_steady"), result, folder);
}

void granular(int maximum, bool automate, const juce::File& folder) {
  using G = vital::GranularSource;
  G source;
  std::array<vital::Value, G::kNumInputs> controls;
  for (int i = 0; i < G::kNumInputs; ++i) source.plug(&controls[i], i);
  vital::Output reset;
  source.plug(&reset, G::kReset); source.setSampleRate(48000); source.setSeed(0x41544c41);
  controls[G::kMidi].set(60); controls[G::kRootKey].set(60); controls[G::kLevel].set(1);
  controls[G::kEnd].set(1); controls[G::kMode].set(1); controls[G::kTransposeQuantizeScale].set(14);
  controls[G::kGrainCount].set(maximum); controls[G::kDensity].set(maximum == 64 ? 1500 : 80);
  controls[G::kGrainSize].set(80); controls[G::kSpeed].set(1); controls[G::kPosition].set(.35f);
  controls[G::kPositionModRate].set(3.25f); controls[G::kPositionMod].set(.7f);
  controls[G::kRandomPosition].set(.8f); controls[G::kRandomVolume].set(.55f);
  controls[G::kRandomPan].set(.65f); controls[G::kRandomPitch].set(7);
  controls[G::kInterval].set(12); controls[G::kIntervalChance].set(.4f); controls[G::kDirection].set(2);
  controls[G::kLowCutoff].set(8); controls[G::kHighCutoff].set(136);
  std::vector<float> audio(8192);
  for (int i = 0; i < int(audio.size()); ++i) audio[i] = .7f * std::sin(.1f * i);
  source.getSample()->loadSample(audio.data(), int(audio.size()), 48000);
  reset.trigger(vital::poly_mask(-1), vital::poly_float(vital::kVoiceOn), vital::poly_int(0));
  Signal result(48000);
  const auto name = "granular_" + std::to_string(maximum) + (automate ? "_automated" : "_steady");
  measured(name, [&] {
    for (int offset = 0, block = 0; offset < int(result.size()); ++block) {
      int count = std::min(block % 2 ? 127 : 17, int(result.size()) - offset);
      if (automate && block % 97 == 0) {
        const bool odd = (block / 97) % 2;
        controls[G::kGrainCount].set(odd ? 1 : maximum);
        controls[G::kMidiDensity].set(odd); controls[G::kMode].set(odd);
        controls[G::kDirection].set((block / 97) % 3);
        // Replacing a sample must invalidate any cached reading pointers.
        source.getSample()->loadSample(audio.data(), odd ? 4096 : 8192, odd ? 44100 : 48000);
      }
      if (automate && block == 301)
        reset.trigger(vital::poly_mask(-1), vital::poly_float(vital::kVoiceOn), vital::poly_int(0));
      source.process(count);
      std::copy_n(source.output(G::kRaw)->buffer, count, result.data() + offset);
      reset.clearTrigger(); offset += count;
    }
  });
  record(name, result, folder);
}

void effects(const juce::File& folder) {
  vital::cr::Value bps(2), key(60);
  vital::ReorderableEffectChain chain(bps.output(), key.output());
  chain.init(); chain.setSampleRate(48000);
  const auto controls = chain.getControls();
  auto input = signal(48000);
  Signal result(input.size());
  measured("effect_switching", [&] {
    for (int offset = 0, block = 0; offset < int(input.size()); ++block) {
      const int count = std::min(127, int(input.size()) - offset);
      if (block % 40 == 0) {
        const int types[] = { vital::constants::kFlanger, vital::constants::kChorus,
                              vital::constants::kTremoloPan, vital::constants::kPhaser };
        controls.at("fx_slot_1_type")->set(types[(block / 40) % 4] + 1);
        controls.at("fx_slot_1_enabled")->set((block / 40) % 3 != 1);
      }
      chain.correctToTime(19.5 + offset / 48000.0);
      chain.processWithInput(input.data() + offset, count);
      std::copy_n(chain.output()->buffer, count, result.data() + offset);
      offset += count;
    }
  });
  record("effect_switching", result, folder);
  controls.at("fx_slot_1_enabled")->set(0);
  measured("empty_chain", [&] {
    for (int block = 0; block < 20000; ++block) {
      chain.correctToTime(block * 128.0 / 48000);
      chain.processWithInput(input.data(), 128);
    }
  });
  expect(std::memcmp(chain.output()->buffer, input.data(), 128 * sizeof(vital::poly_float)) == 0,
         "empty chain must remain bit-transparent");
}

void spectral(const juce::File& folder) {
  vital::SpectralCompressorModule module;
  module.init(); module.setSampleRate(48000);
  auto input = signal(192000);
  Signal result(input.size());
  measured("spectral_compressor", [&] {
    for (int offset = 0; offset < int(input.size()); offset += 128) {
      module.processWithInput(input.data() + offset, 128);
      std::copy_n(module.output()->buffer, 128, result.data() + offset);
    }
  });
  record("spectral_compressor", result, folder);
  vital::SpectralBalanceModule balance;
  balance.init(); balance.setSampleRate(48000);
  balance.getControls().at("spectral_balance_amount")->set(1.0f);
  measured("spectral_balance", [&] {
    for (int offset = 0; offset < int(input.size()); offset += 128) {
      balance.processWithInput(input.data() + offset, 128);
      std::copy_n(balance.output()->buffer, 128, result.data() + offset);
    }
  });
  record("spectral_balance", result, folder);
}

void audioHistory() {
  SynthPlugin plugin;
  plugin.prepareToPlay(48000, 128);
  juce::AudioBuffer<float> buffer(2, 128);
  juce::MidiBuffer midi;
  const auto* memory = plugin.getAudioMemory();
  const auto initial = memory->getOffset();
  plugin.processBlock(buffer, midi);
  expect(memory->getOffset() == initial, "accessible plugin must not collect unused audio history");
  plugin.setAudioHistoryEnabled(true);
  plugin.processBlock(buffer, midi);
  expect(memory->getOffset() != initial, "explicit audio-history consumer must receive fresh samples");
  const auto captured = memory->getOffset();
  plugin.setAudioHistoryEnabled(false);
  plugin.processBlock(buffer, midi);
  expect(memory->getOffset() == captured, "audio-history collection must stop when its consumer leaves");
}
}

int main(int argc, char** argv) {
  juce::ScopedJuceInitialiser_GUI juce;
  if (argc < 2) return 2;
  auto library = std::make_shared<vital::TapeNoiseLibrary>(juce::File(argv[1]));
  for (int clip : { 1, 2 }) {
    for (int attempt = 0; attempt < 500 && !library->request(clip); ++attempt) juce::Thread::sleep(10);
    expect(library->request(clip) != nullptr, "Tape fixture noise must load");
  }
  if (failures) return 1;
  const juce::File folder = argc > 2 ? juce::File(argv[2]) : juce::File();
  for (int rate : { 48000, 96000 }) for (bool automate : { false, true }) tape(library, rate, automate, folder);
  for (int grains : { 4, 64 }) for (bool automate : { false, true }) granular(grains, automate, folder);
  effects(folder); spectral(folder); audioHistory();
  std::cout << "CPU optimization render checks: " << failures << " failures\n";
  return failures ? 1 : 0;
}
