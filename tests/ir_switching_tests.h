#pragma once

namespace {
void irSwitchingKeepsLiveEngine() {
  using namespace vital;
  std::atomic<int> constructions { 0 };
  auto ir_worker = std::make_shared<ConvolutionIrWorker>();
  auto preparation = std::make_shared<EffectPreparationWorker>();
  PreparedEffectModule module(constants::kConvolutionReverb, "", [&] {
    ++constructions;
    return std::make_unique<ConvolutionReverbModule>("", ir_worker);
  }, preparation, ir_worker);
  module.init(); module.setSampleRate(48000); module.setOversampleAmount(1);
  module.getControls().at("convolution_reverb_dry_wet")->set(1);
  auto* state = module.convolutionState();
  ConvolutionReverbModule::IrData ir;
  ir.name = "First delay"; ir.sample_rate = 48000; ir.left.resize(4096); ir.left[640] = 1;
  expect(state->irStateFromJson(ConvolutionReverbModule::irToJson(ir)), "initial switching IR loads");
  module.prepareForPlayback();
  const int initial_constructions = constructions;
  std::array<poly_float, 128> input {};
  RealtimeEffectScope realtime(true);
  audio_allocations = 0;
  auto process = [&] {
    track_allocations = true;
    module.processWithInput(input.data(), int(input.size()));
    track_allocations = false;
  };
  auto hearDelay = [&](int delay) {
    for (int block = 0; block < 1000; ++block) {
      input.fill(poly_float(0));
      if (block % 32 == 0) input[0] = 1;
      process();
      if (block % 32 == delay / 128 &&
          std::abs(module.output()->buffer[delay % 128][0]) > .01f) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
  };
  expect(hearDelay(640), "first IR becomes audible");

  // IR selection must keep the old wet response alive until the new one is ready.
  // An impulse followed by a replacement must not fall back to dry passthrough.
  input.fill(poly_float(0)); input[0] = 1; process();
  std::fill(ir.left.begin(), ir.left.end(), 0); ir.left[1152] = 1; ir.name = "Second delay";
  expect(state->irStateFromJson(ConvolutionReverbModule::irToJson(ir)), "replacement IR loads");
  bool retained_tail = false;
  input.fill(poly_float(0));
  for (int block = 1; block <= 5; ++block) {
    process();
    if (block == 5) retained_tail = std::abs(module.output()->buffer[0][0]) > .01f;
  }
  expect(retained_tail, "old wet tail survives IR replacement");
  const auto start = std::chrono::steady_clock::now();
  expect(hearDelay(1152), "replacement IR becomes audible");
  std::cout << "IR replacement audible_ms="
            << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() << '\n';
  expect(constructions == initial_constructions, "IR changes reuse the live convolution engine");
  expect(audio_allocations == 0, "IR replacement allocates no C++ memory on the audio thread");
}

void asyncIrLoadingIsLatestWins() {
  using Module = vital::ConvolutionReverbModule;
  using Result = Module::IrLoadResult;
  const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory)
      .getNonexistentChildFile("atlas-ir-loading", {}, false);
  expect(root.createDirectory().wasOk(), "IR loading fixtures created");
  Module state("", nullptr, true);
  Module::IrData ir;
  ir.name = "First"; ir.sample_rate = 48000; ir.left.resize(8192); ir.left[640] = 1;
  const auto first = root.getChildFile("First.atpir");
  const auto second = root.getChildFile("Second.atpir");
  expect(first.replaceWithText(Module::irToJson(ir).dump()), "first IR fixture written");
  ir.name = "Second";
  const auto expected = Module::irToJson(ir);
  expect(second.replaceWithText(expected.dump()), "second IR fixture written");
  auto waitFor = [&](uint64_t request) {
    for (int attempt = 0; attempt < 2000; ++attempt) {
      const auto status = state.irLoadStatus();
      if (status.request != request || status.result != Result::loading) return status.result;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return Result::loading;
  };
  uint64_t last = 0;
  for (int i = 0; i < 100; ++i) last = state.requestIrFile(i % 2 ? second : first);
  expect(waitFor(last) == Result::loaded && state.irStateToJson() == expected,
         "rapid requests commit only the final selection");
  const auto revision = state.irRevision();
  last = state.requestIrFile(root.getChildFile("missing.wav"));
  expect(waitFor(last) == Result::failed && state.irRevision() == revision &&
         state.irStateToJson() == expected, "failed file preserves the current IR");
  state.requestIrFile(first);
  state.setDefaultIr();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  expect(!state.hasCustomIr() && state.irName() == "Default Hall", "reset cancels queued and in-flight loads");

  auto invalid = expected;
  invalid["length"] = 2147483647;
  expect(!Module::irFromJson(invalid), "oversized serialized IR rejected before allocation");
  invalid = expected; invalid["samples"] = "AAAA";
  expect(!Module::irFromJson(invalid), "truncated serialized IR rejected");
  invalid = expected; invalid["sample_rate"] = 0;
  expect(!Module::irFromJson(invalid), "invalid IR sample rate rejected");
  invalid = expected; invalid["samples_stereo"] = "AAAA";
  expect(!Module::irFromJson(invalid), "truncated stereo channel rejected");

  juce::MemoryOutputStream wave;
  wave.write("RIFF", 4); wave.writeInt(36 + 512); wave.write("WAVEfmt ", 8);
  wave.writeInt(16); wave.writeShort(3); wave.writeShort(1); // IEEE float, mono
  wave.writeInt(48000); wave.writeInt(48000 * 4); wave.writeShort(4); wave.writeShort(32);
  wave.write("data", 4); wave.writeInt(512);
  for (int i = 0; i < 128; ++i) wave.writeInt(i == 0 ? 0x7fc00000 : 0);
  const auto corrupt = root.getChildFile("Nonfinite.wav");
  expect(corrupt.replaceWithData(wave.getData(), wave.getDataSize()), "nonfinite audio fixture written");
  const auto default_revision = state.irRevision();
  last = state.requestIrFile(corrupt);
  expect(waitFor(last) == Result::failed && state.irRevision() == default_revision,
         "nonfinite audio is rejected without changing the current IR");

  // A closing editor/plugin must safely cancel or finish its outstanding request.
  for (int i = 0; i < 8; ++i) {
    Module closing("", nullptr, true);
    closing.requestIrFile(first);
  }
  expect(root.deleteRecursively(), "IR loading fixtures removed");
}

void irTrimAtEndIsSafe() {
  using Module = vital::ConvolutionReverbModule;
  Module module;
  module.init(); module.setSampleRate(48000); module.setOversampleAmount(1);
  Module::IrData ir;
  ir.name = "End trim"; ir.sample_rate = 48000; ir.left.resize(100); ir.left.back() = 1;
  expect(module.irStateFromJson(Module::irToJson(ir)), "short trim fixture loads");
  const auto controls = module.getControls();
  controls.at("convolution_reverb_ir_start")->set(1);
  controls.at("convolution_reverb_ir_end")->set(1);
  controls.at("convolution_reverb_ir_size")->set(4);
  std::array<vital::poly_float, 128> input {};
  for (int block = 0; block < 100; ++block) {
    module.processWithInput(input.data(), int(input.size()));
    for (int sample = 0; sample < int(input.size()); ++sample)
      expect(std::isfinite(module.output()->buffer[sample][0]), "IR end trim output remains finite");
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}
}
