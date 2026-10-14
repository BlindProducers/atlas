#include "JuceHeader.h"
#include "slot_test_ids.h"
#include "synth_plugin.h"
#include "sound_engine.h"
#include "sample_source.h"
#include "latency_compensated_sum.h"
#include "spectral_gate_module.h"

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <new>

namespace {
  std::atomic<bool> tracking { false };
  std::atomic<size_t> allocations { 0 };
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
  void expect(bool condition, const std::string& message) {
    if (!condition) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
  }

  void testDelayStorage() {
    vital::LatencyCompensatedSum sum(2, 8192);
    vital::Output first, second;
    sum.plug(&first, 0); sum.plug(&second, 1);
    for (int delay : { 0, 1, 17, 1024, 8192, 127, 0 }) {
      const int latencies[] = { 0, delay };
      allocations.store(0); tracking.store(true);
      const int reported = sum.setInputLatencies(latencies);
      sum.hardReset();
      bool identical = true;
      for (int offset = 0; offset < delay + 300;) {
        const int count = std::min(127, delay + 300 - offset);
        for (int i = 0; i < count; ++i) {
          first.buffer[i] = offset + i == 13 ? vital::poly_float(.1f, .2f, .3f, .4f) : 0.0f;
          second.buffer[i] = offset + i == 13 + delay ? vital::poly_float(.4f, .3f, .2f, .1f) : 0.0f;
        }
        sum.process(count);
        for (int i = 0; i < count; ++i)
          for (int lane = 0; lane < vital::poly_float::kSize; ++lane)
            identical &= sum.output()->buffer[i][lane] == (offset + i == 13 + delay ? .5f : 0.0f);
        offset += count;
      }
      tracking.store(false);
      expect(reported == delay && identical, "parallel impulses align exactly, including ring wrap and all SIMD lanes");
      expect(allocations.load() == 0, "delay changes, resets and rendering must not allocate");
    }
    // Disconnect a path with queued audio: it must not leak into its old destination.
    const int connected[] = { 0, 1024 }, disconnected[] = { -1, 1024 };
    sum.setInputLatencies(connected);
    first.buffer[0] = 1.0f; second.buffer[0] = 0.0f; sum.process(1);
    sum.setInputLatencies(disconnected);
    first.buffer[0] = 0.0f;
    for (int i = 0; i < 2048; ++i) {
      sum.process(1);
      expect(sum.output()->buffer[0][0] == 0, "disconnected route must discard queued compensation audio");
    }
  }

  // The engine's sums give a line history only once it needs a delay: until
  // then it passes its input through undelayed, and asks for it.
  void testHistoryOnDemand() {
    vital::LatencyCompensatedSum sum(2, 8192, false);
    vital::Output first, second;
    sum.plug(&first, 0); sum.plug(&second, 1);
    const int aligned[] = { 0, 0 }, delayed[] = { 0, 300 };
    sum.setInputLatencies(aligned);
    expect(!sum.needsHistory(), "aligned paths need no compensation history");
    allocations.store(0); tracking.store(true);
    const int reported = sum.setInputLatencies(delayed);
    first.buffer[0] = 1.0f; second.buffer[0] = 0.0f;
    sum.process(1);
    tracking.store(false);
    expect(allocations.load() == 0, "a line meeting its first delay allocates nothing on the audio thread");
    expect(reported == 300 && sum.needsHistory(), "a line asks for history the first time it needs a delay");
    expect(sum.output()->buffer[0][0] == 1.0f, "a line without history passes its input through");
    sum.reserveNeededHistory();
    expect(!sum.needsHistory(), "reserving gives the line its history");
    sum.setInputLatencies(delayed);
    sum.hardReset();
    bool identical = true;
    for (int offset = 0; offset < 600;) {
      const int count = std::min(127, 600 - offset);
      for (int i = 0; i < count; ++i) {
        first.buffer[i] = offset + i == 13 ? vital::poly_float(.1f, .2f, .3f, .4f) : 0.0f;
        second.buffer[i] = offset + i == 313 ? vital::poly_float(.4f, .3f, .2f, .1f) : 0.0f;
      }
      sum.process(count);
      for (int i = 0; i < count; ++i)
        for (int lane = 0; lane < vital::poly_float::kSize; ++lane)
          identical &= sum.output()->buffer[i][lane] == (offset + i == 313 ? .5f : 0.0f);
      offset += count;
    }
    expect(identical, "a line given its history aligns parallel impulses exactly");
  }

  void set(SynthPlugin& plugin, const std::string& name, float value) {
    plugin.getControls().at(name)->set(value);
  }
  void slot(SynthPlugin& plugin, const std::string& chain, int index, int type, bool enabled = true) {
    const std::string prefix = chain + "fx_slot_" + std::to_string(index) + "_";
    set(plugin, prefix + "type", type + 1);
    set(plugin, prefix + "enabled", enabled);
    set(plugin, prefix + "spectral_compressor_mix", 0);
    set(plugin, prefix + "spectral_lock_mix", 0);
    set(plugin, prefix + "resonator_mix", 0);
    set(plugin, prefix + "suppressor_amount", 0);
    set(plugin, prefix + "spectral_balance_mix", 0);
  }
  void clearSlots(SynthPlugin& plugin) {
    for (int chain = 0; chain < 4; ++chain) {
      const auto prefix = chain == 0 ? "" : "bus_" + std::to_string(chain) + "_";
      for (int i = 1; i <= 20; ++i)
        set(plugin, prefix + "fx_slot_" + std::to_string(i) + "_type", 0);
    }
  }
  void prepare(SynthPlugin& plugin, int rate, int quality) {
    set(plugin, "oversampling", quality);
    plugin.setPlayConfigDetails(0, 2, rate, 127);
    plugin.prepareToPlay(rate, 127);
  }
  void tick(SynthPlugin& plugin, int count = 127) {
    juce::AudioBuffer<float> buffer(2, count);
    juce::MidiBuffer midi;
    buffer.clear();
    plugin.processBlock(buffer, midi);
  }
  struct HostListener : juce::AudioProcessorListener {
    void audioProcessorParameterChanged(juce::AudioProcessor*, int, float) override { }
    void audioProcessorChanged(juce::AudioProcessor*, const ChangeDetails& details) override {
      if (details.latencyChanged) ++changes;
    }
    int changes = 0;
  };

  void testHostReporting() {
    SynthPlugin plugin;
    HostListener host;
    plugin.addListener(&host);
    for (int rate : { 44100, 48000, 96000, 192000 }) {
      for (int quality = 0; quality < 4; ++quality) {
        clearSlots(plugin);
        prepare(plugin, rate, quality);
        expect(plugin.getLatencySamples() == 0, "empty plugin reports zero spectral latency");
        int factor = 1 << quality;
        for (int multiple = rate / 44100; multiple > 1 && factor > 1; multiple >>= 1)
          factor >>= 1;
        for (const std::string chain : { "", "bus_1_", "bus_2_", "bus_3_" }) {
          slot(plugin, chain, 1, vital::constants::kSpectralLock);
          slot(plugin, chain, 20, vital::constants::kSpectralLock);
        }
        prepare(plugin, rate, quality);
        tick(plugin);
        expect(plugin.getLatencySamples() == 0, "multiple Spectral Locks on every bus report zero latency");
        clearSlots(plugin);
        slot(plugin, "", 1, vital::constants::kSuppressor);
        prepare(plugin, rate, quality);
        expect(plugin.getLatencySamples() == 2048 / factor, "Suppressor latency uses effective host rate");
        slot(plugin, "", 20, vital::constants::kSuppressor); tick(plugin);
        expect(plugin.getLatencySamples() == 4096 / factor, "serial Suppressors add their latency");
        set(plugin, "fx_slot_1_enabled", 0); tick(plugin);
        expect(plugin.getLatencySamples() == 2048 / factor, "Suppressor bypass removes its delay");
        clearSlots(plugin);
        slot(plugin, "", 1, vital::constants::kSpectralCompressor);
        prepare(plugin, rate, quality);
        expect(plugin.getLatencySamples() == 1024 / factor,
               "prepare reports compressor latency in host samples at rate " + std::to_string(rate));
        slot(plugin, "", 2, vital::constants::kSpectralLock);
        slot(plugin, "bus_1_", 20, vital::constants::kSpectralLock);
        tick(plugin);
        expect(plugin.getLatencySamples() == 1024 / factor,
               "Spectral Lock preserves another effect's host latency and parallel compensation");
        const int previous = host.changes;
        tick(plugin); tick(plugin, 17); tick(plugin, 0);
        expect(host.changes == previous, "unchanged rendering must not repeatedly notify the host");
        slot(plugin, "", 20, vital::constants::kSpectralCompressor);
        tick(plugin);
        expect(plugin.getLatencySamples() == 2048 / factor && host.changes == previous + 1,
               "live serial slot addition updates the host, including slot 20");
        set(plugin, "fx_slot_1_enabled", 0); tick(plugin);
        expect(plugin.getLatencySamples() == 1024 / factor, "bypassed slot adds no delay");
        set(plugin, "fx_slot_20_type", 0); tick(plugin);
        expect(plugin.getLatencySamples() == 0, "removing the last active compressor restores zero");
      }
    }
    clearSlots(plugin); prepare(plugin, 48000, 0);
    plugin.valueChangedInternal("fx_slot_1_type", vital::constants::kSpectralCompressor + 1);
    plugin.valueChangedInternal("fx_slot_1_enabled", 1);
    expect(plugin.getLatencySamples() == 1024, "editor edits update a stopped host without a render callback");
    plugin.parameterChanged("oversampling", 1);
    expect(plugin.getLatencySamples() == 512, "message-thread host quality changes report immediately");
    plugin.valueChangedInternal("fx_slot_1_enabled", 0);
    expect(plugin.getLatencySamples() == 0, "editor bypass updates a stopped host immediately");
    clearSlots(plugin); prepare(plugin, 48000, 0);
    slot(plugin, "", 1, vital::constants::kSpectralCompressor); tick(plugin);
    set(plugin, "oversampling", 3); tick(plugin);
    expect(plugin.getLatencySamples() == 128, "quality automation updates host latency with editor closed");
    slot(plugin, "", 2, vital::constants::kSpectralGate);
    for (int index = 0; index < vital::SpectralGateModule::kNumFftSizes; ++index) {
      set(plugin, "fx_slot_2_spectral_gate_fft_size", index); tick(plugin);
      expect(plugin.getLatencySamples() == (1024 + (256 << index)) / 8,
             "gate FFT changes participate in the same latency report");
    }
    juce::MemoryBlock state;
    plugin.getStateInformation(state);
    clearSlots(plugin); tick(plugin);
    expect(plugin.getLatencySamples() == 0, "cleared state has no latency");
    plugin.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    expect(plugin.getLatencySamples() == (1024 + 8192) / 8,
           "preset restoration reports latency before the next render");
    // Exercise the storage bound: three cascaded buses followed by the main
    // chain, with every slot using the largest supported FFT.
    for (int chain = 0; chain < 4; ++chain) {
      const auto prefix = chain == 0 ? "" : "bus_" + std::to_string(chain) + "_";
      for (int index = 1; index <= 20; ++index) {
        slot(plugin, prefix, index, vital::constants::kSpectralGate);
        set(plugin, prefix + "fx_slot_" + std::to_string(index) + "_spectral_gate_fft_size", 5);
      }
    }
    set(plugin, "bus_1_destination", vital::constants::kBusDestinationBus2);
    set(plugin, "bus_2_destination", vital::constants::kBusDestinationBus3);
    prepare(plugin, 48000, 0);
    expect(plugin.getLatencySamples() == 80 * 8192, "maximum serial graph fits compensation storage");
    tick(plugin, 17);
    clearSlots(plugin); tick(plugin, 17);
    expect(plugin.getLatencySamples() == 0, "maximum graph can return to zero latency");
    plugin.removeListener(&host);
  }

  std::vector<float> render(SynthPlugin& plugin, int length) {
    std::vector<float> result(length * 2);
    for (int offset = 0, block = 0; offset < length; ++block) {
      const int count = std::min(block % 2 ? 127 : 17, length - offset);
      juce::AudioBuffer<float> audio(2, count);
      juce::MidiBuffer midi;
      audio.clear();
      if (offset == 0)
        midi.addEvent(juce::MidiMessage::noteOn(1, 60, .7f), 11);
      plugin.processBlock(audio, midi);
      for (int i = 0; i < count; ++i)
        for (int channel = 0; channel < 2; ++channel)
          result[(offset + i) * 2 + channel] = audio.getSample(channel, i);
      offset += count;
    }
    return result;
  }

  void testRenderedAlignment(int effect = vital::constants::kSpectralCompressor, int effect_latency = 1024) {
    SynthPlugin reference, delayed;
  // This test renders offline and must wait for selected effect preparation.
    reference.setNonRealtime(true); delayed.setNonRealtime(true);
    std::vector<float> sample(20000);
    for (int i = 0; i < int(sample.size()); ++i)
      sample[i] = .4f * std::sin(.057f * i) * (i % 317 < 100 ? 1.0f : .2f);
    for (auto* plugin : { &reference, &delayed }) {
      for (int osc = 1; osc <= 4; ++osc) {
        const auto prefix = "osc_" + std::to_string(osc) + "_";
        set(*plugin, prefix + "on", osc == 1);
        set(*plugin, prefix + "random_phase", 0);
        set(*plugin, prefix + "unison_voices", 1);
      }
      set(*plugin, "osc_1_level", .15f);
      set(*plugin, slot_ids::id("sample_on"), 1); set(*plugin, slot_ids::id("sample_level"), .2f);
      set(*plugin, slot_ids::id("sample_random_phase"), 0); set(*plugin, slot_ids::id("sample_loop"), 0);
      set(*plugin, "env_1_attack", .001f); set(*plugin, "env_1_decay", .001f);
      set(*plugin, "env_1_sustain", 1);
      plugin->getEngine()->getSample(slot_ids::kSampleSlot)->loadSample(sample.data(), int(sample.size()), 48000);
    }
    using namespace vital::constants;
    for (int quality : { 0, 1, 2, 3 }) {
      for (int scenario = 0; scenario < 6; ++scenario) {
        for (auto* plugin : { &reference, &delayed }) {
          clearSlots(*plugin);
          set(*plugin, "osc_1_destination", kEffects);
          set(*plugin, slot_ids::id("sample_destination"), kEffects);
          for (int bus = 1; bus <= 3; ++bus)
            set(*plugin, "bus_" + std::to_string(bus) + "_destination", kBusDestinationMain);
          if (scenario == 1 || scenario == 2 || scenario == 5) {
            set(*plugin, "osc_1_destination", kBus1);
            set(*plugin, slot_ids::id("sample_destination"), kBus2);
          }
          if (scenario == 2)
            set(*plugin, "bus_1_destination", kBusDestinationBus2);
          if (scenario == 3)
            set(*plugin, "osc_1_destination", kDirectOut);
          if (scenario == 4) {
            set(*plugin, "osc_1_destination", kBus1);
            set(*plugin, "bus_1_destination", kBusDestinationDirectOut);
          }
          if (scenario == 5) {
            set(*plugin, "bus_1_destination", kBusDestinationBus3);
            set(*plugin, "bus_2_destination", kBusDestinationBus3);
          }
        }
        if (scenario == 0 || scenario == 3 || scenario == 4)
          slot(delayed, "", 1, effect);
        if (scenario == 1 || scenario == 2 || scenario == 4 || scenario == 5)
          slot(delayed, "bus_1_", 1, effect);
        if (scenario == 2 || scenario == 5)
          slot(delayed, "bus_2_", 20, effect);
        if (scenario == 4)
          slot(delayed, "", 20, effect);
        if (scenario == 5)
          slot(delayed, "bus_3_", 1, effect);
        for (auto* plugin : { &reference, &delayed }) {
          prepare(*plugin, 48000, quality);
          plugin->getEngine()->allSoundsOff();
          for (int i = 0; i < 64; ++i) tick(*plugin);
        }
        const int expected = (scenario == 2 || scenario == 4 || scenario == 5 ? 2 * effect_latency : effect_latency) / (1 << quality);
        expect(delayed.getLatencySamples() == expected, "serial/parallel routing reports the longest path");
        const auto before = render(reference, 8192);
        const auto after = render(delayed, 8192);
        float error = 0, peak = 0;
        for (int i = 0; i < 8192; ++i) {
          for (int channel = 0; channel < 2; ++channel) {
            const float wanted = i >= expected ? before[(i - expected) * 2 + channel] : 0;
            error = std::max(error, std::abs(after[i * 2 + channel] - wanted));
            peak = std::max(peak, std::abs(wanted));
          }
        }
        expect(peak > .001f && error < 2.0e-5f,
               "render aligns with host-reported delay: route=" + std::to_string(scenario) +
               " oversampling=" + std::to_string(1 << quality) + " peak=" + std::to_string(peak) +
               " error=" + std::to_string(error));
      }
    }
  }
}

int main() {
  juce::ScopedJuceInitialiser_GUI juce;
  testDelayStorage();
  testHistoryOnDemand();
  testHostReporting();
  testRenderedAlignment();
  testRenderedAlignment(vital::constants::kSpectralLock, 0);
  testRenderedAlignment(vital::constants::kResonator, 0);
  testRenderedAlignment(vital::constants::kSuppressor, 2048);
  testRenderedAlignment(vital::constants::kSpectralBalance, 2048);
  if (failures) return 1;
  std::cout << "Latency: host notifications, state recall, rates, bypass, oversampling, FFT changes, "
               "serial/parallel audio alignment and allocation checks passed.\n";
  return 0;
}
