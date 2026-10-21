#include "JuceHeader.h"
#include "prepared_effect_module.h"
#include "convolution_reverb_module.h"
#include "chorus_module.h"
#include "delay_module.h"
#include "reverb_module.h"
#include "phase_shift_module.h"
#include "dimension_expander_module.h"
#include "spectral_gate_module.h"
#include "spectral_compressor_module.h"
#include "spectral_lock_module.h"
#include "suppressor_module.h"
#include "tape_module.h"
#include "analogue_chorus_module.h"
#include "analogue_flanger_module.h"
#include "analogue_echo_module.h"
#include "analogue_rotary_module.h"
#include "analogue_spring_module.h"
#include "analogue_wah_module.h"
#include "spectral_balance_module.h"
#include "glitch_delay_module.h"
#include "compressor_module.h"
#include "distortion_module.h"
#include "equalizer_module.h"
#include "filter_fx_module.h"
#include "flanger_module.h"
#include "phaser_module.h"
#include "utility_module.h"
#include "frequency_shifter_module.h"
#include "limiter_module.h"
#include "waveshaper_module.h"
#include "mid_side_eq_module.h"
#include "jsfx_effect_modules.h"
#include "scream_module.h"
#include "tremolo_pan_module.h"
#include "defuse_module.h"
#include "trance_gate_module.h"
#include "resonator_module.h"
#include "openwurli_amp_module.h"
#include "scanner_vibrato_module.h"
#include "analogue_compressor_module.h"
#include "maim_module.h"
#include "note_monitor.h"
#include "table_filter.h"
#include "reorderable_effect_chain.h"
#include "synth_plugin.h"
#include "synth_strings.h"
#include "sound_engine.h"
#include "value_switch.h"
#include "operators.h"
#include "synth_oscillator.h"
#include "smooth_value.h"
#include "fourier_transform.h"
#include <cstdlib>
#include <new>
#if JUCE_MAC
#include <execinfo.h>
#include <mach/mach.h>
#include <unistd.h>
#include <sys/resource.h>
#elif JUCE_WINDOWS
#include <malloc.h>
#endif
#include <atomic>
#include <functional>
#include <map>
#include <chrono>
#include <iostream>
#include <thread>

namespace {
thread_local bool track_allocations = false;
thread_local size_t audio_allocations = 0;
// Construction measurements count every thread's allocations, including the
// effect preparation worker's.
std::atomic<bool> count_all_allocations { false };
std::atomic<size_t> counted_allocations { 0 }, counted_bytes { 0 };
// With ATLAS_LARGE_ALLOCATION_KIB set, construction measurements also count
// the allocations at least that large by size, to show what dominates.
constexpr int kLargeAllocationSizes = 512;
std::atomic<size_t> large_allocation_threshold { 0 };
std::atomic<size_t> large_allocation_size[kLargeAllocationSizes] {}, large_allocation_count[kLargeAllocationSizes] {};
#if JUCE_MAC
void* allocation_stacks[12][20] {};
int allocation_depths[12] {};
#endif
}
namespace {
void countLargeAllocation(std::size_t size) {
  for (int i = 0; i < kLargeAllocationSizes; ++i) {
    size_t expected = 0;
    if (large_allocation_size[i].compare_exchange_strong(expected, size) || expected == size) {
      large_allocation_count[i].fetch_add(1, std::memory_order_relaxed);
      return;
    }
  }
}
void countAllocation(std::size_t size) {
  if (track_allocations) {
#if JUCE_MAC
    if (audio_allocations < 12) allocation_depths[audio_allocations] = backtrace(allocation_stacks[audio_allocations], 20);
#endif
    ++audio_allocations;
  }
  if (count_all_allocations.load(std::memory_order_relaxed)) {
    counted_allocations.fetch_add(1, std::memory_order_relaxed);
    counted_bytes.fetch_add(size, std::memory_order_relaxed);
    const size_t threshold = large_allocation_threshold.load(std::memory_order_relaxed);
    if (threshold > 0 && size >= threshold) countLargeAllocation(size);
  }
}
}
void* operator new(std::size_t size) {
  countAllocation(size);
  if (void* memory = std::malloc(std::max(size, size_t(1)))) return memory;
  throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
// Processor output buffers use the aligned forms. Windows has no
// posix_memalign, and its aligned blocks must go back through _aligned_free.
void* operator new(std::size_t size, std::align_val_t alignment) {
  countAllocation(size);
  const std::size_t bytes = std::max(size, size_t(1));
  const std::size_t align = std::max(std::size_t(alignment), sizeof(void*));
#if JUCE_WINDOWS
  if (void* memory = _aligned_malloc(bytes, align)) return memory;
#else
  void* memory = nullptr;
  if (posix_memalign(&memory, align, bytes) == 0) return memory;
#endif
  throw std::bad_alloc();
}
void* operator new[](std::size_t size, std::align_val_t alignment) { return ::operator new(size, alignment); }
namespace {
void freeAligned(void* memory) noexcept {
#if JUCE_WINDOWS
  _aligned_free(memory);
#else
  std::free(memory);
#endif
}
}
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete(void* memory, std::align_val_t) noexcept { freeAligned(memory); }
void operator delete[](void* memory, std::align_val_t) noexcept { freeAligned(memory); }
void operator delete(void* memory, std::size_t, std::align_val_t) noexcept { freeAligned(memory); }
void operator delete[](void* memory, std::size_t, std::align_val_t) noexcept { freeAligned(memory); }

namespace {
int failures = 0;
void expect(bool ok, const std::string& message) {
  if (!ok) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
// Inputs the light effects read: Filter FX keytracking (offset from middle C,
// so the cutoff moves), Scream's MIDI note and the Resonator's held notes.
vital::cr::Value keytrack_input(7.0f), scream_note_input(52.0f);
vital::NoteMonitor note_monitor;
std::unique_ptr<vital::SynthModule> create(int type, vital::Output* bpm) {
  using namespace vital;
  switch (type) {
    case constants::kCompressor: return std::make_unique<CompressorModule>("");
    case constants::kDistortion: return std::make_unique<DistortionModule>("");
    case constants::kEq: return std::make_unique<EqualizerModule>("", false);
    case constants::kFilterFx: return std::make_unique<FilterFxModule>(keytrack_input.output(), "");
    case constants::kFlanger: return std::make_unique<FlangerModule>(bpm, "");
    case constants::kPhaser: return std::make_unique<PhaserModule>(bpm, "");
    case constants::kUtility: return std::make_unique<UtilityModule>("");
    case constants::kFrequencyShifter: return std::make_unique<FrequencyShifterModule>("");
    case constants::kLimiter: return std::make_unique<LimiterModule>("");
    case constants::kWaveShaper: return std::make_unique<WaveShaperModule>("");
    case constants::kMidSideEq: return std::make_unique<MidSideEqModule>("");
    case constants::kErosion: return std::make_unique<ErosionModule>("");
    case constants::kFloatyDelay: return std::make_unique<FloatyDelayModule>(bpm, "");
    case constants::kScream: return std::make_unique<ScreamModule>("", scream_note_input.output());
    case constants::kTremoloPan: return std::make_unique<TremoloPanModule>(bpm, "");
    case constants::kDefuse: return std::make_unique<DefuseModule>("");
    case constants::kTranceGate: return std::make_unique<TranceGateModule>(bpm, nullptr, "");
    case constants::kResonator: return std::make_unique<ResonatorModule>(bpm, &note_monitor, "");
    case constants::kChorus: return std::make_unique<ChorusModule>(bpm);
    case constants::kDelay: return std::make_unique<DelayModule>(bpm);
    case constants::kReverb: return std::make_unique<ReverbModule>();
    case constants::kPhaseShift: return std::make_unique<PhaseShiftModule>();
    case constants::kDimensionExpander: return std::make_unique<DimensionExpanderModule>();
    case constants::kSpectralGate: return std::make_unique<SpectralGateModule>();
    case constants::kSpectralLock: return std::make_unique<SpectralLockModule>();
    case constants::kSpectralCompressor: return std::make_unique<SpectralCompressorModule>();
    case constants::kSuppressor: return std::make_unique<SuppressorModule>();
    case constants::kTape: return std::make_unique<TapeModule>();
    case constants::kAnalogueChorus: return std::make_unique<AnalogueChorusModule>();
    case constants::kAnalogueFlanger: return std::make_unique<AnalogueFlangerModule>(bpm);
    case constants::kAnalogueEcho: return std::make_unique<AnalogueEchoModule>(bpm);
    case constants::kAnalogueRotary: return std::make_unique<AnalogueRotaryModule>();
    case constants::kAnalogueSpring: return std::make_unique<AnalogueSpringModule>();
    case constants::kAnalogueWah: return std::make_unique<AnalogueWahModule>();
    case constants::kSpectralBalance: return std::make_unique<SpectralBalanceModule>();
    case constants::kGlitchDelay: return std::make_unique<GlitchDelayModule>(bpm, nullptr);
    case constants::kOpenWurliAmp: return std::make_unique<OpenWurliAmpModule>();
    case constants::kScannerVibrato: return std::make_unique<ScannerVibratoModule>();
    case constants::kAnalogueCompressor: return std::make_unique<AnalogueCompressorModule>();
    case constants::kMaim: return std::make_unique<MaimModule>();
  }
  return {};
}
void compareEffects() {
  using namespace vital;
  cr::Value bpm(2);
  auto worker = std::make_shared<EffectPreparationWorker>();
  note_monitor.noteOn(57, 0);
  // Every effect a slot can hold is prepared on demand (Convolution Reverb has
  // its own IR tests in --ir-switching).
  for (int type = 0; type < constants::kNumEffects; ++type) {
    if (type == constants::kConvolutionReverb) continue;
    if (!create(type, bpm.output())) { expect(false, "test factory covers effect " + std::to_string(type)); continue; }
    for (int factor : { 1, 2, 4, 8 }) {
      auto original = create(type, bpm.output());
      PreparedEffectModule prepared(type, "", [&] { return create(type, bpm.output()); }, worker);
      for (auto* module : { original.get(), static_cast<SynthModule*>(&prepared) }) {
        module->init(); module->setSampleRate(48000); module->setOversampleAmount(factor);
      }
      // A slot selecting the effect makes it live: its modulation wiring.
      prepared.reserveControls();
      auto before = original->getControls(), after = prepared.getControls();
      expect(before.size() == after.size(), "all saved controls retained");
      // Exercise non-default, modulated and smoothed controls, as well as the
      // default graph. Identical input is processed by the original module.
      for (const auto& control : before) {
        expect(after.count(control.first) == 1, "control identity " + control.first);
        expect(control.second->value() == after.at(control.first)->value(), "default " + control.first);
      }
      cr::Value modulation(.13f);
      for (const auto& entry : before) {
        auto* first = original->getMonoModulationDestination(entry.first);
        auto* second = prepared.getMonoModulationDestination(entry.first);
        expect(bool(first) == bool(second), "modulation target identity " + entry.first);
        if (first) {
          first->plugNext(modulation.output()); second->plugNext(modulation.output());
          original->getMonoModulationSwitch(entry.first)->set(1);
          prepared.getMonoModulationSwitch(entry.first)->set(1);
        }
      }
      prepared.prepareForPlayback();
      // In the original Filter FX graph, the filters read resonance, drive and
      // blend before those controls are summed, so a change reaches them one
      // block late. Prepared, the shell sums every control before the filter
      // runs. The two must agree again once the filter has settled.
      const int settled_block = type == constants::kFilterFx ? 96 : 32;
      double error = 0, transient_error = 0, peak = 0;
      std::vector<poly_float> input(113 * factor);
      for (int block = 0; block < 128; ++block) {
        if (block == 32) {
          for (const auto& control : before) {
            if (!original->getMonoModulationDestination(control.first)) continue;
            // Glitch Delay's glitches follow an unseeded random sequence, so
            // they stay off; tests/glitch_delay_tests.cpp covers them.
            if (type == constants::kGlitchDelay && control.first.size() > 5 &&
                control.first.compare(control.first.size() - 5, 5, "_prob") == 0)
              continue;
            const auto& details = Parameters::getDetails(control.first);
            const float value = details.min + .37f * (details.max - details.min);
            control.second->set(value); after.at(control.first)->set(value);
          }
        }
        for (int i = 0; i < int(input.size()); ++i)
          for (int lane = 0; lane < poly_float::kSize; ++lane)
            input[i].set(lane, .1f * std::sin((block * input.size() + i) * .071 / factor + lane));
        const double time = block * input.size() / (48000.0 * factor);
        original->correctToTime(time); prepared.correctToTime(time);
        original->processWithInput(input.data(), int(input.size()));
        prepared.processWithInput(input.data(), int(input.size()));
        for (int i = 0; i < int(input.size()); ++i)
          for (int lane = 0; lane < poly_float::kSize; ++lane) {
            const float a = original->output()->buffer[i][lane], b = prepared.output()->buffer[i][lane];
            expect(std::isfinite(b), "finite prepared output");
            if (block < 32 || block >= settled_block)
              error = std::max(error, double(std::abs(a - b)));
            else
              transient_error = std::max(transient_error, double(std::abs(a - b)));
            peak = std::max(peak, double(std::abs(a)));
          }
      }
      std::cout << "effect=" << type << " factor=" << factor << " error=" << error
                << " transient_error=" << transient_error << " peak=" << peak << '\n';
      expect(error < 2e-5, "unchanged effect audio, type=" + std::to_string(type));
      prepared.enable(false); prepared.enable(true); prepared.prepareForPlayback();
      original->enable(false); original->enable(true);
      original->processWithInput(input.data(), int(input.size()));
      prepared.processWithInput(input.data(), int(input.size()));
      expect(prepared.getControls().begin()->second == after.begin()->second, "control addresses survive preparation");
    }
  }
}
// A slot's Filter FX in the Filter Table model reports the table's latency and
// filters, rather than passing audio through without its buffers.
void slotFilterTable() {
  using namespace vital;
  cr::Value bpm(2), keytrack(0);
  ReorderableEffectChain chain(bpm.output(), keytrack.output(), "bus_2_");
  chain.init(); chain.setSampleRate(48000); chain.setOversampleAmount(1);
  const auto controls = chain.getControls();
  const std::string prefix = "bus_2_fx_slot_3_";
  controls.at(prefix + "type")->set(constants::kFilterFx + 1);
  controls.at(prefix + "enabled")->set(1);
  controls.at(prefix + "filter_fx_style")->set(atlas::filter_table::kLinear);
  expect(chain.getLatencySamples() == 0, "Filter FX has no latency outside the Filter Table model");
  controls.at(prefix + "filter_fx_model")->set(constants::kFilterTable);
  expect(chain.getLatencySamples() == (1 << TableFilter::kFxOrder) / 2,
         "Filter FX reports the linear-phase table latency");
  chain.prepareFilterTable(2);
  chain.prepareSelectedEffects();
  std::array<poly_float, 128> input {};
  double difference = 0;
  for (int block = 0; block < 64; ++block) {
    for (int i = 0; i < 128; ++i) input[i] = .3f * std::sin((block * 128 + i) * .9f);
    chain.processWithInput(input.data(), 128);
    for (int i = 0; i < 128; ++i) {
      expect(std::isfinite(chain.output()->buffer[i][0]), "finite Filter Table output");
      difference = std::max(difference, double(std::abs(chain.output()->buffer[i][0] - input[i][0])));
    }
  }
  expect(difference > .01, "the slot's Filter Table filters once prepared");
}

void cancelledPreparationIsReleased() {
  using namespace vital;
  struct CountedDelay : DelayModule {
    CountedDelay(Output* bpm, std::atomic<int>& count) : DelayModule(bpm), live(count) { ++live; }
    ~CountedDelay() override { --live; }
    std::atomic<int>& live;
  };
  cr::Value bpm(2);
  std::atomic<int> live { 0 };
  std::atomic<bool> allow { true };
  auto worker = std::make_shared<EffectPreparationWorker>();
  PreparedEffectModule module(constants::kDelay, "", [&]() -> std::unique_ptr<SynthModule> {
    while (!allow.load()) std::this_thread::yield();
    return std::make_unique<CountedDelay>(bpm.output(), live);
  }, worker);
  module.init(); module.setSampleRate(48000); module.setOversampleAmount(2);
  module.reserveControls(); // Selected: only a live effect prepares its DSP.
  allow = false;
  std::array<poly_float, 128> input {};
  {
    RealtimeEffectScope realtime(true);
    module.processWithInput(input.data(), 128);
  }
  allow = true;
  worker->prepareSynchronously(module); // Leave its finished publication unconsumed.
  expect(live == 1, "one pending DSP exists before cancellation");
  module.enable(false);
  for (int attempt = 0; attempt < 1000 && live != 0; ++attempt)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  expect(live == 0, "unused prepared DSP is released after the slot is bypassed");
}

void convolutionStateRoundTrip() {
  SynthPlugin original, restored;
  vital::ConvolutionReverbModule::IrData ir;
  ir.name = "Prepared effect regression IR"; ir.sample_rate = 48000;
  ir.left.resize(256); ir.left[0] = 1; ir.left[127] = .25f;
  const auto expected = vital::ConvolutionReverbModule::irToJson(ir);
  for (const auto& entry : original.getEngine()->getConvolutionReverbModules()) {
    if (entry.first == "bus_2_fx_slot_20_") {
      expect(entry.second->irStateFromJson(expected), "load IR into an inactive slot");
      expect(entry.second->hasCustomIr(), "inactive IR marked custom");
    }
  }
  juce::MemoryBlock state;
  original.getStateInformation(state);
  restored.setStateInformation(state.getData(), int(state.getSize()));
  bool found = false;
  for (const auto& entry : restored.getEngine()->getConvolutionReverbModules()) {
    if (entry.first == "bus_2_fx_slot_20_") {
      found = true;
      expect(entry.second->hasCustomIr() && entry.second->irStateToJson() == expected,
             "inactive custom IR survives full plugin state round-trip");
    }
  }
  expect(found, "convolution slot identity retained");
}

void concurrentConvolutionLifecycle() {
  using namespace vital;
  constexpr int instances = 8;
  std::array<std::unique_ptr<ConvolutionReverbModule>, instances> modules;
  ConvolutionReverbModule::IrData ir;
  ir.name = "Concurrent delayed impulse";
  ir.sample_rate = 48000;
  ir.left.resize(2048);
  ir.left[640] = 1.0f;
  const auto state = ConvolutionReverbModule::irToJson(ir);
  for (auto& module : modules) {
    module = std::make_unique<ConvolutionReverbModule>();
    module->init();
    module->setSampleRate(48000);
    module->setOversampleAmount(1);
    expect(module->irStateFromJson(state), "parallel convolution IR loads");
    module->getControls().at("convolution_reverb_dry_wet")->set(1.0f);
  }

  std::atomic<int> waiting { 0 };
  std::atomic<bool> start { false };
  std::array<bool, instances> finite {}, audible {};
  std::array<std::thread, instances> threads;
  for (int index = 0; index < instances; ++index) {
    threads[index] = std::thread([&, index] {
      auto& module = *modules[index];
      std::array<poly_float, 128> input {};
      ++waiting;
      while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
      finite[index] = true;
      // Separate host audio threads may reset/load/process different instances
      // while another instance is being prepared on its effect worker.
      for (int block = 0; block < 1200; ++block) {
        module.hardReset();
        if (block % 16 == 0) module.setSampleRate(block % 32 ? 44100 : 48000);
        if (block % 31 == 0) module.irStateFromJson(state);
        input[0] = .5f;
        module.processWithInput(input.data(), int(input.size()));
        for (int sample = 0; sample < int(input.size()); ++sample)
          for (int lane = 0; lane < poly_float::kSize; ++lane)
            finite[index] &= std::isfinite(module.output()->buffer[sample][lane]);
        if (block % 8 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }

      // A fix must keep the wet effect working, not just suppress the exception.
      module.setSampleRate(48000);
      module.irStateFromJson(state);
      for (int block = 0; block < 1000 && !audible[index]; ++block) {
        input.fill(poly_float(0));
        if (block % 32 == 0) input[0] = 1.0f;
        module.processWithInput(input.data(), int(input.size()));
        if (block % 32 == 5)
          for (int sample = 0; sample < int(input.size()); ++sample)
            audible[index] |= std::abs(module.output()->buffer[sample][0]) > .01f;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    });
  }
  while (waiting.load() != instances) std::this_thread::yield();
  start.store(true, std::memory_order_release);
  for (auto& thread : threads) thread.join();
  for (int index = 0; index < instances; ++index) {
    expect(finite[index], "concurrent convolution output stays finite");
    expect(audible[index], "concurrent convolution retains its delayed wet IR");
  }
  std::cout << "Concurrent convolution lifecycle completed for " << instances << " instances\n";
}

void realtimeTransitions() {
  using namespace vital;
  SynthPlugin plugin;
  plugin.setPlayConfigDetails(0, 2, 48000, 128);
  plugin.prepareToPlay(48000, 128);
  const auto controls = plugin.getControls();
  auto* type1 = controls.at("fx_slot_1_type");
  auto* type2 = controls.at("fx_slot_2_type");
  auto* enabled = controls.at("fx_slot_2_enabled");
  auto* quality = controls.at("oversampling");
  type1->set(constants::kSuppressor + 1);
  controls.at("fx_slot_1_enabled")->set(1);
  plugin.getEngine()->prepareSelectedEffects();
  juce::AudioBuffer<float> audio(2, 128);
  juce::MidiBuffer midi;
  midi.addEvent(juce::MidiMessage::noteOn(1, 60, .5f), 0);
  plugin.processBlock(audio, midi);
  midi.clear();
  double maximum_ms = 0;
  audio_allocations = 0;
  for (int block = 0; block < 600; ++block) {
    if (block % 25 == 0) {
      const int effects[] = { constants::kSuppressor, constants::kReverb,
          constants::kSpectralGate, constants::kDelay, constants::kConvolutionReverb, constants::kSpectralLock,
          constants::kFilterFx, constants::kEq, constants::kCompressor, constants::kMidSideEq };
      // As the editor and host automation do; this reserves the new effect's controls.
      plugin.valueChanged("fx_slot_2_type", effects[(block / 25) % 10] + 1);
      enabled->set(1);
    }
    if (block % 25 == 20) enabled->set(0);
    if (block % 100 == 0) quality->set((block / 100) % 4);
    const auto start = std::chrono::steady_clock::now();
    const auto previous_allocations = audio_allocations;
    track_allocations = true;
    plugin.processBlock(audio, midi);
    track_allocations = false;
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    maximum_ms = std::max(maximum_ms, ms);
    if (audio_allocations != previous_allocations)
      std::cout << "block=" << block << " allocations=" << audio_allocations - previous_allocations << " ms=" << ms << '\n';
    for (int ch = 0; ch < 2; ++ch)
      for (int i = 0; i < 128; ++i) expect(std::isfinite(audio.getSample(ch, i)), "finite realtime transitions");
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  std::cout << "realtime transition allocations=" << audio_allocations << " max_ms=" << maximum_ms << '\n';
#if JUCE_MAC
  if (audio_allocations) for (size_t i = 0; i < std::min(audio_allocations, size_t(12)); ++i)
    backtrace_symbols_fd(allocation_stacks[i], allocation_depths[i], 2);
#endif
  expect(audio_allocations == 0, "effect selection, bypass and quality changes allocate no C++ memory on audio thread");
  // Destruction must join outstanding preparations before destroying graph inputs.
  type2->set(constants::kSuppressor + 1); enabled->set(1);
  plugin.processBlock(audio, midi);
}

#if JUCE_WINDOWS
// Defined at the end of this file, after <windows.h>, so its macros reach nothing else.
double windowsPrivateWorkingSetMiB();
double windowsCommitMiB();
#endif
// Memory the process actually touched, as Activity Monitor reports it (on
// Windows, the private working set Task Manager shows).
double footprintMiB() {
#if JUCE_WINDOWS
  return windowsPrivateWorkingSetMiB();
#else
#if JUCE_MAC
  task_vm_info_data_t info {};
  mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
  if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS)
    return info.phys_footprint / 1048576.0;
#endif
  return 0;
#endif
}
// Windows also charges untouched allocations against the system's commit
// limit (Task Manager's commit size); other systems report 0.
double commitMiB() {
#if JUCE_WINDOWS
  return windowsCommitMiB();
#else
  return 0;
#endif
}
struct Measurement { double ms; size_t allocations, bytes; double footprint; };
template<class Function> Measurement measureConstruction(Function function) {
  counted_allocations = 0; counted_bytes = 0;
  const double footprint = footprintMiB();
  count_all_allocations = true;
  const auto start = std::chrono::steady_clock::now();
  function();
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  count_all_allocations = false;
  return { ms, counted_allocations.load(), counted_bytes.load(), footprintMiB() - footprint };
}
// Prints (largest total first) and clears the sizes ATLAS_LARGE_ALLOCATION_KIB counted.
void reportLargeAllocations(const char* stage) {
  if (large_allocation_threshold.load() == 0) return;
  std::vector<std::pair<size_t, size_t>> sizes;
  for (int i = 0; i < kLargeAllocationSizes; ++i) {
    if (const size_t size = large_allocation_size[i].exchange(0))
      sizes.push_back({ size, large_allocation_count[i].exchange(0) });
  }
  std::sort(sizes.begin(), sizes.end(), [](const auto& a, const auto& b) { return a.first * a.second > b.first * b.second; });
  for (const auto& [size, count] : sizes)
    std::cout << stage << " large allocation KiB=" << size / 1024.0 << " bytes=" << size << " count=" << count
              << " total_MiB=" << size * count / 1048576.0 << '\n';
}

struct EffectFactoryProbe : vital::ReorderableEffectChain {
  using ReorderableEffectChain::ReorderableEffectChain;
  using ReorderableEffectChain::createEffectDsp;
  using ReorderableEffectChain::createEffectModule;
};

// Reports what one plug-in instance costs to build, and what each effect type
// costs per slot, so effect instantiation changes can be compared.
void constructionCost(bool chain_only, int shell_type = -1) {
  using namespace vital;
  cr::Value bpm(2), keytrack(0);
  if (const char* large = std::getenv("ATLAS_LARGE_ALLOCATION_KIB"))
    large_allocation_threshold = size_t(std::max(1, std::atoi(large))) * 1024;
  if (shell_type >= 0) {
    EffectFactoryProbe probe(bpm.output(), keytrack.output());
    std::vector<std::unique_ptr<SynthModule>> modules;
    constexpr int count = 200;
    const auto built = measureConstruction([&] {
      for (int i = 0; i < count; ++i) {
        modules.emplace_back(probe.createEffectModule(shell_type, "fx_slot_1_"));
        modules.back()->init();
        modules.back()->setSampleRate(48000);
        modules.back()->setOversampleAmount(2);
      }
    });
    std::cout << "effect=" << strings::kEffectOrder[shell_type] << " footprint_KiB_each=" << built.footprint * 1024.0 / count
              << " allocations_each=" << built.allocations / count << '\n';
    return;
  }
  if (chain_only) {
    std::unique_ptr<EffectFactoryProbe> chain;
    const auto built = measureConstruction([&] {
      chain = std::make_unique<EffectFactoryProbe>(bpm.output(), keytrack.output());
      chain->init();
      chain->setSampleRate(48000);
      chain->setOversampleAmount(2);
    });
    std::cout << "one effect chain ms=" << built.ms << " allocations=" << built.allocations
              << " requested_MiB=" << built.bytes / 1048576.0 << " footprint_MiB=" << built.footprint << '\n';
    reportLargeAllocations("chain");
    return;
  }
  std::unique_ptr<SynthPlugin> plugin;
  // Shifts the heap before construction, to see how much timings depend on layout.
  std::vector<std::unique_ptr<char[]>> layout_shift;
  if (const char* shift = std::getenv("ATLAS_HEAP_SHIFT"))
    for (int i = 0; i < std::atoi(shift); ++i) layout_shift.emplace_back(new char[1000 + 37 * i]);
  const auto whole = measureConstruction([&] { plugin = std::make_unique<SynthPlugin>(); });
  std::cout << "plugin construction ms=" << whole.ms << " allocations=" << whole.allocations
            << " requested_MiB=" << whole.bytes / 1048576.0 << " footprint_MiB=" << whole.footprint << '\n';
  reportLargeAllocations("construction");
  const auto prepared = measureConstruction([&] {
    plugin->setPlayConfigDetails(0, 2, 48000, 512);
    plugin->prepareToPlay(48000, 512);
  });
  std::cout << "plugin prepareToPlay ms=" << prepared.ms << " allocations=" << prepared.allocations
            << " requested_MiB=" << prepared.bytes / 1048576.0 << " footprint_MiB=" << prepared.footprint << '\n';
  reportLargeAllocations("prepareToPlay");
  large_allocation_threshold = 0;
#if JUCE_WINDOWS
  std::cout << "plugin prepared commit_MiB=" << commitMiB() << '\n';
#endif
  // Keeps the prepared instance alive so heap tools (heap, malloc_history) can attribute its memory.
  if (const char* hold = std::getenv("ATLAS_CONSTRUCTION_HOLD_SECONDS")) {
    std::cout << "holding pid=" << getpid() << " total_footprint_MiB=" << footprintMiB() << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(std::atoi(hold)));
  }
  {
    juce::AudioBuffer<float> audio(2, 512);
    juce::MidiBuffer midi;
    for (int block = 0; block < 100; ++block) plugin->processBlock(audio, midi);
    const auto idle = measureConstruction([&] {
      for (int block = 0; block < 1000; ++block) plugin->processBlock(audio, midi);
    });
    std::cout << "plugin processBlock without notes us_per_block=" << idle.ms << '\n';
    midi.addEvent(juce::MidiMessage::noteOn(1, 60, .5f), 0);
    plugin->processBlock(audio, midi);
    midi.clear();
    const auto blocks = measureConstruction([&] {
      for (int block = 0; block < 1000; ++block) plugin->processBlock(audio, midi);
    });
    std::cout << "plugin processBlock us_per_block=" << blocks.ms << '\n';
    // The same with four light effects running in the first slots.
    const auto controls = plugin->getControls();
    const int light[] = { constants::kFilterFx, constants::kEq, constants::kCompressor, constants::kDistortion };
    for (int slot = 0; slot < 4; ++slot) {
      const std::string prefix = "fx_slot_" + std::to_string(slot + 1) + "_";
      controls.at(prefix + "type")->set(light[slot] + 1);
      controls.at(prefix + "enabled")->set(1);
    }
    plugin->getEngine()->prepareSelectedEffects();
    for (int block = 0; block < 100; ++block) plugin->processBlock(audio, midi);
    const auto effects = measureConstruction([&] {
      for (int block = 0; block < 1000; ++block) plugin->processBlock(audio, midi);
    });
    std::cout << "plugin processBlock with four light effects us_per_block=" << effects.ms << '\n';
    // Keeps processing the light effects so a profiler can sample them.
    if (const char* seconds = std::getenv("ATLAS_LIGHT_EFFECTS_SECONDS")) {
      std::cout << "profiling pid=" << getpid() << std::endl;
      const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(std::atoi(seconds));
      while (std::chrono::steady_clock::now() < end)
        for (int block = 0; block < 100; ++block) plugin->processBlock(audio, midi);
    }
    juce::MemoryBlock state;
    const auto saved = measureConstruction([&] { plugin->getStateInformation(state); });
    std::cout << "plugin getStateInformation ms=" << saved.ms << " KiB=" << state.getSize() / 1024
              << " allocations=" << saved.allocations << '\n';
    const auto restore = measureConstruction([&] {
      plugin->setStateInformation(state.getData(), int(state.getSize()));
    });
    std::cout << "plugin setStateInformation ms=" << restore.ms << " allocations=" << restore.allocations << '\n';
  }
  const auto destroyed = measureConstruction([&] { plugin.reset(); });
  std::cout << "plugin destruction ms=" << destroyed.ms << '\n';

  EffectFactoryProbe probe(bpm.output(), keytrack.output());
  for (int type = 0; type < constants::kNumEffects; ++type) {
    std::unique_ptr<SynthModule> module;
    const auto slot = measureConstruction([&] {
      module.reset(probe.createEffectModule(type, "fx_slot_1_"));
      module->init();
    });
    std::cout << "slot effect=" << type << " " << strings::kEffectOrder[type] << " ms=" << slot.ms
              << " allocations=" << slot.allocations << " requested_KiB=" << slot.bytes / 1024.0
              << " footprint_KiB=" << slot.footprint * 1024.0 << '\n';
  }
}

}
// An effect a slot does not select keeps its parameter values but none of
// their modulation wiring, except where a route reaches it. Selecting it
// wires it; leaving it tears down the wiring no route uses.
void parkedEffectWiring() {
  using namespace vital;
  cr::Value bpm(2), keytrack(0);
  ReorderableEffectChain chain(bpm.output(), keytrack.output(), "bus_1_");
  chain.init(); chain.setSampleRate(48000); chain.setOversampleAmount(2);
  const auto controls = chain.getControls();
  const std::string slot = "bus_1_fx_slot_4_";
  const std::string feedback = slot + "delay_feedback", dry_wet = slot + "delay_dry_wet";
  expect(controls.count(feedback) == 1, "a parked effect keeps its parameter values");
  expect(chain.getMonoModulationDestination(feedback) == nullptr, "a parked effect has no modulation wiring");
  expect(chain.getMonoModulationDestinations().empty(), "nothing is wired before a slot selects an effect");

  controls.at(slot + "type")->set(constants::kDelay + 1);
  chain.reserveSelectedControls();
  expect(chain.getMonoModulationDestination(feedback) != nullptr, "selecting an effect wires its parameters");
  expect(chain.getMonoModulationDestination(dry_wet) != nullptr, "every modulated parameter is wired");
  controls.at(feedback)->set(.61f);

  // A route keeps its parameter wired after the slot moves on.
  cr::Value modulation(.2f);
  chain.getMonoModulationDestination(feedback)->plugNext(modulation.output());
  controls.at(slot + "type")->set(constants::kReverb + 1);
  chain.parkUnselectedEffects();
  chain.reserveSelectedControls();
  expect(chain.getMonoModulationDestination(feedback) != nullptr, "a routed parameter stays wired when parked");
  expect(chain.getMonoModulationDestination(dry_wet) == nullptr, "unrouted parameters lose their wiring when parked");
  expect(chain.getMonoModulationDestination(slot + "reverb_decay_time") != nullptr, "the newly selected effect is live");
  expect(controls.at(feedback)->value() == .61f, "a parked effect keeps its settings");

  // A route can be made to a parked effect's parameter, and only to modulated ones.
  expect(chain.wireModulationDestination(slot + "chorus_frequency"), "a parked parameter can be wired for a route");
  expect(chain.getMonoModulationDestination(slot + "chorus_frequency") != nullptr, "the wired parameter is a target");
  expect(!chain.wireModulationDestination(slot + "type"), "a slot's type is not a modulation target");
  expect(!chain.wireModulationDestination("fx_slot_4_delay_feedback"), "another chain's parameter is not wired here");

  // Once its route goes, the parameter's wiring goes at the next park.
  chain.getMonoModulationDestination(feedback)->unplug(modulation.output());
  chain.parkUnselectedEffects();
  expect(chain.getMonoModulationDestination(feedback) == nullptr, "an unrouted parked parameter is unwired");
  expect(chain.getMonoModulationDestination(slot + "chorus_frequency") == nullptr, "so is one wired for a route that never came");

  // Returning to an effect wires it again and it runs, offline.
  controls.at(slot + "type")->set(constants::kDelay + 1);
  controls.at(slot + "enabled")->set(1);
  std::array<poly_float, 256> input {};
  for (int i = 0; i < 256; ++i) input[i] = .3f * std::sin(i * .9f);
  double difference = 0;
  for (int block = 0; block < 40; ++block) {
    chain.processWithInput(input.data(), 256);
    for (int i = 0; i < 256; ++i)
      difference = std::max(difference, double(std::abs(chain.output()->buffer[i][0] - input[i][0])));
  }
  expect(chain.getMonoModulationDestination(feedback) != nullptr, "a reselected effect is live again");
  expect(difference > .01, "a reselected effect runs");
}

// A slot's effect reserves its controls' buffers only once selected, on the
// message thread; until then the slot passes audio through without allocating.
void controlsReservedOnSelection() {
  using namespace vital;
  expect(SynthBase::isEffectSlotTypeControl("fx_slot_3_type"), "main chain slot type is recognised");
  expect(SynthBase::isEffectSlotTypeControl("bus_2_fx_slot_20_type"), "bus slot type is recognised");
  expect(!SynthBase::isEffectSlotTypeControl("fx_slot_3_distortion_type"), "effect parameters are not slot types");
  expect(!SynthBase::isEffectSlotTypeControl("osc_1_type"), "oscillator types are not slot types");

  cr::Value bpm(2), keytrack(0);
  ReorderableEffectChain chain(bpm.output(), keytrack.output(), "bus_2_");
  chain.init(); chain.setSampleRate(48000); chain.setOversampleAmount(2);
  const auto controls = chain.getControls();
  const std::string prefix = "bus_2_fx_slot_3_";
  auto* shell = chain.filterFx(2);
  expect(!shell->controlsReserved(), "an effect's controls are not reserved before a slot selects it");
  controls.at(prefix + "type")->set(constants::kFilterFx + 1);
  controls.at(prefix + "enabled")->set(1);

  std::array<poly_float, 256> input {};
  auto fill = [&](int block) {
    for (int i = 0; i < 256; ++i) input[i] = .3f * std::sin((block * 256 + i) * .9f);
  };
  {
    RealtimeEffectScope realtime(true);
    bool dry = true;
    audio_allocations = 0;
    for (int block = 0; block < 8; ++block) {
      fill(block);
      track_allocations = true;
      chain.processWithInput(input.data(), 256);
      track_allocations = false;
      for (int i = 0; i < 256; ++i) dry &= chain.output()->buffer[i][0] == input[i][0];
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    expect(audio_allocations == 0, "an unreserved selected effect allocates nothing on the audio thread");
    expect(dry, "an unreserved selected effect passes audio through");
    expect(!shell->controlsReserved(), "realtime processing never reserves an effect's controls");
  }

  chain.reserveSelectedControls();
  expect(shell->controlsReserved(), "selecting an effect reserves its controls");
  RealtimeEffectScope realtime(true);
  double difference = 0;
  for (int block = 8; block < 1000 && difference < .01; ++block) {
    fill(block);
    chain.processWithInput(input.data(), 256);
    for (int i = 0; i < 256; ++i)
      difference = std::max(difference, double(std::abs(chain.output()->buffer[i][0] - input[i][0])));
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  expect(difference > .01, "the effect runs once its controls are reserved");
}

// Stable across builds and platforms, unlike std::hash.
uint32_t stableHash(const std::string& text) {
  uint32_t hash = 2166136261u;
  for (unsigned char c : text) { hash ^= c; hash *= 16777619u; }
  return hash;
}

// Sets every parameter of a slot's effect to a fixed value away from its
// default, through valueChanged as the editor does. The value depends on the
// parameter's name without the slot, so main and bus slots get the same.
void setEffectParameters(SynthPlugin& plugin, const std::string& slot_prefix, int type, uint32_t seed = 0) {
  const std::string prefix = slot_prefix + strings::kEffectOrder[type] + "_";
  std::vector<std::pair<std::string, float>> changes;
  for (const auto& control : plugin.getControls()) {
    if (control.first.rfind(prefix, 0) != 0 || !vital::Parameters::isParameter(control.first))
      continue;
    const auto& details = vital::Parameters::getDetails(control.first);
    const float fraction = .25f + .5f * float((stableHash(control.first.substr(slot_prefix.size())) + seed) % 1000) / 1000.0f;
    float value = details.min + (details.max - details.min) * fraction;
    if (details.value_scale == vital::ValueDetails::kIndexed)
      value = std::round(value);
    changes.emplace_back(control.first, value);
  }
  for (const auto& [name, value] : changes)
    plugin.valueChanged(name, value);
}

// Routes `source` into the first modulatable parameter of a slot's effect
// (which must be selected) and returns its name.
std::string routeIntoEffect(SynthPlugin& plugin, const std::string& slot_prefix, int type,
                            const std::string& source, float amount) {
  const std::string prefix = slot_prefix + strings::kEffectOrder[type] + "_";
  std::vector<std::string> names;
  for (const auto& control : plugin.getControls())
    if (control.first.rfind(prefix, 0) == 0) names.push_back(control.first);
  for (const auto& name : names) {
    if (plugin.getEngine()->getMonoModulationDestination(name) == nullptr)
      continue;
    plugin.connectModulation(source, name);
    const int index = plugin.getConnectionIndex(source, name);
    if (index >= 0)
      plugin.valueChanged("modulation_" + std::to_string(index + 1) + "_amount", amount);
    return name;
  }
  return {};
}

void selectEffect(SynthPlugin& plugin, const std::string& slot_prefix, int type) {
  plugin.valueChanged(slot_prefix + "type", float(type + 1));
  plugin.valueChanged(slot_prefix + "enabled", 1.0f);
}

juce::AudioProcessorParameter* hostParameter(SynthPlugin& plugin, const String& id) {
  for (auto* parameter : plugin.getParameters())
    if (auto* with_id = dynamic_cast<juce::AudioProcessorParameterWithID*>(parameter))
      if (with_id->getParameterID() == id) return parameter;
  return nullptr;
}

// Renders fixed patches to raw float files, so two builds can be compared
// sample for sample after a memory or graph change: spectral morphs, every
// filter model and style, each quality, every effect in a main and a bus slot
// (non-default settings and an LFO route into it), effects switched during
// playback, FX proxy automation, a bus preset and the factory presets.
void renderScenarios(const std::string& directory, const std::string& factory, const std::string& only = {}) {
  using namespace vital;
  struct Scenario {
    std::string name;
    std::vector<std::pair<std::string, float>> values;
    juce::File preset;
    std::function<void(SynthPlugin&)> setup;
    std::function<void(SynthPlugin&, int block)> during;
  };
  std::vector<Scenario> scenarios;
  scenarios.push_back({ "default", {}, {} });
  for (int morph = 0; morph < SynthOscillator::kNumSpectralMorphTypes; ++morph) {
    scenarios.push_back({ "morph_" + std::to_string(morph),
        { { "osc_1_spectral_morph_type", morph }, { "osc_1_spectral_morph_amount", .6f } }, {} });
    scenarios.push_back({ "secondary_morph_" + std::to_string(morph),
        { { "osc_1_spectral_morph_type", (morph + 3) % SynthOscillator::kNumSpectralMorphTypes },
          { "osc_1_spectral_morph_amount", .4f },
          { "osc_1_secondary_spectral_morph_type", morph }, { "osc_1_secondary_spectral_morph_amount", .6f } }, {} });
  }
  for (int model = 0; model < constants::kNumFilterModels; ++model) {
    for (int style = 0; style < 4; ++style) {
      scenarios.push_back({ "filter_" + std::to_string(model) + "_" + std::to_string(style),
          { { "filter_1_on", 1 }, { "filter_1_model", model }, { "filter_1_style", style },
            { "filter_1_resonance", .7f }, { "filter_1_cutoff", 70 } }, {} });
    }
  }
  for (int quality = 0; quality < 4; ++quality) {
    scenarios.push_back({ "quality_" + std::to_string(quality),
        { { "oversampling", quality }, { "osc_1_spectral_morph_type", 2 }, { "osc_1_spectral_morph_amount", .5f },
          { "filter_1_on", 1 }, { "filter_1_model", constants::kComb }, { "filter_1_resonance", .8f } }, {} });
  }
  for (const auto& file : juce::File(factory).findChildFiles(juce::File::findFiles, true, "*.atp"))
    scenarios.push_back({ "preset_" + file.getFileNameWithoutExtension().replaceCharacter(' ', '_').toStdString(), {}, file });

  for (int type = 0; type < constants::kNumEffects; ++type) {
    const std::string effect = strings::kEffectOrder[type];
    scenarios.push_back({ "fx_main_" + effect, {}, {}, [type](SynthPlugin& plugin) {
      selectEffect(plugin, "fx_slot_1_", type);
      setEffectParameters(plugin, "fx_slot_1_", type);
      routeIntoEffect(plugin, "fx_slot_1_", type, "lfo_1", .35f);
    } });
    scenarios.push_back({ "fx_bus_" + effect, { { "osc_1_destination", constants::kBus1 } }, {}, [type](SynthPlugin& plugin) {
      selectEffect(plugin, "bus_1_fx_slot_3_", type);
      setEffectParameters(plugin, "bus_1_fx_slot_3_", type, 17);
      routeIntoEffect(plugin, "bus_1_fx_slot_3_", type, "lfo_2", -.25f);
    } });
  }

  // Effects switched during playback keep the settings and routes of the one
  // a slot leaves, and a slot running several effects in a row.
  const std::pair<int, int> switches[] = {
    { constants::kDelay, constants::kReverb }, { constants::kFilterFx, constants::kGlitchDelay },
    { constants::kChorus, constants::kSpectralGate }, { constants::kCompressor, constants::kTranceGate } };
  for (const auto& [first, second] : switches) {
    const std::string name = std::string("fx_switch_") + strings::kEffectOrder[first] + "_" + strings::kEffectOrder[second];
    scenarios.push_back({ name, {}, {}, [first = first, second = second](SynthPlugin& plugin) {
      selectEffect(plugin, "fx_slot_2_", first);
      setEffectParameters(plugin, "fx_slot_2_", first);
      routeIntoEffect(plugin, "fx_slot_2_", first, "lfo_1", .3f);
      selectEffect(plugin, "fx_slot_2_", second);
      setEffectParameters(plugin, "fx_slot_2_", second, 5);
      routeIntoEffect(plugin, "fx_slot_2_", second, "env_2", .4f);
      selectEffect(plugin, "fx_slot_4_", constants::kDistortion);
      setEffectParameters(plugin, "fx_slot_4_", constants::kDistortion);
    }, [first = first, second = second](SynthPlugin& plugin, int block) {
      if (block == 90) plugin.valueChanged("fx_slot_2_type", float(first + 1));
      if (block == 180) plugin.valueChanged("fx_slot_2_type", float(second + 1));
      if (block == 260) plugin.valueChanged("fx_slot_2_enabled", 0.0f);
      if (block == 300) plugin.valueChanged("fx_slot_2_enabled", 1.0f);
      if (block == 320) plugin.valueChanged("fx_slot_2_type", float(first + 1));
    } });
  }

  // A host automating an FX proxy that drives an effect parameter.
  scenarios.push_back({ "fx_proxy_automation", {}, {}, [](SynthPlugin& plugin) {
    selectEffect(plugin, "fx_slot_1_", constants::kFilterFx);
    setEffectParameters(plugin, "fx_slot_1_", constants::kFilterFx);
    plugin.valueChanged("fx_slot_1_filter_fx_model", constants::kAnalog);
    const int slot = plugin.assignFxParameterToAutomationSlot("fx_slot_1_filter_fx_cutoff");
    expect(slot >= 0, "an FX proxy takes the Filter FX cutoff");
  }, [](SynthPlugin& plugin, int block) {
    if (block % 10 != 0) return;
    if (auto* proxy = hostParameter(plugin, "fx_automation_main_param_1"))
      proxy->setValue(float(block % 200) / 200.0f);
  } });

  // A factory bus preset loaded into bus 1.
  const juce::File bus_preset = juce::File(factory).getParentDirectory()
                                    .getChildFile("FX/Analogue/Analogue Echo - Worn Tape.atfx");
  scenarios.push_back({ "fx_bus_preset", { { "osc_1_destination", constants::kBus1 } }, {}, [bus_preset](SynthPlugin& plugin) {
    String error;
    const bool loaded = bus_preset.existsAsFile() &&
        plugin.loadFxBusPreset(1, json::parse(bus_preset.loadFileAsString().toStdString()), error);
    expect(loaded, "the factory bus preset loads");
  } });

  if (!only.empty())
    scenarios.erase(std::remove_if(scenarios.begin(), scenarios.end(),
        [&](const Scenario& s) { return s.name.find(only) == std::string::npos; }), scenarios.end());
  constexpr int kBlock = 512;
  for (const auto& scenario : scenarios) {
    SynthPlugin plugin;
    // Offline, as a host renders: effects prepare within the block that
    // selects them, so the output does not depend on the worker's timing.
    plugin.setNonRealtime(true);
    plugin.setPlayConfigDetails(0, 2, 48000, kBlock);
    plugin.prepareToPlay(48000, kBlock);
    if (scenario.preset.existsAsFile()) {
      std::string error;
      expect(plugin.loadFromFile(scenario.preset, error), "factory preset loads");
    }
    for (const auto& [name, value] : scenario.values)
      plugin.valueChanged(name, value);
    if (scenario.setup)
      scenario.setup(plugin);
    plugin.getEngine()->prepareSelectedEffects();
    // Impulse responses and tape noise build on background threads, offline
    // too; give them time so the render starts from the finished state.
    juce::MessageManager::getInstance()->runDispatchLoopUntil(250);

    juce::FileOutputStream out(juce::File(directory).getChildFile(scenario.name + ".raw"));
    out.truncate();
    juce::AudioBuffer<float> audio(2, kBlock);
    juce::MidiBuffer midi;
    const int chord[] = { 36, 48, 55, 60, 64, 67, 71, 74, 79, 84, 88 };
    for (int block = 0; block < 400; ++block) {
      midi.clear();
      if (block == 2 || block == 200)
        for (int note : chord) midi.addEvent(juce::MidiMessage::noteOn(1, note + (block == 200 ? 1 : 0), .8f), 17);
      if (block == 150 || block == 330)
        for (int note : chord) midi.addEvent(juce::MidiMessage::noteOff(1, note + (block == 330 ? 1 : 0)), 211);
      if (scenario.during)
        scenario.during(plugin, block);
      audio.clear();
      plugin.processBlock(audio, midi);
      for (int channel = 0; channel < 2; ++channel)
        out.write(audio.getReadPointer(channel), kBlock * sizeof(float));
    }
  }
  std::cout << "rendered scenarios=" << scenarios.size() << '\n';
}

// Compares two --render directories sample for sample. Scenarios whose
// output depends on background loading (impulse responses and tape noise
// build on worker threads, even offline) are reported but never fail.
int compareRenders(const std::string& baseline_directory, const std::string& candidate_directory) {
  static const char* kTimingDependent[] = { "convolution_reverb", "fx_bus_tape" };
  int failed = 0, compared = 0;
  for (const auto& baseline : juce::File(baseline_directory).findChildFiles(juce::File::findFiles, false, "*.raw")) {
    const auto candidate = juce::File(candidate_directory).getChildFile(baseline.getFileName());
    const std::string name = baseline.getFileNameWithoutExtension().toStdString();
    bool timing_dependent = false;
    for (const char* pattern : kTimingDependent) timing_dependent |= name.find(pattern) != std::string::npos;
    juce::MemoryBlock a, b;
    if (!candidate.existsAsFile() || !baseline.loadFileAsData(a) || !candidate.loadFileAsData(b) || a.getSize() != b.getSize()) {
      std::cout << "MISSING " << name << '\n';
      ++failed;
      continue;
    }
    ++compared;
    if (a == b) continue;
    const auto* x = static_cast<const float*>(a.getData());
    const auto* y = static_cast<const float*>(b.getData());
    const size_t count = a.getSize() / sizeof(float);
    size_t first = count;
    double largest = 0.0, signal_power = 0.0, difference_power = 0.0;
    for (size_t i = 0; i < count; ++i) {
      const double difference = std::abs(double(x[i]) - double(y[i]));
      if (difference > 0.0 && first == count) first = i;
      largest = std::max(largest, difference);
      signal_power += double(x[i]) * double(x[i]);
      difference_power += difference * difference;
    }
    // How far below the signal the difference is, in dB.
    const double below_db = signal_power > 0.0 && difference_power > 0.0 ?
        10.0 * std::log10(signal_power / difference_power) : 0.0;
    std::cout << (timing_dependent ? "timing-dependent " : "DIFFERS ") << name << " first_sample=" << first
              << " max_difference=" << largest << " difference_below_signal_db=" << below_db << '\n';
    failed += timing_dependent ? 0 : 1;
  }
  std::cout << "compared=" << compared << " differing=" << failed << '\n';
  return failed == 0 ? 0 : 1;
}

// CPU per host block (512 samples at 48 kHz, an eight-note chord) with each
// effect alone in the first main slot at its default settings, best of five
// runs after the effect has settled. With `only`, just the matching effects.
void effectCpu(const std::string& only) {
  using namespace vital;
  constexpr int kBlock = 512;
  auto measure = [&](int type) {
    SynthPlugin plugin;
    plugin.setPlayConfigDetails(0, 2, 48000, kBlock);
    plugin.prepareToPlay(48000, kBlock);
    if (type >= 0)
      selectEffect(plugin, "fx_slot_1_", type);
    plugin.getEngine()->prepareSelectedEffects();
    // Impulse responses and tape noise load on background threads.
    juce::MessageManager::getInstance()->runDispatchLoopUntil(250);
    juce::AudioBuffer<float> audio(2, kBlock);
    juce::MidiBuffer midi;
    for (int note : { 48, 55, 60, 64, 67, 71, 74, 79 })
      midi.addEvent(juce::MidiMessage::noteOn(1, note, .8f), 0);
    for (int block = 0; block < 200; ++block) {
      audio.clear();
      plugin.processBlock(audio, midi);
      midi.clear();
    }
    double best = std::numeric_limits<double>::max();
    for (int round = 0; round < 5; ++round) {
      const auto start = std::chrono::steady_clock::now();
      for (int block = 0; block < 100; ++block) {
        audio.clear();
        plugin.processBlock(audio, midi);
      }
      best = std::min(best, std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / 100);
    }
    return best;
  };
  const double base = measure(-1);
  std::cout << "no effect us_per_block=" << base << std::endl;
  for (int type = 0; type < constants::kNumEffects; ++type) {
    const std::string name = strings::kEffectOrder[type];
    if (!only.empty() && name.find(only) == std::string::npos)
      continue;
    const double cost = measure(type);
    std::cout << "effect=" << name << " us_per_block=" << cost << " effect_us=" << cost - base << std::endl;
  }
}

// CPU per host block (512 samples at 48 kHz) with no notes playing: the default
// patch and a four-effect chain, at 1x and the default 2x oversampling, then
// every effect alone in a slot at 2x. With an editor open, also the process
// CPU the message thread spends while nothing plays.
void idleCpu(const std::string& only) {
  using namespace vital;
  constexpr int kBlock = 512;
  const double budget_us = 1.0e6 * kBlock / 48000.0;
  auto prepare = [&](SynthPlugin& plugin, float oversampling, const std::vector<int>& effects) {
    plugin.setPlayConfigDetails(0, 2, 48000, kBlock);
    plugin.prepareToPlay(48000, kBlock);
    plugin.valueChanged("oversampling", oversampling);
    for (size_t slot = 0; slot < effects.size(); ++slot)
      selectEffect(plugin, "fx_slot_" + std::to_string(slot + 1) + "_", effects[slot]);
    plugin.getEngine()->prepareSelectedEffects();
    juce::MessageManager::getInstance()->runDispatchLoopUntil(250);
  };
  auto measure = [&](float oversampling, const std::vector<int>& effects) {
    SynthPlugin plugin;
    prepare(plugin, oversampling, effects);
    juce::AudioBuffer<float> audio(2, kBlock);
    juce::MidiBuffer midi;
    for (int block = 0; block < 200; ++block) {
      audio.clear();
      plugin.processBlock(audio, midi);
    }
    double best = std::numeric_limits<double>::max();
    for (int round = 0; round < 5; ++round) {
      const auto start = std::chrono::steady_clock::now();
      for (int block = 0; block < 200; ++block) {
        audio.clear();
        plugin.processBlock(audio, midi);
      }
      best = std::min(best, std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / 200);
    }
    return best;
  };
  auto report = [&](const std::string& name, double us) {
    std::cout << name << " us_per_block=" << us << " budget_percent=" << 100.0 * us / budget_us << std::endl;
  };
  const std::vector<int> chain = { constants::kDistortion, constants::kReverb, constants::kChorus, constants::kDelay };
  if (only.empty() || only == "summary") {
    report("idle default 1x", measure(0.0f, {}));
    report("idle default 2x", measure(1.0f, {}));
    report("idle 4-effect chain 1x", measure(0.0f, chain));
    report("idle 4-effect chain 2x", measure(1.0f, chain));
  }
  if (only.empty() || only == "effects") {
    const double base = measure(1.0f, {});
    for (int type = 0; type < constants::kNumEffects; ++type)
      report(std::string("idle 2x effect=") + strings::kEffectOrder[type], measure(1.0f, { type }) - base);
  }
  if (only.empty() || only == "editor") {
    auto pump = [](int ms) {
      const auto end = juce::Time::getMillisecondCounter() + uint32(ms);
      while (juce::Time::getMillisecondCounter() < end)
        juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
    };
    // User and system CPU seconds of the whole process.
    auto cpu_times = [] {
      rusage usage {};
      getrusage(RUSAGE_SELF, &usage);
      return std::make_pair(usage.ru_utime.tv_sec + 1.0e-6 * usage.ru_utime.tv_usec,
                            usage.ru_stime.tv_sec + 1.0e-6 * usage.ru_stime.tv_usec);
    };
    auto plugin = std::make_unique<SynthPlugin>();
    prepare(*plugin, 1.0f, chain);
    // As in a host, an audio thread renders a block every 512 samples while
    // the message thread runs, so the process CPU includes it and the time
    // per block shows what the editor adds to the audio thread.
    std::atomic<bool> rendering { true };
    std::atomic<int64_t> render_ns { 0 };
    std::atomic<int> render_blocks { 0 };
    std::thread audio_thread([&] {
      juce::AudioBuffer<float> audio(2, kBlock);
      juce::MidiBuffer midi;
      const auto period = std::chrono::microseconds(static_cast<int64_t>(budget_us));
      auto next = std::chrono::steady_clock::now();
      while (rendering.load()) {
        const auto start = std::chrono::steady_clock::now();
        audio.clear();
        plugin->processBlock(audio, midi);
        render_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
        ++render_blocks;
        next += period;
        std::this_thread::sleep_until(next);
      }
    });
    auto measure_cpu = [&](const char* name, int ms) {
      render_ns = 0;
      render_blocks = 0;
      const auto start = cpu_times();
      pump(ms);
      const auto end = cpu_times();
      const double seconds = ms / 1000.0;
      const double block_us = render_blocks > 0 ? 1.0e-3 * render_ns / render_blocks : 0.0;
      std::cout << name << " cpu_percent=" << 100.0 * (end.first - start.first + end.second - start.second) / seconds
                << " user_percent=" << 100.0 * (end.first - start.first) / seconds
                << " system_percent=" << 100.0 * (end.second - start.second) / seconds
                << " audio_us_per_block=" << block_us << std::endl;
    };
    pump(1000);
    measure_cpu("message thread, no editor,", 5000);
    std::unique_ptr<juce::AudioProcessorEditor> editor(plugin->createEditorIfNeeded());
    editor->addToDesktop(juce::ComponentPeer::windowHasTitleBar);
    editor->setVisible(true);
    pump(2000);
    measure_cpu("message thread, editor open,", 5000);
    // The editor refreshes its readouts faster while the user works in it.
    editor->toFront(true);
    if (auto* first = editor->getChildComponent(0)) first->grabKeyboardFocus();
    pump(500);
    if (editor->hasKeyboardFocus(true))
      measure_cpu("message thread, editor focused,", 5000);
    else
      std::cout << "editor could not take the keyboard focus" << std::endl;
    plugin->editorBeingDeleted(editor.get());
    editor.reset();
    rendering = false;
    audio_thread.join();
  }
}

// A slot sleeps once its input and output have stayed silent for its hold
// time, and wakes on the next note's first block. Every effect, alone in the
// main chain's first slot at its defaults: it either keeps sounding (noise) or
// sleeps, and after sleeping a note sounds through it at once. A 4 s Delay
// with feedback must not sleep between its echoes.
void effectSleep() {
  using namespace vital;
  constexpr int kBlock = 512;
  constexpr int kRate = 48000;
  auto prepare = [&](SynthPlugin& plugin, int type) {
    plugin.setPlayConfigDetails(0, 2, kRate, kBlock);
    plugin.prepareToPlay(kRate, kBlock);
    selectEffect(plugin, "fx_slot_1_", type);
    plugin.getEngine()->prepareSelectedEffects();
    juce::MessageManager::getInstance()->runDispatchLoopUntil(250);
  };
  auto peak = [](const juce::AudioBuffer<float>& audio) {
    return std::max(audio.getMagnitude(0, 0, audio.getNumSamples()), audio.getMagnitude(1, 0, audio.getNumSamples()));
  };
  for (int type = 0; type < constants::kNumEffects; ++type) {
    const std::string name = strings::kEffectOrder[type];
    SynthPlugin plugin;
    prepare(plugin, type);
    auto* chain = plugin.getEngine()->getMainEffectChain();
    juce::AudioBuffer<float> audio(2, kBlock);
    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(1, 60, .8f), 0);
    for (int block = 0; block < 20; ++block) {
      audio.clear();
      plugin.processBlock(audio, midi);
      midi.clear();
    }
    midi.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
    int slept_block = -1;
    for (int block = 0; block < 60 * kRate / kBlock && slept_block < 0; ++block) {
      audio.clear();
      plugin.processBlock(audio, midi);
      midi.clear();
      if (chain->isSlotAsleep(0))
        slept_block = block;
    }
    if (slept_block < 0) {
      std::cout << "effect=" << name << " keeps sounding" << std::endl;
      continue;
    }
    // Asleep, the slot passes its silent input through.
    float asleep_peak = 0.0f;
    for (int block = 0; block < 40; ++block) {
      audio.clear();
      plugin.processBlock(audio, midi);
      asleep_peak = std::max(asleep_peak, peak(audio));
    }
    expect(chain->isSlotAsleep(0), name + " stays asleep in silence");
    expect(asleep_peak < 1.0e-4f, name + " is silent while asleep");
    midi.addEvent(juce::MidiMessage::noteOn(1, 64, .8f), 0);
    float woken_peak = 0.0f;
    for (int block = 0; block < 10; ++block) {
      audio.clear();
      plugin.processBlock(audio, midi);
      midi.clear();
      if (block == 0)
        expect(!chain->isSlotAsleep(0), name + " wakes on the note's first block");
      woken_peak = std::max(woken_peak, peak(audio));
    }
    expect(woken_peak > 1.0e-3f, name + " sounds after waking");
    std::cout << "effect=" << name << " slept_after_s=" << (slept_block + 1) * double(kBlock) / kRate
              << " woken_peak=" << woken_peak << std::endl;
  }

  // Echoes 4 s apart: the slot must stay awake through the gap.
  SynthPlugin plugin;
  prepare(plugin, constants::kDelay);
  plugin.valueChanged("fx_slot_1_delay_sync", 0.0f);
  plugin.valueChanged("fx_slot_1_delay_frequency", -2.0f);
  plugin.valueChanged("fx_slot_1_delay_feedback", 0.5f);
  auto* chain = plugin.getEngine()->getMainEffectChain();
  juce::AudioBuffer<float> audio(2, kBlock);
  juce::MidiBuffer midi;
  midi.addEvent(juce::MidiMessage::noteOn(1, 60, .8f), 0);
  midi.addEvent(juce::MidiMessage::noteOff(1, 60), kBlock / 2);
  float echo_peak = 0.0f;
  bool slept_before_echo = false;
  for (int block = 0; block < 5 * kRate / kBlock; ++block) {
    audio.clear();
    plugin.processBlock(audio, midi);
    midi.clear();
    const double seconds = block * double(kBlock) / kRate;
    if (seconds < 3.8 && chain->isSlotAsleep(0)) slept_before_echo = true;
    if (seconds > 3.8 && seconds < 4.6) echo_peak = std::max(echo_peak, peak(audio));
  }
  expect(!slept_before_echo, "a 4 s Delay stays awake between echoes");
  expect(echo_peak > 1.0e-3f, "a 4 s Delay's echo still sounds");
  std::cout << "4 s delay echo_peak=" << echo_peak << std::endl;
}

// Renders blocks of 512 at 48 kHz for `seconds` of audio, holding `notes`
// notes of the default patch (none: idle), for a profiler to sample. With
// `preset`, that patch instead. ATLAS_LIST_MODULATION_SUMS=1 lists the
// modulation totals that run every block.
void cpuLoop(double seconds, int notes, const std::string& preset) {
  constexpr int kBlock = 512;
  SynthPlugin plugin;
  plugin.setPlayConfigDetails(0, 2, 48000, kBlock);
  plugin.prepareToPlay(48000, kBlock);
  if (!preset.empty()) {
    std::string error;
    expect(plugin.loadFromFile(juce::File(preset), error), "the preset loads");
  }
  plugin.getEngine()->prepareSelectedEffects();
  juce::MessageManager::getInstance()->runDispatchLoopUntil(250);
  if (std::getenv("ATLAS_LIST_MODULATION_SUMS")) {
    // Audio-rate modulation totals that run every block, mono and per voice.
    for (const auto& destination : plugin.getEngine()->getMonoModulationDestinations())
      if (dynamic_cast<vital::ModulationSum*>(destination.second) != nullptr && destination.second->enabled())
        std::cout << "mono_sum " << destination.first << std::endl;
    for (const auto& destination : plugin.getEngine()->getPolyModulationDestinations())
      if (dynamic_cast<vital::ModulationSum*>(destination.second) != nullptr && destination.second->enabled())
        std::cout << "poly_sum " << destination.first << std::endl;
  }
  juce::AudioBuffer<float> audio(2, kBlock);
  juce::MidiBuffer midi;
  const int chord[] = { 48, 55, 60, 64, 67, 71, 74, 79 };
  for (int i = 0; i < notes && i < 8; ++i)
    midi.addEvent(juce::MidiMessage::noteOn(1, chord[i], .8f), 0);
  const int blocks = static_cast<int>(seconds * 48000 / kBlock);
  const auto start = std::chrono::steady_clock::now();
  for (int block = 0; block < blocks; ++block) {
    audio.clear();
    plugin.processBlock(audio, midi);
    midi.clear();
  }
  const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / blocks;
  std::cout << "notes=" << notes << " us_per_block=" << us << std::endl;
}

// The arpeggiator's cost in realtime blocks of 512 at 48 kHz: a preset (or the
// default patch) holding a four-note chord, with and without Arp 1 at 1/16 and
// a Distortion, Reverb, Chorus and Delay chain. Prints the mean and worst
// block time against the block's budget, and the most voices at once.
void arpeggiatorCpu(const std::string& preset_path) {
  using namespace vital;
  constexpr int kBlock = 512;
  constexpr int kBlocks = 1000;
  struct Result { double mean_us = 0, worst_us = 0; int max_voices = 0; };
  auto run = [&](bool arp, bool effects, float release_seconds = -1.0f) {
    SynthPlugin plugin;
    plugin.setPlayConfigDetails(0, 2, 48000, kBlock);
    plugin.prepareToPlay(48000, kBlock);
    if (!preset_path.empty()) {
      std::string error;
      expect(plugin.loadFromFile(juce::File(preset_path), error), "the preset loads");
    }
    if (arp) {
      plugin.valueChanged("arp_1_on", 1.0f);
      plugin.valueChanged("arp_1_sync_rate", 13.0f);  // 1/16
    }
    if (release_seconds > 0.0f)
      plugin.valueChanged("env_1_release", std::pow(release_seconds, 0.25f));
    if (effects) {
      const int chain[] = { constants::kDistortion, constants::kReverb, constants::kChorus, constants::kDelay };
      for (int slot = 0; slot < 4; ++slot)
        selectEffect(plugin, "fx_slot_" + std::to_string(slot + 1) + "_", chain[slot]);
    }
    plugin.getEngine()->prepareSelectedEffects();
    juce::MessageManager::getInstance()->runDispatchLoopUntil(200);
    juce::AudioBuffer<float> audio(2, kBlock);
    juce::MidiBuffer midi;
    for (int note : { 48, 55, 60, 64 })
      midi.addEvent(juce::MidiMessage::noteOn(1, note, .8f), 0);
    Result result;
    for (int block = 0; block < kBlocks; ++block) {
      audio.clear();
      const auto start = std::chrono::steady_clock::now();
      plugin.processBlock(audio, midi);
      const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
      midi.clear();
      result.mean_us += us / kBlocks;
      result.worst_us = std::max(result.worst_us, us);
      result.max_voices = std::max(result.max_voices, plugin.getEngine()->getNumActiveVoices());
    }
    return result;
  };
  std::cout << "block budget us=" << kBlock * 1.0e6 / 48000 << std::endl;
  for (bool effects : { false, true }) {
    for (bool arp : { false, true }) {
      const Result result = run(arp, effects);
      std::cout << "arp=" << arp << " effects=" << effects << " mean_us=" << result.mean_us
                << " worst_us=" << result.worst_us << " max_voices=" << result.max_voices << std::endl;
    }
  }
  // A release longer than the time between repeats of a pitch: each tail
  // should end after its own release, so about notes/s x release voices ring.
  for (float release : { 0.5f, 1.0f }) {
    const Result result = run(true, false, release);
    std::cout << "arp=1 effects=0 release_s=" << release << " mean_us=" << result.mean_us
              << " worst_us=" << result.worst_us << " max_voices=" << result.max_voices << std::endl;
  }
}

// CPU per host block (512 samples at 48 kHz, default 2x quality) of the
// voice filters' heaviest models against Analog, with Filter 1 on and 1, 2 or
// 8 held notes; best of five runs of 100 blocks.
void voiceFilterCpu() {
  using namespace vital;
  constexpr int kBlock = 512;
  struct Case { const char* name; int model; float reverb_cutoff_hz; };
  const Case cases[] = { { "analog", constants::kAnalog, 0.0f },
                         { "reverb_100hz", constants::kReverbFilter, 100.0f },
                         { "reverb_500hz", constants::kReverbFilter, 500.0f },
                         { "scream", constants::kScreamFilter, 0.0f } };
  const int chord[] = { 48, 55, 60, 64, 67, 71, 74, 79 };
  for (int notes : { 1, 2, 8 }) {
    for (const auto& filter : cases) {
      SynthPlugin plugin;
      plugin.setPlayConfigDetails(0, 2, 48000, kBlock);
      plugin.prepareToPlay(48000, kBlock);
      plugin.valueChanged("filter_1_on", 1.0f);
      plugin.valueChanged("filter_1_model", float(filter.model));
      if (filter.reverb_cutoff_hz > 0.0f)
        plugin.valueChanged("filter_1_reverb_cutoff", std::log2(filter.reverb_cutoff_hz));
      juce::AudioBuffer<float> audio(2, kBlock);
      juce::MidiBuffer midi;
      for (int i = 0; i < notes; ++i)
        midi.addEvent(juce::MidiMessage::noteOn(1, chord[i], .8f), 0);
      for (int block = 0; block < 100; ++block) {
        audio.clear();
        plugin.processBlock(audio, midi);
        midi.clear();
      }
      double best = std::numeric_limits<double>::max();
      for (int round = 0; round < 5; ++round) {
        const auto start = std::chrono::steady_clock::now();
        for (int block = 0; block < 100; ++block) {
          audio.clear();
          plugin.processBlock(audio, midi);
        }
        best = std::min(best, std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / 100);
      }
      std::cout << "filter=" << filter.name << " notes=" << notes << " us_per_block=" << best
                << " voices=" << plugin.getEngine()->getNumActiveVoices() << std::endl;
    }
  }
}

// Time of one transform of the sizes Atlas uses on the audio thread: the
// wavetable oscillator's 2048-point real inverse (vital::FourierTransform),
// and juce::dsp::FFT at 2048 and 8192 points (spectral effects).
void fftCpu() {
  auto time = [](const char* name, int iterations, auto&& function) {
    for (int i = 0; i < iterations / 10; ++i) function();
    double best = std::numeric_limits<double>::max();
    for (int round = 0; round < 5; ++round) {
      const auto start = std::chrono::steady_clock::now();
      for (int i = 0; i < iterations; ++i) function();
      best = std::min(best, std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / iterations);
    }
    std::cout << name << " us=" << best << std::endl;
  };
  juce::Random random(7);
  {
    constexpr int kBits = 11, kSize = 1 << kBits;
    vital::FourierTransform transform(kBits);
    std::vector<float> spectrum(2 * kSize + 2), data(2 * kSize + 2);
    for (int i = 0; i < kSize + 2; ++i) spectrum[i] = random.nextFloat() - .5f;
    time("wavetable inverse 2048 (copy+transformRealInverse)", 20000, [&] {
      std::copy(spectrum.begin(), spectrum.end(), data.begin());
      transform.transformRealInverse(data.data());
    });
    time("wavetable forward 2048 (copy+transformRealForward)", 20000, [&] {
      std::copy(spectrum.begin(), spectrum.end(), data.begin());
      transform.transformRealForward(data.data());
    });
  }
  for (int order : { 11, 13 }) {
    juce::dsp::FFT fft(order);
    const int size = 1 << order;
    std::vector<float> input(2 * size), data(2 * size);
    for (int i = 0; i < size; ++i) input[i] = random.nextFloat() - .5f;
    const std::string label = "juce::dsp::FFT real forward+inverse " + std::to_string(size);
    time(label.c_str(), order == 11 ? 20000 : 4000, [&] {
      std::copy(input.begin(), input.end(), data.begin());
      fft.performRealOnlyForwardTransform(data.data(), true);
      fft.performRealOnlyInverseTransform(data.data());
    });
  }
}

// How long a host's audio thread waits for Atlas's callback lock while the
// message thread restores state, loads a preset, loads, saves and clears an
// FX bus, and synthesizes the patch to a wavetable. A second thread plays the
// host: each block takes the lock, then renders unless processing is
// suspended, as JUCE's VST3 wrapper does. Notes are held throughout.
void callbackLockWait() {
  using namespace vital;
  constexpr int kBlock = 512;
  SynthPlugin plugin;
  plugin.setPlayConfigDetails(0, 2, 48000, kBlock);
  plugin.prepareToPlay(48000, kBlock);
  const int heavy[] = { constants::kReverb, constants::kConvolutionReverb, constants::kDelay,
                        constants::kSpectralGate, constants::kTape, constants::kGlitchDelay };
  auto stateWith = [&](int rotation) {
    for (int slot = 0; slot < 6; ++slot) {
      const std::string prefix = "fx_slot_" + std::to_string(slot + 1) + "_";
      const int type = heavy[(slot + rotation) % 6];
      selectEffect(plugin, prefix, type);
      setEffectParameters(plugin, prefix, type, uint32_t(rotation));
    }
    juce::MemoryBlock state;
    plugin.getStateInformation(state);
    return state;
  };
  const juce::MemoryBlock state_a = stateWith(0), state_b = stateWith(3);
  // A bus of ten effects to save, load and clear.
  const int bus_effects[] = { constants::kChorus, constants::kCompressor, constants::kDelay, constants::kDistortion,
                              constants::kEq, constants::kFilterFx, constants::kPhaser, constants::kReverb,
                              constants::kSpectralCompressor, constants::kGlitchDelay };
  for (int slot = 0; slot < 10; ++slot) {
    const std::string prefix = "bus_1_fx_slot_" + std::to_string(slot + 1) + "_";
    selectEffect(plugin, prefix, bus_effects[slot]);
    setEffectParameters(plugin, prefix, bus_effects[slot], 5);
  }
  const json bus_preset = plugin.fxBusPreset(1);
  juce::TemporaryFile preset_file(".atp");
  preset_file.getFile().replaceWithText(state_b.toString());
  plugin.setStateInformation(state_a.getData(), int(state_a.getSize()));
  juce::MessageManager::getInstance()->runDispatchLoopUntil(300);

  std::atomic<bool> running { true };
  std::atomic<double> worst_wait_ms { 0.0 };
  std::atomic<int> blocks { 0 };
  std::thread host([&] {
    juce::AudioBuffer<float> audio(2, kBlock);
    juce::MidiBuffer midi;
    for (int note : { 48, 55, 60, 64 })
      midi.addEvent(juce::MidiMessage::noteOn(1, note, .8f), 0);
    while (running.load()) {
      const auto start = std::chrono::steady_clock::now();
      {
        const juce::ScopedLock lock(plugin.getCallbackLock());
        const double waited = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        double worst = worst_wait_ms.load();
        while (waited > worst && !worst_wait_ms.compare_exchange_weak(worst, waited)) {}
        if (!plugin.isSuspended()) {
          audio.clear();
          plugin.processBlock(audio, midi);
          midi.clear();
        }
      }
      blocks.fetch_add(1);
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  });
  auto measure = [&](const char* name, auto&& action) {
    juce::MessageManager::getInstance()->runDispatchLoopUntil(200);
    worst_wait_ms = 0.0;
    const int first_block = blocks.load();
    const auto start = std::chrono::steady_clock::now();
    action();
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
    std::cout << name << " ms=" << ms << " worst_lock_wait_ms=" << worst_wait_ms.load()
              << " host_blocks=" << blocks.load() - first_block << std::endl;
    // Processing is paused (or only read) throughout: the host's audio
    // thread gets silence from Atlas, never a wait as long as a block.
    expect(worst_wait_ms.load() < 10.0, std::string(name) + " keeps the host's audio thread waiting");
  };
  for (int pass = 0; pass < 2; ++pass) {
    measure("setStateInformation (effects rebuilt)", [&] {
      plugin.setStateInformation(state_b.getData(), int(state_b.getSize()));
    });
    measure("setStateInformation (back)", [&] {
      plugin.setStateInformation(state_a.getData(), int(state_a.getSize()));
    });
    measure("editor preset load (loadFromFile)", [&] {
      std::string error;
      plugin.loadFromFile(preset_file.getFile(), error);
    });
    measure("FX bus load (10 effects)", [&] {
      juce::String error;
      plugin.loadFxBusPreset(2, bus_preset, error);
    });
    measure("FX bus preset save (10 effects)", [&] { plugin.fxBusPreset(2); });
    measure("FX bus clear", [&] { plugin.clearFxBus(2); });
  }
  measure("synthesize preset to wavetable", [&] {
    std::vector<float> capture(4 * 48000);
    plugin.renderAudioForResynthesis(capture.data(), int(capture.size()), 16);
  });
  running = false;
  host.join();
}

// Saving, loading into a fresh instance and saving again gives identical state,
// including settings of effects a slot no longer selects, routes into them,
// and FX automation assignments to them.
void stateRoundTrip() {
  using namespace vital;
  {
    // Every effect of every slot and bus has its controls; their defaults
    // are left out (loading restores a missing control to its default).
    SynthPlugin fresh;
    fresh.setPlayConfigDetails(0, 2, 48000, 512);
    fresh.prepareToPlay(48000, 512);
    juce::MemoryBlock state;
    fresh.getStateInformation(state);
    std::cout << "default state KiB=" << state.getSize() / 1024 << std::endl;
    expect(state.getSize() < 512 * 1024, "a default state leaves parked effects' default values out");
  }
  json first;
  {
    SynthPlugin plugin;
    plugin.setPlayConfigDetails(0, 2, 48000, 512);
    plugin.prepareToPlay(48000, 512);
    selectEffect(plugin, "fx_slot_1_", constants::kDelay);
    setEffectParameters(plugin, "fx_slot_1_", constants::kDelay);
    const std::string delay_route = routeIntoEffect(plugin, "fx_slot_1_", constants::kDelay, "lfo_1", .3f);
    expect(!delay_route.empty(), "a route reaches the Delay");
    expect(plugin.assignFxParameterToAutomationSlot("fx_slot_1_delay_feedback") >= 0, "an FX proxy takes the Delay feedback");
    selectEffect(plugin, "fx_slot_1_", constants::kReverb);
    setEffectParameters(plugin, "fx_slot_1_", constants::kReverb, 3);
    routeIntoEffect(plugin, "fx_slot_1_", constants::kReverb, "env_2", .5f);
    selectEffect(plugin, "bus_2_fx_slot_5_", constants::kGlitchDelay);
    setEffectParameters(plugin, "bus_2_fx_slot_5_", constants::kGlitchDelay, 11);
    routeIntoEffect(plugin, "bus_2_fx_slot_5_", constants::kGlitchDelay, "lfo_3", -.2f);
    selectEffect(plugin, "bus_2_fx_slot_5_", constants::kTranceGate);
    setEffectParameters(plugin, "bus_2_fx_slot_5_", constants::kTranceGate, 7);
    juce::MemoryBlock state;
    plugin.getStateInformation(state);
    first = json::parse(state.toString().toStdString());
    expect(first["settings"].value("fx_slot_1_delay_feedback", -1.0f) != Parameters::getDetails("fx_slot_1_delay_feedback").default_value,
           "the unselected Delay keeps its settings");
  }
  juce::MemoryBlock state;
  {
    juce::MemoryBlock saved;
    SynthPlugin plugin;
    plugin.setPlayConfigDetails(0, 2, 48000, 512);
    plugin.prepareToPlay(48000, 512);
    const std::string text = first.dump();
    plugin.setStateInformation(text.data(), int(text.size()));
    plugin.getStateInformation(state);
  }
  const json second = json::parse(state.toString().toStdString());
  auto effect_routes = [](const json& data) {
    int routes = 0;
    for (const auto& modulation : data["settings"]["modulations"])
      routes += modulation["destination"].get<std::string>().find("fx_slot_") != std::string::npos;
    return routes;
  };
  expect(effect_routes(first) == 3, "three routes reach effects, two of them no longer selected");
  expect(effect_routes(second) == 3, "routes into effects survive a reload");
  expect(first.count("fx_automation_slots") && first["fx_automation_slots"].dump().find("fx_slot_1_delay_feedback") != std::string::npos,
         "the FX proxy assignment to the unselected Delay is saved");
  int differences = 0;
  for (auto it = first.begin(); it != first.end(); ++it) {
    if (!second.count(it.key()) || second[it.key()] != it.value()) {
      if (it.value().is_object() && second.count(it.key()) && second[it.key()].is_object()) {
        for (auto inner = it.value().begin(); inner != it.value().end(); ++inner)
          if (!second[it.key()].count(inner.key()) || second[it.key()][inner.key()] != inner.value())
            if (++differences <= 10) std::cout << "differs: " << it.key() << "/" << inner.key() << '\n';
      }
      else if (++differences <= 10)
        std::cout << "differs: " << it.key() << '\n';
    }
  }
  for (auto it = second.begin(); it != second.end(); ++it)
    if (!first.count(it.key()) && ++differences <= 10) std::cout << "only after reload: " << it.key() << '\n';
  std::cout << "state round trip differences=" << differences << '\n';
  expect(differences == 0, "saving, loading and saving again gives the same state");
}

// Footprint of one instance as a host sees it: construction, playback, then
// the editor opened in a window, closed and opened again.
void editorMemory() {
  auto pump = [](int ms) {
    const auto end = juce::Time::getMillisecondCounter() + uint32(ms);
    while (juce::Time::getMillisecondCounter() < end)
      juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
  };
  double last = footprintMiB();
  const double start = last;
  auto report = [&](const char* stage) {
    const double now = footprintMiB();
    std::cout << stage << " total_MiB=" << now << " step_MiB=" << now - last << " since_start_MiB=" << now - start;
#if JUCE_WINDOWS
    std::cout << " commit_MiB=" << commitMiB();
#endif
    std::cout << std::endl;
    last = now;
  };
  report("process start");
  auto plugin = std::make_unique<SynthPlugin>();
  report("constructed");
  plugin->setPlayConfigDetails(0, 2, 48000, 512);
  plugin->prepareToPlay(48000, 512);
  report("prepared");
  juce::AudioBuffer<float> audio(2, 512);
  juce::MidiBuffer midi;
  for (int block = 0; block < 200; ++block) {
    midi.clear();
    if (block == 0) for (int note : { 48, 55, 60, 64 }) midi.addEvent(juce::MidiMessage::noteOn(1, note, .8f), 0);
    audio.clear();
    plugin->processBlock(audio, midi);
  }
  report("played");
  // A host restoring a project hands the instance its saved state.
  for (int restore = 0; restore < 2; ++restore) {
    juce::MemoryBlock state;
    plugin->getStateInformation(state);
    std::cout << "saved state KiB=" << state.getSize() / 1024.0 << std::endl;
    plugin->setStateInformation(state.getData(), int(state.getSize()));
    for (int block = 0; block < 200; ++block) {
      midi.clear();
      if (block == 0) for (int note : { 48, 55, 60, 64 }) midi.addEvent(juce::MidiMessage::noteOn(1, note, .8f), 0);
      audio.clear();
      plugin->processBlock(audio, midi);
    }
    pump(300);
    report(restore == 0 ? "state restored and played" : "state restored again and played");
  }
  std::unique_ptr<juce::AudioProcessorEditor> editor(plugin->createEditorIfNeeded());
  report("editor created");
  editor->addToDesktop(juce::ComponentPeer::windowHasTitleBar);
  editor->setVisible(true);
  pump(2000);
  report("editor shown");
  if (const char* hold = std::getenv("ATLAS_CONSTRUCTION_HOLD_SECONDS")) {
    std::cout << "holding pid=" << getpid() << std::endl;
    pump(1000 * std::atoi(hold));
  }
  plugin->editorBeingDeleted(editor.get());
  editor.reset();
  pump(500);
  report("editor closed");
  editor.reset(plugin->createEditorIfNeeded());
  editor->addToDesktop(juce::ComponentPeer::windowHasTitleBar);
  editor->setVisible(true);
  pump(2000);
  report("editor reopened");
  plugin->editorBeingDeleted(editor.get());
  editor.reset();
  plugin.reset();
  pump(500);
  report("destroyed");
}

// Per effect type: how many parameter controls, and how many are modulated,
// smoothed and audio-rate (the shells' parameter cost).
void describeEffectControls() {
  using namespace vital;
  cr::Value bpm(2), keytrack(0);
  EffectFactoryProbe probe(bpm.output(), keytrack.output());
  int totals[4] = {};
  for (int type = 0; type < constants::kNumEffects; ++type) {
    std::unique_ptr<SynthModule> module(probe.createEffectDsp(type, "fx_slot_1_"));
    module->init();
    const auto controls = module->getControls();
    const auto destinations = module->getMonoModulationDestinations();
    int counts[4] = { int(controls.size()), 0, 0, 0 };
    for (const auto& control : controls) {
      counts[1] += destinations.count(control.first) != 0;
      counts[2] += dynamic_cast<const SmoothValue*>(control.second) != nullptr ||
                   dynamic_cast<const cr::SmoothValue*>(control.second) != nullptr;
      counts[3] += !control.second->isControlRate();
    }
    std::cout << strings::kEffectOrder[type] << " controls=" << counts[0] << " modulated=" << counts[1]
              << " smoothed=" << counts[2] << " audio_rate=" << counts[3] << '\n';
    for (int i = 0; i < 4; ++i) totals[i] += counts[i];
  }
  std::cout << "all controls=" << totals[0] << " modulated=" << totals[1] << " smoothed=" << totals[2]
            << " audio_rate=" << totals[3] << '\n';
}

// Counts the host parameters by group (the first words of their names, or
// the automation proxies); with `list`, prints every ID and name too.
void hostParameterSummary(bool list) {
  SynthPlugin plugin;
  std::map<std::string, int> groups;
  for (auto* parameter : plugin.getParameters()) {
    const String name = parameter->getName(200);
    String group;
    if (name.contains(" Param ") && (name.startsWith("Osc ") || name.startsWith("FX ")))
      group = name.upToFirstOccurrenceOf(" ", false, false) + " automation proxies";
    else {
      const int words = name.startsWith("Oscillator ") ? 3 : 1;
      juce::StringArray tokens = juce::StringArray::fromTokens(name, " ", "");
      tokens.removeRange(words, tokens.size());
      group = tokens.joinIntoString(" ");
    }
    groups[group.toStdString()]++;
    if (list) {
      auto* with_id = dynamic_cast<juce::AudioProcessorParameterWithID*>(parameter);
      std::cout << (with_id ? with_id->getParameterID() : String()) << " | " << name << '\n';
    }
  }
  std::cout << "host parameters=" << plugin.getParameters().size() << '\n';
  for (const auto& [group, count] : groups) std::cout << count << "  " << group << '\n';
}

#include "ir_switching_tests.h"

int main(int argc, char** argv) {
  juce::ScopedJuceInitialiser_GUI gui;
  if (argc > 1 && String(argv[1]) == "--describe-effects") {
    describeEffectControls();
    return 0;
  }
  if (argc > 1 && String(argv[1]) == "--host-parameters") {
    hostParameterSummary(argc > 2 && String(argv[2]) == "list");
    return 0;
  }
  if (argc > 1 && String(argv[1]) == "--editor-memory") {
    editorMemory();
    return failures ? 1 : 0;
  }
  if (argc > 1 && String(argv[1]) == "--state-round-trip") {
    stateRoundTrip();
    return failures ? 1 : 0;
  }
  if (argc > 3 && String(argv[1]) == "--compare")
    return compareRenders(argv[2], argv[3]);
  if (argc > 1 && String(argv[1]) == "--effect-cpu") {
    effectCpu(argc > 2 ? argv[2] : "");
    return failures ? 1 : 0;
  }
  if (argc > 1 && String(argv[1]) == "--effect-sleep") {
    effectSleep();
    return failures ? 1 : 0;
  }
  if (argc > 3 && String(argv[1]) == "--cpu-loop") {
    cpuLoop(String(argv[2]).getDoubleValue(), String(argv[3]).getIntValue(), argc > 4 ? argv[4] : "");
    return failures ? 1 : 0;
  }
  if (argc > 1 && String(argv[1]) == "--idle-cpu") {
    idleCpu(argc > 2 ? argv[2] : "");
    return failures ? 1 : 0;
  }
  if (argc > 1 && String(argv[1]) == "--arp-cpu") {
    arpeggiatorCpu(argc > 2 ? argv[2] : "");
    return failures ? 1 : 0;
  }
  if (argc > 1 && String(argv[1]) == "--filter-cpu") {
    voiceFilterCpu();
    return failures ? 1 : 0;
  }
  if (argc > 1 && String(argv[1]) == "--fft-cpu") {
    fftCpu();
    return failures ? 1 : 0;
  }
  if (argc > 1 && String(argv[1]) == "--lock-wait") {
    callbackLockWait();
    return failures ? 1 : 0;
  }
  if (argc > 3 && String(argv[1]) == "--render") {
    renderScenarios(argv[2], argv[3], argc > 4 ? argv[4] : "");
    return failures ? 1 : 0;
  }
  if (argc > 1 && String(argv[1]) == "--ir-switching") {
    irSwitchingKeepsLiveEngine();
    asyncIrLoadingIsLatestWins();
    irTrimAtEndIsSafe();
    return failures ? 1 : 0;
  }
  if (argc > 1 && String(argv[1]) == "--construction") {
    constructionCost(argc > 2 && String(argv[2]) == "chain",
                     argc > 3 && String(argv[2]) == "type" ? String(argv[3]).getIntValue() : -1);
    return failures ? 1 : 0;
  }
  if (argc > 1 && String(argv[1]) == "--convolution-threading") {
    concurrentConvolutionLifecycle();
    return failures ? 1 : 0;
  }
  compareEffects();
  realtimeTransitions();
  convolutionStateRoundTrip();
  cancelledPreparationIsReleased();
  slotFilterTable();
  controlsReservedOnSelection();
  parkedEffectWiring();
  return failures ? 1 : 0;
}

#if JUCE_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
namespace {
// PROCESS_MEMORY_COUNTERS_EX2 (Windows 10 1809 and later), declared here
// because the SDK hides it behind version macros.
struct MemoryCounters {
  PROCESS_MEMORY_COUNTERS_EX base;
  SIZE_T PrivateWorkingSetSize;
  ULONG64 SharedCommitUsage;
};
bool readMemoryCounters(MemoryCounters& counters) {
  counters = {};
  counters.base.cb = sizeof(counters);
  return GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                              sizeof(counters)) != 0;
}
double windowsPrivateWorkingSetMiB() {
  MemoryCounters counters;
  return readMemoryCounters(counters) ? counters.PrivateWorkingSetSize / 1048576.0 : 0;
}
double windowsCommitMiB() {
  MemoryCounters counters;
  return readMemoryCounters(counters) ? counters.base.PrivateUsage / 1048576.0 : 0;
}
}
#endif
