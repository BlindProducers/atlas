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
#include "synth_plugin.h"
#include "sound_engine.h"
#include "value_switch.h"
#include <cstdlib>
#include <new>
#if JUCE_MAC
#include <execinfo.h>
#endif
#include <chrono>
#include <iostream>
#include <thread>

namespace {
thread_local bool track_allocations = false;
thread_local size_t audio_allocations = 0;
#if JUCE_MAC
void* allocation_stacks[12][20] {};
int allocation_depths[12] {};
#endif
}
void* operator new(std::size_t size) {
  if (track_allocations) {
#if JUCE_MAC
    if (audio_allocations < 12) allocation_depths[audio_allocations] = backtrace(allocation_stacks[audio_allocations], 20);
#endif
    ++audio_allocations;
  }
  if (void* memory = std::malloc(std::max(size, size_t(1)))) return memory;
  throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace {
int failures = 0;
void expect(bool ok, const std::string& message) {
  if (!ok) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
std::unique_ptr<vital::SynthModule> create(int type, vital::Output* bpm) {
  using namespace vital;
  switch (type) {
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
  }
  return {};
}
void compareEffects() {
  using namespace vital;
  cr::Value bpm(2);
  auto worker = std::make_shared<EffectPreparationWorker>();
  for (int type : { constants::kChorus, constants::kDelay, constants::kReverb,
       constants::kPhaseShift, constants::kDimensionExpander, constants::kSpectralGate,
       constants::kSpectralLock, constants::kSpectralCompressor, constants::kSuppressor, constants::kTape,
       constants::kAnalogueChorus, constants::kAnalogueFlanger, constants::kAnalogueEcho,
       constants::kAnalogueRotary, constants::kAnalogueSpring, constants::kAnalogueWah,
       constants::kSpectralBalance, constants::kGlitchDelay }) {
    for (int factor : { 1, 2, 4, 8 }) {
      auto original = create(type, bpm.output());
      PreparedEffectModule prepared(type, "", [&] { return create(type, bpm.output()); }, worker);
      for (auto* module : { original.get(), static_cast<SynthModule*>(&prepared) }) {
        module->init(); module->setSampleRate(48000); module->setOversampleAmount(factor);
      }
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
      double error = 0, peak = 0;
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
            error = std::max(error, double(std::abs(a - b))); peak = std::max(peak, double(std::abs(a)));
          }
      }
      std::cout << "effect=" << type << " factor=" << factor << " error=" << error << " peak=" << peak << '\n';
      expect(error < 2e-5, "unchanged effect audio, type=" + std::to_string(type));
      prepared.enable(false); prepared.enable(true); prepared.prepareForPlayback();
      original->enable(false); original->enable(true);
      original->processWithInput(input.data(), int(input.size()));
      prepared.processWithInput(input.data(), int(input.size()));
      expect(prepared.getControls().begin()->second == after.begin()->second, "control addresses survive preparation");
    }
  }
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
          constants::kSpectralGate, constants::kDelay, constants::kConvolutionReverb, constants::kSpectralLock };
      type2->set(effects[(block / 25) % 6] + 1);
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

}
#include "ir_switching_tests.h"

int main(int argc, char** argv) {
  juce::ScopedJuceInitialiser_GUI gui;
  if (argc > 1 && String(argv[1]) == "--ir-switching") {
    irSwitchingKeepsLiveEngine();
    asyncIrLoadingIsLatestWins();
    irTrimAtEndIsSafe();
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
  return failures ? 1 : 0;
}
