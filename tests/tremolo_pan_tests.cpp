#include "JuceHeader.h"
#include "tremolo_pan_module.h"
#include "reorderable_effect_chain.h"

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <new>

namespace {
  std::atomic<bool> track_allocations { false };
  std::atomic<size_t> allocations { 0 };
}
void* operator new(std::size_t size) {
  if (track_allocations.load()) ++allocations;
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

  struct Fixture {
    vital::Value bpm { 2.0f };
    vital::TremoloPanModule module { bpm.output() };
    vital::control_map controls;
    Fixture(int rate = 48000, int factor = 1) {
      module.init(); module.setSampleRate(rate); module.setOversampleAmount(factor);
      controls = module.getControls();
      set("sync", 0); set("frequency", 2); set("smoothing", 0);
    }
    void set(const char* id, float value) { controls.at(std::string("tremolo_pan_") + id)->set(value); }
  };

  std::vector<vital::poly_float> render(vital::SynthModule& module, int count, int block,
                                      bool transport = false, double start = 0.0) {
    if (block <= 0 || block > vital::kMaxBufferSize * module.getOversampleAmount()) {
      std::cerr << "Test block exceeds Atlas's internal processing capacity\n";
      std::exit(2);
    }
    std::vector<vital::poly_float> result(count), input(block);
    for (auto& sample : input)
      for (int lane = 0; lane < vital::poly_float::kSize; ++lane)
        sample.set(lane, (lane + 1) * 0.2f);
    allocations = 0; track_allocations = true;
    for (int cursor = 0; cursor < count; cursor += block) {
      const int size = std::min(block, count - cursor);
      if (transport) module.correctToTime(start + double(cursor) / module.getSampleRate());
      module.processWithInput(input.data(), size);
      std::copy_n(module.output()->buffer, size, result.data() + cursor);
    }
    track_allocations = false;
    expect(allocations == 0, "processing, transport corrections and first use must not allocate");
    return result;
  }

  double maximumError(const std::vector<vital::poly_float>& a, const std::vector<vital::poly_float>& b) {
    double error = 0;
    for (size_t i = 0; i < a.size(); ++i)
      for (int lane = 0; lane < vital::poly_float::kSize; ++lane)
        error = std::max(error, double(std::abs(a[i][lane] - b[i][lane])));
    return error;
  }

  bool transparent(const std::vector<vital::poly_float>& audio) {
    for (const auto& sample : audio)
      for (int lane = 0; lane < vital::poly_float::kSize; ++lane)
        if (sample[lane] != (lane + 1) * 0.2f) return false;
    return true;
  }
}

int main() {
  juce::ScopedJuceInitialiser_GUI juce;
  constexpr double pi = 3.14159265358979323846;
  for (int rate : { 44100, 48000, 96000 }) {
    for (int factor : { 1, 2, 4, 8 }) {
      Fixture f(rate, factor);
      const int effective_rate = rate * factor;
      const auto audio = render(f.module, effective_rate / 2, 113 * factor);
      double error = 0;
      for (int i = 0; i < static_cast<int>(audio.size()); ++i) {
        for (int lane = 0; lane < vital::poly_float::kSize; ++lane) {
          const double phase = double(i) * 2 / effective_rate + (lane % 2) * 0.5;
          const double expected = (lane + 1) * 0.2f * (0.5 + 0.5 * std::cos(2 * pi * phase));
          error = std::max(error, std::abs(audio[i][lane] - expected));
        }
      }
      expect(error < 2.0e-6, "free sine must have the exact rate and opposite stereo phase on every lane at every oversampling factor");
      f.set("stereo_phase", 0); f.set("offset", 0.25f); f.module.hardReset();
      const auto tremolo = render(f.module, effective_rate / 8, 97 * factor);
      expect(std::abs(tremolo.front()[0] - 0.1f) < 1.0e-6 && std::abs(tremolo.back()[0]) < 0.001f,
             "offset must shift both channels by a quarter cycle");
      bool shared = true;
      for (const auto& sample : tremolo)
        shared &= std::abs(sample[0] * 2 - sample[1]) < 1.0e-6f;
      expect(shared, "zero stereo phase must produce identical tremolo gain in both channels");
      for (const char* dry : { "mix", "depth" }) {
        f.set(dry, 0); f.module.hardReset();
        expect(transparent(render(f.module, 8192, 127)), "Mix 0 and Depth 0 must be bit-transparent with no latency");
        f.set(dry, 1);
      }
    }
  }

  // Musical timing, including freeze and long divisions, uses Atlas's existing
  // note table. Independent rates make this detect wrong sync ratios/modes.
  for (int sync : { 1, 2, 3 }) {
    for (int tempo : { 0, 1, 6, 8, 10, 12 }) {
      Fixture f;
      f.set("sync", float(sync)); f.set("tempo", float(tempo)); f.set("frequency", 19);
      const double beats_per_cycle = tempo == 0 ? 0.0 : 128.0 / std::exp2(tempo - 1);
      const double frequency = tempo == 0 ? 0.0 : 2.0 / beats_per_cycle * (sync == 2 ? 2.0 / 3.0 : sync == 3 ? 1.5 : 1.0);
      const auto audio = render(f.module, 12000, 61, true, 1.125);
      double error = 0;
      for (size_t i = 0; i < audio.size(); ++i) {
        const double phase = (1.125 + double(i) / 48000) * frequency;
        error = std::max(error, std::abs(audio[i][0] - 0.2f * (0.5 + 0.5 * std::cos(2 * pi * phase))));
      }
      expect(error < 2.0e-6, "sync division, dotted/triplet rate and host phase must be correct on first block");
    }
  }

  for (int shape = 0; shape < 5; ++shape) {
    Fixture small, large;
    for (auto* f : { &small, &large }) {
      f->set("shape", float(shape)); f->set("smoothing", 3.7f); f->set("sync", 1);
      f->set("stereo_phase", 0.31f); f->set("offset", 0.13f);
    }
    auto a = render(small.module, 3200, 1, true), b = render(large.module, 3200, 127, true);
    expect(maximumError(a, b) < 1.0e-6, "waveform and edge smoothing must not depend on callback size");
    for (auto* f : { &small, &large }) {
      f->set("shape", float((shape + 1) % 5)); f->set("depth", 0.37f);
      f->set("mix", 0.73f); f->set("stereo_phase", 1); f->set("offset", 0.9f);
      f->bpm.set(3.0f);
    }
    a = render(small.module, 10000, 1, true, 3200.0 / 48000);
    b = render(large.module, 10000, 127, true, 3200.0 / 48000);
    expect(maximumError(a, b) < 1.0e-6, "parameter/shape automation and live tempo changes must be independent of block size");
    bool bounded = true;
    for (const auto& sample : a)
      for (int lane = 0; lane < vital::poly_float::kSize; ++lane)
        bounded &= std::isfinite(sample[lane]) && sample[lane] >= 0 && sample[lane] <= (lane + 1) * 0.2f + 1.0e-6;
    expect(bounded, "automation must produce finite bounded gain without boosting or inverting audio");
  }

  for (int shape = 0; shape < 5; ++shape) {
    Fixture f;
    f.set("shape", float(shape)); f.set("stereo_phase", 0);
    const auto audio = render(f.module, 24000, 128);
    float low = 1, high = 0;
    for (const auto& sample : audio) { low = std::min(low, sample[0]); high = std::max(high, sample[0]); }
    expect(low < 0.0001 && high > 0.1999, "all shapes must reach the full depth range");
    if (shape == 1) expect(std::abs(audio[6000][0] - 0.1f) < 1.0e-6, "Triangle must have straight slopes");
    if (shape == 2) expect(audio[6000][0] == 0.2f && audio[18000][0] == 0, "Square must have high/low plateaus");
    if (shape == 3) expect(audio[6000][0] < audio[18000][0], "Ramp Up must rise");
    if (shape == 4) expect(audio[6000][0] > audio[18000][0], "Ramp Down must fall");
    f.module.hardReset();
    expect(maximumError(audio, render(f.module, 24000, 73)) < 1.0e-6, "hard reset must be deterministic");
    f.module.enable(false); f.module.enable(true);
    expect(maximumError(audio, render(f.module, 24000, 113)) < 1.0e-6, "bypass/re-enable must reset cleanly");
  }

  Fixture seek;
  seek.set("sync", 1);
  render(seek.module, 10000, 127, true);
  auto jumped = render(seek.module, 1, 1, true, 5.25);
  expect(jumped[0][0] < 1.0e-6 && std::abs(jumped[0][1] - 0.4f) < 1.0e-6, "transport seek must realign synced phase");
  seek.bpm.set(4);
  const auto changed = render(seek.module, 3001, 79, true, 5.25 + 1.0 / 48000);
  expect(std::abs(changed.back()[0] - 0.1f) < 0.0001, "live tempo change must change rate while preserving cycle phase");

  Fixture free_seek, free_reference;
  const auto free_a = render(free_seek.module, 3200, 91, true, -3.25);
  const auto free_b = render(free_reference.module, 3200, 91);
  expect(maximumError(free_a, free_b) < 1.0e-6, "free mode must ignore host timeline jumps");

  // Real chain integration: two independent Tremolo Pan slots in series,
  // removal, reordering type selection and bypass on main and all buses.
  for (int bus = 0; bus <= vital::kNumBuses; ++bus) {
    vital::Value bpm(2), keytrack(60);
    const std::string prefix = bus == 0 ? "" : "bus_" + std::to_string(bus) + "_";
    vital::ReorderableEffectChain chain(bpm.output(), keytrack.output(), prefix);
    chain.init(); chain.setSampleRate(48000);
    const auto controls = chain.getControls();
    for (int slot : { 1, 2 }) {
      const std::string id = prefix + "fx_slot_" + std::to_string(slot) + "_";
      controls.at(id + "type")->set(vital::constants::kTremoloPan + 1);
      controls.at(id + "tremolo_pan_sync")->set(1);
      controls.at(id + "tremolo_pan_tempo")->set(0); // static phase
      controls.at(id + "tremolo_pan_stereo_phase")->set(0);
      controls.at(id + "tremolo_pan_offset")->set(slot == 1 ? 0.25f : 0.5f);
      controls.at(id + "tremolo_pan_depth")->set(0.5f);
    }
    chain.prepareSelectedEffects(); // As the plugin does before playback.
    auto both = render(chain, 128, 128);
    expect(std::abs(both[0][0] - 0.2f * 0.75f * 0.5f) < 1.0e-6, "two slots must maintain independent parameters and DSP state");
    controls.at(prefix + "fx_slot_1_enabled")->set(0);
    controls.at(prefix + "fx_slot_2_type")->set(0);
    expect(transparent(render(chain, 128, 128)), "slot bypass and removal must return immediate dry audio");
  }

  std::cout << "Tremolo Pan: " << failures << " failures\n";
  return failures ? 1 : 0;
}
