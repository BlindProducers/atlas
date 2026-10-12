// Glitch Delay tests. --dsp runs the engine suites ported from Astralay with
// its DSP (tests/glitch_delay, MIT). The other flags cover Atlas's side: the
// parameter table and its units, the module's echo timing, tempo sync and
// oversampling, allocation-free glitching, reproducible randomness against
// Atlas's song time, a chain slot, host state and the editor's tap rows.
#include "JuceHeader.h"
#include "glitch_delay_module.h"
#include "glitch_delay/glitch_delay_engine.h"
#include "glitch_delay/note_values.h"
#include "reorderable_effect_chain.h"
#include "synth_editor.h"
#include "synth_parameters.h"
#include "synth_plugin.h"
#include "synth_strings.h"
#include "trance_gate_clock.h"
#include "value_bridge.h"

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <new>
#include <vector>

namespace {
  std::atomic<bool> tracking { false };
  std::atomic<size_t> allocations { 0 };
}
void* operator new(std::size_t size) {
  if (tracking.load()) ++allocations;
  if (void* p = std::malloc(size > 0 ? size : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {
  using vital::GlitchDelayModule;
  namespace gd = atlas::glitch_delay;
  constexpr int kRate = 48000;
  constexpr int kBlock = 128;
  int failures = 0;

  void expect(bool condition, const std::string& message) {
    if (!condition) {
      ++failures;
      std::cerr << "FAIL: " << message << '\n';
    }
  }

  int finish(const char* name) {
    if (failures)
      return 1;
    std::cout << "Glitch Delay " << name << " checks passed.\n";
    return 0;
  }

  std::string tapId(int tap, const char* suffix) {
    return "glitch_delay_tap_" + std::to_string(tap + 1) + "_" + suffix;
  }

  float defaultOf(const std::string& name) { return vital::Parameters::getDetails(name).default_value; }

  // ---- Parameters -------------------------------------------------------------------

  int parameters() {
    const auto near = [](double a, double b, double tolerance) { return std::abs(a - b) <= tolerance; };

    const auto labels = gd::NoteValues::labels();
    expect(labels.size() == 26, "26 note values");
    for (int i = 0; i < labels.size(); ++i)
      expect(labels[i].toStdString() == strings::kGlitchDelayNoteNames[i], "note value " + std::to_string(i) + " reads as Astralay's");
    const auto stutter = gd::NoteValues::labelsBetween(0.0, 1.0);
    expect(stutter.size() == 14, "14 stutter slices");
    for (int i = 0; i < stutter.size(); ++i)
      expect(stutter[i].toStdString() == strings::kGlitchDelayStutterNoteNames[i], "stutter slice " + std::to_string(i));

    // Every control the module makes is a parameter, in the main chain, on
    // every bus and in every slot, with its default inside its range.
    vital::Value bpm(2.0f);
    GlitchDelayModule module(bpm.output(), nullptr);
    module.init();
    const auto controls = module.getControls();
    expect(controls.size() == GlitchDelayModule::kNumGlobalControls +
                              GlitchDelayModule::kNumTaps * GlitchDelayModule::kNumTapControls,
           "one control per global and tap parameter");
    for (const auto& control : controls) {
      for (const std::string prefix : { "", "bus_3_", "fx_slot_20_", "bus_1_fx_slot_7_" }) {
        const std::string name = prefix + control.first;
        expect(vital::Parameters::isParameter(name), name + " is a parameter");
      }
      const auto& details = vital::Parameters::getDetails(control.first);
      expect(details.default_value >= details.min && details.default_value <= details.max, control.first + " default in range");
      expect(control.second->value() == details.default_value, control.first + " starts at its default");
    }
    expect(vital::Parameters::isParameter("glitch_delay_on") && vital::Parameters::isParameter("effect_chain_slot_37"),
           "the chain's switch and order slot exist");
    expect(std::string(strings::kEffectOrder[vital::constants::kGlitchDelay]) == "glitch_delay" &&
           std::string(strings::kEffectSlotTypeNames[vital::constants::kGlitchDelay + 1]) == "Glitch Delay",
           "the effect's id and name");

    // Stored values decode to Astralay's defaults.
    const auto log2ms = [&](const std::string& name) { return std::exp2(defaultOf(name)); };
    const float time = defaultOf(tapId(0, "time"));
    expect(near(time * time * time, 500.0, 0.05), "tap time 500 ms");
    const float glide = defaultOf("glitch_delay_glide");
    expect(near(glide * glide, 100.0, 0.01), "glide 100 ms");
    const float smear = defaultOf("glitch_delay_smear_size");
    expect(near(smear * smear, 200.0, 0.01), "smear size 200 ms");
    expect(near(log2ms("glitch_delay_buffer_size"), 125.0, 0.01), "buffer 125 ms");
    expect(near(log2ms("glitch_delay_stop_time"), 500.0, 0.01) && near(log2ms("glitch_delay_start_time"), 250.0, 0.01),
           "tape stop 500 ms, start 250 ms");
    expect(near(log2ms(tapId(0, "low_cut")), 20.0, 0.01) && near(log2ms(tapId(0, "high_cut")), 20000.0, 0.5),
           "feedback filters open");
    expect(near(log2ms(tapId(0, "stutter_min")), 20.0, 0.01) && near(log2ms(tapId(0, "stutter_max")), 120.0, 0.01), "stutter 20-120 ms");
    expect(near(log2ms(tapId(0, "grain_size_min")), 20.0, 0.01) && near(log2ms(tapId(0, "grain_size_max")), 80.0, 0.01), "grains 20-80 ms");
    expect(near(log2ms(tapId(0, "grain_density_min")), 10.0, 0.01) && near(log2ms(tapId(0, "grain_density_max")), 40.0, 0.01), "10-40 grains/s");
    expect(near(log2ms(tapId(0, "ring_min")), 30.0, 0.01) && near(log2ms(tapId(0, "ring_max")), 800.0, 0.05), "ring 30-800 Hz");
    expect(near(log2ms(tapId(0, "fm_ratio_min")), 0.5, 1e-4) && near(log2ms(tapId(0, "fm_ratio_max")), 3.0, 1e-4), "FM ratio 0.5-3");
    expect(near(log2ms(tapId(0, "crush_rate_min")), 1.0, 1e-4) && near(log2ms(tapId(0, "crush_rate_max")), 8.0, 1e-4), "rate 1-8x");
    expect(strings::kGlitchDelayNoteNames[int(defaultOf(tapId(0, "time_sync")))] == "1/4" &&
           strings::kGlitchDelayNoteNames[int(defaultOf("glitch_delay_buffer_sync"))] == "1/16" &&
           strings::kGlitchDelayStutterNoteNames[int(defaultOf(tapId(0, "stutter_sync_min")))] == "1/32" &&
           strings::kGlitchDelayStutterNoteNames[int(defaultOf(tapId(0, "stutter_sync_max")))] == "1/8",
           "synced defaults 1/4, 1/16, 1/32 and 1/8");
    expect(defaultOf(tapId(0, "enabled")) == 1.0f && defaultOf(tapId(1, "enabled")) == 0.0f &&
           defaultOf(tapId(3, "enabled")) == 0.0f, "only tap 1 starts on");
    return finish("parameters");
  }

  // ---- Module -----------------------------------------------------------------------

  struct Rig {
    vital::Value bpm { 2.0f };
    vital::TranceGateClock clock;
    GlitchDelayModule module { bpm.output(), &clock };
    double seconds = 0.0;
    bool count_allocations = false;   // Counts allocations inside processWithInput only.

    explicit Rig(int rate = kRate, int oversample = 1) {
      module.init();
      module.setSampleRate(rate);
      module.setOversampleAmount(oversample);
      set("glitch_delay_mix", 1.0f);
      set("glitch_delay_smear_amount", 0.0f);
      set("glitch_delay_output_gain", 0.0f);
      set("glitch_delay_glide", 0.0f);
    }

    void set(const std::string& name, float value) { module.getControls().at(name)->set(value); }
    int rate() const { return module.getSampleRate(); }

    // Renders a mono signal; returns left and right.
    std::pair<std::vector<float>, std::vector<float>> render(const std::vector<float>& input, int block = kBlock,
                                                             double jitter = 0.0) {
      std::vector<float> left(input.size()), right(input.size());
      std::vector<vital::poly_float> buffer(static_cast<size_t>(block));
      int blocks = 0;
      for (size_t start = 0; start < input.size(); start += static_cast<size_t>(block)) {
        const int count = static_cast<int>(std::min<size_t>(static_cast<size_t>(block), input.size() - start));
        for (int i = 0; i < count; ++i)
          buffer[static_cast<size_t>(i)] = vital::poly_float(input[start + static_cast<size_t>(i)]);
        // Atlas's song time, with optional rounding noise of the kind the host's
        // seconds-from-PPQ conversion adds.
        module.correctToTime(seconds + ((blocks++ % 2) ? jitter : -jitter));
        if (count_allocations) tracking = true;
        module.processWithInput(buffer.data(), count);
        tracking = false;
        for (int i = 0; i < count; ++i) {
          left[start + static_cast<size_t>(i)] = module.output()->buffer[i][0];
          right[start + static_cast<size_t>(i)] = module.output()->buffer[i][1];
        }
        seconds += static_cast<double>(count) / rate();
      }
      return { left, right };
    }
  };

  std::vector<float> impulse(int length) {
    std::vector<float> signal(static_cast<size_t>(length), 0.0f);
    signal[0] = 1.0f;
    return signal;
  }

  std::vector<float> noise(int length, int seed) {
    juce::Random random(seed);
    std::vector<float> signal(static_cast<size_t>(length));
    for (auto& sample : signal)
      sample = 0.5f * (random.nextFloat() * 2.0f - 1.0f);
    return signal;
  }

  int peakIndex(const std::vector<float>& signal) {
    int index = 0;
    for (int i = 1; i < static_cast<int>(signal.size()); ++i)
      if (std::abs(signal[static_cast<size_t>(i)]) > std::abs(signal[static_cast<size_t>(index)]))
        index = i;
    return index;
  }

  int moduleTest() {
    // A 100 ms tap at 48 kHz, panned centre at constant power.
    {
      Rig rig;
      rig.set(tapId(0, "time"), std::cbrt(100.0f));
      rig.set(tapId(0, "feedback"), 0.0f);
      const auto [left, right] = rig.render(impulse(kRate / 4));
      expect(peakIndex(left) == 4800 && peakIndex(right) == 4800, "the echo arrives after 100 ms: " + std::to_string(peakIndex(left)));
      expect(std::abs(left[4800] - 0.7071f) < 0.01f && std::abs(right[4800] - 0.7071f) < 0.01f,
             "a centred tap is at constant power: " + std::to_string(left[4800]));
      bool mirrored = true;
      for (int i = 0; i < kBlock; ++i) {
        const auto out = rig.module.output()->buffer[i];
        mirrored = mirrored && out[0] == out[2] && out[1] == out[3];
      }
      expect(mirrored, "lanes 2 and 3 mirror the processed pair");
    }
    // Oversampled, the delay is the same time at the higher rate.
    {
      Rig rig(kRate, 2);
      expect(rig.rate() == 2 * kRate, "oversampling doubles the module rate");
      rig.set(tapId(0, "time"), std::cbrt(100.0f));
      const auto [left, right] = rig.render(impulse(kRate / 2));
      expect(peakIndex(left) == 9600, "oversampled echo after 100 ms: " + std::to_string(peakIndex(left)));
    }
    // Host Sync: a 1/8 note at 120 BPM is 250 ms. Pan and volume reach the tap.
    {
      Rig rig;
      rig.set("glitch_delay_sync", 1.0f);
      rig.set(tapId(0, "time_sync"), 10.0f);
      rig.set(tapId(0, "pan"), -1.0f);
      rig.set(tapId(0, "volume"), -6.0f);
      const auto [left, right] = rig.render(impulse(kRate / 2));
      expect(peakIndex(left) == 12000, "a synced 1/8 at 120 BPM is 250 ms: " + std::to_string(peakIndex(left)));
      expect(std::abs(left[12000] - 0.5012f) < 0.01f && std::abs(right[12000]) < 1e-4f,
             "hard left at -6 dB: " + std::to_string(left[12000]) + ", " + std::to_string(right[12000]));
      Rig silent;
      silent.set(tapId(0, "time"), std::cbrt(100.0f));
      silent.set(tapId(0, "volume"), -60.0f);
      const auto [quiet, unused] = silent.render(impulse(kRate / 2));
      float loudest = 0.0f;
      for (float sample : quiet) loudest = std::max(loudest, std::abs(sample));
      expect(loudest < 1e-6f, "the lowest volume is silent");
    }
    // Four taps at once; the dry signal passes at zero mix.
    {
      Rig rig;
      for (int tap = 0; tap < GlitchDelayModule::kNumTaps; ++tap) {
        rig.set(tapId(tap, "enabled"), 1.0f);
        rig.set(tapId(tap, "time"), std::cbrt(50.0f * (tap + 1)));
        rig.set(tapId(tap, "feedback"), 0.0f);
      }
      const auto [left, right] = rig.render(impulse(kRate / 4));
      for (int tap = 0; tap < GlitchDelayModule::kNumTaps; ++tap)
        expect(std::abs(left[static_cast<size_t>(2400 * (tap + 1))]) > 0.6f, "tap " + std::to_string(tap + 1) + " echoes");
      rig.set("glitch_delay_mix", 0.0f);
      rig.module.hardReset();
      const auto signal = noise(4096, 3);
      const auto [dry, dry_right] = rig.render(signal);
      float error = 0.0f;
      for (size_t i = 0; i < signal.size(); ++i) error = std::max(error, std::abs(dry[i] - signal[i]));
      expect(error < 1e-5f, "zero mix is dry");
    }
    // Every glitch at full probability, frozen, at full feedback: no
    // allocation, finite and bounded.
    {
      Rig rig;
      for (int tap = 0; tap < GlitchDelayModule::kNumTaps; ++tap) {
        rig.set(tapId(tap, "enabled"), 1.0f);
        rig.set(tapId(tap, "time"), std::cbrt(30.0f + 40.0f * tap));
        rig.set(tapId(tap, "feedback"), 1.0f);
        for (const char* type : { "reverse", "stutter", "grain", "pitch", "lpc", "ceps", "ring", "fm", "crush" })
          rig.set(tapId(tap, (std::string(type) + "_prob").c_str()), 1.0f);
      }
      rig.set("glitch_delay_max_glitches", 4.0f);
      rig.set("glitch_delay_buffer_size", std::log2(40.0f));
      rig.set("glitch_delay_placement", 1.0f);
      rig.set("glitch_delay_smear_amount", 1.0f);
      rig.render(noise(kRate / 2, 5));
      rig.set("glitch_delay_freeze", 1.0f);
      rig.set("glitch_delay_freeze_sustain", 1.0f);
      const auto signal = noise(4 * kRate, 6);
      rig.count_allocations = true;
      allocations = 0;
      const auto [left, right] = rig.render(signal);
      rig.count_allocations = false;
      expect(allocations == 0, "glitching allocates nothing on the audio thread: " + std::to_string(allocations.load()));
      bool finite = true;
      float loudest = 0.0f;
      for (size_t i = 0; i < left.size(); ++i) {
        finite = finite && std::isfinite(left[i]) && std::isfinite(right[i]);
        loudest = std::max(loudest, std::max(std::abs(left[i]), std::abs(right[i])));
      }
      expect(finite && loudest < 8.0f, "everything at maximum stays finite and bounded: peak " + std::to_string(loudest));
    }
    // The tape stop holds the repeats; the dry signal still passes.
    {
      Rig rig;
      rig.set(tapId(0, "time"), std::cbrt(100.0f));
      rig.set("glitch_delay_stop_tape", 1.0f);
      rig.set("glitch_delay_stop_time", std::log2(50.0f));
      rig.render(std::vector<float>(kRate / 2, 0.0f));
      const auto [left, right] = rig.render(noise(kRate / 2, 8));
      double energy = 0.0;
      for (float sample : left) energy += sample * sample;
      expect(energy < 1e-6, "a stopped tape is silent at full mix");
    }
    return finish("module");
  }

  // ---- Reproducible randomness --------------------------------------------------------

  std::vector<float> glitchy(Rig& rig, bool reproducible, double jitter, int block = kBlock) {
    rig.set(tapId(0, "time"), std::cbrt(60.0f));
    rig.set(tapId(0, "feedback"), 0.6f);
    for (const char* type : { "reverse", "stutter", "pitch", "ring", "crush" })
      rig.set(tapId(0, (std::string(type) + "_prob").c_str()), 0.5f);
    rig.set("glitch_delay_placement", 1.0f);
    rig.set("glitch_delay_buffer_size", std::log2(30.0f));
    rig.set("glitch_delay_reproducible", reproducible ? 1.0f : 0.0f);
    rig.set("glitch_delay_seed", 42.0f);
    rig.clock.setPlaying(true);
    return rig.render(noise(2 * kRate, 9), block, jitter).first;
  }

  int reproducible() {
    Rig first, second, jittered, unseeded_a, unseeded_b;
    const auto reference = glitchy(first, true, 0.0);
    expect(glitchy(second, true, 0.0) == reference, "a seed repeats exactly from the start of playback");
    expect(glitchy(jittered, true, 0.4 / kRate) == reference,
           "rounding in the song time doesn't restart the randomness");
    expect(glitchy(unseeded_a, false, 0.0) != glitchy(unseeded_b, false, 0.0), "without the seed, runs differ");

    // A jump in the song position restarts the sequence, as Astralay does on a
    // host loop or seek: two passes over the same span then glitch alike, once
    // the first pass's tail has left the delay line and the DC blocker.
    Rig looped;
    looped.set(tapId(0, "time"), std::cbrt(60.0f));
    looped.set(tapId(0, "feedback"), 0.0f);
    looped.set(tapId(0, "reverse_prob"), 0.5f);
    looped.set(tapId(0, "stutter_prob"), 0.5f);
    looped.set("glitch_delay_placement", 1.0f);
    looped.set("glitch_delay_buffer_size", std::log2(30.0f));
    looped.set("glitch_delay_reproducible", 1.0f);
    looped.clock.setPlaying(true);
    const auto signal = noise(kRate, 4);
    const auto once = looped.render(signal).first;
    looped.seconds = 0.0;
    const auto again = looped.render(signal).first;
    int differences = 0;
    for (size_t i = kRate / 2; i < once.size(); ++i)
      differences += std::abs(once[i] - again[i]) > 1e-4f ? 1 : 0;
    expect(differences == 0, "a jump back replays the same glitches: " + std::to_string(differences) + " samples differ");
    return finish("reproducible");
  }

  // ---- Chain ----------------------------------------------------------------------

  int chain() {
    vital::Value bpm(2.0f), keytrack(0.0f);
    vital::TranceGateClock clock;
    for (const std::string prefix : { "", "bus_2_" }) {
      vital::ReorderableEffectChain chain(bpm.output(), keytrack.output(), prefix, nullptr, nullptr, &clock, nullptr, nullptr);
      chain.init();
      chain.setSampleRate(kRate);
      auto controls = chain.getControls();
      controls.at(prefix + "fx_slot_1_type")->set(vital::constants::kGlitchDelay + 1);
      controls.at(prefix + "fx_slot_1_enabled")->set(1);
      controls.at(prefix + "fx_slot_1_glitch_delay_tap_1_time")->set(std::cbrt(100.0f));
      controls.at(prefix + "fx_slot_1_glitch_delay_smear_amount")->set(0.0f);
      std::vector<vital::poly_float> block(kBlock);
      std::vector<float> left;
      for (int offset = 0; offset < kRate / 4; offset += kBlock) {
        for (int i = 0; i < kBlock; ++i)
          block[static_cast<size_t>(i)] = offset + i == 0 ? 1.0f : 0.0f;
        chain.processWithInput(block.data(), kBlock);
        for (int i = 0; i < kBlock; ++i)
          left.push_back(chain.output()->buffer[i][0]);
      }
      // Default mix 50%: the dry impulse at 0, the repeat at 100 ms.
      float echo = 0.0f;
      for (int i = 4790; i < 4810; ++i) echo = std::max(echo, std::abs(left[static_cast<size_t>(i)]));
      expect(echo > 0.3f, "Glitch Delay echoes in chain '" + prefix + "': " + std::to_string(echo));
    }
    return finish("chain");
  }

  // ---- Host state -------------------------------------------------------------------

  int state() {
    juce::MemoryBlock saved;
    {
      SynthPlugin plugin;
      for (auto* parameter : plugin.getParameters()) {
        const auto* bridge = dynamic_cast<ValueBridge*>(parameter);
        expect((bridge == nullptr || !bridge->getParameterId().contains("glitch_delay")) &&
               !parameter->getName(128).contains("Glitch Delay"),
               "Glitch Delay is automated through FX slots, not host parameters");
      }
      auto& controls = plugin.getControls();
      controls.at("bus_1_fx_slot_2_type")->set(vital::constants::kGlitchDelay + 1);
      controls.at("bus_1_fx_slot_2_glitch_delay_tap_3_enabled")->set(1.0f);
      controls.at("bus_1_fx_slot_2_glitch_delay_tap_3_stutter_prob")->set(0.25f);
      controls.at("bus_1_fx_slot_2_glitch_delay_seed")->set(1234.0f);
      controls.at("bus_1_fx_slot_2_glitch_delay_tap_view")->set(2.0f);
      plugin.getStateInformation(saved);
    }
    SynthPlugin restored;
    restored.setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
    auto& controls = restored.getControls();
    expect(controls.at("bus_1_fx_slot_2_type")->value() == vital::constants::kGlitchDelay + 1, "the slot type is restored");
    expect(controls.at("bus_1_fx_slot_2_glitch_delay_tap_3_enabled")->value() == 1.0f &&
           controls.at("bus_1_fx_slot_2_glitch_delay_tap_3_stutter_prob")->value() == 0.25f &&
           controls.at("bus_1_fx_slot_2_glitch_delay_seed")->value() == 1234.0f &&
           controls.at("bus_1_fx_slot_2_glitch_delay_tap_view")->value() == 2.0f, "the slot's settings are restored");
    return finish("state");
  }

  // ---- Editor -----------------------------------------------------------------------

  void collectTitles(Component& parent, juce::StringArray& titles) {
    if (parent.getTitle().isNotEmpty())
      titles.add(parent.getTitle());
    for (auto* child : parent.getChildren())
      collectTitles(*child, titles);
  }

  Component* findTitled(Component& parent, const std::function<bool(const String&)>& match) {
    if (match(parent.getTitle()))
      return &parent;
    for (auto* child : parent.getChildren())
      if (auto* found = findTitled(*child, match))
        return found;
    return nullptr;
  }

  template <typename Predicate>
  void pumpUntil(Predicate done, int timeout_ms = 5000) {
    const uint32 start = juce::Time::getMillisecondCounter();
    while (!done() && juce::Time::getMillisecondCounter() - start < static_cast<uint32>(timeout_ms))
      juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
    juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
  }

  int editorTest() {
    SynthPlugin synth;
    synth.parameterChanged("fx_slot_1_type", static_cast<float>(vital::constants::kGlitchDelay + 1));
    synth.parameterChanged("fx_slot_1_enabled", 1.0f);
    SynthEditor editor(synth);
    editor.updateFullGui();
    pumpUntil([] { return false; }, 200);

    const auto titles = [&] {
      juce::StringArray result;
      collectTitles(editor, result);
      return result;
    };
    const auto has = [&](const String& text) {
      return findTitled(editor, [&text](const String& title) { return title.contains(text); }) != nullptr;
    };
    const bool verbose = juce::SystemStats::getEnvironmentVariable("GLITCH_DELAY_TEST_VERBOSE", {}).isNotEmpty();
    if (verbose)
      for (const auto& title : titles()) std::cout << "  " << title << '\n';

    expect(has("Glitch Delay"), "the slot is titled after the effect");
    expect(has("Show Tap"), "a Show Tap menu");
    expect(has("Tap 1 Time") && has("Tap 1 Stutter Probability") && has("Tap 1 Bit Crusher Maximum Rate Reduction"),
           "tap 1's rows are shown");
    expect(!has("Tap 2 Time") && !has("Tap 4 Volume"), "the other taps' rows are hidden");
    expect(!has("Tap 1 Synced Time") && !has("Synced Buffer Size") && has("Buffer Size"),
           "free times show while Host Sync is off");
    expect(has("Mix") && has("Freeze Sustain") && has("Tape Stop"), "the global rows are shown");

    // Rows sit in nested groups: Global > Timing ..., Tap 1 > Delay, and
    // Tap 1 > Glitches > one group per glitch type.
    const auto exactly = [](Component& parent, const String& title) {
      return findTitled(parent, [&title](const String& t) { return t == title; });
    };
    const auto inside = [&](const String& row, std::initializer_list<const char*> path) {
      Component* scope = &editor;
      for (const auto* group : path) {
        scope = scope != nullptr ? exactly(*scope, group) : nullptr;
        if (scope == nullptr || dynamic_cast<ComboBox*>(scope) != nullptr)
          return false;
      }
      return scope != nullptr && exactly(*scope, row) != nullptr;
    };
    expect(inside("Glide Time", { "Slot 1 (Glitch Delay)", "Global", "Timing" }) &&
           inside("Tape Stop", { "Slot 1 (Glitch Delay)", "Global", "Tape stop" }) &&
           inside("Seed", { "Slot 1 (Glitch Delay)", "Global", "Glitch engine" }) &&
           inside("Mix", { "Slot 1 (Glitch Delay)", "Global", "Output" }), "global rows are grouped");
    expect(inside("Tap 1 Feedback", { "Slot 1 (Glitch Delay)", "Tap 1", "Delay" }) &&
           inside("Tap 1 High Cut", { "Slot 1 (Glitch Delay)", "Tap 1", "Delay" }), "delay rows sit in the tap's Delay group");
    expect(inside("Tap 1 Stutter Maximum Slice", { "Slot 1 (Glitch Delay)", "Tap 1", "Glitches", "Stutter" }) &&
           inside("Tap 1 Pitch Mode", { "Slot 1 (Glitch Delay)", "Tap 1", "Glitches", "Pitch" }) &&
           inside("Tap 1 Bit Crusher Probability", { "Slot 1 (Glitch Delay)", "Tap 1", "Glitches", "Bit crusher" }),
           "each glitch type has its own group under Glitches");
    expect(inside("Show Tap", { "Slot 1 (Glitch Delay)" }) && !inside("Show Tap", { "Slot 1 (Glitch Delay)", "Global" }),
           "Show Tap sits between Global and the tap");
    if (auto* glitches = findTitled(editor, [](const String& t) { return t == "Glitches"; }))
      expect(glitches->getDescription().isNotEmpty(), "groups are described");

    if (auto* menu = dynamic_cast<ComboBox*>(findTitled(editor, [](const String& t) { return t == "Show Tap"; }))) {
      expect(menu->getNumItems() == 4 && menu->getItemText(2) == "Tap 3", "Show Tap lists the four taps");
      menu->setSelectedItemIndex(2, juce::sendNotificationSync);
      pumpUntil([&] { return has("Tap 3 Time"); });
      expect(has("Tap 3 Time") && !has("Tap 1 Time"), "choosing a tap shows its rows");
      expect(inside("Tap 3 Time", { "Slot 1 (Glitch Delay)", "Tap 3", "Delay" }), "the tap group follows Show Tap");
      expect(synth.getControls()["fx_slot_1_glitch_delay_tap_view"]->value() == 2.0f, "the menu sets Show Tap");
    }
    else {
      expect(false, "Show Tap is a menu");
    }

    synth.parameterChanged("fx_slot_1_glitch_delay_sync", 1.0f);
    synth.getControls()["fx_slot_1_glitch_delay_sync"]->set(1.0f);
    pumpUntil([&] { return has("Synced Time"); });
    expect(has("Tap 3 Synced Time") && has("Synced Buffer Size") && has("Tap 3 Stutter Minimum Synced Slice"),
           "Host Sync shows note values");
    expect(!has("Tap 3 Time") && !has("Tap 3 Stutter Minimum Slice"), "and hides the free times");

    auto* stutter = findTitled(editor, [](const String& t) { return t == "Tap 3 Stutter Probability"; });
    expect(stutter != nullptr && stutter->getDescription().contains("chance"), "rows describe what they do");
    return finish("editor");
  }

  int engineSuites() {
    struct Runner final : juce::UnitTestRunner {
      void logMessage(const juce::String& message) override { std::cout << message << '\n'; }
    } runner;
    runner.setAssertOnFailure(false);
    runner.runTestsInCategory("Glitch Delay");
    int passes = 0;
    for (int i = 0; i < runner.getNumResults(); ++i) {
      if (const auto* result = runner.getResult(i)) {
        failures += result->failures;
        passes += result->passes;
      }
    }
    std::cout << passes << " Astralay engine checks passed, " << failures << " failed.\n";
    return finish("engine");
  }
} // namespace

int main(int argc, char** argv) {
  juce::ScopedJuceInitialiser_GUI juce;
  const juce::String flag = argc > 1 ? juce::String(argv[1]) : juce::String();
  if (flag == "--dsp") return engineSuites();
  if (flag == "--parameters") return parameters();
  if (flag == "--module") return moduleTest();
  if (flag == "--reproducible") return reproducible();
  if (flag == "--chain") return chain();
  if (flag == "--state") return state();
  if (flag == "--editor") return editorTest();
  std::cerr << "Usage: AtlasGlitchDelayTests --dsp | --parameters | --module | --reproducible | --chain | --state | --editor\n";
  return 2;
}
