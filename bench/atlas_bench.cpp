// SPDX-License-Identifier: GPL-3.0-or-later
//
// AtlasBench times Atlas builds against each other. It hosts each build as a
// VST3, so any two bundles can be compared: a saved copy of an earlier build,
// the build just made, or a released version. Each round runs every build once,
// one child process per build, alternating, so that a CPU warming up or a
// background task lands on all of them alike. bench/README.md explains how to
// use it.

#include <juce_audio_processors/juce_audio_processors.h>

#if JUCE_WINDOWS
#include "stack_sampler_windows.h"
#include <psapi.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <map>
#include <thread>
#include <vector>

namespace {
  constexpr int kResultsVersion = 1;

  // Values saved in presets. Atlas only ever appends to these enums
  // (Effect in synth_constants.h, source_types::Type, FilterModel), so the
  // numbers mean the same thing in every build.
  enum Effect {
    kChorus, kCompressor, kDelay, kDistortion, kEq, kFilterFx, kFlanger, kPhaser, kReverb, kUtility,
    kPhaseShift, kFrequencyShifter, kLimiter, kDimensionExpander, kWaveShaper, kConvolutionReverb,
    kMidSideEq, kSpectralGate, kErosion, kFloatyDelay, kSpectralCompressor, kScream, kTremoloPan,
    kTape, kDefuse, kSuppressor, kTranceGate, kSpectralLock, kResonator, kAnalogueChorus,
    kAnalogueFlanger, kAnalogueEcho, kAnalogueRotary, kAnalogueSpring, kAnalogueWah,
    kSpectralBalance, kGlitchDelay, kOpenWurliAmp, kScannerVibrato, kAnalogueCompressor, kMaim,
    kNumEffects
  };

  const char* const kEffectIds[kNumEffects] = {
    "chorus", "compressor", "delay", "distortion", "eq", "filter-fx", "flanger", "phaser", "reverb",
    "utility", "phase-shift", "frequency-shifter", "limiter", "dimension-expander", "wave-shaper",
    "convolution-reverb", "mid-side-eq", "spectral-gate", "erosion", "floaty-delay",
    "spectral-compressor", "scream", "tremolo-pan", "tape", "defuse", "suppressor", "trance-gate",
    "spectral-lock", "resonator", "analogue-chorus", "analogue-flanger", "analogue-echo",
    "analogue-rotary", "analogue-spring", "analogue-wah", "spectral-balance", "glitch-delay",
    "openwurli-amp", "scanner-vibrato", "analogue-compressor", "maim"
  };

  enum SourceType { kWavetable, kAnalog, kSample, kGranular, kSpectral, kNoise, kMultiSample, kBraids,
                    kOpenWurli, kOpenB3, kSpeech };

  enum FilterModel { kAnalogFilter, kDirty, kLadder, kDigital, kDiode, kFormant, kComb, kPhase,
                     kReverbFilter, kScreamFilter, kRingModFilter, kSampleHoldFilter, kFilterTable,
                     kDistortionFilter };

  using Settings = std::vector<std::pair<juce::String, double>>;

  struct Scenario {
    juce::String name;
    juce::String description;
    int notes = 4;
    Settings settings;
  };

  Settings effectChain(std::initializer_list<int> effects) {
    Settings settings;
    int slot = 1;
    for (int effect : effects) {
      const juce::String prefix = "fx_slot_" + juce::String(slot++) + "_";
      settings.emplace_back(prefix + "type", effect + 1);
      settings.emplace_back(prefix + "enabled", 1.0);
    }
    return settings;
  }

  Settings operator+(Settings one, const Settings& two) {
    one.insert(one.end(), two.begin(), two.end());
    return one;
  }

  Settings sourceType(int slot, int type) {
    const juce::String prefix = "osc_" + juce::String(slot) + "_";
    return { { prefix + "on", 1.0 }, { prefix + "type", type } };
  }

  Settings filter(int slot, int model) {
    const juce::String prefix = "filter_" + juce::String(slot) + "_";
    return { { prefix + "on", 1.0 }, { prefix + "model", model } };
  }

  // The scenarios a run measures by default. Each starts from the build's own
  // initial patch and changes only the controls listed.
  std::vector<Scenario> standardScenarios() {
    return {
      { "init", "initial patch, 8-note chords", 8, {} },
      { "wavetable-unison", "two wavetable slots with 16 unison voices, 8-note chords", 8,
        Settings { { "osc_1_unison_voices", 16 }, { "osc_2_on", 1 }, { "osc_2_unison_voices", 16 } } },
      { "analog-unison", "analog oscillator with 8 unison voices, 8-note chords", 8,
        sourceType(1, kAnalog) + Settings { { "osc_1_unison_voices", 8 } } },
      { "analog-sync", "analog oscillator with 8 unison voices and Hard Sync, 8-note chords", 8,
        sourceType(1, kAnalog) + Settings { { "osc_1_unison_voices", 8 }, { "osc_1_distortion_type", 1 },
                                            { "osc_1_distortion_amount", .33 } } },
      { "braids", "Braids oscillator, 4-note chords", 4, sourceType(1, kBraids) },
      { "openwurli", "OpenWurli oscillator, 4-note chords", 4, sourceType(1, kOpenWurli) },
      { "openb3", "OpenB3 oscillator, 4-note chords", 4, sourceType(1, kOpenB3) },
      { "filters-classic", "analog and ladder filters, 4 unison voices, 8-note chords", 8,
        filter(1, kAnalogFilter) + filter(2, kLadder) + Settings { { "osc_1_unison_voices", 4 } } },
      { "filters-digital", "digital and formant filters, 4 unison voices, 8-note chords", 8,
        filter(1, kDigital) + filter(2, kFormant) + Settings { { "osc_1_unison_voices", 4 } } },
      { "filters-atlas", "Scream and Reverb filters, 4-note chords", 4,
        filter(1, kScreamFilter) + filter(2, kReverbFilter) },
      { "fx-vital", "Vital's effects: chorus, compressor, delay, distortion, EQ, flanger, phaser, reverb", 4,
        effectChain({ kChorus, kCompressor, kDelay, kDistortion, kEq, kFlanger, kPhaser, kReverb }) },
      { "fx-spectral", "spectral gate, compressor, lock and balance", 4,
        effectChain({ kSpectralGate, kSpectralCompressor, kSpectralLock, kSpectralBalance }) },
      { "fx-convolution", "convolution reverb", 4, effectChain({ kConvolutionReverb }) },
      { "fx-analogue", "analogue chorus, flanger, echo, rotary, spring, wah and compressor", 4,
        effectChain({ kAnalogueChorus, kAnalogueFlanger, kAnalogueEcho, kAnalogueRotary, kAnalogueSpring,
                      kAnalogueWah, kAnalogueCompressor }) },
      { "fx-character", "tape, scream, defuse, maim and resonator", 4,
        effectChain({ kTape, kScream, kDefuse, kMaim, kResonator }) },
      { "fx-modulation", "glitch delay, floaty delay, frequency shifter, dimension expander, tremolo pan, "
                         "trance gate, scanner vibrato, phase shift", 4,
        effectChain({ kGlitchDelay, kFloatyDelay, kFrequencyShifter, kDimensionExpander, kTremoloPan,
                      kTranceGate, kScannerVibrato, kPhaseShift }) },
      { "fx-dynamics", "wave shaper, erosion, suppressor, mid-side EQ, OpenWurli amp, limiter", 4,
        effectChain({ kWaveShaper, kErosion, kSuppressor, kMidSideEq, kOpenWurliAmp, kLimiter }) },
    };
  }

  // One scenario per effect, alone in the first slot (--effects).
  std::vector<Scenario> effectScenarios() {
    std::vector<Scenario> scenarios;
    for (int effect = 0; effect < kNumEffects; ++effect) {
      if (effect == kUtility)
        continue;
      scenarios.push_back({ juce::String("fx-") + kEffectIds[effect], juce::String(kEffectIds[effect]) + " alone", 4,
                            effectChain({ effect }) });
    }
    // Spectral Lock's default mode is a filterbank; its Spectral mode runs an FFT.
    scenarios.push_back({ "fx-spectral-lock-spectral", "spectral lock in Spectral mode", 4,
                          effectChain({ kSpectralLock }) + Settings { { "fx_slot_1_spectral_lock_mode", 1 } } });
    return scenarios;
  }

  // One scenario per voice filter model, alone in filter 1 (--filters).
  std::vector<Scenario> filterScenarios() {
    static const char* const kModelIds[] = { "analog", "dirty", "ladder", "digital", "diode", "formant", "comb",
                                             "phase", "reverb", "scream", "ring-mod", "sample-hold", "table",
                                             "distortion" };
    std::vector<Scenario> scenarios;
    for (int model = 0; model <= kDistortionFilter; ++model) {
      scenarios.push_back({ juce::String("filter-") + kModelIds[model],
                            juce::String(kModelIds[model]) + " filter, 4-note chords", 4, filter(1, model) });
    }
    // The Reverb model's FIR only runs above 300 Hz; its default cutoff is 100 Hz.
    scenarios.push_back({ "filter-reverb-bright", "reverb filter at 2 kHz, 4-note chords", 4,
                          filter(1, kReverbFilter) + Settings { { "filter_1_reverb_cutoff", std::log2(2000.0) } } });
    // Scream at full resonance and drive, without autogain, and as a Filter FX
    // (one stereo pair, gated by silence between chords).
    scenarios.push_back({ "filter-scream-driven", "scream filter at full resonance, scream and drive, 4-note chords", 4,
                          filter(1, kScreamFilter) + Settings { { "filter_1_resonance", 1.0 }, { "filter_1_scream_amount", 1.0 },
                                                                { "filter_1_scream_input_gain", 24.0 },
                                                                { "filter_1_scream_autogain", 0.0 } } });
    scenarios.push_back({ "filter-fx-scream", "Filter FX with the scream model, 4-note chords", 4,
                          effectChain({ kFilterFx }) + Settings { { "fx_slot_1_filter_fx_model", kScreamFilter } } });
    // Reverb on single notes (half of each voice pair is empty) and as a Filter FX.
    scenarios.push_back({ "filter-reverb-single", "reverb filter, single notes", 1, filter(1, kReverbFilter) });
    scenarios.push_back({ "filter-fx-reverb", "Filter FX with the reverb model, 4-note chords", 4,
                          effectChain({ kFilterFx }) + Settings { { "fx_slot_1_filter_fx_model", kReverbFilter } } });
    return scenarios;
  }

  struct Options {
    juce::StringArray plugins;
    juce::File out;
    juce::StringArray only;
    double seconds = 6.0;
    double warmup_seconds = 1.0;
    int settle_ms = 1500;
    int rounds = 3;
    int block_size = 128;
    double sample_rate = 48000.0;
    int core = -1;
    bool effects = false;
    bool filters = false;
    bool child = false;
    juce::File save_audio;   // where each build's timed output goes (--save-audio)
    bool load = false;       // time loading and opening the editor instead of rendering (--load)
    bool focus = false;      // --load: bring the editor to the front and focus it, as opening it in a host does
    juce::File profile;      // --load: where the message thread's sampled call stacks go (--profile, Windows)
    Settings set;            // --load: controls changed in the state that is restored (--set name=value)
  };

  std::vector<Scenario> allScenarios(const Options& options) {
    if (options.effects)
      return effectScenarios();
    return options.filters ? filterScenarios() : standardScenarios();
  }

  std::vector<Scenario> selectedScenarios(const Options& options) {
    std::vector<Scenario> all = allScenarios(options);
    if (options.only.isEmpty())
      return all;
    std::vector<Scenario> chosen;
    for (const Scenario& scenario : all) {
      for (const juce::String& pattern : options.only) {
        if (scenario.name.matchesWildcard(pattern, true)) {
          chosen.push_back(scenario);
          break;
        }
      }
    }
    return chosen;
  }

  // The plugin's own state (Atlas's JSON text) from the VST3 component state.
  juce::var readPluginState(juce::AudioPluginInstance& instance) {
    juce::MemoryBlock data;
    instance.getStateInformation(data);
    auto xml = juce::AudioProcessor::getXmlFromBinary(data.getData(), int(data.getSize()));
    if (xml == nullptr)
      return {};
    auto* component = xml->getChildByName("IComponent");
    juce::MemoryBlock bytes;
    if (component == nullptr || !bytes.fromBase64Encoding(component->getAllSubText()))
      return {};
    // The JSON ends at a NUL; JUCE's private data follows it.
    const char* text = static_cast<const char*>(bytes.getData());
    const size_t length = std::find(text, text + bytes.getSize(), '\0') - text;
    return juce::JSON::parse(juce::String::fromUTF8(text, int(length)));
  }

  void writePluginState(juce::AudioPluginInstance& instance, const juce::var& state) {
    const juce::String json = juce::JSON::toString(state, true);
    juce::MemoryBlock bytes(json.toRawUTF8(), json.getNumBytesAsUTF8());
    bytes.append("", 1);
    juce::XmlElement xml("VST3PluginState");
    xml.createNewChildElement("IComponent")->addTextElement(bytes.toBase64Encoding());
    juce::MemoryBlock data;
    juce::AudioProcessor::copyXmlToBinary(xml, data);
    instance.setStateInformation(data.getData(), int(data.getSize()));
  }

  std::unique_ptr<juce::AudioPluginInstance> loadPlugin(const juce::String& path, const Options& options,
                                                        juce::String& error) {
    juce::VST3PluginFormat format;
    juce::OwnedArray<juce::PluginDescription> types;
    format.findAllTypesForFile(types, path);
    if (types.isEmpty()) {
      error = "no VST3 plugin found in " + path;
      return nullptr;
    }
    auto instance = format.createInstanceFromDescription(*types[0], options.sample_rate, options.block_size, error);
    if (instance == nullptr)
      return nullptr;
    instance->enableAllBuses();
    instance->setRateAndBufferSizeDetails(options.sample_rate, options.block_size);
    instance->prepareToPlay(options.sample_rate, options.block_size);
    return instance;
  }

  struct Measurement {
    double load_percent = 0.0;   // processing time / audio time
    double p99_block_percent = 0.0;
    double max_block_percent = 0.0;
    double rms = 0.0;
    bool finite = true;
    juce::uint64 checksum = 14695981039346656037ull;  // FNV-1a of the timed output's bits
    std::vector<float> audio;  // the timed output, interleaved stereo (with --save-audio)
  };

  // Plays chords of `notes` notes, one a second, each held for 0.8 s, and
  // times every block after the warm-up.
  Measurement render(juce::AudioPluginInstance& instance, const Scenario& scenario, const Options& options) {
    static constexpr int kChord[] = { 48, 55, 60, 64, 67, 71, 74, 79 };
    const int block = options.block_size;
    const int chord_period = int(options.sample_rate);
    const int note_length = int(options.sample_rate * 0.8);
    const int warmup = int(options.warmup_seconds * options.sample_rate);
    const int total = warmup + int(options.seconds * options.sample_rate);
    const int channels = std::max(instance.getTotalNumInputChannels(), instance.getTotalNumOutputChannels());

    juce::AudioBuffer<float> audio(std::max(channels, 2), block);
    juce::MidiBuffer midi;
    std::vector<double> block_seconds;
    block_seconds.reserve(size_t(total / block) + 1);
    double sum_squares = 0.0;
    long long measured_samples = 0;
    Measurement result;
    if (options.save_audio != juce::File())
      result.audio.reserve(size_t(2 * (total - warmup + block)));

    for (int position = 0; position < total; position += block) {
      midi.clear();
      for (int offset = 0; offset < block; ++offset) {
        const int in_chord = (position + offset) % chord_period;
        for (int note = 0; note < scenario.notes; ++note) {
          if (in_chord == 0)
            midi.addEvent(juce::MidiMessage::noteOn(1, kChord[note], 0.8f), offset);
          else if (in_chord == note_length)
            midi.addEvent(juce::MidiMessage::noteOff(1, kChord[note]), offset);
        }
      }
      audio.clear();
      const auto start = std::chrono::steady_clock::now();
      instance.processBlock(audio, midi);
      const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
      if (position < warmup)
        continue;

      block_seconds.push_back(elapsed);
      for (int channel = 0; channel < std::min(2, instance.getTotalNumOutputChannels()); ++channel) {
        const float* samples = audio.getReadPointer(channel);
        for (int i = 0; i < block; ++i) {
          result.finite = result.finite && std::isfinite(samples[i]);
          sum_squares += double(samples[i]) * samples[i];
          juce::uint32 bits;
          std::memcpy(&bits, samples + i, sizeof(bits));
          result.checksum = (result.checksum ^ bits) * 1099511628211ull;
        }
      }
      measured_samples += block;
      if (options.save_audio != juce::File()) {
        const int output_channels = instance.getTotalNumOutputChannels();
        for (int i = 0; i < block; ++i) {
          for (int channel = 0; channel < 2; ++channel)
            result.audio.push_back(audio.getSample(std::min(channel, std::max(0, output_channels - 1)), i));
        }
      }
    }

    const double block_duration = block / options.sample_rate;
    double processing = 0.0;
    for (double seconds : block_seconds)
      processing += seconds;
    std::sort(block_seconds.begin(), block_seconds.end());
    result.load_percent = 100.0 * processing / (block_seconds.size() * block_duration);
    result.p99_block_percent = 100.0 * block_seconds[size_t(0.99 * (block_seconds.size() - 1))] / block_duration;
    result.max_block_percent = 100.0 * block_seconds.back() / block_duration;
    result.rms = std::sqrt(sum_squares / std::max<long long>(1, 2 * measured_samples));
    return result;
  }

  // Runs `work` on a high-priority thread (as a host's audio thread would be)
  // while this, the message thread, keeps dispatching the plugin's messages.
  template<typename Work>
  void runOnAudioThread(const Options& options, Work&& work) {
    std::atomic<bool> done { false };
    juce::Thread::launch(juce::Thread::Priority::highest, [&] {
      if (options.core >= 0)
        juce::Thread::setCurrentThreadAffinityMask(juce::uint32(1) << options.core);
      work();
      done = true;
    });
    while (!done)
      juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
  }

  bool valuesMatch(const juce::var& actual, double expected) {
    return !actual.isVoid() && std::abs(double(actual) - expected) < 1.0e-4;
  }

  juce::var machineInfo() {
    auto* machine = new juce::DynamicObject();
    machine->setProperty("cpu", juce::SystemStats::getCpuModel().trim());
    machine->setProperty("cores", juce::SystemStats::getNumPhysicalCpus());
    machine->setProperty("threads", juce::SystemStats::getNumCpus());
    machine->setProperty("os", juce::SystemStats::getOperatingSystemName());
    machine->setProperty("avx2", juce::SystemStats::hasAVX2());
    machine->setProperty("avx512f", juce::SystemStats::hasAVX512F());
    return machine;
  }

  juce::String describePluginFile(const juce::File& bundle) {
    juce::File binary = bundle;
    if (bundle.isDirectory()) {
      juce::Array<juce::File> found = bundle.findChildFiles(juce::File::findFiles, true, "*.vst3");
      if (!found.isEmpty())
        binary = found.getFirst();
    }
    return binary.getLastModificationTime().toISO8601(true);
  }

  // What the process has committed (its private bytes), which is what Windows
  // runs out of; 0 where there is no such count.
  double privateMiB() {
#if JUCE_WINDOWS
    PROCESS_MEMORY_COUNTERS_EX counters = {};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                             sizeof(counters)))
      return double(counters.PrivateUsage) / (1024.0 * 1024.0);
#endif
    return 0.0;
  }

  // --load, in a child process: what a user waits for when a host loads the
  // plugin and opens its window. Each step runs on the message thread, as in a
  // host, and is timed; an audio thread plays the instance meanwhile. Then the
  // message thread is watched for a while: every stretch a message held it, the
  // host's own window and a screen reader stood still too.
  int runLoadChild(const Options& options) {
    using Clock = std::chrono::steady_clock;
    auto since = [](Clock::time_point from) {
      return std::chrono::duration<double, std::milli>(Clock::now() - from).count();
    };

    juce::Array<juce::var> stages;
    std::vector<std::string> stage_names;
#if JUCE_WINDOWS
    atlas_bench::StackSampler sampler;
    if (options.profile != juce::File())
      sampler.start();
#endif
    auto beginStage = [&](const char* name) {
      stage_names.emplace_back(name);
#if JUCE_WINDOWS
      sampler.setStage(int(stage_names.size()) - 1);
#endif
    };
    auto timed = [&](const char* name, auto&& work) {
      beginStage(name);
      const auto start = Clock::now();
      work();
      const double ms = since(start);
      const double memory = privateMiB();
      std::cout << "  " << juce::String(name).paddedRight(' ', 22) << juce::String(ms, 1).paddedLeft(' ', 8) << " ms";
      if (memory > 0.0)
        std::cout << juce::String(juce::roundToInt(memory)).paddedLeft(' ', 8) << " MiB committed";
      std::cout << std::endl;
      auto* stage = new juce::DynamicObject();
      stage->setProperty("name", name);
      stage->setProperty("ms", juce::Array<juce::var> { ms });
      stage->setProperty("private_mib", juce::Array<juce::var> { memory });
      stages.add(stage);
    };

    const juce::String path = options.plugins[0];
    juce::VST3PluginFormat format;
    juce::OwnedArray<juce::PluginDescription> types;
    timed("scan", [&] { format.findAllTypesForFile(types, path); });
    if (types.isEmpty()) {
      std::cerr << "no VST3 plugin found in " << path << "\n";
      return 1;
    }
    std::unique_ptr<juce::AudioPluginInstance> instance;
    juce::String error;
    timed("instantiate", [&] {
      instance = format.createInstanceFromDescription(*types[0], options.sample_rate, options.block_size, error);
    });
    if (instance == nullptr) {
      std::cerr << "cannot load " << path << ": " << error << "\n";
      return 1;
    }
    timed("prepare", [&] {
      instance->enableAllBuses();
      instance->setRateAndBufferSizeDetails(options.sample_rate, options.block_size);
      instance->prepareToPlay(options.sample_rate, options.block_size);
    });
    // A host restoring a project hands the instance its saved state: the
    // initial patch, with the controls --set names changed.
    juce::MemoryBlock state;
    timed("save state", [&] { instance->getStateInformation(state); });
    if (!options.set.empty()) {
      juce::var changed = readPluginState(*instance);
      if (auto* settings = changed.getProperty("settings", {}).getDynamicObject()) {
        for (const auto& [name, value] : options.set)
          settings->setProperty(name, value);
        writePluginState(*instance, changed);
        instance->getStateInformation(state);
      }
    }
    timed("restore state", [&] { instance->setStateInformation(state.getData(), int(state.getSize())); });

    // The host's audio thread, a block at a time at the speed of the audio.
    std::atomic<bool> audio_running { true };
    std::atomic<double> worst_block_ms { 0.0 };
    std::thread audio([&] {
      const int channels = std::max(2, std::max(instance->getTotalNumInputChannels(),
                                                instance->getTotalNumOutputChannels()));
      juce::AudioBuffer<float> buffer(channels, options.block_size);
      juce::MidiBuffer midi;
      const auto block_time = std::chrono::duration_cast<Clock::duration>(
          std::chrono::duration<double>(options.block_size / options.sample_rate));
      auto due = Clock::now();
      while (audio_running) {
        buffer.clear();
        midi.clear();
        const auto start = Clock::now();
        instance->processBlock(buffer, midi);
        worst_block_ms = std::max(worst_block_ms.load(), since(start));
        due = std::max(due + block_time, Clock::now() - 10 * block_time);
        std::this_thread::sleep_until(due);
      }
    });

    // Dispatches messages for `ms`, keeping every stretch longer than 20 ms
    // that one of them took.
    juce::Array<juce::var> holds;
    double longest_hold = 0.0;
    auto watch = [&](const char* name, int ms) {
      beginStage(name);
      const auto start = Clock::now();
      while (since(start) < ms) {
        const auto before = Clock::now();
        juce::MessageManager::getInstance()->runDispatchLoopUntil(1);
        const double held = since(before);
        if (held >= 20.0) {
          const double at = std::chrono::duration<double, std::milli>(before - start).count();
          auto* hold = new juce::DynamicObject();
          hold->setProperty("after", name);
          hold->setProperty("at_ms", at);
          hold->setProperty("ms", held);
          holds.add(hold);
          longest_hold = std::max(longest_hold, held);
          std::cout << "    message thread held " << juce::roundToInt(held) << " ms, " << juce::roundToInt(at)
                    << " ms into " << name << std::endl;
        }
      }
    };

    auto show = [&](juce::AudioProcessorEditor& editor) {
      editor.addToDesktop(juce::ComponentPeer::windowHasTitleBar | juce::ComponentPeer::windowAppearsOnTaskbar);
      editor.setVisible(true);
      if (options.focus) {
        editor.toFront(true);
        editor.grabKeyboardFocus();
      }
    };

    std::unique_ptr<juce::AudioProcessorEditor> editor;
    timed("create editor", [&] { editor.reset(instance->createEditorAndMakeActive()); });
    if (editor == nullptr) {
      std::cerr << "the plugin gave no editor\n";
      audio_running = false;
      audio.join();
      return 1;
    }
    timed("show editor", [&] { show(*editor); });
    watch("editor open", options.settle_ms);
#if JUCE_WINDOWS
    if (!options.focus && GetForegroundWindow() == static_cast<HWND>(editor->getWindowHandle()))
      std::cout << "    note: the editor's window became the foreground window without --focus" << std::endl;
#endif
#if JUCE_WINDOWS
    // The plugin and the libraries it brought are loaded now, and gone by the end.
    sampler.noteModules();
#endif
    timed("close editor", [&] { editor.reset(); });
    watch("editor closed", std::min(options.settle_ms, 500));
    // Opening it again finds the plugin's code and data already in memory.
    timed("create again", [&] { editor.reset(instance->createEditorAndMakeActive()); });
    timed("show again", [&] { show(*editor); });
    watch("editor open again", options.settle_ms);
    timed("close again", [&] { editor.reset(); });

    // A second instance beside the first, which is what every further
    // instance in a project costs: the plugin's code and its tables are loaded.
    std::unique_ptr<juce::AudioPluginInstance> second;
    timed("second: instantiate", [&] {
      second = format.createInstanceFromDescription(*types[0], options.sample_rate, options.block_size, error);
    });
    if (second != nullptr) {
      timed("second: prepare", [&] {
        second->enableAllBuses();
        second->setRateAndBufferSizeDetails(options.sample_rate, options.block_size);
        second->prepareToPlay(options.sample_rate, options.block_size);
      });
      timed("second: restore", [&] { second->setStateInformation(state.getData(), int(state.getSize())); });
      timed("second: create editor", [&] { editor.reset(second->createEditorAndMakeActive()); });
      if (editor != nullptr) {
        timed("second: show editor", [&] { show(*editor); });
        watch("second editor open", std::min(options.settle_ms, 1000));
        timed("second: close", [&] { editor.reset(); });
      }
      timed("second: release", [&] {
        second->releaseResources();
        second.reset();
      });
    }

    audio_running = false;
    audio.join();
    timed("release", [&] {
      instance->releaseResources();
      instance.reset();
    });

#if JUCE_WINDOWS
    if (sampler.running()) {
      sampler.stop();
      if (!sampler.write(options.profile.getFullPathName().toWideCharPointer(), stage_names))
        std::cerr << "cannot write " << options.profile.getFullPathName() << "\n";
    }
#endif

    auto* plugin = new juce::DynamicObject();
    plugin->setProperty("path", path);
    plugin->setProperty("version", types[0]->version);
    plugin->setProperty("modified", describePluginFile(juce::File(path)));
    plugin->setProperty("stages", stages);
    plugin->setProperty("holds", holds);
    plugin->setProperty("longest_hold_ms", juce::Array<juce::var> { longest_hold });
    plugin->setProperty("worst_block_ms", juce::Array<juce::var> { worst_block_ms.load() });
    auto* root = new juce::DynamicObject();
    root->setProperty("results_version", kResultsVersion);
    root->setProperty("plugins", juce::Array<juce::var> { juce::var(plugin) });
    return options.out.replaceWithText(juce::JSON::toString(juce::var(root))) ? 0 : 1;
  }

  // Child process: measure every scenario once for one build and write the results.
  int runChild(const Options& options) {
    const juce::String path = options.plugins[0];
    juce::String error;
    juce::Process::setPriority(juce::Process::HighPriority);
    auto instance = loadPlugin(path, options, error);
    if (instance == nullptr) {
      std::cerr << "cannot load " << path << ": " << error << "\n";
      return 1;
    }
    // Let the plugin finish anything it started while loading.
    juce::MessageManager::getInstance()->runDispatchLoopUntil(options.settle_ms);

    const juce::var defaults = readPluginState(*instance);
    if (!defaults.getProperty("settings", {}).isObject()) {
      std::cerr << "cannot read the plugin's state; is this an Atlas build?\n";
      return 1;
    }

    auto* plugin = new juce::DynamicObject();
    plugin->setProperty("path", path);
    plugin->setProperty("name", instance->getPluginDescription().name);
    plugin->setProperty("version", instance->getPluginDescription().version);
    plugin->setProperty("modified", describePluginFile(juce::File(path)));
    plugin->setProperty("synth_version", defaults.getProperty("synth_version", ""));

    auto* scenarios = new juce::DynamicObject();
    for (const Scenario& scenario : selectedScenarios(options)) {
      juce::var state = juce::JSON::parse(juce::JSON::toString(defaults, true));
      auto* settings = state.getProperty("settings", {}).getDynamicObject();
      for (const auto& [name, value] : scenario.settings)
        settings->setProperty(name, value);
      writePluginState(*instance, state);
      juce::MessageManager::getInstance()->runDispatchLoopUntil(options.settle_ms);

      // A control this build does not know is dropped on load; the scenario
      // then measures something else, so say so. Effect slot controls are
      // saved only where they differ from their defaults, so a missing one
      // was set to its default.
      const juce::var loaded = readPluginState(*instance).getProperty("settings", {});
      juce::StringArray unsupported;
      for (const auto& [name, value] : scenario.settings) {
        const juce::var actual = loaded.getProperty(name, {});
        if (actual.isVoid() ? !name.startsWith("fx_slot_") : !valuesMatch(actual, value))
          unsupported.addIfNotAlreadyThere(name);
      }

      Measurement measurement;
      runOnAudioThread(options, [&] { measurement = render(*instance, scenario, options); });
      if (options.save_audio != juce::File()) {
        options.save_audio.createDirectory();
        options.save_audio.getChildFile(scenario.name + ".f32")
            .replaceWithData(measurement.audio.data(), measurement.audio.size() * sizeof(float));
      }

      std::cout << "  " << scenario.name.paddedRight(' ', 22) << juce::String(measurement.load_percent, 2) << " % load, p99 "
                << juce::String(measurement.p99_block_percent, 1) << " % of a block";
      if (!unsupported.isEmpty())
        std::cout << " (not supported: " << unsupported.joinIntoString(", ") << ")";
      if (measurement.rms < 1.0e-6)
        std::cout << " (silent)";
      if (!measurement.finite)
        std::cout << " (NON-FINITE OUTPUT)";
      std::cout << std::endl;

      auto* result = new juce::DynamicObject();
      result->setProperty("description", scenario.description);
      result->setProperty("load_percent", juce::Array<juce::var> { measurement.load_percent });
      result->setProperty("p99_block_percent", juce::Array<juce::var> { measurement.p99_block_percent });
      result->setProperty("max_block_percent", juce::Array<juce::var> { measurement.max_block_percent });
      result->setProperty("checksum", juce::Array<juce::var> { juce::String::toHexString((juce::int64) measurement.checksum) });
      result->setProperty("rms", measurement.rms);
      result->setProperty("finite", measurement.finite);
      juce::Array<juce::var> unsupported_list;
      for (const juce::String& name : unsupported)
        unsupported_list.add(name);
      result->setProperty("unsupported", unsupported_list);
      scenarios->setProperty(scenario.name, result);
    }
    instance->releaseResources();
    instance.reset();

    plugin->setProperty("scenarios", scenarios);
    auto* root = new juce::DynamicObject();
    root->setProperty("results_version", kResultsVersion);
    root->setProperty("plugins", juce::Array<juce::var> { juce::var(plugin) });
    return options.out.replaceWithText(juce::JSON::toString(juce::var(root))) ? 0 : 1;
  }

  juce::var findScenario(const juce::var& plugin, const juce::String& name) {
    return plugin.getProperty("scenarios", {}).getProperty(name, {});
  }

  // Adds a later round of the same build to its first.
  void appendRound(juce::var& into, const juce::var& round) {
    auto* scenarios = round.getProperty("scenarios", {}).getDynamicObject();
    if (scenarios == nullptr)
      return;
    for (const auto& entry : scenarios->getProperties()) {
      juce::var existing = findScenario(into, entry.name.toString());
      if (existing.isVoid()) {
        into.getProperty("scenarios", {}).getDynamicObject()->setProperty(entry.name, entry.value);
        continue;
      }
      for (const char* metric : { "load_percent", "p99_block_percent", "max_block_percent", "checksum" }) {
        auto* from = entry.value.getProperty(metric, {}).getArray();
        auto* to = existing.getProperty(metric, {}).getArray();
        if (from != nullptr && to != nullptr)
          to->addArray(*from);
      }
    }
  }

  double best(const juce::var& values) {
    double result = 0.0;
    bool first = true;
    if (auto* array = values.getArray()) {
      for (const juce::var& value : *array) {
        result = first ? double(value) : std::min(result, double(value));
        first = false;
      }
    }
    return result;
  }

  double median(const juce::var& values) {
    std::vector<double> sorted;
    if (auto* array = values.getArray()) {
      for (const juce::var& value : *array)
        sorted.push_back(double(value));
    }
    if (sorted.empty())
      return 0.0;
    std::sort(sorted.begin(), sorted.end());
    const size_t middle = sorted.size() / 2;
    return sorted.size() % 2 ? sorted[middle] : 0.5 * (sorted[middle - 1] + sorted[middle]);
  }

  juce::String percent(double value, int places = 2) {
    return juce::String(value, places) + " %";
  }

  // The output's checksum if every round rendered the same audio, else empty.
  juce::String steadyChecksum(const juce::var& scenario) {
    auto* checksums = scenario.getProperty("checksum", {}).getArray();
    if (checksums == nullptr || checksums->isEmpty())
      return {};
    for (const juce::var& checksum : *checksums) {
      if (checksum != checksums->getFirst())
        return {};
    }
    return checksums->getFirst().toString();
  }

  // Prints each scenario's best load per build and the change from the first
  // build (the baseline). Lower is better.
  void printComparison(const juce::Array<juce::var>& plugins) {
    auto* names = plugins.isEmpty() ? nullptr : plugins[0].getProperty("scenarios", {}).getDynamicObject();
    if (names == nullptr || names->getProperties().isEmpty())
      return;
    std::cout << "\nCPU load (processing time as a share of the audio's duration; best of "
              << names->getProperties().getValueAt(0).getProperty("load_percent", {}).size()
              << " rounds, lower is better)\n";
    for (int i = 0; i < plugins.size(); ++i) {
      std::cout << "  [" << char('A' + i) << "] " << plugins[i].getProperty("path", "").toString()
                << "  (version " << plugins[i].getProperty("version", "").toString() << ", built "
                << plugins[i].getProperty("modified", "").toString() << ")\n";
    }
    std::cout << "\n" << juce::String("scenario").paddedRight(' ', 22);
    for (int i = 0; i < plugins.size(); ++i) {
      std::cout << juce::String("[") + char('A' + i) + "]" << juce::String().paddedRight(' ', 9);
      if (i > 0)
        std::cout << juce::String("vs A").paddedRight(' ', 10) << juce::String("output").paddedRight(' ', 9);
    }
    std::cout << "p99 block [A] -> [last]\n";

    std::map<int, std::vector<double>> log_ratios;
    for (const auto& entry : names->getProperties()) {
      const juce::String name = entry.name.toString();
      const double base = best(entry.value.getProperty("load_percent", {}));
      std::cout << name.paddedRight(' ', 22) << percent(base).paddedRight(' ', 12);
      juce::String flags;
      if (entry.value.getProperty("unsupported", {}).size() > 0)
        flags << " [A lacks some controls]";
      for (int i = 1; i < plugins.size(); ++i) {
        const juce::var other = findScenario(plugins[i], name);
        if (other.isVoid()) {
          std::cout << juce::String("-").paddedRight(' ', 31);
          continue;
        }
        const double value = best(other.getProperty("load_percent", {}));
        const double change = base > 0.0 ? 100.0 * (value / base - 1.0) : 0.0;
        // "same" means bit-identical audio, so the change kept the sound;
        // "varies" means a build's own rounds disagree and nothing can be said.
        const juce::String base_output = steadyChecksum(entry.value), output = steadyChecksum(other);
        const char* output_state = base_output.isEmpty() || output.isEmpty() ? "varies"
                                   : base_output == output ? "same" : "differs";
        std::cout << percent(value).paddedRight(' ', 12)
                  << ((change > 0 ? "+" : "") + juce::String(change, 1) + " %").paddedRight(' ', 10)
                  << juce::String(output_state).paddedRight(' ', 9);
        if (base > 0.0 && value > 0.0)
          log_ratios[i].push_back(std::log(value / base));
        if (other.getProperty("unsupported", {}).size() > 0)
          flags << " [" << char('A' + i) << " lacks some controls]";
      }
      const juce::var last = findScenario(plugins[plugins.size() - 1], name);
      std::cout << percent(median(entry.value.getProperty("p99_block_percent", {})), 1) << " -> "
                << percent(median(last.getProperty("p99_block_percent", {})), 1) << flags << "\n";
    }
    for (const auto& [index, ratios] : log_ratios) {
      double sum = 0.0;
      for (double ratio : ratios)
        sum += ratio;
      const double change = 100.0 * (std::exp(sum / ratios.size()) - 1.0);
      std::cout << "\n[" << char('A' + index) << "] vs [A], geometric mean over " << ratios.size() << " scenarios: "
                << (change > 0 ? "+" : "") << juce::String(change, 1) << " %";
    }
    std::cout << "\n";
  }

  std::vector<float> readAudio(const juce::File& file) {
    juce::MemoryBlock data;
    file.loadFileAsData(data);
    std::vector<float> samples(data.getSize() / sizeof(float));
    std::memcpy(samples.data(), data.getData(), samples.size() * sizeof(float));
    return samples;
  }

  // How far each build's audio is from the first build's, in dB below the
  // first build's level. A difference far below hearing (-100 dB and lower)
  // keeps the sound; a scenario that varies between rounds of one build
  // differs for that reason alone.
  void compareAudio(const juce::File& folder, int builds, const juce::Array<juce::var>& plugins) {
    auto* names = plugins.isEmpty() ? nullptr : plugins[0].getProperty("scenarios", {}).getDynamicObject();
    if (names == nullptr || builds < 2)
      return;
    std::cout << "\nAudio difference from [A] (RMS of the difference, in dB below [A]'s RMS; written to "
              << folder.getFullPathName() << ")\n";
    for (const auto& entry : names->getProperties()) {
      const juce::String name = entry.name.toString();
      const auto base = readAudio(folder.getChildFile("A").getChildFile(name + ".f32"));
      std::cout << name.paddedRight(' ', 26);
      for (int i = 1; i < builds; ++i) {
        const auto letter = juce::String::charToString(char('A' + i));
        const auto other = readAudio(folder.getChildFile(letter).getChildFile(name + ".f32"));
        if (base.empty() || other.size() != base.size()) {
          std::cout << juce::String("-").paddedRight(' ', 34);
          continue;
        }
        double signal = 0.0, difference = 0.0, peak = 0.0;
        for (size_t sample = 0; sample < base.size(); ++sample) {
          const double delta = double(other[sample]) - base[sample];
          signal += double(base[sample]) * base[sample];
          difference += delta * delta;
          peak = std::max(peak, std::abs(delta));
        }
        juce::String text = "[" + letter + "] ";
        if (difference == 0.0)
          text << "identical";
        else
          text << juce::String(10.0 * std::log10(difference / std::max(signal, 1.0e-30)), 1) << " dB (peak "
               << juce::String(peak, 7) << ")";
        std::cout << text.paddedRight(' ', 34);
      }
      std::cout << (steadyChecksum(entry.value).isEmpty() ? "varies between rounds of [A]" : "") << "\n";
    }
  }

  juce::StringArray childArguments(const Options& options, const juce::String& plugin, const juce::File& out) {
    juce::StringArray args;
    args.add(juce::File::getSpecialLocation(juce::File::currentExecutableFile).getFullPathName());
    args.addArray({ "--child", "--plugin", plugin, "--out", out.getFullPathName(),
                    "--seconds", juce::String(options.seconds), "--warmup", juce::String(options.warmup_seconds),
                    "--settle", juce::String(options.settle_ms), "--block", juce::String(options.block_size),
                    "--rate", juce::String(options.sample_rate), "--core", juce::String(options.core) });
    if (options.effects)
      args.add("--effects");
    if (options.filters)
      args.add("--filters");
    if (options.save_audio != juce::File())
      args.addArray({ "--save-audio", options.save_audio.getFullPathName() });
    if (!options.only.isEmpty())
      args.addArray({ "--only", options.only.joinIntoString(",") });
    if (options.load)
      args.add("--load");
    if (options.focus)
      args.add("--focus");
    if (options.profile != juce::File())
      args.addArray({ "--profile", options.profile.getFullPathName() });
    for (const auto& [name, value] : options.set)
      args.addArray({ "--set", name + "=" + juce::String(value) });
    return args;
  }

  double median(std::vector<double> values) {
    if (values.empty())
      return 0.0;
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
  }

  double largest(const std::vector<double>& values) {
    return values.empty() ? 0.0 : *std::max_element(values.begin(), values.end());
  }

  double smallest(const std::vector<double>& values) {
    return values.empty() ? 0.0 : *std::min_element(values.begin(), values.end());
  }

  std::vector<double> numbers(const juce::var& values) {
    std::vector<double> result;
    if (auto* array = values.getArray())
      for (const juce::var& value : *array)
        result.push_back(double(value));
    return result;
  }

  juce::String milliseconds(double ms) {
    return juce::String(juce::roundToInt(ms)).paddedLeft(' ', 8) + " ms";
  }

  // --load: every build in its own process each round, alternating, then a
  // table of each step's median with its best and worst round.
  int runLoadRounds(const Options& options) {
    juce::Array<juce::var> merged;
    merged.resize(options.plugins.size());
    const juce::File temp = juce::File::createTempFile("atlas-bench");
    for (int round = 0; round < options.rounds; ++round) {
      for (int i = 0; i < options.plugins.size(); ++i) {
        std::cout << "round " << round + 1 << "/" << options.rounds << ", [" << char('A' + i) << "] "
                  << options.plugins[i] << std::endl;
        Options child_options = options;
        // The stacks of the first build's first round only.
        if (round != 0 || i != 0)
          child_options.profile = juce::File();
        juce::ChildProcess child;
        if (!child.start(childArguments(child_options, options.plugins[i], temp))) {
          std::cerr << "cannot start the benchmark process\n";
          return 1;
        }
        char buffer[256];
        for (int read; (read = child.readProcessOutput(buffer, sizeof(buffer))) > 0;) {
          std::cout.write(buffer, read);
          std::cout.flush();
        }
        child.waitForProcessToFinish(-1);
        const juce::var result = juce::JSON::parse(temp);
        temp.deleteFile();
        if (child.getExitCode() != 0 || !result.getProperty("plugins", {}).isArray()) {
          std::cerr << "the run for " << options.plugins[i] << " failed\n";
          return 1;
        }
        const juce::var plugin = result.getProperty("plugins", {})[0];
        if (round == 0) {
          merged.set(i, plugin);
          continue;
        }
        juce::var& into = merged.getReference(i);
        auto* stages = plugin.getProperty("stages", {}).getArray();
        auto* into_stages = into.getProperty("stages", {}).getArray();
        for (int stage = 0; stages != nullptr && into_stages != nullptr && stage < stages->size()
                            && stage < into_stages->size(); ++stage)
          for (const char* metric : { "ms", "private_mib" })
            into_stages->getReference(stage).getProperty(metric, {}).getArray()->addArray(
                *stages->getReference(stage).getProperty(metric, {}).getArray());
        for (const char* metric : { "longest_hold_ms", "worst_block_ms", "holds" })
          into.getProperty(metric, {}).getArray()->addArray(*plugin.getProperty(metric, {}).getArray());
      }
    }

    for (int i = 0; i < merged.size(); ++i) {
      const juce::var& plugin = merged.getReference(i);
      std::cout << "\n[" << char('A' + i) << "] " << plugin.getProperty("path", "").toString() << " (version "
                << plugin.getProperty("version", "").toString() << ", built "
                << plugin.getProperty("modified", "").toString() << ")\n"
                << juce::String("step").paddedRight(' ', 22) << "     median        best       worst   committed\n";
      double to_instance = 0.0, to_editor = 0.0, second = 0.0;
      if (auto* stages = plugin.getProperty("stages", {}).getArray()) {
        for (const juce::var& stage : *stages) {
          const juce::String name = stage.getProperty("name", "").toString();
          const std::vector<double> ms = numbers(stage.getProperty("ms", {}));
          std::cout << name.paddedRight(' ', 22) << milliseconds(median(ms)) << milliseconds(smallest(ms))
                    << milliseconds(largest(ms));
          const std::vector<double> memory = numbers(stage.getProperty("private_mib", {}));
          if (!memory.empty() && median(memory) > 0.0)
            std::cout << juce::String(juce::roundToInt(median(memory))).paddedLeft(' ', 9) << " MiB";
          std::cout << "\n";
          if (name == "scan" || name == "instantiate" || name == "prepare" || name == "restore state")
            to_instance += median(ms);
          if (name == "create editor" || name == "show editor")
            to_editor += median(ms);
          if (name.startsWith("second:"))
            second += median(ms);
        }
      }
      const std::vector<double> holds = numbers(plugin.getProperty("longest_hold_ms", {}));
      const std::vector<double> blocks = numbers(plugin.getProperty("worst_block_ms", {}));
      std::cout << "loading the first instance, median: " << juce::roundToInt(to_instance) << " ms\n"
                << "opening its editor, median: " << juce::roundToInt(to_editor) << " ms\n"
                << "loading and opening a second instance, median: " << juce::roundToInt(second) << " ms\n"
                << "longest hold of the message thread with an editor open, median: "
                << juce::roundToInt(median(holds)) << " ms (worst " << juce::roundToInt(largest(holds)) << " ms)\n"
                << "slowest audio block meanwhile, median: " << juce::String(median(blocks), 1) << " ms\n";
    }

    if (options.out != juce::File()) {
      auto* root = new juce::DynamicObject();
      root->setProperty("results_version", kResultsVersion);
      root->setProperty("date", juce::Time::getCurrentTime().toISO8601(true));
      root->setProperty("machine", machineInfo());
      root->setProperty("plugins", merged);
      options.out.getParentDirectory().createDirectory();
      options.out.replaceWithText(juce::JSON::toString(juce::var(root)));
      std::cout << "results written to " << options.out.getFullPathName() << "\n";
    }
    return 0;
  }

  int runRounds(const Options& options) {
    juce::Array<juce::var> merged;
    merged.resize(options.plugins.size());
    const juce::File temp = juce::File::createTempFile("atlas-bench");
    for (int round = 0; round < options.rounds; ++round) {
      for (int i = 0; i < options.plugins.size(); ++i) {
        std::cout << "round " << round + 1 << "/" << options.rounds << ", [" << char('A' + i) << "] "
                  << options.plugins[i] << std::endl;
        juce::ChildProcess child;
        // Each build's audio goes to its own folder, from the first round only.
        Options child_options = options;
        child_options.save_audio = round == 0 && options.save_audio != juce::File()
                                       ? options.save_audio.getChildFile(juce::String::charToString(char('A' + i)))
                                       : juce::File();
        if (!child.start(childArguments(child_options, options.plugins[i], temp))) {
          std::cerr << "cannot start the benchmark process\n";
          return 1;
        }
        char buffer[256];
        for (int read; (read = child.readProcessOutput(buffer, sizeof(buffer))) > 0;) {
          std::cout.write(buffer, read);
          std::cout.flush();
        }
        child.waitForProcessToFinish(-1);
        const juce::var result = juce::JSON::parse(temp);
        temp.deleteFile();
        if (child.getExitCode() != 0 || !result.getProperty("plugins", {}).isArray()) {
          std::cerr << "the run for " << options.plugins[i] << " failed\n";
          return 1;
        }
        const juce::var plugin = result.getProperty("plugins", {})[0];
        if (round == 0)
          merged.set(i, plugin);
        else
          appendRound(merged.getReference(i), plugin);
      }
    }

    auto* root = new juce::DynamicObject();
    root->setProperty("results_version", kResultsVersion);
    root->setProperty("date", juce::Time::getCurrentTime().toISO8601(true));
    root->setProperty("machine", machineInfo());
    auto* settings = new juce::DynamicObject();
    settings->setProperty("seconds", options.seconds);
    settings->setProperty("rounds", options.rounds);
    settings->setProperty("block_size", options.block_size);
    settings->setProperty("sample_rate", options.sample_rate);
    settings->setProperty("core", options.core);
    root->setProperty("settings", settings);
    root->setProperty("plugins", merged);
    if (options.out != juce::File()) {
      options.out.getParentDirectory().createDirectory();
      options.out.replaceWithText(juce::JSON::toString(juce::var(root)));
      std::cout << "results written to " << options.out.getFullPathName() << "\n";
    }
    printComparison(merged);
    if (options.save_audio != juce::File())
      compareAudio(options.save_audio, options.plugins.size(), merged);
    return 0;
  }

  // Compares saved result files: every build in every file, the first one
  // being the baseline.
  int compareFiles(const juce::StringArray& files) {
    juce::Array<juce::var> plugins;
    for (const juce::String& path : files) {
      const juce::var result = juce::JSON::parse(juce::File(path));
      if (!result.getProperty("plugins", {}).isArray()) {
        std::cerr << "not an AtlasBench result file: " << path << "\n";
        return 1;
      }
      std::cout << path << ": " << result.getProperty("date", "").toString() << " on "
                << result.getProperty("machine", {}).getProperty("cpu", "").toString() << "\n";
      for (const juce::var& plugin : *result.getProperty("plugins", {}).getArray())
        plugins.add(plugin);
    }
    printComparison(plugins);
    return 0;
  }

  void printUsage() {
    std::cout <<
      "usage:\n"
      "  AtlasBench --plugin <Atlas.vst3> [--plugin <Atlas.vst3> ...] [options]\n"
      "      Times each build. The first is the baseline the others are compared with.\n"
      "  AtlasBench --compare <results.json> [<results.json> ...]\n"
      "      Prints saved results side by side.\n"
      "  AtlasBench --load --plugin <Atlas.vst3> [--plugin <Atlas.vst3> ...] [options]\n"
      "      Times loading each build and opening its editor, and lists every stretch the\n"
      "      message thread was held while the editor was open.\n"
      "  AtlasBench --list [--effects | --filters]\n"
      "\n"
      "options:\n"
      "  --out <file>        save the results as JSON\n"
      "  --rounds <n>        runs of every build, alternating (default 3); the best is reported\n"
      "  --seconds <s>       audio timed per scenario and round (default 6)\n"
      "  --only <a,b*>       only these scenarios (wildcards allowed)\n"
      "  --effects           one scenario per effect instead of the standard set\n"
      "  --filters           one scenario per voice filter model instead of the standard set\n"
      "  --save-audio <dir>  keep each build's audio there and print how far it is from [A]'s\n"
      "  --block <n>         block size (default 128)\n"
      "  --rate <hz>         sample rate (default 48000)\n"
      "  --core <n>          pin the audio thread to this logical core\n"
      "  --warmup <s>        audio rendered before timing starts (default 1)\n"
      "  --settle <ms>       time given to background work after loading a scenario (default 1500);\n"
      "                      with --load, how long the open editor is watched\n"
      "  --set <name=value>  with --load, change this control in the state that is restored (the\n"
      "                      initial patch otherwise); repeat it for more controls\n"
      "  --focus             with --load, bring the editor to the front and focus it, as a host\n"
      "                      does: a running screen reader then reads it, and its work is timed too\n"
      "  --profile <file>    with --load on Windows, sample the message thread's call stacks into\n"
      "                      <file> (the first build's first round) for bench/symbolize_profile.py\n";
  }
}

int main(int argc, char** argv) {
  juce::ScopedJuceInitialiser_GUI juce_init;
  Options options;
  juce::StringArray compare;
  bool list = false;
  for (int i = 1; i < argc; ++i) {
    const juce::String arg(argv[i]);
    const bool has_value = i + 1 < argc;
    if (arg == "--plugin" && has_value)
      options.plugins.add(juce::File::getCurrentWorkingDirectory().getChildFile(argv[++i]).getFullPathName());
    else if (arg == "--out" && has_value)
      options.out = juce::File::getCurrentWorkingDirectory().getChildFile(argv[++i]);
    else if (arg == "--rounds" && has_value)
      options.rounds = std::max(1, juce::String(argv[++i]).getIntValue());
    else if (arg == "--seconds" && has_value)
      options.seconds = std::max(0.5, juce::String(argv[++i]).getDoubleValue());
    else if (arg == "--warmup" && has_value)
      options.warmup_seconds = std::max(0.0, juce::String(argv[++i]).getDoubleValue());
    else if (arg == "--settle" && has_value)
      options.settle_ms = std::max(0, juce::String(argv[++i]).getIntValue());
    else if (arg == "--block" && has_value)
      options.block_size = std::max(16, juce::String(argv[++i]).getIntValue());
    else if (arg == "--rate" && has_value)
      options.sample_rate = std::max(8000.0, juce::String(argv[++i]).getDoubleValue());
    else if (arg == "--core" && has_value)
      options.core = juce::String(argv[++i]).getIntValue();
    else if (arg == "--only" && has_value)
      options.only.addTokens(argv[++i], ",", "");
    else if (arg == "--effects")
      options.effects = true;
    else if (arg == "--filters")
      options.filters = true;
    else if (arg == "--save-audio" && has_value)
      options.save_audio = juce::File::getCurrentWorkingDirectory().getChildFile(argv[++i]);
    else if (arg == "--child")
      options.child = true;
    else if (arg == "--load")
      options.load = true;
    else if (arg == "--focus")
      options.focus = true;
    else if (arg == "--profile" && has_value)
      options.profile = juce::File::getCurrentWorkingDirectory().getChildFile(argv[++i]);
    else if (arg == "--set" && has_value) {
      const juce::String setting(argv[++i]);
      options.set.emplace_back(setting.upToFirstOccurrenceOf("=", false, false),
                               setting.fromFirstOccurrenceOf("=", false, false).getDoubleValue());
    }
    else if (arg == "--list")
      list = true;
    else if (arg == "--compare") {
      while (i + 1 < argc && !juce::String(argv[i + 1]).startsWith("--"))
        compare.add(argv[++i]);
    }
    else {
      printUsage();
      return arg == "--help" || arg == "-h" ? 0 : 2;
    }
  }
  options.only.trim();
  options.only.removeEmptyStrings();

  if (list) {
    for (const Scenario& scenario : allScenarios(options))
      std::cout << scenario.name.paddedRight(' ', 26) << scenario.description << "\n";
    return 0;
  }
  if (!compare.isEmpty())
    return compareFiles(compare);
  if (options.plugins.isEmpty()) {
    printUsage();
    return 2;
  }
  if (options.load)
    return options.child ? runLoadChild(options) : runLoadRounds(options);
  if (selectedScenarios(options).empty()) {
    std::cerr << "no scenario matches --only\n";
    return 2;
  }
  return options.child ? runChild(options) : runRounds(options);
}
