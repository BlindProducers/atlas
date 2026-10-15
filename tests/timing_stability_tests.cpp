#include "JuceHeader.h"

#include "convolution_reverb_module.h"
#include "delay.h"
#include "envelope.h"
#include "frequency_shifter_module.h"
#include "granular_source.h"
#include "line_generator.h"
#include "load_save.h"
#include "modulation_connection_processor.h"
#include "operators.h"
#include "random_lfo.h"
#include "reverb.h"
#include "reverb_module.h"
#include "sound_engine.h"
#include "../src/plugin/synth_plugin.h"
#include "../src/plugin/value_bridge.h"
#include "synth_gui_interface.h"
#include "synth_constants.h"
#include "synth_lfo.h"
#include "synth_oscillator.h"
#include "trigger_random.h"
#include "value.h"
#include "voice_handler.h"
#include "utility_module.h"
#include "noise_source.h"
#include "sample_source.h"
#include "../src/synthesis/lookups/wavetable.h"
#include "../src/synthesis/lookups/wave_frame.h"
#include "../src/common/wavetable/wave_source.h"

#include <array>
#include <functional>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <new>
#include <string>
#include <thread>
#include <vector>

namespace {
  std::atomic<bool> track_allocations { false };
  std::atomic<size_t> tracked_allocations { 0 };
}

void* operator new(std::size_t size) {
  if (track_allocations.load(std::memory_order_relaxed))
    tracked_allocations.fetch_add(1, std::memory_order_relaxed);
  if (void* memory = std::malloc(size))
    return memory;
  throw std::bad_alloc();
}

void* operator new[](std::size_t size) {
  return ::operator new(size);
}

void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace {
  int failures = 0;

  void expect(bool condition, const std::string& message) {
    if (!condition) {
      std::cerr << "FAIL: " << message << '\n';
      ++failures;
    }
  }

  void expectClose(float actual, float expected, float tolerance, const std::string& message) {
    expect(std::isfinite(actual) && std::abs(actual - expected) <= tolerance,
           message + " (actual=" + std::to_string(actual) +
           ", expected=" + std::to_string(expected) + ")");
  }

  void testRepeatedSoundEngineConstructionIsMemorySafe() {
    // Hosts create a second plug-in instance while pasting or duplicating an
    // FX chain. Keep several engines alive together, then repeat the complete
    // construction/destruction sequence so ASan exercises that exact lifetime.
    const int baseline_workers = vital::ConvolutionIrWorker::liveWorkerCount();
    // The REAPER report had five existing Atlas spectral workers and crashed
    // while constructing another instance. Keep eight engines alive at once
    // so peak live-instance pressure matches and exceeds that host case.
    constexpr int kSimultaneousEngines = 8;
    for (int batch = 0; batch < 3; ++batch) {
      std::array<std::unique_ptr<vital::SoundEngine>, kSimultaneousEngines> engines;
      for (auto& engine : engines)
        engine = std::make_unique<vital::SoundEngine>();

      expect(vital::ConvolutionIrWorker::liveWorkerCount() ==
                 baseline_workers + kSimultaneousEngines,
             "each SoundEngine should own exactly one convolution IR worker");
    }
    expect(vital::ConvolutionIrWorker::liveWorkerCount() == baseline_workers,
           "convolution IR workers should stop when their SoundEngines are destroyed");
  }

  void testHostStylePluginConstructionIsMemorySafe() {
    // Exercise the complete AudioProcessor/SynthBase wrapper that a VST3 host
    // creates, while keeping enough prepared instances alive to mirror the
    // REAPER paste crash. Processing one block also gives existing instances
    // an opportunity to expose a delayed overwrite before the next factory
    // allocation checks the Nano heap.
    constexpr int kSimultaneousPlugins = 8;
    const int baseline_workers = vital::ConvolutionIrWorker::liveWorkerCount();
    std::array<std::unique_ptr<SynthPlugin>, kSimultaneousPlugins> plugins;
    for (auto& plugin : plugins) {
      plugin = std::make_unique<SynthPlugin>();
      plugin->setRateAndBufferSizeDetails(48000.0, 512);
      plugin->prepareToPlay(48000.0, 512);

      juce::AudioBuffer<float> audio(2, 512);
      audio.clear();
      juce::MidiBuffer midi;
      midi.addEvent(juce::MidiMessage::noteOn(1, 60, 0.8f), 0);
      midi.addEvent(juce::MidiMessage::noteOff(1, 60), 480);
      plugin->processBlock(audio, midi);
    }

    expect(vital::ConvolutionIrWorker::liveWorkerCount() ==
               baseline_workers + kSimultaneousPlugins,
           "each complete plug-in instance should own one convolution IR worker");
    for (auto& plugin : plugins)
      plugin.reset();
    expect(vital::ConvolutionIrWorker::liveWorkerCount() == baseline_workers,
           "complete plug-in destruction should stop every convolution IR worker");
  }

  void testCutoffAccuracyAndOpenEnd() {
    // Compare measured audio against the complete Butterworth transfer function,
    // not merely against another implementation of the processing loop.
    for (int rate : { 44100, 48000, 96000, 384000 }) {
      for (bool high_pass : { false, true }) {
        for (int slope = 0; slope < 8; ++slope) {
          for (double frequency : { 500.0, 2000.0, 8000.0 }) {
            vital::CutFilter filter(high_pass);
            filter.configure(vital::utils::frequencyToMidiNote(2000.0f), slope, rate);
            double input_energy = 0.0, output_energy = 0.0;
            for (int i = 0; i < rate / 5; ++i) {
              const float input = 0.1 * std::sin(2.0 * juce::MathConstants<double>::pi * frequency * i / rate);
              const auto output = filter.tick(input);
              if (i >= rate / 10) {
                input_energy += input * input;
                output_energy += output[0] * output[0];
              }
            }
            const double ratio = std::tan(juce::MathConstants<double>::pi * frequency / rate) /
                                 std::tan(juce::MathConstants<double>::pi * 2000.0 / rate);
            const double power = std::pow(high_pass ? 1.0 / ratio : ratio, 2 * (slope + 1));
            const double expected = 1.0 / (1.0 + power);
            expectClose(output_energy / input_energy, expected, 0.002f,
                        "cut filter must track the stated cutoff and slope at every processing rate");
          }
        }
      }
    }

    // Exercise the actual Utility/Sample/Noise signal paths around the reported
    // bypass threshold. Independent copies use identical seeded noise.
    for (int rate : { 44100, 48000, 96000 }) {
      std::vector<float> wave(rate);
      for (int i = 0; i < rate; ++i)
        wave[i] = 0.1 * std::sin(2.0 * juce::MathConstants<double>::pi * 10000.0 * i / rate);
      std::array<double, 3> previous{};
      int iteration = 0;
      for (float cutoff : { 20400.0f, 20600.0f, 21096.164f }) {
        vital::Value zero(0.0f), one(1.0f), low(8.0f), high(vital::utils::frequencyToMidiNote(cutoff)), slope(7.0f);
        vital::UtilityModule utility;
        utility.init();
        utility.setSampleRate(rate);
        utility.getControls().at("utility_filter_slope")->set(7.0f);
        utility.getControls().at("utility_high_cutoff")->set(high.value());
        std::array<vital::poly_float, 128> input{};
        for (int block = 0; block < rate / 64; ++block)
          utility.processWithInput(input.data(), 128);
        vital::SampleSource sample;
        sample.setSampleRate(rate);
        sample.getSample()->loadSample(wave.data(), rate, rate);
        for (int port = 0; port < vital::SampleSource::kNumInputs; ++port) sample.plug(&zero, port);
        for (int port : { vital::SampleSource::kLevel, vital::SampleSource::kEnd, vital::SampleSource::kLoop,
                          vital::SampleSource::kLoopEnd, vital::SampleSource::kNotePressed }) sample.plug(&one, port);
        sample.plug(&low, vital::SampleSource::kLowCutoff);
        sample.plug(&high, vital::SampleSource::kHighCutoff);
        sample.plug(&slope, vital::SampleSource::kFilterSlope);
        const int seed = vital::utils::RandomGenerator::next_seed_;
        vital::utils::RandomGenerator::next_seed_ = 6789;
        vital::NoiseSource noise;
        vital::utils::RandomGenerator::next_seed_ = seed;
        noise.setSampleRate(rate);
        for (int port = 0; port < vital::NoiseSource::kNumInputs; ++port) noise.plug(&zero, port);
        noise.plug(&one, vital::NoiseSource::kLevel);
        noise.plug(&one, vital::NoiseSource::kNotePressed);
        noise.plug(&low, vital::NoiseSource::kLowCutoff);
        noise.plug(&high, vital::NoiseSource::kHighCutoff);
        noise.plug(&slope, vital::NoiseSource::kFilterSlope);
        std::array<double, 3> energy{};
        for (int offset = 0; offset + 128 <= rate; offset += 128) {
          for (int i = 0; i < 128; ++i) input[i] = wave[offset + i];
          utility.processWithInput(input.data(), 128);
          sample.process(128);
          noise.process(128);
          if (offset < rate / 4) continue;
          const vital::poly_float* buffers[] = { utility.output()->buffer, sample.output(vital::SampleSource::kRaw)->buffer,
                                                noise.output(vital::NoiseSource::kRaw)->buffer };
          for (int path = 0; path < 3; ++path)
            for (int i = 0; i < 128; ++i) energy[path] += buffers[path][i][0] * buffers[path][i][0];
        }
        if (iteration > 0) {
          for (int path = 0; path < 3; ++path) {
            const double change_db = 10.0 * std::log10(energy[path] / previous[path]);
            expect(std::isfinite(change_db) && std::abs(change_db) < (path == 2 ? 2.5 : 0.1),
                   "opening Utility/Sample/Noise high cut must not cause a large tonal jump");
          }
        }
        previous = energy;
        ++iteration;
      }
    }

    // Rapid edits, partial voice resets, all orders, and both open endpoints.
    // Also exercise low cutoffs at 8x/96 kHz without allocating in processing.
    for (bool high_pass : { false, true }) {
      vital::CutFilter filter(high_pass);
      bool finite = true;
      float peak = 0.0f;
      tracked_allocations = 0;
      track_allocations = true;
      for (int block = 0; block < 2000; ++block) {
        const float cutoffs[] = { 8.0f, 8.01f, 8.49f, 8.51f, 50.0f, 110.0f, 135.49f, 135.51f, 135.99f, 136.0f };
        const float cutoff = cutoffs[(block / 30) % 10];
        filter.configure(vital::poly_float(cutoff, 136.0f - cutoff + 8.0f, cutoff, cutoff), (block / 240) % 8, 768000);
        if (block % 127 == 0) filter.reset(vital::constants::kFirstMask);
        for (int i = 0; i < 128; ++i) {
          const auto out = filter.tick(0.05f * std::sin(0.1f * (block * 128 + i)));
          finite &= vital::utils::isFinite(out);
          for (int lane = 0; lane < 4; ++lane) peak = std::max(peak, std::abs(out[lane]));
        }
      }
      track_allocations = false;
      expect(tracked_allocations == 0, "cutoff changes and resets must not allocate");
      expect(finite && peak < 1.0f, "rapid cutoff edits must remain finite and bounded");
    }
  }

  void testSelectableCutFilterSlopes() {
    constexpr int rate = 48000;
    constexpr int length = 48000;
    const auto energy = [](const vital::poly_float* values, int count) {
      double result = 0.0;
      for (int i = 0; i < count; ++i) {
        for (int lane = 0; lane < 4; ++lane) {
          if (!std::isfinite(values[i][lane]))
            return std::numeric_limits<double>::infinity();
          result += values[i][lane] * values[i][lane];
        }
      }
      return result;
    };
    for (bool high_pass : { false, true }) {
      double previous = 1.0e30;
      for (int slope = 0; slope < 8; ++slope) {
        vital::UtilityModule utility;
        utility.init();
        utility.setSampleRate(rate);
        const auto controls = utility.getControls();
        controls.at("utility_filter_slope")->set(slope);
        controls.at(high_pass ? "utility_low_cutoff" : "utility_high_cutoff")->set(69.0f); // 440 Hz
        std::array<vital::poly_float, 128> input;
        input.fill(0.0f);
        // Settle the existing cutoff smoothing before measuring a stationary
        // filter response; otherwise the initial open filter dominates steep slopes.
        for (int block = 0; block < 400; ++block)
          utility.processWithInput(input.data(), 128);
        double total = 0.0;
        const double frequency = high_pass ? 110.0 : 1760.0;
        tracked_allocations = 0;
        track_allocations = true;
        for (int offset = 0; offset < length; offset += 128) {
          for (int i = 0; i < 128; ++i)
            input[i] = 0.5f * std::sin(2.0 * juce::MathConstants<double>::pi * frequency * (offset + i) / rate);
          utility.processWithInput(input.data(), 128);
          if (offset > 4096) total += energy(utility.output()->buffer, 128);
        }
        track_allocations = false;
        expect(tracked_allocations == 0, "Utility slope processing must not allocate");
        if (!(std::isfinite(total) && total > 0.0 && total < previous * 0.2))
          std::cerr << "Utility high-pass=" << high_pass << " slope=" << slope << " energy=" << total << " previous=" << previous << '\n';
        expect(std::isfinite(total) && total > 0.0 && total < previous * 0.2,
               "each Utility pole must increase stop-band attenuation");
        previous = total;
      }
    }

    std::vector<float> sample_audio(length);
    for (int i = 0; i < length; ++i)
      sample_audio[i] = 0.5f * std::sin(2.0 * juce::MathConstants<double>::pi * 1760.0 * i / rate);
    double previous_sample = 1.0e30;
    for (int slope = 0; slope < 8; ++slope) {
      vital::Value zero(0.0f), one(1.0f), low(8.0f), high(69.0f), slope_value(static_cast<float>(slope));
      vital::SampleSource sample;
      sample.setSampleRate(rate);
      sample.getSample()->loadSample(sample_audio.data(), length, rate);
      for (int input = 0; input < vital::SampleSource::kNumInputs; ++input)
        sample.plug(&zero, input);
      for (int input : { vital::SampleSource::kLevel, vital::SampleSource::kEnd, vital::SampleSource::kLoop,
                         vital::SampleSource::kLoopEnd, vital::SampleSource::kNotePressed })
        sample.plug(&one, input);
      sample.plug(&low, vital::SampleSource::kLowCutoff);
      sample.plug(&high, vital::SampleSource::kHighCutoff);
      sample.plug(&slope_value, vital::SampleSource::kFilterSlope);
      // Use identical noise for every slope so differences measure filtering.
      const int previous_seed = vital::utils::RandomGenerator::next_seed_;
      vital::utils::RandomGenerator::next_seed_ = 12345;
      vital::NoiseSource noise;
      vital::utils::RandomGenerator::next_seed_ = previous_seed;
      noise.setSampleRate(rate);
      for (int input = 0; input < vital::NoiseSource::kNumInputs; ++input)
        noise.plug(&zero, input);
      noise.plug(&one, vital::NoiseSource::kLevel);
      noise.plug(&one, vital::NoiseSource::kNotePressed);
      noise.plug(&low, vital::NoiseSource::kLowCutoff);
      noise.plug(&high, vital::NoiseSource::kHighCutoff);
      noise.plug(&slope_value, vital::NoiseSource::kFilterSlope);
      double sample_energy = 0.0, noise_energy = 0.0;
      tracked_allocations = 0;
      track_allocations = true;
      for (int offset = 0; offset < length; offset += 128) {
        sample.process(128);
        noise.process(128);
        if (offset > 4096) {
          sample_energy += energy(sample.output(vital::SampleSource::kRaw)->buffer, 128);
          noise_energy += energy(noise.output(vital::NoiseSource::kRaw)->buffer, 128);
        }
      }
      track_allocations = false;
      expect(tracked_allocations == 0, "Sample and Noise slope processing must not allocate");
      expect(std::isfinite(sample_energy) && sample_energy > 0.0 && sample_energy < previous_sample * 0.2,
             "each Sample pole must increase stop-band attenuation");
      expect(std::isfinite(noise_energy) && noise_energy > 0.0 && noise_energy < length * 0.1,
             "Noise low pass must remove out-of-band energy at every slope");
      previous_sample = sample_energy;
    }
  }

  void testChannelScopedAllNotesOffPreservesOtherKeys() {
    vital::SoundEngine engine;
    engine.noteOn(60, 1.0f, 0, 0);
    engine.noteOn(60, 1.0f, 0, 1);
    engine.noteOn(64, 1.0f, 0, 4);
    engine.allNotesOff(0, 0);
    expect(engine.getNumPressedNotes() == 2,
           "All Notes Off on one channel must preserve held keys on other channels");
    engine.noteOff(60, 0.0f, 0, 1);
    expect(engine.getNumPressedNotes() == 1,
           "A surviving key must remain individually releasable");
    engine.noteOn(62, 1.0f, 0, 1);
    engine.noteOn(63, 1.0f, 0, 2);
    engine.allNotesOffRange(0, 1, 2);
    expect(engine.getNumPressedNotes() == 1,
           "All Notes Off for an MPE range must preserve held keys outside that range");
    engine.allNotesOff(0, 15);
    expect(engine.getNumPressedNotes() == 1,
           "All Notes Off on an empty channel must not change other keys");
    engine.allNotesOffRange(0, 5, 3);
    expect(engine.getNumPressedNotes() == 1,
           "An empty channel range must not clear unrelated keys");
    engine.allNotesOff(0);
    expect(engine.getNumPressedNotes() == 0, "Global All Notes Off must still clear all keys");

    engine.getControls().at("polyphony")->set(1.0f);
    engine.getControls().at("legato")->set(1.0f);
    engine.process(128);
    engine.noteOn(60, 1.0f, 0, 1);
    engine.process(128);
    engine.allNotesOff(0, 0);
    engine.noteOn(64, 1.0f, 0, 1);
    engine.process(128);
    engine.noteOff(64, 0.0f, 0, 1);
    engine.process(128);
    expectClose(engine.getLastActiveNote(), 60.0f, 0.0f,
                "Mono note priority must return to a surviving held note after another channel's All Notes Off");
  }

  class RecordingGui : public SynthGuiInterface {
    public:
      explicit RecordingGui(SynthBase& synth) : SynthGuiInterface(&synth, false) { }
      void updateGuiControl(const std::string& name, float value) override {
        values[name] = value;
        ++updates;
      }
      void notifyMidiLearned(const std::string& name, int midi_id) override { learned[name] = midi_id; }
      std::map<std::string, float> values;
      std::map<std::string, int> learned;
      int updates = 0;
  };

  void testParameterNotificationsAreCoalescedWithoutAllocations() {
    SynthPlugin plugin;
    ValueBridge* parameter = nullptr;
    const std::string name = "osc_1_spectral_morph_amount"; // Exceeds small-string storage.
    for (auto* candidate : plugin.getParameters()) {
      auto* bridge = dynamic_cast<ValueBridge*>(candidate);
      if (bridge && bridge->getParameterId().toStdString() == name)
        parameter = bridge;
    }
    expect(parameter != nullptr, "Long-name automation parameter must exist");
    if (parameter == nullptr)
      return;

    const std::string fx_name = "bus_3_fx_slot_20_reverb_decay_time";
    expect(plugin.assignFxParameterToAutomationSlot(fx_name) >= 0, "Long-name FX automation binding must exist");
    FxAutomationParameter* fx_parameter = nullptr;
    for (auto* candidate : plugin.getParameters()) {
      auto* proxy = dynamic_cast<FxAutomationParameter*>(candidate);
      if (proxy && proxy->isAssignedTo(fx_name))
        fx_parameter = proxy;
    }
    expect(fx_parameter != nullptr, "Assigned FX proxy must be available");
    if (fx_parameter == nullptr)
      return;

    RecordingGui gui(plugin);
    plugin.clearPendingGuiUpdates();
    tracked_allocations.store(0, std::memory_order_relaxed);
    track_allocations.store(true, std::memory_order_relaxed);
    for (int i = 0; i < 1000; ++i) {
      const float value = i / 1000.0f;
      parameter->setValue(value);
      plugin.valueChangedThroughMidi(name, value);
      fx_parameter->setValue(value);
      plugin.valueChangedThroughMidi(fx_name, value);
      plugin.pitchWheelMidiChanged(value);
      plugin.modWheelMidiChanged(value);
    }
    track_allocations.store(false, std::memory_order_relaxed);
    expect(tracked_allocations.load(std::memory_order_relaxed) == 0,
           "Host automation and MIDI display updates must not allocate, even without an editor");
    expect(gui.updates == 0, "Audio-side changes must not invoke GUI code synchronously");
    plugin.dispatchPendingGuiUpdates(gui);
    // Effect parameters queue no display updates: there are tens of thousands
    // of them, and the editor reads the ones it shows itself.
    expect(gui.updates == 3, "Thousands of events must coalesce to one display update per control");
    expectClose(gui.values.at(name), 0.999f, 1.0e-6f, "Coalescing must preserve the latest parameter value");
    expect(gui.values.count(fx_name) == 0, "Effect parameters queue no display updates");
    expectClose(plugin.getControls().at(fx_name)->value(), 0.999f, 1.0e-6f, "FX automation must still reach the control");
    plugin.dispatchPendingGuiUpdates(gui);
    expect(gui.updates == 3, "An idle GUI refresh must not repeat consumed updates");

    plugin.valueMidiLearned(name, 74);
    plugin.dispatchPendingGuiUpdates(gui);
    expect(gui.learned.at(name) == 74, "MIDI-learn notification must reach the message-thread consumer");
    plugin.valueMidiLearned(fx_name, 75);
    plugin.dispatchPendingGuiUpdates(gui);
    expect(gui.learned.count(fx_name) && gui.learned.at(fx_name) == 75,
           "MIDI-learn notification must reach the consumer for effect parameters too");
    parameter->setValue(0.1f);
    plugin.clearPendingGuiUpdates();
    plugin.dispatchPendingGuiUpdates(gui);
    expect(gui.updates == 3, "Preset replacement must discard obsolete pending display values");
  }

  void testLineGeneratorPublishesCompleteBuffers() {
    LineGenerator line(64);
    auto old_read = line.acquireBuffer();
    std::array<float, 67> old_values {};
    std::copy(old_read.get(), old_read.get() + old_values.size(), old_values.begin());

    line.setPoint(0, { 0.0f, 0.0f });
    line.render();
    for (size_t i = 0; i < old_values.size(); ++i) {
      expectClose(old_read.get()[i], old_values[i], 0.0f,
                  "An in-flight LFO reader must keep a stable, complete lookup buffer");
    }

    auto current_read = line.acquireBuffer();
    expect(std::abs(current_read.get()[1] - old_read.get()[1]) > 0.5f,
           "A completed LFO render must atomically publish the new lookup buffer");
    current_read = LineGenerator::BufferRead();

    line.setPoint(0, { 0.0f, 0.5f });
    std::atomic<bool> render_started { false };
    std::atomic<bool> render_finished { false };
    std::thread renderer([&] {
      render_started.store(true, std::memory_order_release);
      line.render();
      render_finished.store(true, std::memory_order_release);
    });
    while (!render_started.load(std::memory_order_acquire))
      std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    expect(!render_finished.load(std::memory_order_acquire),
           "An LFO render must not overwrite a lookup buffer still held by audio");
    old_read = LineGenerator::BufferRead();
    renderer.join();
    expect(render_finished.load(std::memory_order_acquire),
           "An LFO render must publish after the prior audio reader releases its buffer");
  }

  void testPresentationOrdersPreserveSerializedEnums() {
    expect(vital::constants::kDirectOut == 6 && vital::constants::kBus3 == 7,
           "Source destination engine values must remain frozen for preset compatibility");
    const std::array<int, vital::constants::kNumSourceDestinations> expected_destinations = {{
      vital::constants::kFilter1,
      vital::constants::kFilter2,
      vital::constants::kDualFilters,
      vital::constants::kEffects,
      vital::constants::kBus1,
      vital::constants::kBus2,
      vital::constants::kBus3,
      vital::constants::kDirectOut,
    }};
    for (int i = 0; i < vital::constants::kNumSourceDestinations; ++i) {
      expect(vital::constants::kSourceDestinationPresentationOrder[i] ==
                 expected_destinations[static_cast<size_t>(i)],
             "Source destinations must present Bus 3 before Direct Out without renumbering either value");
    }

    expect(vital::SynthOscillator::kFmSample == 9 &&
               vital::SynthOscillator::kRmSample == 12 &&
               vital::SynthOscillator::kFmOscillatorC == 13 &&
               vital::SynthOscillator::kRmOscillatorC == 15,
           "Oscillator distortion engine values must remain frozen for preset compatibility");
    const std::array<int, vital::SynthOscillator::kNumDistortionTypes> expected_distortions = {{
      vital::SynthOscillator::kNone,
      vital::SynthOscillator::kSync,
      vital::SynthOscillator::kFormant,
      vital::SynthOscillator::kQuantize,
      vital::SynthOscillator::kBend,
      vital::SynthOscillator::kSqueeze,
      vital::SynthOscillator::kPulseWidth,
      vital::SynthOscillator::kFmOscillatorA,
      vital::SynthOscillator::kFmOscillatorB,
      vital::SynthOscillator::kFmOscillatorC,
      vital::SynthOscillator::kFmSample,
      vital::SynthOscillator::kFmGranular,
      vital::SynthOscillator::kFmNoise,
      vital::SynthOscillator::kFmSpectral,
      vital::SynthOscillator::kRmOscillatorA,
      vital::SynthOscillator::kRmOscillatorB,
      vital::SynthOscillator::kRmOscillatorC,
      vital::SynthOscillator::kRmSample,
      vital::SynthOscillator::kRmGranular,
      vital::SynthOscillator::kRmNoise,
      vital::SynthOscillator::kRmSpectral,
    }};
    expect(vital::SynthOscillator::kDistortionPresentationOrder == expected_distortions,
           "Oscillator distortion modes must present all FM sources before all ring-mod sources");
  }

  void testWavetableTransactionalPublication() {
    vital::Wavetable wavetable(8);
    wavetable.markUsed();
    const auto* original = wavetable.getAllActiveData();
    const int original_version = original->version;
    const float original_sample = original->wave_data[0][0];
    wavetable.markUnused();

    expect(wavetable.beginEdit(), "A wavetable edit must create private staging data");
    expect(!wavetable.beginEdit(), "A nested wavetable render must reuse the active transaction");
    wavetable.loadWaveFrame(vital::PredefinedWaveFrames::getWaveFrame(
        vital::PredefinedWaveFrames::kSquare), 0);

    wavetable.markUsed();
    expect(wavetable.getAllActiveData() == original,
           "Audio must keep the published wavetable while an editor render is incomplete");
    expectClose(wavetable.getAllActiveData()->wave_data[0][0], original_sample, 0.0f,
                "An incomplete wavetable render must not modify audio-visible samples");
    wavetable.markUnused();

    wavetable.commitEdit();
    wavetable.markUsed();
    const auto* published = wavetable.getAllActiveData();
    expect(published != original && published->version > original_version,
           "A complete wavetable render must publish one new table version atomically");
    wavetable.markUnused();
  }

  float waveFrameDifference(const vital::WaveFrame& first, const vital::WaveFrame& second) {
    float difference = 0.0f;
    for (int sample = 0; sample < vital::WaveFrame::kWaveformSize; ++sample)
      difference += std::abs(first.time_domain[sample] - second.time_domain[sample]);
    return difference / vital::WaveFrame::kWaveformSize;
  }

  void testWavetableFrameInterpolationModes() {
    WaveSource source;
    source.insertNewKeyframe(0);
    source.insertNewKeyframe(8);
    source.getKeyframe(0)->wave_frame()->copy(
        vital::PredefinedWaveFrames::getWaveFrame(vital::PredefinedWaveFrames::kSin));
    source.getKeyframe(1)->wave_frame()->copy(
        vital::PredefinedWaveFrames::getWaveFrame(vital::PredefinedWaveFrames::kSquare));

    vital::WaveFrame none;
    vital::WaveFrame waveform;
    vital::WaveFrame spectral;
    vital::WaveFrame smooth_waveform;
    vital::WaveFrame smooth_spectral;

    source.setInterpolationStyle(WavetableComponent::kNone);
    source.render(&none, 2.0f);
    source.setInterpolationStyle(WavetableComponent::kLinear);
    source.setInterpolationMode(WaveSource::kTime);
    source.render(&waveform, 2.0f);
    source.setInterpolationMode(WaveSource::kFrequency);
    source.render(&spectral, 2.0f);
    source.setInterpolationStyle(WavetableComponent::kCubic);
    source.setInterpolationMode(WaveSource::kTime);
    source.render(&smooth_waveform, 2.0f);
    source.setInterpolationMode(WaveSource::kFrequency);
    source.render(&smooth_spectral, 2.0f);

    expect(waveFrameDifference(none, waveform) > 0.01f,
           "Waveform interpolation must generate frames between source keyframes");
    expect(waveFrameDifference(waveform, spectral) > 0.001f,
           "Waveform and spectral frame interpolation must render differently");
    expect(waveFrameDifference(waveform, smooth_waveform) > 0.001f,
           "Linear and smooth waveform interpolation must render differently");
    expect(waveFrameDifference(smooth_waveform, smooth_spectral) > 0.001f,
           "Smooth waveform and smooth spectral interpolation must render differently");
  }

  void testVitalPresetDestinationMigration() {
    json state = {
      { "synth_version", "1.5.5" },
      { "settings", {
        { "modulations", json::array() },
        { "sample", json::object() },
        { "wavetables", json::array() },
        { "osc_1_destination", 4.0f },
        { "osc_2_destination", 5.0f },
        { "osc_3_destination", 3.0f },
        { "sample_destination", 4.0f },
        { "filter_1_destination", 4.0f }
      } }
    };

    const json migrated = LoadSave::updateFromOldVersion(state);
    const json& settings = migrated.at("settings");
    expect(settings.at("osc_1_destination") == vital::constants::kDirectOut,
           "Vital oscillator Direct Out must migrate past Atlas's inserted buses");
    expect(settings.at("osc_2_destination") == vital::constants::kNumSourceDestinations,
           "Vital direct-effect destinations must migrate past Atlas's inserted buses");
    expect(settings.at("osc_3_destination") == vital::constants::kEffects,
           "Vital filter/effects destinations before Direct Out must stay unchanged");
    expect(settings.at("sample_destination") == vital::constants::kDirectOut,
           "Vital sample Direct Out must migrate past Atlas's inserted buses");
    expect(settings.at("filter_1_destination") == vital::constants::kDirectOut,
           "Vital filter Direct Out must migrate past Atlas's inserted buses");

    state["settings"]["bus_1_destination"] = 0.0f;
    const json native_atlas = LoadSave::updateFromOldVersion(state);
    expect(native_atlas.at("settings").at("osc_1_destination") == 4.0f,
           "Native Atlas routing values must not be mistaken for Vital values");
  }

  void testVitalLineRasterizationCompatibility() {
    LineGenerator line(5);
    line.setLoop(false);
    line.jsonToState({
      { "num_points", 5 },
      { "points", { 0.0f, 1.0f, 0.0f, 0.0f, 0.5f, 0.0f, 0.5f, 1.0f, 1.0f, 1.0f } },
      { "powers", { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f } },
      { "smooth", false }
    });

    expectClose(line.getBuffer()[0], 0.0f, 0.0f,
                "Vital line rasterization must preserve the first vertical-edge sample");
    expectClose(line.getBuffer()[1], 0.0f, 0.0f,
                "Vital line rasterization must advance duplicate points after rendering");
    expectClose(line.getBuffer()[2], 1.0f, 0.0f,
                "Vital line rasterization must preserve the sample before the next edge");
  }

  struct LfoFixture {
    LineGenerator line { 256 };
    vital::SynthLfo lfo { &line };
    std::array<vital::Value, vital::SynthLfo::kNumInputs> controls;
    vital::Output trigger;

    LfoFixture() {
      line.setLoop(true);
      line.initLinear();
      for (int i = 0; i < vital::SynthLfo::kNumInputs; ++i)
        lfo.plug(&controls[static_cast<size_t>(i)], i);
      lfo.plug(&trigger, vital::SynthLfo::kNoteTrigger);
      lfo.setSampleRate(48000);
      controls[vital::SynthLfo::kFrequency].set(137.25f);
      controls[vital::SynthLfo::kRateX10].set(0.0f);
      controls[vital::SynthLfo::kPhase].set(0.0f);
      controls[vital::SynthLfo::kAmplitude].set(1.0f);
      controls[vital::SynthLfo::kSyncType].set(vital::SynthLfo::kTrigger);
      controls[vital::SynthLfo::kSmoothMode].set(0.0f);
      controls[vital::SynthLfo::kFade].set(0.0f);
      controls[vital::SynthLfo::kSmoothTime].set(0.0f);
      controls[vital::SynthLfo::kStereoPhase].set(0.0f);
      controls[vital::SynthLfo::kDelay].set(0.0f);
      controls[vital::SynthLfo::kNoteCount].set(0.0f);
      controls[vital::SynthLfo::kDeform].set(LineGenerator::defaultDeform());
      controls[vital::SynthLfo::kSwing].set(LineGenerator::defaultSwing());
    }
  };

  std::vector<float> renderLfo(const std::vector<int>& blocks, int total_samples) {
    LfoFixture fixture;
    std::vector<float> result;
    result.reserve(static_cast<size_t>(total_samples));
    int rendered = 0;
    size_t block_index = 0;
    while (rendered < total_samples) {
      const int host_block = std::min(blocks[block_index++ % blocks.size()], total_samples - rendered);
      int host_rendered = 0;
      while (host_rendered < host_block) {
        const int block = std::min(vital::kMaxBufferSize, host_block - host_rendered);
        fixture.lfo.process(block);
        for (int sample = 0; sample < block; ++sample)
          result.push_back(fixture.lfo.output(vital::SynthLfo::kValue)->buffer[sample][0]);
        host_rendered += block;
      }
      rendered += host_block;
    }
    return result;
  }

  void testLfoTriggerPlacement() {
    for (int trigger_sample = 0; trigger_sample < vital::kMaxBufferSize; ++trigger_sample) {
      LfoFixture reference;
      LfoFixture triggered;
      reference.lfo.process(73);
      triggered.lfo.process(73);

      reference.lfo.process(vital::kMaxBufferSize);
      triggered.trigger.trigger(vital::poly_mask(-1), vital::poly_float(vital::kVoiceOn),
                                vital::poly_int(trigger_sample));
      triggered.lfo.process(vital::kMaxBufferSize);

      for (int sample = 0; sample < trigger_sample; ++sample) {
        expectClose(triggered.lfo.output()->buffer[sample][0], reference.lfo.output()->buffer[sample][0],
                    1.0e-6f, "LFO samples before retrigger must be unchanged");
      }
      expectClose(triggered.lfo.output()->buffer[trigger_sample][0], 0.0f, 2.0e-4f,
                  "LFO trigger sample must contain the restarted value");
    }
  }

  void testLfoBlockInvariance() {
    const auto reference = renderLfo({ 1 }, 4096);
    const auto blocked = renderLfo({ 1, 2, 7, 16, 31, 32, 63, 64, 127, 128, 256, 512 }, 4096);
    for (size_t i = 0; i < reference.size(); ++i)
      expectClose(blocked[i], reference[i], 2.0e-5f, "LFO phase must be block-size invariant");
  }

  void testLfoFadeInAcrossProcessingModes() {
    constexpr float kFadeSeconds = 0.1f;
    constexpr int kFirstBlock = vital::kMaxBufferSize;
    constexpr float kExpectedFirstBlockGain = kFirstBlock / (48000.0f * kFadeSeconds);

    for (bool control_rate : { false, true }) {
      for (int smooth_mode : { 0, 1 }) {
        LfoFixture fixture;
        fixture.line.setLoop(true);
        fixture.line.setNumPoints(2);
        fixture.line.setPoint(0, { 0.0f, 0.0f });
        fixture.line.setPoint(1, { 1.0f, 0.0f });
        fixture.line.setPower(0, 0.0f);
        fixture.line.setPower(1, 0.0f);
        fixture.line.render();
        fixture.lfo.setControlRate(control_rate);
        fixture.controls[vital::SynthLfo::kFade].set(kFadeSeconds);
        fixture.controls[vital::SynthLfo::kSmoothMode].set(smooth_mode);
        fixture.controls[vital::SynthLfo::kSmoothTime].set(0.0f);
        fixture.trigger.trigger(vital::poly_mask(-1), vital::poly_float(vital::kVoiceOn), vital::poly_int(0));

        fixture.lfo.process(kFirstBlock);
        const int result_sample = control_rate ? 0 : kFirstBlock - 1;
        const float first_block_gain = fixture.lfo.output(vital::SynthLfo::kValue)->buffer[result_sample][0];
        expectClose(first_block_gain, kExpectedFirstBlockGain, 3.0e-4f,
                    "LFO fade must advance over the first block in both Fade In and Smooth modes");
        if (!control_rate) {
          const float first_sample = fixture.lfo.output(vital::SynthLfo::kValue)->buffer[0][0];
          expect(first_sample > 0.0f && first_sample < first_block_gain,
                 "Audio-rate LFO fade must rise gradually from the trigger sample");
        }

        fixture.trigger.clearTrigger();
        for (int block = 1; block < 38; ++block)
          fixture.lfo.process(vital::kMaxBufferSize);
        const int final_sample = control_rate ? 0 : vital::kMaxBufferSize - 1;
        expectClose(fixture.lfo.output(vital::SynthLfo::kValue)->buffer[final_sample][0], 1.0f, 1.0e-5f,
                    "LFO fade must reach full modulation level after its configured duration");
      }
    }

    LfoFixture zero_fade;
    zero_fade.line.setNumPoints(2);
    zero_fade.line.setPoint(0, { 0.0f, 0.0f });
    zero_fade.line.setPoint(1, { 1.0f, 0.0f });
    zero_fade.line.render();
    zero_fade.controls[vital::SynthLfo::kFade].set(0.0f);
    zero_fade.controls[vital::SynthLfo::kSmoothMode].set(1.0f);
    zero_fade.trigger.trigger(vital::poly_mask(-1), vital::poly_float(vital::kVoiceOn), vital::poly_int(0));
    zero_fade.lfo.process(1);
    expectClose(zero_fade.lfo.output(vital::SynthLfo::kValue)->buffer[0][0], 1.0f, 1.0e-5f,
                "Zero LFO fade must preserve the existing immediate modulation response");
  }

  void testLfoSyncClockReachesVoiceClones() {
    LfoFixture graph_lfo;
    LfoFixture reference;
    graph_lfo.controls[vital::SynthLfo::kSyncType].set(vital::SynthLfo::kSync);
    reference.controls[vital::SynthLfo::kSyncType].set(vital::SynthLfo::kSync);

    std::unique_ptr<vital::SynthLfo> voice_clone(
        static_cast<vital::SynthLfo*>(graph_lfo.lfo.clone()));
    for (double seconds : { 0.375, 2.125 }) {
      graph_lfo.lfo.correctToTime(seconds);
      reference.lfo.correctToTime(seconds);
      voice_clone->process(64);
      reference.lfo.process(64);
      for (int sample = 0; sample < 64; ++sample) {
        expectClose(voice_clone->output()->buffer[sample][0], reference.lfo.output()->buffer[sample][0],
                    1.0e-6f, "Host sync time must propagate to cloned voice LFO processors");
      }
    }
  }

  struct RandomFixture {
    vital::RandomLfo lfo;
    std::array<vital::Value, vital::RandomLfo::kNumInputs> controls;
    vital::Output trigger;

    explicit RandomFixture(int style) {
      for (int i = 0; i < vital::RandomLfo::kNumInputs; ++i)
        lfo.plug(&controls[static_cast<size_t>(i)], i);
      lfo.plug(&trigger, vital::RandomLfo::kReset);
      lfo.setSampleRate(48000);
      lfo.setSeed(0x123456789abcdef0ull);
      controls[vital::RandomLfo::kFrequency].set(1800.0f);
      controls[vital::RandomLfo::kRateX10].set(1.0f);
      controls[vital::RandomLfo::kSync].set(0.0f);
      controls[vital::RandomLfo::kStyle].set(style);
      controls[vital::RandomLfo::kStereo].set(1.0f);
    }
  };

  std::vector<float> renderRandom(int style, const std::vector<int>& blocks, int total_samples) {
    RandomFixture fixture(style);
    std::vector<float> result;
    result.reserve(static_cast<size_t>(total_samples));
    int rendered = 0;
    size_t block_index = 0;
    while (rendered < total_samples) {
      const int host_block = std::min(blocks[block_index++ % blocks.size()], total_samples - rendered);
      int host_rendered = 0;
      while (host_rendered < host_block) {
        const int block = std::min(vital::kMaxBufferSize, host_block - host_rendered);
        fixture.lfo.process(block);
        for (int sample = 0; sample < block; ++sample)
          result.push_back(fixture.lfo.output()->buffer[sample][0]);
        host_rendered += block;
      }
      rendered += host_block;
    }
    return result;
  }

  void testRandomBlockInvariance() {
    for (int style : { vital::RandomLfo::kPerlin, vital::RandomLfo::kSampleAndHold,
                       vital::RandomLfo::kSinInterpolate, vital::RandomLfo::kLorenzAttractor }) {
      const auto reference = renderRandom(style, { 1 }, 1024);
      const auto blocked = renderRandom(style,
                                        { 1, 2, 7, 16, 31, 32, 63, 64, 127, 128, 256, 512 },
                                        1024);
      for (size_t i = 0; i < reference.size(); ++i)
        expectClose(blocked[i], reference[i], 1.0e-6f,
                    "Random LFO sequence must be block-size invariant, style " + std::to_string(style) +
                    ", sample " + std::to_string(i));
    }
  }

  void testRandomSyncClockReachesVoiceClones() {
    RandomFixture graph_lfo(vital::RandomLfo::kPerlin);
    RandomFixture reference(vital::RandomLfo::kPerlin);
    graph_lfo.controls[vital::RandomLfo::kSync].set(1.0f);
    reference.controls[vital::RandomLfo::kSync].set(1.0f);

    std::unique_ptr<vital::RandomLfo> voice_clone(
        static_cast<vital::RandomLfo*>(graph_lfo.lfo.clone()));
    voice_clone->setSeed(0xfeedface12345678ull);
    reference.lfo.setSeed(0xfeedface12345678ull);
    graph_lfo.lfo.correctToTime(1.75);
    reference.lfo.correctToTime(1.75);
    voice_clone->process(64);
    reference.lfo.process(64);
    for (int sample = 0; sample < 64; ++sample) {
      expectClose(voice_clone->output()->buffer[sample][0], reference.lfo.output()->buffer[sample][0],
                  1.0e-6f, "Host sync time must propagate to cloned Random LFO processors");
    }
  }

  void testNoteOnRandomHoldsPerVoice() {
    vital::TriggerRandom note_on_random;
    vital::Output note_on;
    note_on_random.plug(&note_on, vital::TriggerRandom::kReset);
    note_on_random.setIndependentOutputActive(0, true);
    note_on_random.setIndependentOutputActive(1, true);

    note_on.trigger(vital::poly_mask(-1), vital::poly_float(vital::kVoiceOn), vital::poly_int(0));
    note_on_random.process(1);
    note_on.clearTrigger();
    const vital::poly_float first_values = note_on_random.output()->buffer[0];
    const vital::poly_float first_independent_a = note_on_random.independentOutput(0)->buffer[0];
    const vital::poly_float first_independent_b = note_on_random.independentOutput(1)->buffer[0];
    bool independent_routes_differ = false;

    for (int lane = 0; lane < vital::poly_float::kSize; ++lane) {
      expect(first_values[lane] >= 0.0f && first_values[lane] <= 1.0f,
             "Note-on random values must remain in the unipolar modulation range");
      expect(first_independent_a[lane] >= 0.0f && first_independent_a[lane] <= 1.0f &&
                 first_independent_b[lane] >= 0.0f && first_independent_b[lane] <= 1.0f,
             "Independent note-on random values must remain in the unipolar modulation range");
      independent_routes_differ |= first_independent_a[lane] != first_independent_b[lane];
    }
    expect(independent_routes_differ,
           "Two independent note-on random routes must not receive the same random stream");
    for (int lane = 0; lane < vital::poly_float::kSize; lane += 2) {
      expectClose(first_values[lane + 1], first_values[lane], 0.0f,
                  "A note-on random value must be shared by the stereo lanes of one voice");
      expectClose(first_independent_a[lane + 1], first_independent_a[lane], 0.0f,
                  "An independent note-on random value must remain paired across stereo lanes");
      expectClose(first_independent_b[lane + 1], first_independent_b[lane], 0.0f,
                  "Every independent note-on random route must remain paired across stereo lanes");
    }

    for (int block = 0; block < 8; ++block) {
      note_on_random.process(vital::kMaxBufferSize);
      for (int lane = 0; lane < vital::poly_float::kSize; ++lane) {
        expectClose(note_on_random.output()->buffer[0][lane], first_values[lane], 0.0f,
                    "Note-on random must hold its value until that voice receives another note-on");
        expectClose(note_on_random.independentOutput(0)->buffer[0][lane], first_independent_a[lane], 0.0f,
                    "Independent note-on random must hold its value until that voice retriggers");
        expectClose(note_on_random.independentOutput(1)->buffer[0][lane], first_independent_b[lane], 0.0f,
                    "Each independent note-on random route must hold its value until retriggered");
      }
    }

    const vital::poly_mask first_voice(vital::poly_mask::kFullMask, vital::poly_mask::kFullMask, 0, 0);
    note_on.trigger(first_voice, vital::poly_float(vital::kVoiceOn), vital::poly_int(0));
    note_on_random.process(1);
    note_on.clearTrigger();
    const vital::poly_float retriggered_values = note_on_random.output()->buffer[0];
    expectClose(retriggered_values[1], retriggered_values[0], 0.0f,
                "Retriggering one voice must generate one value for both of its stereo lanes");
    for (int lane = 2; lane < vital::poly_float::kSize; ++lane) {
      expectClose(retriggered_values[lane], first_values[lane], 0.0f,
                  "Retriggering one voice must not change another voice's note-on random value");
      expectClose(note_on_random.independentOutput(0)->buffer[0][lane], first_independent_a[lane], 0.0f,
                  "Retriggering one voice must not change other voices on an independent route");
      expectClose(note_on_random.independentOutput(1)->buffer[0][lane], first_independent_b[lane], 0.0f,
                  "Retriggering one voice must leave every other independent voice unchanged");
    }
  }

  std::vector<float> renderIndependentRandom(int style, int connection,
                                             const std::vector<int>& blocks, int total_samples) {
    RandomFixture fixture(style);
    fixture.controls[vital::RandomLfo::kStereo].set(0.0f);
    fixture.lfo.setIndependentOutputActive(connection, true);
    std::vector<float> result;
    result.reserve(static_cast<size_t>(total_samples));
    int rendered = 0;
    size_t block_index = 0;
    while (rendered < total_samples) {
      const int host_block = std::min(blocks[block_index++ % blocks.size()], total_samples - rendered);
      int host_rendered = 0;
      while (host_rendered < host_block) {
        const int block = std::min(vital::kMaxBufferSize, host_block - host_rendered);
        fixture.lfo.process(block);
        for (int sample = 0; sample < block; ++sample)
          result.push_back(fixture.lfo.independentOutput(connection)->buffer[sample][0]);
        host_rendered += block;
      }
      rendered += host_block;
    }
    return result;
  }

  void testIndependentRandomLfoStreams() {
    for (int style : { vital::RandomLfo::kPerlin, vital::RandomLfo::kSampleAndHold,
                       vital::RandomLfo::kSinInterpolate, vital::RandomLfo::kLorenzAttractor }) {
      const auto single_sample = renderIndependentRandom(style, 0, { 1 }, 1024);
      const auto blocked = renderIndependentRandom(style, 0,
                                                    { 1, 2, 7, 16, 31, 64, 127, 256, 512 }, 1024);
      for (size_t sample = 0; sample < single_sample.size(); ++sample) {
        expectClose(blocked[sample], single_sample[sample], 1.0e-6f,
                    "Independent Random LFO must be block-size invariant, style " +
                        std::to_string(style) + ", sample " + std::to_string(sample));
      }

      RandomFixture shared(style);
      RandomFixture enabled(style);
      shared.controls[vital::RandomLfo::kStereo].set(0.0f);
      enabled.controls[vital::RandomLfo::kStereo].set(0.0f);
      enabled.lfo.setIndependentOutputActive(0, true);
      enabled.lfo.setIndependentOutputActive(1, true);
      bool variants_differ = false;
      for (int block : { 1, 7, 31, 64, 127, 128, 89 }) {
        shared.lfo.process(block);
        enabled.lfo.process(block);
        for (int sample = 0; sample < block; ++sample) {
          for (int lane = 0; lane < vital::poly_float::kSize; ++lane) {
            expectClose(enabled.lfo.output()->buffer[sample][lane], shared.lfo.output()->buffer[sample][lane],
                        0.0f, "Enabling independent routes must not alter the shared Random LFO stream, style " +
                                  std::to_string(style) + ", block " + std::to_string(block) +
                                  ", sample " + std::to_string(sample) + ", lane " + std::to_string(lane));
            const float route_a = enabled.lfo.independentOutput(0)->buffer[sample][lane];
            const float route_b = enabled.lfo.independentOutput(1)->buffer[sample][lane];
            expect(route_a >= 0.0f && route_a <= 1.0f && route_b >= 0.0f && route_b <= 1.0f,
                   "Independent Random LFO streams must stay in the unipolar modulation range");
            variants_differ |= route_a != route_b;
          }
          expectClose(enabled.lfo.independentOutput(0)->buffer[sample][1],
                      enabled.lfo.independentOutput(0)->buffer[sample][0], 0.0f,
                      "Mono independent Random LFO streams must remain stereo-paired");
          expectClose(enabled.lfo.independentOutput(1)->buffer[sample][1],
                      enabled.lfo.independentOutput(1)->buffer[sample][0], 0.0f,
                      "Every mono independent Random LFO route must remain stereo-paired");
        }
      }
      expect(variants_differ,
             "Two independent Random LFO routes must not receive the same stream, style " +
                 std::to_string(style));
    }
  }

  void testModulationRandomCorrelationSelector() {
    vital::ModulationConnectionProcessor processor(0);
    vital::cr::Value shared(0.25f);
    vital::cr::Value independent(0.75f);
    vital::cr::Value amount(1.0f);
    vital::cr::Value power(0.0f);
    vital::Output reset;
    shared.output()->trigger_value = 0.25f;
    independent.output()->trigger_value = 0.75f;
    processor.plug(shared.output(), vital::ModulationConnectionProcessor::kModulationInput);
    processor.plug(independent.output(), vital::ModulationConnectionProcessor::kIndependentRandomInput);
    processor.plug(amount.output(), vital::ModulationConnectionProcessor::kModulationAmount);
    processor.plug(power.output(), vital::ModulationConnectionProcessor::kModulationPower);
    processor.plug(&reset, vital::ModulationConnectionProcessor::kReset);
    processor.init();

    auto controls = processor.getControls();
    auto* random_mode = controls.at("modulation_1_random_mode");
    expectClose(random_mode->value(), 0.0f, 0.0f,
                "Existing and newly created modulation routes must default to Shared random correlation");
    processor.process(1);
    expectClose(processor.output(vital::ModulationConnectionProcessor::kModulationSource)->buffer[0][0],
                0.25f, 0.0f, "Shared random correlation must use the original modulation source");

    random_mode->set(1.0f);
    processor.process(1);
    expect(processor.usesIndependentRandom(),
           "The modulation route must report Independent after its correlation control changes");
    expectClose(processor.output(vital::ModulationConnectionProcessor::kModulationSource)->buffer[0][0],
                0.75f, 0.0f, "Independent random correlation must select the route-specific stream");

    processor.unplugIndex(vital::ModulationConnectionProcessor::kIndependentRandomInput);
    processor.process(1);
    expectClose(processor.output(vital::ModulationConnectionProcessor::kModulationSource)->buffer[0][0],
                0.25f, 0.0f,
                "A route without an independent source must fall back safely to its shared source");
  }

  float renderSmoothedExpression(const std::vector<int>& blocks) {
    vital::SmoothControlSource smoother;
    vital::cr::Value source(0.0f);
    vital::cr::Value reset(0.0f);
    smoother.setSampleRate(48000);
    smoother.plug(&source, vital::SmoothControlSource::kTarget);
    smoother.plug(&reset, vital::SmoothControlSource::kReset);

    smoother.process(1);
    source.set(1.0f);
    for (int block : blocks)
      smoother.process(block);
    return smoother.output()->buffer[blocks.back() - 1][0];
  }

  std::vector<float> renderAudioRateSmoothedExpression(const std::vector<int>& blocks) {
    vital::SmoothControlSource smoother;
    vital::cr::Value source(0.0f);
    vital::cr::Value reset(0.0f);
    smoother.setSampleRate(48000);
    smoother.plug(&source, vital::SmoothControlSource::kTarget);
    smoother.plug(&reset, vital::SmoothControlSource::kReset);

    smoother.process(1);
    source.set(1.0f);
    std::vector<float> rendered;
    for (int block : blocks) {
      smoother.process(block);
      for (int sample = 0; sample < block; ++sample)
        rendered.push_back(smoother.output()->buffer[sample][0]);
    }
    return rendered;
  }

  void testModWheelSourceSmoothing() {
    const float first_block = renderSmoothedExpression({ 64 });
    expect(first_block > 0.0f && first_block < 0.5f,
           "Smoothed Mod Wheel modulation must move immediately without jumping to the next CC step");

    const float uniform = renderSmoothedExpression({ 64, 64, 64, 64, 64, 64, 64, 64 });
    const float mixed = renderSmoothedExpression({ 17, 111, 29, 99, 128, 128 });
    expectClose(mixed, uniform, 1.0e-5f,
                "Mod Wheel smoothing time must not depend on audio block boundaries");
    expect(uniform > first_block && uniform < 1.0f,
           "Smoothed Mod Wheel modulation must converge monotonically toward its target");

    const auto one_block = renderAudioRateSmoothedExpression({ 128, 128, 128, 128 });
    const auto partitioned = renderAudioRateSmoothedExpression({ 17, 111, 29, 99, 128, 128 });
    expect(one_block.size() == partitioned.size(),
           "Audio-rate Mod Wheel smoothing must render the requested number of samples");
    if (one_block.size() == partitioned.size() && !one_block.empty()) {
      expect(one_block.front() > 0.0f && one_block.front() < 0.01f,
             "Audio-rate Mod Wheel smoothing must begin with a small sample-level step");
      for (size_t i = 1; i < one_block.size(); ++i) {
        expect(one_block[i] >= one_block[i - 1],
               "Audio-rate Mod Wheel smoothing must be monotonic within a host block");
        expect(std::abs(one_block[i] - one_block[i - 1]) < 0.01f,
               "Audio-rate Mod Wheel smoothing must not contain block-sized jumps");
      }
      for (size_t i = 0; i < one_block.size(); ++i)
        expectClose(partitioned[i], one_block[i], 1.0e-5f,
                    "Audio-rate Mod Wheel smoothing must be sample-identical across block partitions");
    }
  }

  void testModWheelUsesAudioRateSmoothingForAudioDestinations() {
    vital::SoundEngine engine;
    auto* processor = engine.getModulationBank().atIndex(0)->modulation_processor.get();

    vital::modulation_change change {};
    change.source = engine.getModulationSource("mod_wheel");
    change.mono_destination = engine.getMonoModulationDestination("osc_1_transpose");
    change.poly_destination = engine.getPolyModulationDestination("osc_1_transpose");
    change.destination_scale = vital::Parameters::getParameterRange("osc_1_transpose");
    change.mono_modulation_switch = engine.getMonoModulationSwitch("osc_1_transpose");
    change.poly_modulation_switch = engine.getPolyModulationSwitch("osc_1_transpose");
    change.modulation_processor = processor;
    engine.connectModulation(change);

    expect(!change.source->isControlRate(),
           "Mod Wheel must enter the modulation graph as a smoothed audio-rate source");
    expect(!processor->isControlRate(),
           "A Mod Wheel route to an audio-rate destination must use the normal audio-rate path");
    engine.disconnectModulation(change);
  }

  void testModWheelUsesAudioRateSmoothingForLfoSpeed() {
    vital::SoundEngine engine;
    auto* processor = engine.getModulationBank().atIndex(0)->modulation_processor.get();

    vital::modulation_change change {};
    change.source = engine.getModulationSource("mod_wheel");
    change.mono_destination = engine.getMonoModulationDestination("lfo_1_frequency");
    change.poly_destination = engine.getPolyModulationDestination("lfo_1_frequency");
    change.destination_scale = vital::Parameters::getParameterRange("lfo_1_frequency");
    change.mono_modulation_switch = engine.getMonoModulationSwitch("lfo_1_frequency");
    change.poly_modulation_switch = engine.getPolyModulationSwitch("lfo_1_frequency");
    change.modulation_processor = processor;
    engine.connectModulation(change);

    expect(change.mono_destination != nullptr && !change.mono_destination->isControlRate(),
           "Free LFO speed must expose an audio-rate monophonic modulation destination");
    expect(change.poly_destination != nullptr && !change.poly_destination->isControlRate(),
           "Free LFO speed must expose an audio-rate polyphonic modulation destination");
    expect(!processor->isControlRate(),
           "Mod Wheel to free LFO speed must preserve sample-level controller smoothing");
    engine.disconnectModulation(change);
  }

  void testTempoChooserPreservesAudioRateFreeFrequency() {
    constexpr int kSamples = 64;
    vital::TempoChooser chooser(true);
    vital::Output frequency;
    vital::cr::Value tempo(7.0f);
    vital::cr::Value beats_per_second(2.0f);
    vital::cr::Value sync(vital::TempoChooser::kFrequencyMode);
    vital::cr::Value midi(60.0f);
    vital::cr::Value transpose(0.0f);
    vital::cr::Value tune(0.0f);

    chooser.plug(&frequency, vital::TempoChooser::kFrequency);
    chooser.plug(&tempo, vital::TempoChooser::kTempoIndex);
    chooser.plug(&beats_per_second, vital::TempoChooser::kBeatsPerSecond);
    chooser.plug(&sync, vital::TempoChooser::kSync);
    chooser.plug(&midi, vital::TempoChooser::kMidi);
    chooser.plug(&transpose, vital::TempoChooser::kKeytrackTranspose);
    chooser.plug(&tune, vital::TempoChooser::kKeytrackTune);

    for (int sample = 0; sample < kSamples; ++sample)
      frequency.buffer[sample] = 0.5f + sample * 0.01f;
    chooser.process(kSamples);

    expect(!chooser.isControlRate(),
           "An audio-rate tempo chooser must retain a full output buffer");
    for (int sample = 0; sample < kSamples; ++sample) {
      expectClose(chooser.output()->buffer[sample][0], frequency.buffer[sample][0], 0.0f,
                  "Free-running LFO mode must not sample-and-hold its frequency within a block");
    }

    sync.set(vital::TempoChooser::kTempoMode);
    chooser.process(kSamples);
    const float synced_frequency = chooser.output()->buffer[0][0];
    for (int sample = 1; sample < kSamples; ++sample) {
      expectClose(chooser.output()->buffer[sample][0], synced_frequency, 0.0f,
                  "Tempo-synced LFO divisions must remain stable and intentionally discrete");
    }
  }

  // Converting a fraction to an integer truncates on every platform. x86 used
  // to round to nearest while the arm64 macOS build truncated, so the same
  // preset picked other values on Windows.
  void testFloatToIntTruncatesOnEveryPlatform() {
    const vital::poly_float values(2.7f, -2.7f, 0.5f, 1.5f);
    const vital::poly_int integers = vital::utils::toInt(values);
    const std::array<int, 4> truncated { 2, -2, 0, 1 };
    const std::array<float, 4> floored { 2.0f, -3.0f, 0.0f, 1.0f };
    const std::array<float, 4> ceiled { 3.0f, -2.0f, 1.0f, 2.0f };
    for (int i = 0; i < 4; ++i) {
      const std::string value = std::to_string(values[i]);
      expect(static_cast<int>(integers[i]) == truncated[i],
             "toInt(" + value + ") truncates to " + std::to_string(truncated[i]) +
             " (actual=" + std::to_string(static_cast<int>(integers[i])) + ")");
      expectClose(vital::utils::trunc(values)[i], static_cast<float>(truncated[i]), 0.0f, "trunc(" + value + ")");
      expectClose(vital::utils::floor(values)[i], floored[i], 0.0f, "floor(" + value + ")");
      expectClose(vital::utils::ceil(values)[i], ceiled[i], 0.0f, "ceil(" + value + ")");
    }
  }

  // A modulated LFO Tempo moves to the next division once it passes 0.7 of a
  // step, as on the arm64 macOS build. x86 builds moved at 0.2, so an LFO at
  // 1/8 modulated by half a step ran at 1/16 on Windows only.
  void testModulatedTempoPicksTheSameDivisionOnEveryPlatform() {
    vital::TempoChooser chooser;
    vital::cr::Value frequency(1.0f);
    vital::cr::Value tempo(9.0f);
    vital::cr::Value beats_per_second(2.0f);
    vital::cr::Value sync(vital::TempoChooser::kTempoMode);
    vital::cr::Value midi(60.0f);
    vital::cr::Value transpose(0.0f);
    vital::cr::Value tune(0.0f);
    chooser.plug(&frequency, vital::TempoChooser::kFrequency);
    chooser.plug(&tempo, vital::TempoChooser::kTempoIndex);
    chooser.plug(&beats_per_second, vital::TempoChooser::kBeatsPerSecond);
    chooser.plug(&sync, vital::TempoChooser::kSync);
    chooser.plug(&midi, vital::TempoChooser::kMidi);
    chooser.plug(&transpose, vital::TempoChooser::kKeytrackTranspose);
    chooser.plug(&tune, vital::TempoChooser::kKeytrackTune);

    // Tempo 9 is 1/8 and 10 is 1/16; at 120 BPM they run at 4 Hz and 8 Hz.
    const float eighth = vital::constants::kSyncedFrequencyRatios[9] * 2.0f;
    const float sixteenth = vital::constants::kSyncedFrequencyRatios[10] * 2.0f;
    const std::array<std::pair<float, float>, 6> cases {{
      { 9.0f, eighth }, { 9.25f, eighth }, { 9.5f, eighth }, { 9.65f, eighth },
      { 9.75f, sixteenth }, { 10.0f, sixteenth }
    }};
    for (const auto& [modulated_tempo, expected] : cases) {
      tempo.set(modulated_tempo);
      chooser.process(1);
      expectClose(chooser.output()->buffer[0][0], expected, 1.0e-4f,
                  "Tempo modulated to " + std::to_string(modulated_tempo) + " picks the arm64 division");
    }
  }

  struct EnvelopeFixture {
    vital::Envelope envelope;
    std::array<vital::Value, vital::Envelope::kNumInputs> controls;
    vital::Output trigger;

    EnvelopeFixture() {
      for (int i = 0; i < vital::Envelope::kNumInputs; ++i)
        envelope.plug(&controls[static_cast<size_t>(i)], i);
      envelope.plug(&trigger, vital::Envelope::kTrigger);
      envelope.setSampleRate(1000);
      envelope.setControlRate(true);
      controls[vital::Envelope::kDelay].set(0.002f);
      controls[vital::Envelope::kAttack].set(0.003f);
      controls[vital::Envelope::kAttackPower].set(0.0f);
      controls[vital::Envelope::kHold].set(0.002f);
      controls[vital::Envelope::kDecay].set(0.004f);
      controls[vital::Envelope::kDecayPower].set(0.0f);
      controls[vital::Envelope::kSustain].set(0.25f);
      controls[vital::Envelope::kRelease].set(0.005f);
      controls[vital::Envelope::kReleasePower].set(0.0f);
      controls[vital::Envelope::kTempoSync].set(0.0f);
      controls[vital::Envelope::kBeatsPerSecond].set(2.0f);
    }
  };

  std::array<float, 2> renderEnvelope(const std::vector<int>& blocks, int total_samples) {
    EnvelopeFixture fixture;
    fixture.trigger.trigger(vital::poly_mask(-1), vital::poly_float(vital::kVoiceOn), vital::poly_int(0));
    int rendered = 0;
    size_t block_index = 0;
    while (rendered < total_samples) {
      const int block = std::min(blocks[block_index++ % blocks.size()], total_samples - rendered);
      fixture.envelope.process(block);
      fixture.trigger.clearTrigger();
      rendered += block;
    }
    return { fixture.envelope.output(vital::Envelope::kValue)->buffer[0][0],
             fixture.envelope.output(vital::Envelope::kPhase)->buffer[0][0] };
  }

  void testEnvelopeShortStages() {
    const auto reference = renderEnvelope({ 1 }, 24);
    const auto blocked = renderEnvelope({ 7, 5, 12 }, 24);
    expectClose(blocked[0], reference[0], 1.0e-6f,
                "Envelope value must not depend on control block boundaries");
    expectClose(blocked[1], reference[1], 1.0e-6f,
                "Envelope stage time must not depend on control block boundaries");
    expectClose(reference[0], 0.25f, 1.0e-5f,
                "Short delay/attack/hold/decay stages must reach sustain in one block");
  }

  void testEnvelopeRetriggerStartsFromCurrentValue() {
    EnvelopeFixture fixture;
    fixture.envelope.setControlRate(false);
    fixture.controls[vital::Envelope::kDelay].set(0.0f);
    fixture.controls[vital::Envelope::kAttack].set(0.1f);
    fixture.controls[vital::Envelope::kHold].set(0.0f);
    fixture.controls[vital::Envelope::kDecay].set(0.1f);
    fixture.controls[vital::Envelope::kSustain].set(1.0f);

    fixture.trigger.trigger(vital::poly_mask(-1), vital::poly_float(vital::kVoiceOn), vital::poly_int(0));
    fixture.envelope.process(50);
    fixture.trigger.clearTrigger();

    constexpr int kRetriggerSample = 16;
    fixture.trigger.trigger(vital::poly_mask(-1), vital::poly_float(vital::kVoiceOn),
                            vital::poly_int(kRetriggerSample));
    fixture.envelope.process(64);
    const vital::poly_float* output = fixture.envelope.output(vital::Envelope::kValue)->buffer;
    expect(output[kRetriggerSample][0] > 0.4f,
           "A modulation envelope retrigger must not jump from its audible value to zero");
    expect(std::abs(output[kRetriggerSample][0] - output[kRetriggerSample - 1][0]) < 1.0e-5f,
           "A modulation envelope retrigger must begin exactly at its preceding value");
  }

  void testForcedVoiceReleaseMatchesShortProfessionalHandoff() {
    EnvelopeFixture fixture;
    fixture.envelope.setControlRate(false);
    fixture.controls[vital::Envelope::kDelay].set(0.0f);
    fixture.controls[vital::Envelope::kAttack].set(0.001f);
    fixture.controls[vital::Envelope::kHold].set(0.0f);
    fixture.controls[vital::Envelope::kDecay].set(0.001f);
    fixture.controls[vital::Envelope::kSustain].set(1.0f);

    fixture.trigger.trigger(vital::poly_mask(-1), vital::poly_float(vital::kVoiceOn), vital::poly_int(0));
    fixture.envelope.process(8);
    fixture.trigger.clearTrigger();
    fixture.trigger.trigger(vital::poly_mask(-1), vital::poly_float(vital::kVoiceKill), vital::poly_int(0));
    fixture.envelope.process(32);

    int first_silent_sample = -1;
    const vital::poly_float* output = fixture.envelope.output(vital::Envelope::kValue)->buffer;
    for (int sample = 0; sample < 32; ++sample) {
      if (std::abs(output[sample][0]) <= 1.0e-6f) {
        first_silent_sample = sample;
        break;
      }
    }
    expect(first_silent_sample >= 10 && first_silent_sample <= 13,
           "A forced voice handoff must use the approximately 11 ms Surge-style uber release "
           "instead of retaining a 50 ms overlapping mono tail");
  }

  void testCollapsedModulationVoiceHandoffIsDezipped() {
    vital::ModulationConnectionProcessor processor(0);
    processor.setControlRate(false);
    processor.setSampleRate(1000);
    processor.setCollapsePolyphonicSource(true);

    constexpr int kSamples = 16;
    vital::poly_float* output = processor.output()->buffer;
    for (int sample = 0; sample < kSamples; ++sample)
      output[sample] = vital::poly_float(0.25f, 0.25f, 1.0f, 1.0f);
    processor.collapseToVoice(vital::constants::kFirstMask, kSamples);

    for (int sample = 0; sample < kSamples; ++sample)
      output[sample] = vital::poly_float(0.25f, 0.25f, 1.0f, 1.0f);
    processor.collapseToVoice(~vital::constants::kFirstMask, kSamples, 4);

    for (int sample = 0; sample < 4; ++sample)
      expectClose(output[sample][0], 0.25f, 0.0f,
                  "A global modulation route must retain its previous value before the MIDI event");
    expectClose(output[4][0], 0.25f, 1.0e-6f,
                "A polyphonic-to-global modulation handoff must begin without a parameter step");
    expect(output[5][0] > output[4][0] && output[kSamples - 1][0] <= 1.0f,
           "A modulation handoff must move smoothly toward the new voice's value");

    float largest_step = 0.0f;
    for (int sample = 1; sample < kSamples; ++sample)
      largest_step = std::max(largest_step, std::abs(output[sample][0] - output[sample - 1][0]));
    expect(largest_step < 0.08f,
           "The approximately 11 ms modulation handoff must not contain an audible control step");
  }

  void testReleasedVoiceReusePreservesAudibleState() {
    vital::AggregateVoice aggregate;
    vital::Voice voice(&aggregate);
    voice.activate(60, 60.0f, 1.0f, 60.0f, 1, 1, 0, 0);
    expect(voice.needsSourceResetOnActivation(),
           "A newly allocated voice must reset its source state");
    voice.completeVoiceEvent();
    voice.deactivate(127);
    voice.completeVoiceEvent();
    // A long release may remain audible for seconds. Its age must not change
    // the state-preserving reuse policy.
    voice.activate(62, 62.0f, 1.0f, 60.0f, 1, 2, 0, 0);
    expect(!voice.needsSourceResetOnActivation(),
           "Reusing any released voice must preserve audible oscillator and filter state");
    voice.completeVoiceEvent();
    voice.markDead();
    voice.activate(64, 64.0f, 1.0f, 62.0f, 1, 3, 0, 0);
    expect(voice.needsSourceResetOnActivation(),
           "A voice may reset source state after its amplitude reaches zero");
  }

  void configureReleaseClickEngine(vital::SoundEngine& engine) {
    const vital::control_map controls = engine.getControls();
    controls.at("oversampling")->set(0.0f);
    controls.at("polyphony")->set(1.0f);
    controls.at("voice_override")->set(vital::VoiceHandler::kKill);
    controls.at("env_1_attack")->set(0.001f);
    controls.at("env_1_decay")->set(0.001f);
    controls.at("env_1_sustain")->set(1.0f);
    controls.at("env_1_release")->set(1.0f);
    controls.at("env_1_release_power")->set(0.0f);
    controls.at("osc_1_fixed_frequency_mode")->set(1.0f);
    controls.at("osc_1_fixed_frequency")->set(41.0f);
    controls.at("osc_1_random_phase")->set(0.0f);
    controls.at("osc_1_unison_voices")->set(1.0f);
    controls.at("osc_1_level")->set(1.0f);
    engine.checkOversampling();
  }

  void processEngineSamples(vital::SoundEngine& engine, int total_samples) {
    int rendered = 0;
    while (rendered < total_samples) {
      const int block = std::min(vital::kMaxBufferSize, total_samples - rendered);
      engine.process(block);
      rendered += block;
    }
  }

  void testLongReleaseRetriggerIsClickless() {
    vital::SoundEngine reference;
    vital::SoundEngine retriggered;
    configureReleaseClickEngine(reference);
    configureReleaseClickEngine(retriggered);

    for (vital::SoundEngine* engine : { &reference, &retriggered }) {
      engine->noteOn(24, 1.0f, 0, 0);
      processEngineSamples(*engine, 4096);
      engine->noteOff(24, 0.5f, 0, 0);
      processEngineSamples(*engine, 4096);
    }

    constexpr int kRetriggerSample = 64;
    retriggered.noteOn(24, 1.0f, kRetriggerSample, 0);
    reference.process(vital::kMaxBufferSize);
    retriggered.process(vital::kMaxBufferSize);

    const vital::poly_float* reference_output = reference.output()->buffer;
    const vital::poly_float* retriggered_output = retriggered.output()->buffer;
    float maximum_pretrigger_difference = 0.0f;
    for (int sample = 0; sample < kRetriggerSample; ++sample) {
      maximum_pretrigger_difference = std::max(maximum_pretrigger_difference,
          std::abs(retriggered_output[sample][0] - reference_output[sample][0]));
    }
    expect(maximum_pretrigger_difference < 1.0e-5f,
           "A queued retrigger must not alter samples before its event");

    const float retrigger_jump = std::abs(retriggered_output[kRetriggerSample][0] -
                                          retriggered_output[kRetriggerSample - 1][0]);
    const float natural_jump = std::abs(reference_output[kRetriggerSample][0] -
                                        reference_output[kRetriggerSample - 1][0]);
    expect(retrigger_jump <= natural_jump + 0.01f,
           "Reusing a voice during a long Env 1 release must not introduce a waveform reset click "
           "(retrigger jump=" + std::to_string(retrigger_jump) +
           ", natural jump=" + std::to_string(natural_jump) + ")");
  }

  void testHeldMonoRetriggerIsClickless() {
    vital::SoundEngine reference;
    vital::SoundEngine retriggered;
    configureReleaseClickEngine(reference);
    configureReleaseClickEngine(retriggered);

    for (vital::SoundEngine* engine : { &reference, &retriggered }) {
      engine->noteOn(24, 1.0f, 0, 0);
      processEngineSamples(*engine, 4096);
    }

    constexpr int kRetriggerSample = 64;
    retriggered.noteOn(24, 1.0f, kRetriggerSample, 0);
    reference.process(vital::kMaxBufferSize);
    retriggered.process(vital::kMaxBufferSize);

    const vital::poly_float* reference_output = reference.output()->buffer;
    const vital::poly_float* retriggered_output = retriggered.output()->buffer;
    float maximum_pretrigger_difference = 0.0f;
    for (int sample = 0; sample < kRetriggerSample; ++sample) {
      maximum_pretrigger_difference = std::max(maximum_pretrigger_difference,
          std::abs(retriggered_output[sample][0] - reference_output[sample][0]));
    }
    expect(maximum_pretrigger_difference < 1.0e-5f,
           "A held-note mono retrigger must not alter samples before its MIDI event");

    const float retrigger_jump = std::abs(retriggered_output[kRetriggerSample][0] -
                                          retriggered_output[kRetriggerSample - 1][0]);
    const float natural_jump = std::abs(reference_output[kRetriggerSample][0] -
                                        reference_output[kRetriggerSample - 1][0]);
    expect(retrigger_jump <= natural_jump + 0.01f,
           "Retriggering a held mono note must use a continuous outgoing voice tail "
           "(retrigger jump=" + std::to_string(retrigger_jump) +
           ", natural jump=" + std::to_string(natural_jump) + ")");
  }

  void testHeldMonoLegatoIsSignalContinuous() {
    vital::SoundEngine reference;
    vital::SoundEngine transitioned;
    configureReleaseClickEngine(reference);
    configureReleaseClickEngine(transitioned);

    for (vital::SoundEngine* engine : { &reference, &transitioned }) {
      engine->getControls().at("legato")->set(1.0f);
      // SoundEngine latches the legato control at the audio-block boundary.
      engine->process(1);
      engine->noteOn(24, 1.0f, 0, 0);
      processEngineSamples(*engine, 4096);
    }

    constexpr int kTransitionSample = 64;
    transitioned.noteOn(36, 1.0f, kTransitionSample, 0);
    reference.process(vital::kMaxBufferSize);
    transitioned.process(vital::kMaxBufferSize);

    const vital::poly_float* reference_output = reference.output()->buffer;
    const vital::poly_float* transitioned_output = transitioned.output()->buffer;
    const float transition_jump = std::abs(transitioned_output[kTransitionSample][0] -
                                           transitioned_output[kTransitionSample - 1][0]);
    const float natural_jump = std::abs(reference_output[kTransitionSample][0] -
                                        reference_output[kTransitionSample - 1][0]);
    expect(transition_jump <= natural_jump + 1.0e-5f,
           "A held mono-legato pitch change must not add a waveform step "
           "(transition jump=" + std::to_string(transition_jump) +
           ", natural jump=" + std::to_string(natural_jump) + ")");
  }

  vital::modulation_change connectTestModWheelRoute(vital::SoundEngine& engine,
                                                     const std::string& destination) {
    auto* processor = engine.getModulationBank().atIndex(0)->modulation_processor.get();
    engine.getControls().at("modulation_1_amount")->set(1.0f);

    vital::modulation_change change {};
    change.source = engine.getModulationSource("mod_wheel");
    change.mono_destination = engine.getMonoModulationDestination(destination);
    change.poly_destination = engine.getPolyModulationDestination(destination);
    change.destination_scale = vital::Parameters::getParameterRange(destination);
    change.mono_modulation_switch = engine.getMonoModulationSwitch(destination);
    change.poly_modulation_switch = engine.getPolyModulationSwitch(destination);
    change.modulation_processor = processor;
    engine.connectModulation(change);
    return change;
  }

  vital::modulation_change connectTestMacroRoute(vital::SoundEngine& engine,
                                                  const std::string& destination) {
    auto* processor = engine.getModulationBank().atIndex(0)->modulation_processor.get();
    engine.getControls().at("modulation_1_amount")->set(1.0f);

    vital::modulation_change change {};
    change.source = engine.getModulationSource("macro_control_1");
    change.mono_destination = engine.getMonoModulationDestination(destination);
    change.poly_destination = engine.getPolyModulationDestination(destination);
    change.destination_scale = vital::Parameters::getParameterRange(destination);
    change.mono_modulation_switch = engine.getMonoModulationSwitch(destination);
    change.poly_modulation_switch = engine.getPolyModulationSwitch(destination);
    change.modulation_processor = processor;
    processor->setSourceSmoothing(true);
    engine.connectModulation(change);
    return change;
  }

  vital::modulation_change connectTestLfoRoute(vital::SoundEngine& engine,
                                                const std::string& destination) {
    auto* processor = engine.getModulationBank().atIndex(0)->modulation_processor.get();
    engine.getControls().at("modulation_1_amount")->set(1.0f);

    vital::modulation_change change {};
    change.source = engine.getModulationSource("lfo_1");
    change.mono_destination = engine.getMonoModulationDestination(destination);
    change.poly_destination = engine.getPolyModulationDestination(destination);
    change.destination_scale = vital::Parameters::getParameterRange(destination);
    change.mono_modulation_switch = engine.getMonoModulationSwitch(destination);
    change.poly_modulation_switch = engine.getPolyModulationSwitch(destination);
    change.modulation_processor = processor;
    engine.connectModulation(change);
    return change;
  }

  void configureMonoVoiceHandler(vital::VoiceHandler& voices, bool legato) {
    static vital::Value polyphony(1.0f);
    static vital::Value priority(vital::VoiceHandler::kRoundRobin);
    static vital::Value override_mode(vital::VoiceHandler::kKill);
    voices.plug(&polyphony, vital::VoiceHandler::kPolyphony);
    voices.plug(&priority, vital::VoiceHandler::kVoicePriority);
    voices.plug(&override_mode, vital::VoiceHandler::kVoiceOverride);
    voices.setLegato(legato);
    voices.init();
  }

  void testMonoRetriggerUsesSampleAccurateTailVoice() {
    vital::VoiceHandler voices(0, 1, true);
    configureMonoVoiceHandler(voices, false);

    voices.noteOn(60, 1.0f, 0, 0);
    voices.process(32);

    constexpr int kRetriggerSample = 16;
    voices.noteOn(64, 1.0f, kRetriggerSample, 0);
    voices.process(32);
    expect(voices.getNumActiveVoices() == 2,
           "Mono retrigger Kill mode must keep the outgoing tail and incoming note on separate voices");

    const vital::Output* event = voices.retrigger();
    bool found_note_on = false;
    bool found_kill = false;
    for (int lane = 0; lane < vital::poly_float::kSize; ++lane) {
      if (!event->trigger_mask[lane])
        continue;
      expect(static_cast<int>(event->trigger_offset[lane]) == kRetriggerSample,
             "Both sides of a mono voice handoff must start at the exact MIDI sample");
      found_note_on = found_note_on || event->trigger_value[lane] == vital::kVoiceOn;
      found_kill = found_kill || event->trigger_value[lane] == vital::kVoiceKill;
    }
    expect(found_note_on, "A mono voice handoff must trigger the fresh incoming voice");
    expect(found_kill, "A mono voice handoff must fade the outgoing voice at the note offset");
  }

  void testMonoLegatoKeepsVoiceAndRandomContinuous() {
    vital::VoiceHandler voices(0, 1, true);
    configureMonoVoiceHandler(voices, true);
    vital::TriggerRandom random;
    random.plug(voices.retrigger());

    voices.noteOn(60, 0.25f, 0, 0);
    voices.process(32);
    random.process(1);
    const vital::poly_float initial_random = random.output()->buffer[0];

    constexpr int kLegatoSample = 12;
    voices.noteOn(67, 1.0f, kLegatoSample, 0);
    voices.process(32);
    random.process(1);

    expect(voices.getNumActiveVoices() == 1,
           "A held mono-legato note change must stay on the existing voice");
    expect(voices.voice_event()->trigger_mask.anyMask(),
           "A mono-legato pitch change must still publish its note event");
    expect(!voices.retrigger()->trigger_mask.anyMask(),
           "A held mono-legato note change must not retrigger envelopes, LFOs, samples, or random");
    expect(!voices.reset()->trigger_mask.anyMask(),
           "A held mono-legato note change must not reset oscillator or filter state");
    const vital::poly_float held_velocity = voices.velocity()->trigger_value & voices.getCurrentVoiceMask();
    expectClose(vital::utils::maxFloat(held_velocity), 0.25f, 0.0f,
                "Single-trigger mono legato must preserve the sounding voice's velocity articulation");
    for (int lane = 0; lane < vital::poly_float::kSize; ++lane) {
      expectClose(random.output()->buffer[0][lane], initial_random[lane], 0.0f,
                  "Note-on random must remain constant throughout a legato phrase");
    }
  }

  void testLegatoModeSeamlesslyRetriggersAfterARealGap() {
    vital::VoiceHandler voices(0, 1, true);
    configureMonoVoiceHandler(voices, true);

    voices.noteOn(60, 1.0f, 0, 0);
    voices.process(32);
    voices.noteOff(60, 0.5f, 0, 0);
    voices.process(32);

    constexpr int kRetriggerSample = 20;
    voices.noteOn(64, 1.0f, kRetriggerSample, 0);
    voices.process(32);
    expect(voices.getNumActiveVoices() == 1,
           "A post-gap note in mono Legato mode must reuse the released voice without a Kill tail");

    const vital::Output* event = voices.retrigger();
    bool found_note_on = false;
    bool found_kill = false;
    for (int lane = 0; lane < vital::poly_float::kSize; ++lane) {
      if (!event->trigger_mask[lane])
        continue;
      expect(static_cast<int>(event->trigger_offset[lane]) == kRetriggerSample,
             "A seamless post-gap retrigger must preserve its exact MIDI sample offset");
      found_note_on = found_note_on || event->trigger_value[lane] == vital::kVoiceOn;
      found_kill = found_kill || event->trigger_value[lane] == vital::kVoiceKill;
    }
    expect(found_note_on && !found_kill,
           "A post-gap Legato note must retrigger without replacing its natural release with a Kill fade");
    expect(!voices.reset()->trigger_mask.anyMask(),
           "Reusing an audible released voice must preserve oscillator and filter state");
  }

  void testLegatoTransitionIgnoresFadingTail() {
    vital::VoiceHandler voices(0, 1, true);
    configureMonoVoiceHandler(voices, false);

    voices.noteOn(60, 1.0f, 0, 0);
    voices.process(32);
    // Retrig mode leaves note 60 on a Kill tail and starts note 64 on a
    // fresh voice.
    voices.noteOn(64, 1.0f, 4, 0);
    voices.process(32);
    expect(voices.getNumActiveVoices() == 2,
           "A new mono phrase must have one current voice plus one outgoing tail");

    // Switching to Legato for the next held-key transition must reuse note 64,
    // not revive note 60's fading voice or allocate a third voice.
    voices.setLegato(true);
    voices.noteOn(67, 1.0f, 18, 0);
    voices.process(32);
    expect(voices.getNumActiveVoices() == 2,
           "Mono legato must ignore a still-fading tail and reuse only the current held voice");
    expect(!voices.retrigger()->trigger_mask.anyMask(),
           "A held legato transition must remain continuous while an older tail is fading");
  }

  void testReverbMixConsumesAudioRateModulation() {
    vital::ReverbModule module;
    module.init();
    vital::Processor* mix_destination = module.getMonoModulationDestination("reverb_dry_wet");
    expect(mix_destination != nullptr && !mix_destination->isControlRate(),
           "Reverb mix must expose an audio-rate modulation destination");

    vital::Reverb reverb;
    std::array<vital::Value, vital::Reverb::kNumInputs> controls;
    for (int input = vital::Reverb::kDecayTime; input < vital::Reverb::kNumInputs; ++input)
      reverb.plug(&controls[static_cast<size_t>(input)], input);
    controls[vital::Reverb::kDecayTime].set(1.0f);
    controls[vital::Reverb::kPreLowCutoff].set(0.0f);
    controls[vital::Reverb::kPreHighCutoff].set(128.0f);
    controls[vital::Reverb::kLowCutoff].set(0.0f);
    controls[vital::Reverb::kLowGain].set(0.0f);
    controls[vital::Reverb::kHighCutoff].set(128.0f);
    controls[vital::Reverb::kHighGain].set(0.0f);
    controls[vital::Reverb::kChorusAmount].set(0.0f);
    controls[vital::Reverb::kChorusFrequency].set(0.0f);
    controls[vital::Reverb::kSize].set(0.5f);
    controls[vital::Reverb::kDelay].set(0.0f);

    vital::Output wet;
    reverb.plug(&wet, vital::Reverb::kWet);
    constexpr int kSamples = 16;
    std::array<vital::poly_float, kSamples> input;
    for (int sample = 0; sample < kSamples; ++sample) {
      input[static_cast<size_t>(sample)] = vital::poly_float(1.0f, 1.0f, 0.0f, 0.0f);
      wet.buffer[sample] = sample % 2 == 0 ? 0.0f : 1.0f;
    }
    reverb.hardReset();
    reverb.processWithInput(input.data(), kSamples);

    expectClose(reverb.output()->buffer[0][0], 1.0f, 1.0e-5f,
                "A zero-wet sample must remain fully dry");
    expect(std::abs(reverb.output()->buffer[1][0]) < 1.0e-5f,
           "The following fully-wet sample must use its own modulation value");
    expectClose(reverb.output()->buffer[2][0], 1.0f, 1.0e-5f,
                "Reverb mix must follow audio-rate modulation on every sample");
  }

  void testMonoVoiceLaneRotationKeepsLfoToFxIdentical() {
    vital::SoundEngine engine;
    configureReleaseClickEngine(engine);
    const vital::control_map controls = engine.getControls();
    controls.at("legato")->set(1.0f);
    controls.at("env_1_release")->set(0.02f);
    controls.at("lfo_1_sync_type")->set(vital::SynthLfo::kTrigger);
    controls.at("lfo_1_sync")->set(vital::TempoChooser::kTempoMode);
    controls.at("lfo_1_tempo")->set(9.0f);
    controls.at("lfo_1_smooth_mode")->set(1.0f);
    controls.at("lfo_1_smooth_time")->set(-9.769477844238281f);
    controls.at("lfo_1_fade_time")->set(0.0f);
    controls.at("lfo_1_delay_time")->set(0.0f);
    engine.getLfoSource(0)->jsonToState({
      { "num_points", 4 },
      { "points", { 0.0f, 1.0f, 0.2198067605f, 0.0f,
                      0.5f, 0.1305688769f, 1.0f, 1.0f } },
      { "powers", { 0.0f, 0.0f, -2.6808521748f, 0.0f } },
      { "smooth", false }
    });
    engine.setBpm(130.0f);
    const auto change = connectTestLfoRoute(engine, "reverb_dry_wet");
    engine.process(1);

    constexpr int kCaptureSamples = 256;
    std::array<std::array<float, kCaptureSamples>, 4> attack_captures {};
    std::array<std::array<float, kCaptureSamples>, 4> release_captures {};
    std::array<std::array<float, kCaptureSamples>, 4> idle_tail_captures {};
    // The engine accepts at most kMaxBufferSize host samples per call. Keep
    // the full 256-sample capture, but collect it through valid internal blocks.
    auto capture = [&](std::array<float, kCaptureSamples>& destination) {
      for (int cursor = 0; cursor < kCaptureSamples; cursor += vital::kMaxBufferSize) {
        const int block = std::min(vital::kMaxBufferSize, kCaptureSamples - cursor);
        engine.process(block);
        for (int sample = 0; sample < block; ++sample)
          destination[static_cast<size_t>(cursor + sample)] =
              change.modulation_processor->output()->buffer[sample][0];
      }
    };
    for (int repetition = 0; repetition < 4; ++repetition) {
      engine.noteOn(48 + repetition * 5, 1.0f, 0, 0);
      capture(attack_captures[static_cast<size_t>(repetition)]);
      processEngineSamples(engine, 6615 - kCaptureSamples);
      engine.noteOff(48 + repetition * 5, 0.5f, 0, 0);
      capture(release_captures[static_cast<size_t>(repetition)]);
      processEngineSamples(engine, 8192);
      capture(idle_tail_captures[static_cast<size_t>(repetition)]);
    }

    for (int repetition = 1; repetition < 4; ++repetition) {
      float maximum_attack_difference = 0.0f;
      float maximum_release_difference = 0.0f;
      float maximum_idle_tail_difference = 0.0f;
      // The first note has no preceding global modulation value to hand off
      // from. Once primed, every equally timed repetition must follow the same
      // dezipped attack regardless of which physical voice lane it uses.
      const int attack_reference = 1;
      for (int sample = 0; sample < kCaptureSamples; ++sample) {
        if (repetition > 1) {
          maximum_attack_difference = std::max(maximum_attack_difference,
              std::abs(attack_captures[static_cast<size_t>(repetition)][static_cast<size_t>(sample)] -
                       attack_captures[static_cast<size_t>(attack_reference)][static_cast<size_t>(sample)]));
        }
        maximum_release_difference = std::max(maximum_release_difference,
            std::abs(release_captures[static_cast<size_t>(repetition)][static_cast<size_t>(sample)] -
                     release_captures[0][static_cast<size_t>(sample)]));
        maximum_idle_tail_difference = std::max(maximum_idle_tail_difference,
            std::abs(idle_tail_captures[static_cast<size_t>(repetition)][static_cast<size_t>(sample)] -
                     idle_tail_captures[0][static_cast<size_t>(sample)]));
      }
      expect(maximum_attack_difference < 1.0e-6f && maximum_release_difference < 1.0e-6f &&
                 maximum_idle_tail_difference < 1.0e-6f,
             "Rotating mono notes through physical voice lanes must not change an LFO-to-FX trajectory "
             "(attack difference=" + std::to_string(maximum_attack_difference) +
             ", release difference=" + std::to_string(maximum_release_difference) +
             ", idle tail difference=" + std::to_string(maximum_idle_tail_difference) + ")");
    }
  }

  void testStationaryZeroModWheelRouteIsTransparent() {
    vital::SoundEngine reference;
    vital::SoundEngine modulated;
    configureReleaseClickEngine(reference);
    configureReleaseClickEngine(modulated);
    for (vital::SoundEngine* engine : { &reference, &modulated }) {
      const auto controls = engine->getControls();
      controls.at("filter_1_on")->set(1.0f);
      controls.at("filter_1_cutoff")->set(72.0f);
      controls.at("filter_1_resonance")->set(0.2f);
      engine->setModWheelAllChannels(0.0f);
    }

    const auto change = connectTestModWheelRoute(modulated, "filter_1_cutoff");
    reference.noteOn(48, 1.0f, 0, 0);
    modulated.noteOn(48, 1.0f, 0, 0);
    processEngineSamples(reference, 512);
    processEngineSamples(modulated, 512);

    float maximum_difference = 0.0f;
    for (int sample = 0; sample < vital::kMaxBufferSize; ++sample) {
      maximum_difference = std::max(maximum_difference,
                                    std::abs(reference.output()->buffer[sample][0] -
                                             modulated.output()->buffer[sample][0]));
    }
    expect(maximum_difference < 1.0e-5f,
           "Connecting a zero Mod Wheel to filter cutoff must not alter the patch");
    modulated.disconnectModulation(change);
  }

  void testMovingModWheelFilterRouteIsContinuous() {
    vital::SoundEngine engine;
    configureReleaseClickEngine(engine);
    const auto controls = engine.getControls();
    controls.at("filter_1_on")->set(1.0f);
    controls.at("filter_1_cutoff")->set(72.0f);
    controls.at("filter_1_resonance")->set(0.2f);
    controls.at("oversampling")->set(1.0f);
    engine.checkOversampling();
    engine.setModWheelAllChannels(0.0f);

    auto change = connectTestModWheelRoute(engine, "filter_1_cutoff");
    controls.at("modulation_1_amount")->set(0.25f);
    engine.noteOn(48, 1.0f, 0, 0);
    processEngineSamples(engine, 512);
    engine.setModWheelAllChannels(1.0f);
    engine.process(vital::kMaxBufferSize);

    const auto* processor = change.modulation_processor;
    const auto* modulation = processor->output()->buffer;
    float largest_modulation_step = 0.0f;
    float largest_audio_step = 0.0f;
    for (int sample = 1; sample < vital::kMaxBufferSize; ++sample) {
      largest_modulation_step = std::max(largest_modulation_step,
                                         std::abs(modulation[sample][0] - modulation[sample - 1][0]));
      largest_audio_step = std::max(largest_audio_step,
                                    std::abs(engine.output()->buffer[sample][0] -
                                             engine.output()->buffer[sample - 1][0]));
    }
    expect(modulation[0][0] >= 0.0f && modulation[vital::kMaxBufferSize - 1][0] <= 32.0f,
           "Moving Mod Wheel filter modulation must remain within its assigned cutoff range");
    expect(largest_modulation_step < 0.25f,
           "Moving Mod Wheel filter modulation must not contain a block-sized parameter jump");
    expect(largest_audio_step < 1.0f,
           "Moving Mod Wheel filter modulation must not create a catastrophic audio discontinuity");
    engine.disconnectModulation(change);
  }

  void testStaticMacroOscillatorLevelRouteIsTransparent() {
    vital::SoundEngine direct;
    vital::SoundEngine macro_routed;
    configureReleaseClickEngine(direct);
    configureReleaseClickEngine(macro_routed);
    for (vital::SoundEngine* engine : { &direct, &macro_routed }) {
      engine->getControls().at("oversampling")->set(1.0f);
      engine->checkOversampling();
    }

    constexpr float kLevel = 0.625f;
    direct.getControls().at("osc_1_level")->set(kLevel);
    macro_routed.getControls().at("osc_1_level")->set(0.0f);
    macro_routed.getControls().at("macro_control_1")->set(kLevel);
    const auto change = connectTestMacroRoute(macro_routed, "osc_1_level");

    // Let both the base level and macro route reach their identical stationary
    // value before comparing the oscillator signal itself.
    processEngineSamples(direct, 48000);
    processEngineSamples(macro_routed, 48000);
    direct.noteOn(48, 1.0f, 0, 0);
    macro_routed.noteOn(48, 1.0f, 0, 0);
    direct.process(vital::kMaxBufferSize);
    macro_routed.process(vital::kMaxBufferSize);

    float maximum_difference = 0.0f;
    for (int sample = 0; sample < vital::kMaxBufferSize; ++sample) {
      maximum_difference = std::max(maximum_difference,
                                    std::abs(direct.output()->buffer[sample][0] -
                                             macro_routed.output()->buffer[sample][0]));
    }
    expect(maximum_difference < 1.0e-5f,
           "A stationary macro-to-oscillator-level route must be sonically identical to the same "
           "direct oscillator level (maximum difference=" + std::to_string(maximum_difference) + ")");
    macro_routed.disconnectModulation(change);
  }

  std::vector<float> renderMacroOscillatorLevelTransition(const std::vector<int>& blocks) {
    vital::SoundEngine engine;
    configureReleaseClickEngine(engine);
    engine.getControls().at("osc_1_level")->set(0.0f);
    engine.getControls().at("macro_control_1")->set(0.0f);
    const auto change = connectTestMacroRoute(engine, "osc_1_level");
    processEngineSamples(engine, 48000);
    engine.noteOn(48, 1.0f, 0, 0);
    processEngineSamples(engine, 512);

    engine.getControls().at("macro_control_1")->set(1.0f);
    std::vector<float> rendered;
    constexpr int kRenderSamples = 2048;
    rendered.reserve(kRenderSamples);
    int position = 0;
    size_t block_index = 0;
    while (position < kRenderSamples) {
      const int block = std::min(blocks[block_index++ % blocks.size()], kRenderSamples - position);
      engine.process(block);
      for (int sample = 0; sample < block; ++sample)
        rendered.push_back(engine.output()->buffer[sample][0]);
      position += block;
    }
    engine.disconnectModulation(change);
    return rendered;
  }

  void testMacroOscillatorLevelRouteIsBlockInvariant() {
    const std::vector<float> uniform = renderMacroOscillatorLevelTransition({ 128 });
    const std::vector<float> event_partitioned =
        renderMacroOscillatorLevelTransition({ 17, 111, 29, 99, 64, 64 });
    float maximum_difference = 0.0f;
    for (size_t sample = 0; sample < uniform.size(); ++sample)
      maximum_difference = std::max(maximum_difference,
                                    std::abs(uniform[sample] - event_partitioned[sample]));
    expect(maximum_difference < 1.0e-4f,
           "Macro modulation of oscillator level must not depend on MIDI/host block partitioning "
           "(maximum difference=" + std::to_string(maximum_difference) + ")");
  }

  struct GranularFixture {
    vital::GranularSource granular;
    std::array<vital::Value, vital::GranularSource::kNumInputs> controls;
    vital::Output reset;

    GranularFixture() {
      for (int i = 0; i < vital::GranularSource::kNumInputs; ++i)
        granular.plug(&controls[static_cast<size_t>(i)], i);
      granular.plug(&reset, vital::GranularSource::kReset);
      granular.setSampleRate(48000);
      granular.setSeed(0x41544c41);

      controls[vital::GranularSource::kMidi].set(60.0f);
      controls[vital::GranularSource::kKeytrack].set(0.0f);
      controls[vital::GranularSource::kRootKey].set(60.0f);
      controls[vital::GranularSource::kLevel].set(1.0f);
      controls[vital::GranularSource::kTranspose].set(0.0f);
      controls[vital::GranularSource::kTransposeQuantize].set(0.0f);
      controls[vital::GranularSource::kTransposeQuantizeScale].set(14.0f);
      controls[vital::GranularSource::kTune].set(0.0f);
      controls[vital::GranularSource::kStart].set(0.0f);
      controls[vital::GranularSource::kEnd].set(1.0f);
      controls[vital::GranularSource::kMode].set(1.0f);
      controls[vital::GranularSource::kMidiDensity].set(0.0f);
      controls[vital::GranularSource::kGrainCount].set(64.0f);
      controls[vital::GranularSource::kDensity].set(96.0f);
      controls[vital::GranularSource::kGrainSize].set(80.0f);
      controls[vital::GranularSource::kSpeed].set(1.0f);
      controls[vital::GranularSource::kPosition].set(0.35f);
      controls[vital::GranularSource::kPositionMod].set(0.0f);
      controls[vital::GranularSource::kPositionModRate].set(0.25f);
      controls[vital::GranularSource::kRandomPosition].set(0.0f);
      controls[vital::GranularSource::kRandomVolume].set(0.0f);
      controls[vital::GranularSource::kRandomPan].set(0.0f);
      controls[vital::GranularSource::kRandomPitch].set(0.0f);
      controls[vital::GranularSource::kInterval].set(12.0f);
      controls[vital::GranularSource::kIntervalChance].set(0.0f);
      controls[vital::GranularSource::kDirection].set(0.0f);
      controls[vital::GranularSource::kLowCutoff].set(8.0f);
      controls[vital::GranularSource::kHighCutoff].set(136.0f);
      controls[vital::GranularSource::kPan].set(0.0f);
      controls[vital::GranularSource::kNoteCount].set(0.0f);

      std::vector<float> sample(8192);
      for (size_t i = 0; i < sample.size(); ++i) {
        const float phase = static_cast<float>(i) / sample.size();
        sample[i] = 0.65f * std::sin(phase * vital::kPi * 2.0f * 37.0f) +
                    0.25f * std::sin(phase * vital::kPi * 2.0f * 91.0f);
      }
      granular.getSample()->loadSample(sample.data(), static_cast<int>(sample.size()), 48000);
    }

    void triggerReset() {
      reset.trigger(vital::poly_mask(-1), vital::poly_float(vital::kVoiceOn), vital::poly_int(0));
    }
  };

  std::vector<std::array<float, 2>> renderGranular(const std::vector<int>& blocks, int total_samples) {
    GranularFixture fixture;
    fixture.controls[vital::GranularSource::kPositionMod].set(0.7f);
    fixture.controls[vital::GranularSource::kPositionModRate].set(3.25f);
    fixture.controls[vital::GranularSource::kRandomPosition].set(0.8f);
    fixture.controls[vital::GranularSource::kRandomVolume].set(0.55f);
    fixture.controls[vital::GranularSource::kRandomPan].set(0.65f);
    fixture.controls[vital::GranularSource::kRandomPitch].set(7.0f);
    fixture.controls[vital::GranularSource::kIntervalChance].set(0.4f);
    fixture.controls[vital::GranularSource::kDirection].set(2.0f);
    fixture.controls[vital::GranularSource::kTimingJitter].set(0.6f);
    fixture.controls[vital::GranularSource::kWindowShape].set(vital::GranularSource::kSoftPlateau);
    fixture.triggerReset();

    std::vector<std::array<float, 2>> result;
    result.reserve(static_cast<size_t>(total_samples));
    int rendered = 0;
    size_t block_index = 0;
    while (rendered < total_samples) {
      const int block = std::min(blocks[block_index++ % blocks.size()], total_samples - rendered);
      fixture.granular.process(block);
      fixture.reset.clearTrigger();
      for (int sample = 0; sample < block; ++sample) {
        const vital::poly_float value = fixture.granular.output(vital::GranularSource::kRaw)->buffer[sample];
        result.push_back({ value[0], value[1] });
      }
      rendered += block;
    }
    return result;
  }

  void testGranularBlockInvariance() {
    const auto reference = renderGranular({ 1 }, 4096);
    const auto blocked = renderGranular({ 1, 7, 32, 63, 127, 128 }, 4096);
    float maximum_left_difference = 0.0f;
    float maximum_right_difference = 0.0f;
    for (size_t i = 0; i < reference.size(); ++i) {
      maximum_left_difference = std::max(maximum_left_difference, std::abs(blocked[i][0] - reference[i][0]));
      maximum_right_difference = std::max(maximum_right_difference, std::abs(blocked[i][1] - reference[i][1]));
    }
    expect(maximum_left_difference <= 2.0e-5f,
           "Granular scheduling and modulation must be block-size invariant (maximum difference=" +
           std::to_string(maximum_left_difference) + ")");
    expect(maximum_right_difference <= 2.0e-5f,
           "Granular stereo scheduling must be block-size invariant (maximum difference=" +
           std::to_string(maximum_right_difference) + ")");
  }

  void testGranularStereoGrainsStayPaired() {
    GranularFixture fixture;
    fixture.controls[vital::GranularSource::kRandomPosition].set(1.0f);
    fixture.controls[vital::GranularSource::kRandomVolume].set(1.0f);
    fixture.controls[vital::GranularSource::kRandomPitch].set(12.0f);
    fixture.controls[vital::GranularSource::kIntervalChance].set(0.5f);
    fixture.controls[vital::GranularSource::kDirection].set(2.0f);
    fixture.triggerReset();
    fixture.granular.process(vital::kMaxBufferSize);

    const vital::poly_float* output = fixture.granular.output(vital::GranularSource::kRaw)->buffer;
    for (int sample = 0; sample < vital::kMaxBufferSize; ++sample)
      expectClose(output[sample][1], output[sample][0], 1.0e-6f,
                  "A mono sample must use paired random grain decisions across stereo");
  }

  void testGranularGrainsFinishOutsideLaunchRegion() {
    GranularFixture fixture;
    std::vector<float> constant_sample(8192, 1.0f);
    fixture.granular.getSample()->loadSample(constant_sample.data(), static_cast<int>(constant_sample.size()), 48000);
    fixture.controls[vital::GranularSource::kEnd].set(0.002f);
    fixture.controls[vital::GranularSource::kPosition].set(0.99f);
    fixture.controls[vital::GranularSource::kDensity].set(1.0f);
    fixture.controls[vital::GranularSource::kGrainCount].set(1.0f);
    fixture.controls[vital::GranularSource::kGrainSize].set(100.0f);
    fixture.triggerReset();

    float late_grain_sample = 0.0f;
    int rendered = 0;
    while (rendered <= 3000) {
      const int block = std::min(vital::kMaxBufferSize, 3001 - rendered);
      fixture.granular.process(block);
      fixture.reset.clearTrigger();
      if (rendered + block > 3000)
        late_grain_sample = fixture.granular.output(vital::GranularSource::kRaw)->buffer[3000 - rendered][0];
      rendered += block;
    }
    expect(std::abs(late_grain_sample) > 0.25f,
           "A grain launched near a region edge must finish its window instead of being cut short");
  }


  // Renders a constant sample with short grains and returns grain onset samples.
  std::vector<int> granularOnsets(GranularFixture& fixture, int total_samples, double start_seconds,
                                  std::vector<int>* grain_lengths = nullptr) {
    std::vector<float> ones(8192, 1.0f);
    fixture.granular.getSample()->loadSample(ones.data(), static_cast<int>(ones.size()), 48000);
    fixture.controls[vital::GranularSource::kGrainCount].set(64.0f);
    fixture.triggerReset();

    std::vector<int> onsets;
    bool sounding = false;
    int onset = 0;
    int rendered = 0;
    while (rendered < total_samples) {
      const int block = std::min(vital::kMaxBufferSize, total_samples - rendered);
      fixture.granular.correctToTime(start_seconds + rendered / 48000.0);
      fixture.granular.process(block);
      fixture.reset.clearTrigger();
      const vital::poly_float* output = fixture.granular.output(vital::GranularSource::kRaw)->buffer;
      for (int i = 0; i < block; ++i) {
        const bool now = std::abs(output[i][0]) > 1.0e-7f;
        if (now && !sounding) {
          onset = rendered + i;
          onsets.push_back(onset);
        }
        if (!now && sounding && grain_lengths)
          grain_lengths->push_back(rendered + i - onset);
        sounding = now;
      }
      rendered += block;
    }
    return onsets;
  }

  void testGranularGridTimingFollowsHostBeats() {
    // 120 BPM, 1/16 grid: one grain every 6000 samples at 48 kHz, on the
    // host's grid lines, plus the grain that starts each note.
    GranularFixture fixture;
    fixture.controls[vital::GranularSource::kBeatsPerSecond].set(2.0f);
    fixture.controls[vital::GranularSource::kTimingMode].set(vital::GranularSource::kGridTiming);
    fixture.controls[vital::GranularSource::kGridRate].set(4.0f);
    fixture.controls[vital::GranularSource::kGrainSize].set(10.0f);
    // Density must be ignored in grid timing.
    fixture.controls[vital::GranularSource::kDensity].set(120.0f);
    // The note starts 0.05 s into the song: next grid line is 0.125 s.
    const auto onsets = granularOnsets(fixture, 20000, 0.05);
    const std::vector<int> expected = { 0, 3600, 9600, 15600 };
    bool matches = onsets.size() == expected.size();
    for (size_t i = 0; matches && i < expected.size(); ++i)
      matches = std::abs(onsets[i] - expected[i]) <= 2;
    std::string found;
    for (int value : onsets)
      found += " " + std::to_string(value);
    expect(matches, "Grid-timed grains must start with the note and then land on host 1/16 grid lines (onsets:" +
                    found + ")");

    GranularFixture triplet;
    triplet.controls[vital::GranularSource::kBeatsPerSecond].set(2.0f);
    triplet.controls[vital::GranularSource::kTimingMode].set(vital::GranularSource::kGridTripletTiming);
    triplet.controls[vital::GranularSource::kGridRate].set(3.0f);
    triplet.controls[vital::GranularSource::kGrainSize].set(10.0f);
    const auto triplet_onsets = granularOnsets(triplet, 24010, 0.0);
    // 1/8 triplet at 120 BPM = 1/6 s = 8000 samples.
    expect(triplet_onsets.size() == 4 && std::abs(triplet_onsets[3] - 24000) <= 2,
           "Triplet grid timing must space grains by two thirds of the straight step");
  }

  void testGranularGridSizeFollowsTempo() {
    GranularFixture fixture;
    fixture.controls[vital::GranularSource::kBeatsPerSecond].set(2.0f);
    fixture.controls[vital::GranularSource::kTimingMode].set(vital::GranularSource::kGridTiming);
    fixture.controls[vital::GranularSource::kGridRate].set(4.0f);
    fixture.controls[vital::GranularSource::kSizeMode].set(vital::GranularSource::kGridStepSize);
    fixture.controls[vital::GranularSource::kGridLength].set(0.5f);
    std::vector<int> lengths;
    granularOnsets(fixture, 13000, 0.0, &lengths);
    // Half of a 6000-sample step; the window reaches zero at both ends.
    expect(!lengths.empty() && std::abs(lengths[0] - 3000) <= 3,
           "Grid-step grain size must equal the step length times the grid length (length=" +
           std::to_string(lengths.empty() ? -1 : lengths[0]) + ")");
  }

  void testGranularTimingJitterVariesSpacing() {
    GranularFixture regular;
    regular.controls[vital::GranularSource::kDensity].set(20.0f);
    regular.controls[vital::GranularSource::kGrainSize].set(5.0f);
    const auto even = granularOnsets(regular, 48000, 0.0);

    GranularFixture jittered;
    jittered.controls[vital::GranularSource::kDensity].set(20.0f);
    jittered.controls[vital::GranularSource::kGrainSize].set(5.0f);
    jittered.controls[vital::GranularSource::kTimingJitter].set(1.0f);
    const auto uneven = granularOnsets(jittered, 48000, 0.0);

    int distinct_even = 0, distinct_uneven = 0;
    for (size_t i = 2; i < even.size(); ++i)
      distinct_even += std::abs((even[i] - even[i - 1]) - (even[1] - even[0])) > 2;
    for (size_t i = 2; i < uneven.size(); ++i)
      distinct_uneven += std::abs((uneven[i] - uneven[i - 1]) - (uneven[1] - uneven[0])) > 2;
    expect(even.size() == 20 && distinct_even == 0, "Without jitter grains must be evenly spaced");
    expect(uneven.size() >= 18 && distinct_uneven > 5, "Timing jitter must vary the spacing between grains");
  }

  void testGranularPlayThroughUsesRealTimeSpeed() {
    GranularFixture fixture;
    std::vector<float> sample(4800, 0.25f);
    fixture.granular.getSample()->loadSample(sample.data(), static_cast<int>(sample.size()), 48000);
    fixture.controls[vital::GranularSource::kMode].set(0.0f);
    fixture.controls[vital::GranularSource::kSpeed].set(1.0f);
    fixture.triggerReset();

    int rendered = 0;
    while (rendered < 2401) {
      const int block = std::min(vital::kMaxBufferSize, 2401 - rendered);
      fixture.granular.process(block);
      fixture.reset.clearTrigger();
      rendered += block;
    }
    const auto decoded = vital::utils::decodePhaseAndVoice(fixture.granular.getPhaseOutput()->buffer[0]);
    expectClose(decoded.first[0], 0.5f, 2.0e-4f,
                "Granular play-through at 1x must cross half a sample in half its real duration");
  }

  void testRealtimeModulatorsDoNotAllocate() {
    LfoFixture lfo;
    RandomFixture random(vital::RandomLfo::kPerlin);
    EnvelopeFixture envelope;
    GranularFixture granular;
    vital::FrequencyShifterModule frequency_shifter;
    frequency_shifter.init();
    frequency_shifter.setSampleRate(48000);
    const vital::control_map shifter_controls = frequency_shifter.getControls();
    shifter_controls.at("frequency_shifter_shift")->set(-500.0f);
    shifter_controls.at("frequency_shifter_feedback")->set(0.5f);
    shifter_controls.at("frequency_shifter_delay")->set(12.5f);
    shifter_controls.at("frequency_shifter_dry_wet")->set(1.0f);
    std::array<vital::poly_float, vital::kMaxBufferSize> shifter_input {};
    lfo.lfo.process(8);
    random.lfo.process(8);
    envelope.envelope.process(8);
    granular.triggerReset();
    granular.granular.process(8);
    granular.reset.clearTrigger();
    frequency_shifter.processWithInput(shifter_input.data(), 8);

    tracked_allocations.store(0, std::memory_order_relaxed);
    track_allocations.store(true, std::memory_order_release);
    lfo.lfo.process(vital::kMaxBufferSize);
    random.lfo.process(vital::kMaxBufferSize);
    envelope.envelope.process(vital::kMaxBufferSize);
    granular.granular.process(vital::kMaxBufferSize);
    frequency_shifter.processWithInput(shifter_input.data(), vital::kMaxBufferSize);
    track_allocations.store(false, std::memory_order_release);
    expect(tracked_allocations.load(std::memory_order_relaxed) == 0,
           "LFO, Random LFO, envelope, granular, and frequency shifter processing must not allocate");
  }

  void testExternalSampleFolderNavigation() {
    const File temp_root = File::getSpecialLocation(File::tempDirectory)
                               .getNonexistentChildFile("atlas_sample_navigation", {}, false);
    expect(temp_root.createDirectory().wasOk(), "External sample test folder must be created");

    const File first = temp_root.getChildFile("01 First.wav");
    const File middle = temp_root.getChildFile("02 Middle.flac");
    const File last = temp_root.getChildFile("03 Last.aiff");
    expect(first.create().wasOk() && middle.create().wasOk() && last.create().wasOk(),
           "External sample test files must be created");
    temp_root.getChildFile("Ignore.txt").create();
    const File nested = temp_root.getChildFile("Nested");
    nested.createDirectory();
    nested.getChildFile("00 Nested.wav").create();

    const auto shifted = [](const File& current, int direction) {
      return LoadSave::getShiftedFile(LoadSave::kSampleFolderName, vital::kSampleExtensionsList,
                                      LoadSave::kAdditionalSampleFoldersName, current, direction);
    };
    expect(shifted(middle, 1) == last, "Next sample must stay in the external file's folder");
    expect(shifted(last, 1) == first, "External sample navigation must wrap within the folder");
    expect(shifted(first, -1) == last, "Previous sample must wrap within the external folder");

    expect(temp_root.deleteRecursively(), "External sample test folder must be removed");
  }

  void testConvolutionIrBecomesAudible() {
    vital::ConvolutionReverbModule convolution("fx_slot_1_");
    convolution.init();
    convolution.setSampleRate(48000);
    convolution.setOversampleAmount(1);

    const vital::control_map controls = convolution.getControls();
    controls.at("fx_slot_1_convolution_reverb_dry_wet")->set(1.0f);
    controls.at("fx_slot_1_convolution_reverb_ir_gain")->set(0.0f);

    vital::ConvolutionReverbModule::IrData ir;
    ir.name = "Delayed regression impulse";
    ir.sample_rate = 48000;
    ir.left.resize(4096, 0.0f);
    ir.right.resize(4096, 0.0f);
    uint32_t noise = 0x41544c41u;
    for (int sample = 640; sample < 1600; ++sample) {
      noise = noise * 1664525u + 1013904223u;
      const float random = static_cast<int32_t>(noise >> 8) / static_cast<float>(1 << 23) - 1.0f;
      const float envelope = 1.0f - (sample - 640) / 960.0f;
      ir.left[static_cast<size_t>(sample)] = random * envelope;
      ir.right[static_cast<size_t>(sample)] = -random * envelope;
    }
    expect(convolution.irStateFromJson(vital::ConvolutionReverbModule::irToJson(ir)),
           "A valid convolution regression IR must load");

    std::array<vital::poly_float, vital::kMaxBufferSize> input {};
    bool heard_ir_tail = false;
    constexpr int kImpulsePeriod = 4096;
    int rendered = 0;
    for (int block_index = 0; block_index < 500 && !heard_ir_tail; ++block_index) {
      for (auto& sample : input)
        sample = 0.0f;
      if (rendered % kImpulsePeriod == 0)
        input[0] = 1.0f;

      convolution.processWithInput(input.data(), vital::kMaxBufferSize);
      for (int sample = 0; sample < vital::kMaxBufferSize; ++sample) {
        const int impulse_phase = (rendered + sample) % kImpulsePeriod;
        if (impulse_phase >= 640 && impulse_phase < 1600 &&
            std::abs(convolution.output()->buffer[sample][0]) > 0.01f) {
          heard_ir_tail = true;
          break;
        }
      }
      rendered += vital::kMaxBufferSize;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    expect(heard_ir_tail,
           "The slot convolution reverb must render the loaded IR instead of remaining dry or silent");
  }

  float measureTone(const std::vector<float>& signal, float frequency, float sample_rate) {
    double real = 0.0;
    double imaginary = 0.0;
    for (size_t i = 0; i < signal.size(); ++i) {
      const double phase = 2.0 * juce::MathConstants<double>::pi * frequency * i / sample_rate;
      real += signal[i] * std::cos(phase);
      imaginary -= signal[i] * std::sin(phase);
    }
    return static_cast<float>(2.0 * std::sqrt(real * real + imaginary * imaginary) / signal.size());
  }

  // Predelay (free and synced), Fade in, Fade out and Tone, measured on the
  // wet response to one impulse after the IR has been built and loaded.
  void testConvolutionPredelayFadesAndTone() {
    constexpr int kRate = 48000;
    constexpr int kLength = kRate;
    vital::cr::Value bpm(2.0f);   // 120 BPM
    auto render = [&](const std::function<void(const vital::control_map&)>& configure) {
      vital::ConvolutionReverbModule convolution("fx_slot_1_", nullptr, false, bpm.output());
      convolution.init();
      convolution.setSampleRate(kRate);
      convolution.setOversampleAmount(1);
      const vital::control_map controls = convolution.getControls();
      controls.at("fx_slot_1_convolution_reverb_dry_wet")->set(1.0f);
      configure(controls);
      vital::ConvolutionReverbModule::IrData ir;
      ir.name = "Spike and noise tail";
      ir.sample_rate = kRate;
      ir.left.assign(8000, 0.0f);
      uint32_t noise = 0x5052454du;
      ir.left[0] = 1.0f;
      for (int sample = 1; sample < 8000; ++sample) {
        noise = noise * 1664525u + 1013904223u;
        ir.left[static_cast<size_t>(sample)] = (static_cast<int32_t>(noise >> 8) / float(1 << 23) - 1.0f) * 0.3f;
      }
      ir.right = ir.left;
      convolution.irStateFromJson(vital::ConvolutionReverbModule::irToJson(ir));
      std::array<vital::poly_float, vital::kMaxBufferSize> block {};
      for (int i = 0; i < 400; ++i) {   // let the worker and JUCE load the IR
        convolution.processWithInput(block.data(), vital::kMaxBufferSize);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      std::vector<float> response;
      for (int rendered = 0; rendered < kLength; rendered += vital::kMaxBufferSize) {
        block.fill(0.0f);
        if (rendered == 0)
          block[0] = 1.0f;
        convolution.processWithInput(block.data(), vital::kMaxBufferSize);
        for (int i = 0; i < vital::kMaxBufferSize; ++i)
          response.push_back(convolution.output()->buffer[i][0]);
      }
      return response;
    };
    auto arrival = [](const std::vector<float>& response) {
      float peak = 0.0f;
      for (float value : response) peak = std::max(peak, std::abs(value));
      for (size_t i = 0; i < response.size(); ++i)
        if (std::abs(response[i]) > 0.25f * peak) return int(i);
      return -1;
    };
    auto energy = [](const std::vector<float>& response, int from, int to) {
      double total = 0.0;
      for (int i = from; i < to && i < int(response.size()); ++i) total += double(response[i]) * response[i];
      return total;
    };

    const auto plain = render([](const vital::control_map&) {});
    const int base = arrival(plain);
    expect(base >= 0, "the reverb answers the impulse");
    const auto free = render([](const vital::control_map& c) {
      c.at("fx_slot_1_convolution_reverb_predelay")->set(0.1f);
    });
    expect(std::abs(arrival(free) - base - kRate / 10) <= 2, "Free predelay of 100 ms delays the reverb by 4800 samples");
    const auto synced = render([](const vital::control_map& c) {
      c.at("fx_slot_1_convolution_reverb_predelay_sync")->set(1.0f);
      c.at("fx_slot_1_convolution_reverb_predelay_tempo")->set(10.0f);   // 1/16
    });
    expect(std::abs(arrival(synced) - base - 6000) <= 2, "a synced 1/16 predelay at 120 BPM is 125 ms");
    const auto dotted = render([](const vital::control_map& c) {
      c.at("fx_slot_1_convolution_reverb_predelay_sync")->set(2.0f);
      c.at("fx_slot_1_convolution_reverb_predelay_tempo")->set(10.0f);
    });
    expect(std::abs(arrival(dotted) - base - 9000) <= 2, "a dotted 1/16 predelay is one and a half times as long");

    // Fades: compare early and late energy against the untouched response.
    const auto faded_in = render([](const vital::control_map& c) {
      c.at("fx_slot_1_convolution_reverb_fade_in")->set(0.5f);
    });
    const auto faded_out = render([](const vital::control_map& c) {
      c.at("fx_slot_1_convolution_reverb_fade_out")->set(0.5f);
    });
    const double plain_early = energy(plain, base, base + 800), plain_late = energy(plain, base + 6000, base + 8000);
    expect(energy(faded_in, base, base + 800) < 0.3 * plain_early * (energy(faded_in, base + 6000, base + 8000) / plain_late),
           "Fade in softens the start of the reverb");
    expect(energy(faded_out, base + 6000, base + 8000) < 0.3 * plain_late * (energy(faded_out, base, base + 800) / plain_early),
           "Fade out shortens the end of the reverb");

    // Tone: brighter positive, darker negative, against the low-to-high balance.
    auto brightness = [&](const std::vector<float>& response) {
      std::vector<float> slice(response.begin() + base, response.begin() + base + 8192);
      double low = 0.0, high = 0.0;
      for (float frequency : { 100.0f, 200.0f, 300.0f })
        low += measureTone(slice, frequency, kRate);
      for (float frequency : { 6000.0f, 9000.0f, 12000.0f })
        high += measureTone(slice, frequency, kRate);
      return high / std::max(1.0e-9, low);
    };
    const auto bright = render([](const vital::control_map& c) { c.at("fx_slot_1_convolution_reverb_tone")->set(1.0f); });
    const auto dark = render([](const vital::control_map& c) { c.at("fx_slot_1_convolution_reverb_tone")->set(-1.0f); });
    const double neutral = brightness(plain);
    expect(brightness(bright) > 2.0 * neutral && brightness(dark) < 0.5 * neutral,
           "Tone tilts the reverb brighter above zero and darker below");
  }

  std::vector<float> renderFrequencyShifter(float input_frequency, float shift, float mix,
                                            float delay_milliseconds, int sample_rate,
                                            int oversample, int render_samples) {
    vital::FrequencyShifterModule shifter;
    shifter.init();
    shifter.setSampleRate(sample_rate);
    shifter.setOversampleAmount(oversample);
    const vital::control_map controls = shifter.getControls();
    controls.at("frequency_shifter_shift")->set(shift);
    controls.at("frequency_shifter_feedback")->set(0.0f);
    controls.at("frequency_shifter_delay")->set(delay_milliseconds);
    controls.at("frequency_shifter_dry_wet")->set(mix);

    const int effective_sample_rate = sample_rate * oversample;
    const int warmup_samples = effective_sample_rate;
    std::vector<float> output;
    output.reserve(static_cast<size_t>(render_samples));
    std::array<vital::poly_float, vital::kMaxBufferSize * vital::kMaxOversample> input {};
    int processed = 0;
    while (processed < warmup_samples + render_samples) {
      const int block = std::min(vital::kMaxBufferSize * oversample,
                                 warmup_samples + render_samples - processed);
      for (int sample = 0; sample < block; ++sample) {
        const float phase = 2.0f * vital::kPi * input_frequency * (processed + sample) /
                            effective_sample_rate;
        input[static_cast<size_t>(sample)] = std::sin(phase);
      }
      shifter.processWithInput(input.data(), block);
      for (int sample = 0; sample < block; ++sample) {
        if (processed + sample >= warmup_samples)
          output.push_back(shifter.output()->buffer[sample][0]);
      }
      processed += block;
    }
    return output;
  }

  void testFrequencyShifterRejectsOppositeSideband() {
    constexpr int kSampleRate = 48000;
    constexpr int kOversample = 2;
    constexpr int kEffectiveSampleRate = kSampleRate * kOversample;
    const std::vector<float> shifted = renderFrequencyShifter(
        1000.0f, 200.0f, 1.0f, 0.0f, kSampleRate, kOversample, kEffectiveSampleRate);
    const float wanted = measureTone(shifted, 1200.0f, kEffectiveSampleRate);
    const float unwanted = measureTone(shifted, 800.0f, kEffectiveSampleRate);
    const float rejection = 20.0f * std::log10(wanted / std::max(unwanted, 1.0e-12f));
    expect(wanted > 0.5f, "Frequency shifter must preserve the wanted shifted sideband");
    expect(rejection > 55.0f,
           "Frequency shifter must reject the mirror sideband at 2x oversampling "
           "(rejection=" + std::to_string(rejection) + " dB)");
  }

  void testFrequencyShifterRejectsOppositeSidebandDownward() {
    constexpr int kSampleRate = 48000;
    constexpr int kOversample = 2;
    constexpr int kEffectiveSampleRate = kSampleRate * kOversample;
    const std::vector<float> shifted = renderFrequencyShifter(
        1000.0f, -200.0f, 1.0f, 0.0f, kSampleRate, kOversample, kEffectiveSampleRate);
    const float wanted = measureTone(shifted, 800.0f, kEffectiveSampleRate);
    const float unwanted = measureTone(shifted, 1200.0f, kEffectiveSampleRate);
    const float rejection = 20.0f * std::log10(wanted / std::max(unwanted, 1.0e-12f));
    expect(wanted > 0.5f,
           "A downward frequency shift must preserve its wanted sideband "
           "(wanted=" + std::to_string(wanted) +
           ", unwanted=" + std::to_string(unwanted) + ")");
    expect(rejection > 55.0f,
           "A downward frequency shift must reject the mirror sideband at 2x oversampling "
           "(rejection=" + std::to_string(rejection) + " dB)");
  }

  void testFrequencyShifterSuppressesDcFold() {
    constexpr int kSampleRate = 48000;
    constexpr int kOversample = 2;
    constexpr int kEffectiveSampleRate = kSampleRate * kOversample;
    const std::vector<float> shifted = renderFrequencyShifter(
        500.0f, -1000.0f, 1.0f, 0.0f, kSampleRate, kOversample, kEffectiveSampleRate);
    const float folded = measureTone(shifted, 500.0f, kEffectiveSampleRate);
    expect(folded < 0.1f,
           "Content shifted through DC must not fold back as a strong upward tone "
           "(folded level=" + std::to_string(folded) + ")");
  }

  void testFrequencyShifterHighDownshiftTransition() {
    constexpr int kSampleRate = 48000;
    constexpr int kOversample = 2;
    constexpr int kEffectiveSampleRate = kSampleRate * kOversample;
    const std::vector<float> valid = renderFrequencyShifter(
        6000.0f, -5000.0f, 1.0f, 0.0f, kSampleRate, kOversample, kEffectiveSampleRate / 2);
    const std::vector<float> crossing = renderFrequencyShifter(
        4500.0f, -5000.0f, 1.0f, 0.0f, kSampleRate, kOversample, kEffectiveSampleRate / 2);
    const float valid_level = measureTone(valid, 1000.0f, kEffectiveSampleRate);
    const float reflected_level = measureTone(crossing, 500.0f, kEffectiveSampleRate);
    expect(valid_level > 0.8f,
           "A high downward shift must retain content safely above its DC transition "
           "(level=" + std::to_string(valid_level) + ")");
    expect(reflected_level < 0.25f,
           "A high downward shift must smoothly suppress content immediately below its DC transition "
           "(reflected level=" + std::to_string(reflected_level) + ")");
  }

  std::vector<float> renderMovingFrequencyShifter(const std::vector<int>& block_sizes,
                                                   int render_samples) {
    constexpr int kSampleRate = 48000;
    vital::FrequencyShifterModule shifter;
    shifter.init();
    shifter.setSampleRate(kSampleRate);
    const vital::control_map controls = shifter.getControls();
    controls.at("frequency_shifter_shift")->set(-5000.0f);
    controls.at("frequency_shifter_feedback")->set(0.0f);
    controls.at("frequency_shifter_delay")->set(0.0f);
    controls.at("frequency_shifter_dry_wet")->set(1.0f);

    std::array<vital::poly_float, vital::kMaxBufferSize> input {};
    std::vector<float> output;
    output.reserve(static_cast<size_t>(render_samples));
    int rendered = 0;
    size_t block_index = 0;
    while (rendered < render_samples) {
      const int block = std::min(block_sizes[block_index++ % block_sizes.size()],
                                 render_samples - rendered);
      for (int sample = 0; sample < block; ++sample) {
        const float time = static_cast<float>(rendered + sample) / kSampleRate;
        input[static_cast<size_t>(sample)] =
            0.45f * std::sin(2.0f * vital::kPi * 1200.0f * time) +
            0.35f * std::sin(2.0f * vital::kPi * 3600.0f * time) +
            0.20f * std::sin(2.0f * vital::kPi * 7200.0f * time);
      }
      shifter.processWithInput(input.data(), block);
      for (int sample = 0; sample < block; ++sample)
        output.push_back(shifter.output()->buffer[sample][0]);
      rendered += block;
    }
    return output;
  }

  void testFrequencyShifterMovingDownshiftIsBlockInvariant() {
    const std::vector<float> reference = renderMovingFrequencyShifter({ 1 }, 8192);
    const std::vector<float> blocked = renderMovingFrequencyShifter({ 128, 7, 63, 32, 1 }, 8192);
    float maximum_difference = 0.0f;
    float maximum_level = 0.0f;
    bool finite = true;
    for (size_t i = 0; i < reference.size(); ++i) {
      finite = finite && std::isfinite(reference[i]) && std::isfinite(blocked[i]);
      maximum_difference = std::max(maximum_difference, std::abs(reference[i] - blocked[i]));
      maximum_level = std::max(maximum_level, std::abs(reference[i]));
    }
    expect(finite && maximum_level < 4.0f,
           "The moving high-order downshift transition must remain finite and bounded");
    expect(maximum_difference < 2.0e-5f,
           "A moving downward Shift control must sound independent of host block boundaries "
           "(maximum difference=" + std::to_string(maximum_difference) + ")");
  }

  void testFrequencyShifterWetDelay() {
    constexpr int kSampleRate = 48000;
    constexpr float kDelayMilliseconds = 10.0f;
    constexpr int kDelaySamples = 480;
    vital::FrequencyShifterModule shifter;
    shifter.init();
    shifter.setSampleRate(kSampleRate);
    const vital::control_map controls = shifter.getControls();
    controls.at("frequency_shifter_shift")->set(0.0f);
    controls.at("frequency_shifter_feedback")->set(0.0f);
    controls.at("frequency_shifter_delay")->set(kDelayMilliseconds);
    controls.at("frequency_shifter_dry_wet")->set(1.0f);

    std::array<vital::poly_float, vital::kMaxBufferSize> input {};
    int settled = 0;
    while (settled < kSampleRate) {
      const int block = std::min(vital::kMaxBufferSize, kSampleRate - settled);
      shifter.processWithInput(input.data(), block);
      settled += block;
    }

    constexpr int kCaptureSamples = 2048;
    std::vector<float> output;
    output.reserve(kCaptureSamples);
    int rendered = 0;
    while (rendered < kCaptureSamples) {
      const int block = std::min(vital::kMaxBufferSize, kCaptureSamples - rendered);
      std::fill(input.begin(), input.end(), vital::poly_float(0.0f));
      if (rendered == 0)
        input[0] = 1.0f;
      shifter.processWithInput(input.data(), block);
      for (int sample = 0; sample < block; ++sample)
        output.push_back(shifter.output()->buffer[sample][0]);
      rendered += block;
    }

    float early_peak = 0.0f;
    float delayed_peak = 0.0f;
    for (int sample = 0; sample < kDelaySamples - 2; ++sample)
      early_peak = std::max(early_peak, std::abs(output[static_cast<size_t>(sample)]));
    for (int sample = kDelaySamples - 1; sample < kCaptureSamples; ++sample)
      delayed_peak = std::max(delayed_peak, std::abs(output[static_cast<size_t>(sample)]));
    expect(early_peak < 1.0e-6f,
           "Frequency shifter wet delay must not leak before its requested delay");
    expect(delayed_peak > 0.01f,
           "Frequency shifter wet delay must emit the shifted signal after its requested delay");
  }

  template<class DelayType>
  std::vector<vital::poly_float> renderPingPong(int style, int sample_rate, int left_period, int right_period,
                                              float feedback, float mix, vital::poly_float impulse,
                                              int block_size) {
    DelayType delay(16384);
    std::array<vital::Value, DelayType::kNumInputs> controls;
    for (int i = 0; i < DelayType::kNumInputs; ++i)
      delay.plug(&controls[i], i);
    delay.setSampleRate(sample_rate);
    controls[DelayType::kStyle].set(style);
    controls[DelayType::kFrequency].set(static_cast<float>(sample_rate) / left_period);
    controls[DelayType::kFrequencyAux].set(static_cast<float>(sample_rate) / right_period);
    controls[DelayType::kFeedback].set(feedback);
    controls[DelayType::kWet].set(mix);
    controls[DelayType::kFilterCutoff].set(96.0f);
    controls[DelayType::kFilterSpread].set(1.0f);

    std::array<vital::poly_float, vital::kMaxBufferSize> input {};
    // Let delay-time, mix and filter smoothing settle before injecting audio.
    for (int sample = 0; sample < sample_rate;) {
      const int count = std::min(block_size, sample_rate - sample);
      delay.processWithInput(input.data(), count);
      sample += count;
    }
    const int total = 4 * (left_period + right_period);
    std::vector<vital::poly_float> result;
    for (int sample = 0; sample < total;) {
      const int count = std::min(block_size, total - sample);
      input[0] = sample == 0 ? impulse : vital::poly_float(0.0f);
      delay.processWithInput(input.data(), count);
      result.insert(result.end(), delay.output()->buffer, delay.output()->buffer + count);
      sample += count;
    }
    return result;
  }

  template<class DelayType>
  void checkPingPongDirections() {
    for (int sample_rate : { 44100, 48000, 96000 }) {
      for (int block_size : { 1, 64, 127 }) {
        for (float feedback : { -0.5f, 0.0f, 0.5f }) {
          for (int right_period : { 240, 480 }) {
            constexpr int left_period = 240;
            const auto lr = renderPingPong<DelayType>(DelayType::kPingPongLR, sample_rate,
                left_period, right_period, feedback, 1.0f, vital::poly_float(0.1f), block_size);
            const auto rl = renderPingPong<DelayType>(DelayType::kPingPong, sample_rate,
                right_period, left_period, feedback, 1.0f, vital::poly_float(0.1f), block_size);
            float mirror_error = 0.0f;
            for (size_t sample = 0; sample < lr.size(); ++sample) {
              for (int channel = 0; channel < 2; ++channel) {
                expect(std::isfinite(lr[sample][channel]) && std::isfinite(rl[sample][channel]),
                       "Ping Pong output must remain finite");
                mirror_error = std::max(mirror_error, std::abs(lr[sample][channel] - rl[sample][1 - channel]));
              }
            }
            expect(mirror_error < 1.0e-5f,
                   "Ping Pong L-R must mirror R-L when channel delay times are exchanged");
            const auto peak = [&lr](int channel, int center) {
              float result = 0.0f;
              for (int sample = center - 4; sample < center + 32; ++sample)
                result = std::max(result, std::abs(lr[sample][channel]));
              return result;
            };
            expect(peak(0, left_period) > 0.01f && peak(1, left_period) < 1.0e-6f,
                   "Ping Pong L-R first echo must occur only on the left at the main delay time");
            expect(peak(1, left_period + right_period) > 0.01f,
                   "Ping Pong must pass the second echo to the right even with zero feedback");
            const float third_echo = peak(0, 2 * left_period + right_period);
            expect(feedback == 0.0f ? third_echo < 1.0e-4f : third_echo > 0.005f,
                   "Ping Pong feedback must control repeats after the first left/right pair");
          }
        }
      }
    }

    const vital::poly_float stereo_impulse = vital::utils::maskLoad(
        vital::poly_float(0.1f), vital::poly_float(-0.03f), vital::constants::kRightMask);
    for (int style : { DelayType::kPingPong, DelayType::kPingPongLR }) {
      const auto dry = renderPingPong<DelayType>(style, 48000, 240, 480, 0.5f, 0.0f, stereo_impulse, 64);
      for (int channel = 0; channel < 2; ++channel) {
        expectClose(dry[0][channel], stereo_impulse[channel], 1.0e-6f,
                    "Ping Pong direction must preserve the original dry stereo channels");
        float tail = 0.0f;
        for (size_t sample = 1; sample < dry.size(); ++sample)
          tail = std::max(tail, std::abs(dry[sample][channel]));
        expect(tail < 1.0e-6f, "Zero delay mix must contain no echoes");
      }
    }
  }

  void testDelayPingPongDirections() {
    static_assert(vital::StereoDelay::kMono == 0 && vital::StereoDelay::kStereo == 1 &&
                  vital::StereoDelay::kPingPong == 2 && vital::StereoDelay::kMidPingPong == 3,
                  "Existing delay style values must remain compatible with saved presets");
    checkPingPongDirections<vital::StereoDelay>();
    checkPingPongDirections<vital::MultiDelay>();
  }

  void testFrequencyShifterZeroShiftMixIsCoherent() {
    constexpr int kSampleRate = 48000;
    constexpr int kMeasureSamples = kSampleRate / 2;
    vital::FrequencyShifterModule shifter;
    shifter.init();
    shifter.setSampleRate(kSampleRate);
    const vital::control_map controls = shifter.getControls();
    controls.at("frequency_shifter_shift")->set(0.0f);
    controls.at("frequency_shifter_feedback")->set(0.0f);

    std::array<vital::poly_float, vital::kMaxBufferSize> input {};
    int processed = 0;
    const auto render_level = [&](float mix, int settle_samples) {
      controls.at("frequency_shifter_dry_wet")->set(mix);
      const int total_samples = settle_samples + kMeasureSamples;
      double square_sum = 0.0;
      int segment_processed = 0;
      while (segment_processed < total_samples) {
        const int block = std::min(vital::kMaxBufferSize, total_samples - segment_processed);
        for (int sample = 0; sample < block; ++sample) {
          const float phase = 2.0f * vital::kPi * 1000.0f * (processed + sample) / kSampleRate;
          input[static_cast<size_t>(sample)] = std::sin(phase);
        }
        shifter.processWithInput(input.data(), block);
        for (int sample = 0; sample < block; ++sample) {
          if (segment_processed + sample >= settle_samples) {
            const float value = shifter.output()->buffer[sample][0];
            square_sum += value * value;
          }
        }
        processed += block;
        segment_processed += block;
      }
      return static_cast<float>(std::sqrt(square_sum / kMeasureSamples));
    };

    const float dry_level = render_level(0.0f, kSampleRate);
    const float half_level = render_level(0.5f, kSampleRate / 4);
    const float wet_level = render_level(1.0f, kSampleRate / 4);
    const float maximum_level_change = std::max(std::abs(half_level - dry_level),
                                                std::abs(wet_level - dry_level));
    expect(maximum_level_change < 1.0e-4f,
           "Frequency shifter dry/wet paths must remain phase coherent at zero shift "
           "(dry=" + std::to_string(dry_level) +
           ", half=" + std::to_string(half_level) +
           ", wet=" + std::to_string(wet_level) + ")");
  }
}

#include "envelope_regression_tests.h"
#include "oscillator_level_tests.h"
#include "spectral_oscillator_tests.h"
#include "oscillator_warp_tests.h"
#include "random_amplitude_seed_tests.h"
#include "sample_navigation_tests.h"
#include "filter_keytrack_tests.h"
#include "filter_routing_tests.h"
#include "reverb_filter_tests.h"
#include "scream_filter_tests.h"
#include "ring_mod_filter_tests.h"
#include "sample_hold_filter_tests.h"
#include "release_velocity_glide_tests.h"
#include "analog_oscillator_tests.h"
#include "distortion_filter_tests.h"

int main(int argc, char** argv) {
  if (argc > 1 && std::string(argv[1]) == "--distortion-filter")
    return distortion_filter_tests::run();
  if (argc > 1 && std::string(argv[1]) == "--analog-oscillator")
    return analog_oscillator_tests::runDsp();
  if (argc > 1 && std::string(argv[1]) == "--analog-oscillator-controls")
    return analog_oscillator_tests::runControls();
  if (argc > 1 && std::string(argv[1]) == "--analog-oscillator-benchmark")
    return analog_oscillator_tests::benchmark();
  if (argc > 2 && std::string(argv[1]) == "--analog-wavetable-reference")
    return wavetable_reference_tests::wavetableReference(argv[2], argc > 3 && std::string(argv[3]) == "--write");
  if (argc > 1 && std::string(argv[1]) == "--random-amplitude-seeds")
    return random_amplitude_seed_tests::runDsp();
  if (argc > 1 && std::string(argv[1]) == "--random-amplitude-seed-controls")
    return random_amplitude_seed_tests::runControls();
  if (argc > 1 && std::string(argv[1]) == "--spectral-oscillator")
    return spectral_oscillator_tests::run();
  if (argc > 2 && std::string(argv[1]) == "--oscillator-warp-legacy")
    return oscillator_warp_tests::compareLegacy(argv[2]);
  if (argc > 1 && std::string(argv[1]) == "--oscillator-warp-controls")
    return oscillator_warp_tests::runControls();
  if (argc > 1 && std::string(argv[1]) == "--oscillator-warp-benchmark")
    return oscillator_warp_tests::benchmark();
  if (argc > 1 && std::string(argv[1]) == "--oscillator-warps")
    return oscillator_warp_tests::run();
  if (argc > 1 && std::string(argv[1]) == "--ir-navigation-benchmark") {
    juce::ScopedJuceInitialiser_GUI gui;
    Array<File> files;
    for (const auto& root : LoadSave::getIrDirectories())
      root.findChildFiles(files, File::findFiles, true, vital::kIrExtensionsList);
    LoadSave::FileSorterAscending sorter;
    files.sort(sorter);
    if (files.size() < 3) { std::cerr << "Benchmark needs at least three library IR files\n"; return 1; }
    std::cout << "IR library files=" << files.size() << std::endl;
    SynthPlugin synth;
    synth.parameterChanged("fx_slot_1_type", vital::constants::kConvolutionReverb + 1);
    SynthEditor editor(synth);
    vital::ConvolutionReverbModule* module = nullptr;
    for (const auto& entry : synth.getConvolutionReverbModules())
      if (entry.first == "fx_slot_1_") module = entry.second;
    if (!module) { std::cerr << "IR module missing\n"; return 1; }
    auto click = [&](const char* title) {
      auto* button = dynamic_cast<Button*>(sample_navigation::find(editor, title));
      if (!button) { expect(false, "IR navigation button exists"); return; }
      const auto start = std::chrono::steady_clock::now();
      button->onClick();
      const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
      std::cout << button->getTitle() << " callback_ms=" << ms << std::endl;
      expect(ms < 100, "IR navigation callback remains responsive");
    };
    auto awaitSelection = [&](int index) {
      const auto start = std::chrono::steady_clock::now();
      for (int i = 0; i < 500; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        Timer::callPendingTimersSynchronously();
        if (File(module->getLastBrowsedFile()) == files[index] &&
            module->irLoadStatus().result == vital::ConvolutionReverbModule::IrLoadResult::loaded) {
          std::cout << "Selected=" << module->irName() << " completion_ms="
                    << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() << std::endl;
          return;
        }
      }
      expect(false, "IR navigation selects the expected file after scanning/loading");
    };
    // Preserve every arrow press made before the asynchronous initial scan ends.
    for (const char* title : {"Next impulse response", "Next impulse response", "Previous impulse response", "Next impulse response"}) click(title);
    awaitSelection(1);
    click("Next impulse response"); awaitSelection(2);
    click("Previous impulse response"); awaitSelection(1);
    {
      SynthEditor closing(synth);
      auto* button = dynamic_cast<Button*>(sample_navigation::find(closing, "Next impulse response"));
      if (button) button->onClick();
    }
    return failures ? 1 : 0;
  }
  if (argc > 1 && std::string(argv[1]) == "--release-velocity-glide")
    return release_velocity_glide_tests::run();
  if (argc > 1 && std::string(argv[1]) == "--reverb-filter")
    return reverb_filter_tests::run();
  if (argc > 1 && std::string(argv[1]) == "--scream-filter")
    return scream_filter_tests::run();
  if (argc > 1 && std::string(argv[1]) == "--ring-mod-filter")
    return ring_mod_filter_tests::run();
  if (argc > 1 && std::string(argv[1]) == "--sample-hold-filter")
    return sample_hold_filter_tests::run();
  if (argc > 1 && std::string(argv[1]) == "--filter-routing")
    return filter_routing_tests::run();
  if (argc > 1 && std::string(argv[1]) == "--filter-keytrack")
    return filter_keytrack::run();
  if (argc > 1 && std::string(argv[1]) == "--sample-navigation")
    return sample_navigation::run();
  if (argc > 1 && std::string(argv[1]) == "--oscillator-levels")
    return oscillator_levels::run();
  if (argc > 1 && std::string(argv[1]) == "--envelopes")
    return envelope_regressions::run();
  if (std::getenv("ATLAS_FILTER_TEST_ONLY") != nullptr) {
    testCutoffAccuracyAndOpenEnd();
    testSelectableCutFilterSlopes();
    return failures == 0 ? 0 : 1;
  }
  testCutoffAccuracyAndOpenEnd();
  testSelectableCutFilterSlopes();
  testChannelScopedAllNotesOffPreservesOtherKeys();
  testParameterNotificationsAreCoalescedWithoutAllocations();
  testHostStylePluginConstructionIsMemorySafe();
  testRepeatedSoundEngineConstructionIsMemorySafe();
  testPresentationOrdersPreserveSerializedEnums();
  testLineGeneratorPublishesCompleteBuffers();
  testWavetableTransactionalPublication();
  testWavetableFrameInterpolationModes();
  testVitalPresetDestinationMigration();
  testVitalLineRasterizationCompatibility();
  testLfoTriggerPlacement();
  testLfoBlockInvariance();
  testLfoFadeInAcrossProcessingModes();
  testLfoSyncClockReachesVoiceClones();
  testRandomBlockInvariance();
  testRandomSyncClockReachesVoiceClones();
  testNoteOnRandomHoldsPerVoice();
  testIndependentRandomLfoStreams();
  testModulationRandomCorrelationSelector();
  testModWheelSourceSmoothing();
  testModWheelUsesAudioRateSmoothingForAudioDestinations();
  testModWheelUsesAudioRateSmoothingForLfoSpeed();
  testTempoChooserPreservesAudioRateFreeFrequency();
  testFloatToIntTruncatesOnEveryPlatform();
  testModulatedTempoPicksTheSameDivisionOnEveryPlatform();
  testEnvelopeShortStages();
  testEnvelopeRetriggerStartsFromCurrentValue();
  testForcedVoiceReleaseMatchesShortProfessionalHandoff();
  testCollapsedModulationVoiceHandoffIsDezipped();
  testReleasedVoiceReusePreservesAudibleState();
  testLongReleaseRetriggerIsClickless();
  testHeldMonoRetriggerIsClickless();
  testHeldMonoLegatoIsSignalContinuous();
  testMonoRetriggerUsesSampleAccurateTailVoice();
  testMonoLegatoKeepsVoiceAndRandomContinuous();
  testLegatoModeSeamlesslyRetriggersAfterARealGap();
  testLegatoTransitionIgnoresFadingTail();
  testReverbMixConsumesAudioRateModulation();
  testMonoVoiceLaneRotationKeepsLfoToFxIdentical();
  testStationaryZeroModWheelRouteIsTransparent();
  testMovingModWheelFilterRouteIsContinuous();
  testStaticMacroOscillatorLevelRouteIsTransparent();
  testMacroOscillatorLevelRouteIsBlockInvariant();
  testGranularBlockInvariance();
  testGranularStereoGrainsStayPaired();
  testGranularGrainsFinishOutsideLaunchRegion();
  testGranularPlayThroughUsesRealTimeSpeed();
  testGranularGridTimingFollowsHostBeats();
  testGranularGridSizeFollowsTempo();
  testGranularTimingJitterVariesSpacing();
  testRealtimeModulatorsDoNotAllocate();
  testExternalSampleFolderNavigation();
  testConvolutionIrBecomesAudible();
  testConvolutionPredelayFadesAndTone();
  testFrequencyShifterRejectsOppositeSideband();
  testFrequencyShifterRejectsOppositeSidebandDownward();
  testFrequencyShifterSuppressesDcFold();
  testFrequencyShifterHighDownshiftTransition();
  testFrequencyShifterMovingDownshiftIsBlockInvariant();
  testFrequencyShifterWetDelay();
  testDelayPingPongDirections();
  testFrequencyShifterZeroShiftMixIsCoherent();

  if (failures == 0)
    std::cout << "Atlas timing stability tests passed\n";
  return failures == 0 ? 0 : 1;
}
