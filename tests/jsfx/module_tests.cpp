#include "JuceHeader.h"
#include "dimension_expander_module.h"
#include "frequency_shifter_module.h"
#include "jsfx_effect_modules.h"
#include "reorderable_effect_chain.h"

#include <atomic>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <new>

namespace {
  std::atomic<bool> tracking { false };
  std::atomic<int> allocations { 0 };
}
void* operator new(std::size_t size) {
  if (tracking) ++allocations;
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
  void expect(bool okay, const char* message) {
    if (!okay) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
  }

  template<class Core>
  void compareModule(vital::SynthModule& module, Core& core, int block, int factor = 1, double wet = 1) {
    std::array<vital::poly_float, vital::kMaxBufferSize> input;
    vital::jsfx::HostRate adapter;
    adapter.prepare(factor);
    double worst = 0;
    for (int offset = 0; offset < 24000;) {
      const int count = std::min(block, 24000 - offset);
      for (int i = 0; i < count; ++i) {
        const float x = std::sin((offset + i) * 0.137);
        input[i] = vital::poly_float(x * 0.4f, x * -0.11f, x * -0.25f, x * 0.38f);
      }
      allocations = 0;
      tracking = true;
      module.processWithInput(input.data(), count);
      tracking = false;
      expect(allocations == 0, "JSFX module processing must not allocate, including its first block");
      for (int i = 0; i < count; ++i) {
        auto expected = adapter.process({ input[i][0], input[i][1], input[i][2], input[i][3] }, core);
        for (int lane = 0; lane < 4; ++lane) {
          expected[lane] = (1 - wet) * input[i][lane] + wet * expected[lane];
          const float actual = module.output()->buffer[i][lane];
          if (!std::isfinite(actual)) worst = INFINITY;
          worst = std::max(worst, std::abs(actual - expected[lane]));
        }
      }
      offset += count;
    }
    expect(worst < 1.0e-6, "module parameter mapping, all SIMD lanes, and block boundaries must match the DSP core");
    if (worst >= 1.0e-6) std::cerr << "  maximum error: " << worst << '\n';
  }

  void testDimensionDelayMilliseconds() {
    for (int rate : { 44100, 48000, 96000 }) {
      for (int oversample : { 1, 2, 4, 8 }) {
        vital::DimensionExpanderModule dimension;
        dimension.init();
        dimension.setSampleRate(rate);
        dimension.setOversampleAmount(oversample);
        const auto controls = dimension.getControls();
        controls.at("dimension_expander_amount")->set(1.0f);
        for (float milliseconds : { 0.0f, 0.5f, 5.0f, 20.0f, 40.0f }) {
          controls.at("dimension_expander_delay")->set(milliseconds);
          dimension.hardReset();
          std::array<vital::poly_float, 128> block;
          block.fill(0.0f);
          dimension.processWithInput(block.data(), 128); // Settle the amount ramp.
          const double delay_samples = milliseconds * 0.001 * dimension.getSampleRate();
          const int last = static_cast<int>(std::ceil(delay_samples)) + 128;
          int first_delayed_sample = -1;
          for (int offset = 0; offset < last; offset += 128) {
            block.fill(0.0f);
            if (offset == 0) block[0] = 0.5f;
            allocations = 0;
            tracking = true;
            dimension.processWithInput(block.data(), 128);
            tracking = false;
            expect(allocations == 0, "Mode 1 delay processing must not allocate");
            for (int i = 0; i < 128; ++i) {
              const auto sample = dimension.output()->buffer[i];
              if (first_delayed_sample < 0 && std::abs(sample[1] - sample[0]) > 1.0e-5f)
                first_delayed_sample = offset + i;
              expect(std::isfinite(sample[0]) && std::isfinite(sample[1]), "Mode 1 delay output must stay finite");
            }
          }
          expect(std::abs(first_delayed_sample - delay_samples) <= 1.01,
                 "Mode 1 delay must match milliseconds across sample rates and oversampling");
        }
      }
    }
  }

  void testModuleMapping() {
    vital::Value tempo(2.0f);
    for (int oversampling : { 1, 2, 4, 8 }) {
      for (int block : { 1, 127, vital::kMaxBufferSize }) {
        vital::DimensionExpanderModule dimension;
        dimension.init(); dimension.setSampleRate(48000); dimension.setOversampleAmount(oversampling);
        auto dc = dimension.getControls();
        expect(dc.at("dimension_expander_mode")->value() == 0, "existing presets must default to Dimension Mode 1");
        dc.at("dimension_expander_mode")->set(1);
        dc.at("dimension_expander_quorax_low_cut")->set(400);
        dc.at("dimension_expander_quorax_high_cut")->set(4000);
        dc.at("dimension_expander_quorax_slope")->set(3);
        vital::jsfx::Quorax q;
        q.prepare(48000 * oversampling);
        vital::jsfx::Quorax::Parameters qp;
        qp.motion = static_cast<float>(0.4); qp.low_cut = 400; qp.high_cut = 4000; qp.slope = 3;
        q.setParameters(qp);
        compareModule(dimension, q, block);

        vital::ErosionModule erosion;
        erosion.init(); erosion.setSampleRate(48000); erosion.setOversampleAmount(oversampling);
        erosion.getControls().at("erosion_mode")->set(1);
        vital::jsfx::Erosion e;
        e.prepare(48000);
        vital::jsfx::Erosion::Parameters ep; ep.mode = 1;
        e.setParameters(ep);
        compareModule(erosion, e, block, oversampling);

        vital::FloatyDelayModule floaty(tempo.output());
        floaty.init(); floaty.setSampleRate(48000); floaty.setOversampleAmount(oversampling);
        floaty.getControls().at("floaty_delay_time")->set(17);
        vital::jsfx::Floaty f;
        f.prepare(48000);
        vital::jsfx::Floaty::Parameters fp; fp.delay = 17; fp.warp_rate = static_cast<float>(0.1); fp.mix = 100;
        f.setParameters(fp, 120);
        compareModule(floaty, f, block, oversampling, 0.4);
      }
    }
  }

  void testMemoryAndBoundaries() {
    vital::jsfx::Memory memory;
    memory.prepare(4097);
    expect(memory.read(0) == 0 && memory.read(4096) == 0, "unwritten delay memory must be silent");
    for (size_t i : { 0u, 63u, 64u, 1023u, 1024u, 4096u }) memory.write(i, 0.25);
    expect(memory.read(1024) == 0.25 && memory.read(1025) == 0, "delay validity must not leak into neighboring samples");
    memory.clear(); memory.write(1025, 0.5);
    expect(memory.read(1024) == 0 && memory.read(1025) == 0.5 && memory.read(4096) == 0,
           "constant-time reset must clear all delay history");

    vital::jsfx::Floaty floaty;
    floaty.prepare(48000);
    vital::jsfx::Floaty::Parameters p; p.delay = 0; p.playback = -2; p.warp = 4;
    floaty.setParameters(p, 120);
    for (int i = 0; i < 8192; ++i) {
      const auto out = floaty.process({ 0.4, -0.2, 0, 0 });
      expect(std::isfinite(out[0]) && std::isfinite(out[1]) && out[2] == 0 && out[3] == 0,
             "zero delay / reverse playback must be finite and stereo pairs must remain isolated");
    }
    p.delay = 100; floaty.setParameters(p, 120);
    p.delay = 1; floaty.setParameters(p, 120);
    for (int i = 0; i < 8192; ++i) {
      const auto out = floaty.process({ 0.2, 0.1, 0, 0 });
      expect(out[2] == 0 && out[3] == 0, "delay resizing must never expose another SIMD pair's history");
    }
    // Maximum tempo-sync duration must not allocate or address beyond capacity.
    p.sync = 64; p.playback = 2;
    tracking = true; allocations = 0;
    floaty.setParameters(p, 24);
    for (int i = 0; i < 1024; ++i) floaty.process({ 0, 0, 0, 0 });
    floaty.setParameters(p, 0.000001);
    for (int i = 0; i < 1024; ++i) floaty.process({ 0, 0, 0, 0 });
    floaty.reset();
    tracking = false;
    expect(allocations == 0, "Floaty delay changes and resets must not allocate");
  }

  void testHostRateConversion() {
    struct Identity {
      int calls = 0;
      vital::jsfx::Frame process(const vital::jsfx::Frame& x) { ++calls; return x; }
    };
    constexpr double pi = 3.14159265358979323846;
    for (int factor : { 2, 4, 8 }) {
      for (double frequency : { 0.02, 0.125, 0.4, 0.62 }) {
        vital::jsfx::HostRate adapter;
        adapter.prepare(factor);
        Identity identity;
        double error = 0;
        const int count = 2000 * factor;
        for (int i = 0; i < count; ++i) {
          const double x = std::sin(2 * pi * frequency * i / factor);
          const auto out = adapter.process({ x, -x, x * 0.5, 0 }, identity);
          if (i > 200 * factor) {
            const double expected = frequency > 0.5 ? 0 :
                std::sin(2 * pi * frequency * (i - 64 * factor) / factor);
            error = std::max(error, std::abs(out[0] - expected));
            expect(std::abs(out[1] + out[0]) < 1.0e-12 && out[3] == 0,
                   "host-rate conversion must preserve lane isolation and polarity");
          }
        }
        expect(identity.calls == 2000, "DSP must run once per host sample regardless of Atlas oversampling");
        expect(error < 0.0005, "rate conversion must preserve the passband and reject above-Nyquist input");
        if (error >= 0.0005) std::cerr << "  factor=" << factor << " frequency=" << frequency << " error=" << error << '\n';
      }
    }

    vital::Value tempo(2.0f);
    for (int factor : { 1, 2, 4, 8 }) {
      vital::ErosionModule erosion;
      erosion.init(); erosion.setSampleRate(48000); erosion.setOversampleAmount(factor);
      erosion.getControls().at("erosion_amount")->set(0);
      vital::FloatyDelayModule floaty(tempo.output());
      floaty.init(); floaty.setSampleRate(48000); floaty.setOversampleAmount(factor);
      auto c = floaty.getControls();
      c.at("floaty_delay_time")->set(10); c.at("floaty_delay_mix")->set(100);
      c.at("floaty_delay_cutoff")->set(0); c.at("floaty_delay_feedback")->set(0);
      c.at("floaty_delay_stereo_offset")->set(100);
      int erosion_peak = 0, floaty_peak = 0;
      double erosion_max = 0, floaty_max = 0;
      // Odd block boundaries exercise conversion phases across callbacks.
      std::array<vital::poly_float, 127> input;
      for (int offset = 0; offset < 600 * factor; offset += 127) {
        const int count = std::min(127, 600 * factor - offset);
        input.fill(0); if (offset == 0) input[0] = 1;
        erosion.processWithInput(input.data(), count); floaty.processWithInput(input.data(), count);
        for (int i = 0; i < count; ++i) {
          const double e = std::abs(erosion.output()->buffer[i][0]);
          const double f = std::abs(floaty.output()->buffer[i][0]);
          if (e > erosion_max) { erosion_max = e; erosion_peak = offset + i; }
          if (f > floaty_max) { floaty_max = f; floaty_peak = offset + i; }
        }
      }
      const int conversion_delay = factor > 1 ? 64 : 0;
      expect(erosion_peak == (64 + conversion_delay) * factor,
             "Erosion center delay must remain 64 host samples plus documented conversion latency");
      expect(floaty_peak == (480 - 3 + conversion_delay) * factor,
             "Floaty delay must retain the original cursor timing at all oversampling settings");
      c.at("floaty_delay_mix")->set(0);
      input.fill(vital::poly_float(0.2f, -0.3f, 0.4f, -0.5f));
      floaty.processWithInput(input.data(), 127);
      for (int i = 0; i < 127; ++i)
        expect(floaty.output()->buffer[i][0] == input[i][0] && floaty.output()->buffer[i][1] == input[i][1],
               "Floaty zero mix must be immediate, transparent dry audio at every oversampling setting");
    }
  }

  void testSlotBypassAndModeSwitch() {
    vital::Value tempo(2.0f), keytrack(60.0f);
    vital::ReorderableEffectChain chain(tempo.output(), keytrack.output(), "bus_2_");
    chain.init();
    const auto c = chain.getControls();
    const std::string prefix = "bus_2_fx_slot_20_";
    std::array<vital::poly_float, 128> input; input.fill(0.4f);
    for (int type : { vital::constants::kDimensionExpander, vital::constants::kErosion, vital::constants::kFloatyDelay }) {
      c.at(prefix + "type")->set(type + 1); c.at(prefix + "enabled")->set(1);
      c.at(prefix + "dimension_expander_mode")->set(1);
      for (int i = 0; i < 20; ++i) chain.processWithInput(input.data(), 128);
      c.at(prefix + "enabled")->set(0);
      chain.processWithInput(input.data(), 128);
      expect(chain.output()->buffer[0][0] == 0.4f, "bypassed JSFX slot must pass input through");
      input.fill(0.0f); c.at(prefix + "enabled")->set(1);
      for (int i = 0; i < 20; ++i) {
        chain.processWithInput(input.data(), 128);
        expect(chain.output()->buffer[127][0] == 0, "reenabling JSFX slots must not resurrect stale tails");
      }
      input.fill(0.4f);
    }
    c.at(prefix + "type")->set(vital::constants::kDimensionExpander + 1);
    c.at(prefix + "dimension_expander_mode")->set(0);
    c.at(prefix + "dimension_expander_amount")->set(0);
    chain.processWithInput(input.data(), 128);
    expect(chain.output()->buffer[0][0] == 0.4f, "returning to Mode 1 must restore its independent amount setting");
  }
}

namespace {
  // The Frequency Shifter's Mode 2 is the Serum 2 Bode port (tests/bode checks
  // the port against the script): the module hands it each block's controls
  // unchanged and plays its left and right on both lane pairs, allocating nothing.
  void testFrequencyShifterMode2() {
    for (int oversampling : { 1, 2 }) {
      for (int block : { 1, 127, vital::kMaxBufferSize }) {
        vital::FrequencyShifterModule shifter;
        shifter.init(); shifter.setSampleRate(48000); shifter.setOversampleAmount(oversampling);
        auto c = shifter.getControls();
        expect(c.at("frequency_shifter_mode")->value() == 0, "existing presets must default to Frequency Shifter Mode 1");
        c.at("frequency_shifter_mode")->set(1);
        c.at("frequency_shifter_bode_shift")->set(-35);
        c.at("frequency_shifter_bode_range")->set(8);
        c.at("frequency_shifter_bode_direction")->set(20);
        c.at("frequency_shifter_bode_width")->set(60);
        c.at("frequency_shifter_bode_delay")->set(0.125f);
        c.at("frequency_shifter_bode_feedback")->set(40);
        c.at("frequency_shifter_bode_blur")->set(25);
        c.at("frequency_shifter_bode_mix")->set(70);

        std::array<vital::poly_float, vital::kMaxBufferSize> input;
        auto fill = [&input](int offset, int count) {
          for (int i = 0; i < count; ++i) {
            const int n = offset + i;
            const float left = 0.5f * std::sin(0.031f * n), right = 0.4f * std::sin(0.0173f * n + 1.0f);
            input[i] = vital::poly_float(left, right, left, right);
          }
        };
        fill(0, block);
        shifter.processWithInput(input.data(), block);
        bool through = true;
        for (int i = 0; i < block; ++i)
          through = through && vital::utils::equal(shifter.output()->buffer[i], input[i]);
        expect(through, "Mode 2 must pass audio through until its buffers are prepared");

        shifter.prepareMode2();
        atlas::bode::Shifter core;
        core.prepare(48000.0 * oversampling);
        atlas::bode::Shifter::Parameters p;
        p.shift = -35; p.range = 256; p.direction = 20; p.width = 60; p.delay = 0.125; p.feedback = 40; p.blur = 25; p.mix = 70;
        core.setParameters(p);
        bool same = true;
        tracking = true; allocations = 0;
        for (int offset = 0; offset < 24000; offset += block) {
          const int count = std::min(block, 24000 - offset);
          fill(offset, count);
          shifter.processWithInput(input.data(), count);
          for (int i = 0; i < count; ++i) {
            double left = input[i][0], right = input[i][1];
            core.process(left, right);
            const vital::poly_float expected { float(left), float(right), float(left), float(right) };
            same = same && std::memcmp(&shifter.output()->buffer[i], &expected, sizeof(expected)) == 0;
          }
        }
        tracking = false;
        expect(same, "Mode 2 must be the Bode port, on both lane pairs, at every block size and oversampling");
        expect(allocations == 0, "Mode 2 must not allocate while it plays");
      }
    }
  }

  // In a slot, Mode 2 is prepared when chosen, reports the script's latency
  // and goes back to Mode 1 with Mode 1's settings.
  void testFrequencyShifterMode2Slot() {
    vital::Value tempo(2.0f), keytrack(60.0f);
    vital::ReorderableEffectChain chain(tempo.output(), keytrack.output(), "bus_2_");
    chain.init();
    const auto c = chain.getControls();
    const std::string prefix = "bus_2_fx_slot_20_";
    std::array<vital::poly_float, 128> input; input.fill(0.4f);
    c.at(prefix + "type")->set(vital::constants::kFrequencyShifter + 1); c.at(prefix + "enabled")->set(1);
    c.at(prefix + "frequency_shifter_dry_wet")->set(0.25f);
    chain.processWithInput(input.data(), 128);
    expect(chain.getLatencySamples() == 0, "Frequency Shifter Mode 1 must have no latency");

    c.at(prefix + "frequency_shifter_mode")->set(1);
    c.at(prefix + "frequency_shifter_bode_shift")->set(50);
    c.at(prefix + "frequency_shifter_bode_range")->set(8);
    bool moving = false, finite = true;
    float previous = 0.0f;
    for (int block = 0; block < 40; ++block) {
      chain.processWithInput(input.data(), 128);
      for (int i = 0; i < 128; ++i) {
        const float value = chain.output()->buffer[i][0];
        finite = finite && std::isfinite(value);
        moving = moving || (block > 2 && std::abs(value - previous) > 1.0e-4f);
        previous = value;
      }
    }
    expect(chain.getLatencySamples() == atlas::bode::Shifter::kLatency, "Mode 2 must report the script's latency");
    expect(finite && moving, "Mode 2 must shift the input once a slot chooses it");

    c.at(prefix + "frequency_shifter_mode")->set(0);
    for (int block = 0; block < 4; ++block) chain.processWithInput(input.data(), 128);
    expect(chain.getLatencySamples() == 0, "returning to Mode 1 must remove Mode 2's latency");
    expect(c.at(prefix + "frequency_shifter_dry_wet")->value() == 0.25f, "Mode 1 must keep its own settings");
  }
}

int main() {
  ScopedJuceInitialiser_GUI juce;
  testFrequencyShifterMode2(); testFrequencyShifterMode2Slot();
  testDimensionDelayMilliseconds();
  testModuleMapping(); testMemoryAndBoundaries(); testHostRateConversion(); testSlotBypassAndModeSwitch();
  if (failures) return 1;
  std::cout << "Atlas JSFX module tests passed (mapping, rates, blocks, memory, allocation, modes, bus slots)\n";
}
