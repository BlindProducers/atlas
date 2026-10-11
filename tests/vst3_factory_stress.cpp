#include "JuceHeader.h"

#include <algorithm>
#include <atomic>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

namespace {
  std::vector<juce::MemoryBlock> extractAtlasStates(const juce::File& project) {
    std::vector<juce::MemoryBlock> states;
    const juce::StringArray lines = juce::StringArray::fromLines(project.loadFileAsString());
    bool in_atlas_chunk = false;
    juce::MemoryBlock decoded_chunk;

    const auto finishChunk = [&]() {
      if (decoded_chunk.isEmpty())
        return;

      const auto* bytes = static_cast<const char*>(decoded_chunk.getData());
      const size_t size = decoded_chunk.getSize();
      size_t json_start = 0;
      while (json_start < size && bytes[json_start] != '{')
        ++json_start;
      if (json_start == size)
        return;

      bool in_string = false;
      bool escaped = false;
      int depth = 0;
      for (size_t index = json_start; index < size; ++index) {
        const char character = bytes[index];
        if (in_string) {
          if (escaped)
            escaped = false;
          else if (character == '\\')
            escaped = true;
          else if (character == '"')
            in_string = false;
          continue;
        }

        if (character == '"')
          in_string = true;
        else if (character == '{')
          ++depth;
        else if (character == '}' && --depth == 0) {
          states.emplace_back(bytes + json_start, index - json_start + 1);
          return;
        }
      }
    };

    for (const juce::String& raw_line : lines) {
      const juce::String line = raw_line.trim();
      if (!in_atlas_chunk) {
        // Older projects name the vendor Alessio, newer ones Blind Producers.
        if (line.startsWith("<VST \"VST3i: Atlas (Alessio)\"") || line.startsWith("<VST \"VST3i: Atlas (Blind Producers)\"")) {
          in_atlas_chunk = true;
          decoded_chunk.reset();
        }
        continue;
      }

      if (line == ">") {
        finishChunk();
        in_atlas_chunk = false;
        continue;
      }

      juce::MemoryOutputStream decoded_line;
      if (juce::Base64::convertFromBase64(decoded_line, line))
        decoded_chunk.append(decoded_line.getData(), decoded_line.getDataSize());
    }
    return states;
  }

  std::unique_ptr<juce::AudioPluginInstance> createPreparedInstance(
      const juce::AudioPluginFormatManager& manager,
      const juce::PluginDescription& description,
      const juce::MemoryBlock* state,
      juce::String& error) {
    auto instance = manager.createPluginInstance(description, 48000.0, 512, error);
    if (instance == nullptr)
      return nullptr;

    instance->setRateAndBufferSizeDetails(48000.0, 512);
    instance->prepareToPlay(48000.0, 512);
    if (state != nullptr)
      instance->setStateInformation(state->getData(), static_cast<int>(state->getSize()));

    const int channels = std::max(2, std::max(instance->getTotalNumInputChannels(),
                                              instance->getTotalNumOutputChannels()));
    juce::AudioBuffer<float> audio(channels, 512);
    audio.clear();
    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(1, 60, 0.8f), 0);
    midi.addEvent(juce::MidiMessage::noteOff(1, 60), 480);
    instance->processBlock(audio, midi);
    return instance;
  }

  void processStressBlock(juce::AudioPluginInstance& instance, int block_index) {
    const int channels = std::max(2, std::max(instance.getTotalNumInputChannels(),
                                              instance.getTotalNumOutputChannels()));
    juce::AudioBuffer<float> audio(channels, 512);
    audio.clear();
    juce::MidiBuffer midi;
    if (block_index % 8 == 0)
      midi.addEvent(juce::MidiMessage::noteOn(1, 48 + (block_index / 8) % 24, 0.8f), 0);
    if (block_index % 8 == 6)
      midi.addEvent(juce::MidiMessage::noteOff(1, 48 + (block_index / 8) % 24), 400);
    instance.processBlock(audio, midi);
  }

  int testLatencyReporting(const juce::AudioPluginFormatManager& manager,
                          const juce::PluginDescription& description) {
    juce::String error;
    auto instance = createPreparedInstance(manager, description, nullptr, error);
    if (!instance) {
      std::cerr << "VST3 creation failed: " << error << '\n';
      return 1;
    }
    juce::MemoryBlock original;
    instance->getStateInformation(original);
    auto wrapper = juce::AudioProcessor::getXmlFromBinary(original.getData(), int(original.getSize()));
    auto* component = wrapper ? wrapper->getChildByName("IComponent") : nullptr;
    juce::MemoryBlock component_data;
    if (!component || !component_data.fromBase64Encoding(component->getAllSubText()))
      return 1;
    // Atlas's JSON precedes JUCE's null-separated private state in IComponent.
    auto state = juce::JSON::parse(juce::String::fromUTF8(static_cast<const char*>(component_data.getData()),
                                                       int(component_data.getSize())));
    auto* settings = state.getProperty("settings", {}).getDynamicObject();
    if (!settings) {
      std::cerr << "Unable to read VST3 component preset JSON\n";
      return 1;
    }

    struct Case { int quality, first, last, gate, fft, latency; int type = 21; };
    const Case cases[] = {
      { 0, 0, 0, 0, 3, 0 }, { 0, 1, 0, 0, 3, 1024 },
      { 0, 1, 1, 0, 3, 2048 }, { 1, 1, 1, 0, 3, 1024 },
      { 1, 0, 1, 0, 3, 512 }, { 3, 1, 0, 1, 5, 1152 },
      { 3, 0, 0, 0, 3, 0 },
      { 0, 1, 0, 0, 3, 2048, 26 }, { 0, 1, 1, 0, 3, 4096, 26 },
      { 1, 1, 1, 0, 3, 2048, 26 }, { 1, 0, 1, 0, 3, 1024, 26 },
      { 3, 1, 0, 1, 5, 1280, 26 }, { 3, 0, 0, 0, 3, 0, 26 },
      { 0, 1, 1, 0, 3, 0, 28 }, { 3, 1, 1, 0, 3, 0, 28 },
      { 0, 1, 1, 1, 3, 2048, 28 }, { 3, 1, 1, 1, 5, 1024, 28 },
      { 0, 0, 0, 0, 3, 0, 28 }
    };
    for (const auto& test : cases) {
      settings->setProperty("oversampling", test.quality);
      // These are the frozen preset type IDs, independent of the host binary.
      settings->setProperty("fx_slot_1_type", test.type); // Spectral Compressor, Suppressor or Spectral Lock.
      settings->setProperty("fx_slot_1_enabled", test.first);
      settings->setProperty("fx_slot_20_type", test.type);
      settings->setProperty("fx_slot_20_enabled", test.last);
      settings->setProperty("fx_slot_2_type", 18); // Spectral Gate.
      settings->setProperty("fx_slot_2_enabled", test.gate);
      settings->setProperty("fx_slot_2_spectral_gate_fft_size", test.fft);
      for (int bus = 1; bus <= 3; ++bus) {
        const juce::String prefix = "bus_" + juce::String(bus) + "_fx_slot_20_";
        settings->setProperty(prefix + "type", test.type == 28 ? 28 : 0);
        settings->setProperty(prefix + "enabled", test.first);
        settings->setProperty(prefix + "spectral_lock_root", bus * 3);
        settings->setProperty(prefix + "spectral_lock_scale", bus);
      }
      const auto json = juce::JSON::toString(state, true);
      juce::MemoryBlock raw(json.toRawUTF8(), json.getNumBytesAsUTF8());
      juce::XmlElement updated("VST3PluginState");
      updated.createNewChildElement("IComponent")->addTextElement(raw.toBase64Encoding());
      juce::MemoryBlock binary;
      juce::AudioProcessor::copyXmlToBinary(updated, binary);
      instance->setStateInformation(binary.getData(), int(binary.getSize()));
      processStressBlock(*instance, 0);
      const int actual = instance->getLatencySamples();
      std::cout << "VST3 host latency: expected=" << test.latency << " actual=" << actual << '\n';
      if (actual != test.latency) return 1;
      if (test.type == 28 && test.first && !test.gate) {
        auto duplicate = createPreparedInstance(manager, description, &binary, error);
        if (!duplicate || duplicate->getLatencySamples() != 0) return 1;
        for (int block = 0; block < 16; ++block) {
          processStressBlock(*instance, block);
          processStressBlock(*duplicate, block);
        }
        duplicate->releaseResources();
      }
    }
    std::cout << "Atlas VST3 host latency reporting passed\n";
    return 0;
  }
}

int main(int argc, char** argv) {
  if (argc < 2 || argc > 3) {
    std::cerr << "Usage: AtlasVstFactoryStress /absolute/path/to/Atlas.vst3 [project.RPP | --latency-test]\n";
    return 2;
  }

  const juce::File bundle(argv[1]);
  if (!bundle.exists()) {
    std::cerr << "VST3 bundle does not exist: " << bundle.getFullPathName() << '\n';
    return 2;
  }

  juce::ScopedJuceInitialiser_GUI juce;
  juce::AudioPluginFormatManager manager;
  juce::addHeadlessDefaultFormatsToManager(manager);

  juce::AudioPluginFormat* vst3 = nullptr;
  for (juce::AudioPluginFormat* format : manager.getFormats()) {
    if (format != nullptr && format->getName() == "VST3") {
      vst3 = format;
      break;
    }
  }
  if (vst3 == nullptr) {
    std::cerr << "JUCE VST3 host support is unavailable\n";
    return 2;
  }

  juce::OwnedArray<juce::PluginDescription> descriptions;
  vst3->findAllTypesForFile(descriptions, bundle.getFullPathName());
  if (descriptions.isEmpty()) {
    std::cerr << "No VST3 types found in " << bundle.getFullPathName() << '\n';
    return 2;
  }

  if (argc == 3 && juce::String(argv[2]) == "--latency-test")
    return testLatencyReporting(manager, *descriptions[0]);

  std::vector<juce::MemoryBlock> project_states;
  if (argc == 3) {
    const juce::File project(argv[2]);
    project_states = extractAtlasStates(project);
    if (project_states.empty()) {
      std::cerr << "No Atlas VST3 states found in " << project.getFullPathName() << '\n';
      return 2;
    }
    std::cout << "Extracted " << project_states.size() << " Atlas states from "
              << project.getFullPathName() << '\n';
  }

  if (!project_states.empty()) {
    // Load the complete project population, then duplicate each saved Atlas in
    // turn. Love.RPP has five existing instances and REAPER crashes while the
    // sixth factory object is being created.
    for (size_t duplicate = 0; duplicate < project_states.size(); ++duplicate) {
      std::vector<std::unique_ptr<juce::AudioPluginInstance>> instances;
      instances.reserve(project_states.size() + 1);
      for (const juce::MemoryBlock& state : project_states) {
        juce::String error;
        auto instance = createPreparedInstance(manager, *descriptions[0], &state, error);
        if (instance == nullptr) {
          std::cerr << "Project instance creation failed: " << error << '\n';
          return 1;
        }
        instances.push_back(std::move(instance));
      }

      // Keep the five existing instances rendering while the main thread
      // creates the pasted sixth instance, matching REAPER's host/audio split.
      std::atomic<bool> stop_audio { false };
      std::thread audio_thread([&instances, &stop_audio] {
        for (int block = 0; block < 2048 && !stop_audio.load(std::memory_order_relaxed); ++block) {
          for (auto& instance : instances)
            processStressBlock(*instance, block);
        }
      });

      juce::String error;
      auto pasted = createPreparedInstance(manager, *descriptions[0],
                                           &project_states[duplicate], error);
      stop_audio.store(true, std::memory_order_relaxed);
      audio_thread.join();
      if (pasted == nullptr) {
        std::cerr << "Pasted instance creation failed: " << error << '\n';
        return 1;
      }
      instances.push_back(std::move(pasted));
      std::cout << "Duplicated project Atlas state " << duplicate + 1 << " with "
                << instances.size() << " simultaneous instances\n";
    }

    std::cout << "Atlas project-state VST3 factory stress passed\n";
    return 0;
  }

  constexpr int kSimultaneousInstances = 8;
  constexpr int kCycles = 3;
  for (int cycle = 0; cycle < kCycles; ++cycle) {
    std::vector<std::unique_ptr<juce::AudioPluginInstance>> instances;
    instances.reserve(kSimultaneousInstances);

    for (int index = 0; index < kSimultaneousInstances; ++index) {
      juce::String error;
      auto instance = createPreparedInstance(manager, *descriptions[0], nullptr, error);
      if (instance == nullptr) {
        std::cerr << "Factory creation failed in cycle " << cycle << ", instance " << index
                  << ": " << error << '\n';
        return 1;
      }

      instances.push_back(std::move(instance));
    }

    std::cout << "Completed VST3 factory cycle " << cycle + 1 << " with "
              << instances.size() << " simultaneous instances\n";
    instances.clear();
  }

  std::cout << "Atlas VST3 factory stress passed\n";
  return 0;
}
