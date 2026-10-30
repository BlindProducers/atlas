// SPDX-License-Identifier: GPL-3.0-or-later
// Runs the original Serum 2 Bode JSFX through WDL's EEL2 next to the
// Frequency Shifter's Mode 2 and requires every output sample to be the same
// double, bit for bit, at several sample rates and through parameter changes
// that reach every slider, the feedback line, the blur and EEL2's flushing of
// tiny values. The script is not part of the repository: CMake's
// ATLAS_BODE_JSFX names it, and the test takes its path as its argument.
#include "bode_shifter.h"
#include "ns-eel.h"
extern "C" {
void NSEEL_HOSTSTUB_EnterMutex() {}
void NSEEL_HOSTSTUB_LeaveMutex() {}
}

#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using atlas::bode::Shifter;

namespace {
  // A minimal offline JSFX host: it compiles the script's @init, @slider,
  // @block and @sample sections, never a second copy of its DSP.
  class Reference {
    public:
      Reference(const std::string& path, double rate) : vm_(NSEEL_VM_alloc()) {
        // FB starts at 40000 and holds 5 s.
        NSEEL_VM_setramsize(vm_, 16 * 1024 * 1024);
        std::ifstream file(path);
        if (!file) throw std::runtime_error("Cannot open " + path);
        std::map<std::string, std::string> source;
        std::string line, section;
        while (std::getline(file, line)) {
          if (!line.empty() && line.back() == '\r') line.pop_back();
          // sliderN:name=default<...>
          if (section.empty() && line.rfind("slider", 0) == 0) {
            const auto colon = line.find(':'), equals = line.find('=');
            if (colon != std::string::npos && equals != std::string::npos && equals > colon)
              set(line.substr(colon + 1, equals - colon - 1), std::stod(line.substr(equals + 1)));
          }
          if (!line.empty() && line[0] == '@') { section = line.substr(1, line.find(' ') - 1); continue; }
          if (!section.empty()) source[section] += line + "\n";
        }
        set("srate", rate); set("samplesblock", 64);
        for (const auto* name : { "init", "slider", "block", "sample" }) {
          if (source[name].empty()) continue;
          compile(name, source[name], std::string(name) == "init");
        }
        run("init");
      }
      ~Reference() {
        for (auto& entry : code_) NSEEL_code_free(entry.second);
        NSEEL_VM_free(vm_);
      }
      void compile(const std::string& name, const std::string& code, bool common = false) {
        const auto handle = NSEEL_code_compile_ex(vm_, code.c_str(), 0, common ? NSEEL_CODE_COMPILE_FLAG_COMMONFUNCS : 0);
        if (!handle) throw std::runtime_error("@" + name + ": " + NSEEL_code_getcodeerror(vm_));
        code_[name] = handle;
      }
      void set(const std::string& name, double value) { *NSEEL_VM_regvar(vm_, name.c_str()) = value; }
      double get(const char* name) { return *NSEEL_VM_regvar(vm_, name); }
      void run(const char* section) { if (code_.count(section)) NSEEL_code_execute(code_[section]); }
      std::pair<double, double> sample(double left, double right) {
        set("spl0", left); set("spl1", right);
        run("sample");
        return { get("spl0"), get("spl1") };
      }

      // What this EEL2 build does where builds differ (atlas::bode::Eel2Build),
      // asked of the interpreter itself.
      atlas::bode::Eel2Build build() {
        set("probe_big", 3000000000.0);
        set("probe_tiny", std::ldexp(1.5, -1022));
        set("probe_one", 1.0);
        compile("probe", "probe_mod = probe_big % 2147483648; probe_flush = probe_tiny * probe_one;");
        run("probe");
        atlas::bode::Eel2Build result;
        const double mod = get("probe_mod");
        if (mod == 852516352.0) result.mod_2_31 = 2147483648LL;
        else if (mod == 852516353.0) result.mod_2_31 = 2147483647LL;
        else throw std::runtime_error("Unexpected EEL2 % result " + std::to_string(mod));
        // The smallest normal binade survives an assignment unless the build flushes it too.
        result.flush_exponent = get("probe_flush") == 0.0 ? 1 : 0;
        return result;
      }

    private:
      NSEEL_VMCTX vm_;
      std::map<std::string, NSEEL_CODEHANDLE> code_;
  };

  using Bindings = std::vector<std::pair<const char*, double*>>;

  bool sameBits(double a, double b) {
    uint64_t x, y;
    std::memcpy(&x, &a, sizeof(x));
    std::memcpy(&y, &b, sizeof(y));
    return x == y;
  }

  // Unrelated left and right material: tones, a sweep-like chirp, an impulse
  // on each side and loud and quiet passages.
  std::pair<double, double> signal(int i, double rate, double scale) {
    const double t = i / rate;
    const double swell = 0.02 + 0.98 * std::pow(0.5 + 0.5 * std::sin(2 * 3.14159265358979 * 1.7 * t), 3.0);
    const double left = swell * (0.6 * std::sin(i * 0.0731) + 0.3 * std::sin(i * 0.5113 + 0.0001 * i * t) +
                                 0.1 * std::sin(i * 1.9));
    const double right = swell * (0.5 * std::sin(i * 0.0573 + 1.0) - 0.4 * std::cos(i * 0.2911) + 0.1 * std::sin(i * 2.6));
    return { scale * (i == 0 ? 0.9 : left), scale * (i == 177 ? -0.8 : right) };
  }

  // One run: a list of (time in seconds, change to the parameters) events.
  struct Run {
    std::string name;
    double seconds;
    double scale = 1.0;
    std::vector<std::pair<double, std::function<void()>>> events;
  };

  int failures = 0;

  void compare(const std::string& path, Shifter::Parameters& p, const Bindings& bindings, const Run& run) {
    const int failures_before = failures;
    for (double rate : { 44100.0, 48000.0, 96000.0, 192000.0 }) {
      const Shifter::Parameters start = p;
      Reference ref(path, rate);
      Shifter core(ref.build());
      core.prepare(rate);
      auto update = [&] {
        for (const auto& binding : bindings) ref.set(binding.first, *binding.second);
        ref.run("slider");
        core.setParameters(p);
      };
      update();
      if (Shifter::kLatency != static_cast<int>(ref.get("pdc_delay")))
        throw std::runtime_error("Mode 2 latency differs from pdc_delay");
      size_t next = 0;
      long differing = 0;
      const int total = static_cast<int>(run.seconds * rate);
      for (int i = 0; i < total; ++i) {
        bool changed = false;
        while (next < run.events.size() && i == static_cast<int>(run.events[next].first * rate)) {
          run.events[next++].second();
          changed = true;
        }
        if (changed) update();
        if (i % 64 == 0) ref.run("block");
        auto [left, right] = signal(i, rate, run.scale);
        const auto expected = ref.sample(left, right);
        core.process(left, right);
        if (!sameBits(left, expected.first) || !sameBits(right, expected.second)) {
          if (differing++ == 0)
            std::cerr << "FAIL " << run.name << " at " << rate << " Hz, sample " << i << ": native "
                      << left << ", " << right << ", JSFX " << expected.first << ", " << expected.second << '\n';
        }
      }
      if (differing != 0) {
        std::cerr << "FAIL " << run.name << " at " << rate << " Hz: " << differing << " samples differ\n";
        ++failures;
      }
      p = start;
    }
    if (failures == failures_before)
      std::cout << run.name << ": every sample identical\n";
  }
}

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "usage: AtlasBodeReferenceTests <path to Serum2Bode.jsfx>\n";
    return 2;
  }
  NSEEL_init();
  const std::string path = argv[1];
  try {
    {
      Reference probe(path, 48000);
      const auto build = probe.build();
      std::cout << "EEL2 build: % divides by " << build.mod_2_31 << ", assignments flush exponents up to "
                << build.flush_exponent << '\n';
    }
    Shifter::Parameters p;
    const Bindings bindings = {
      { "shift", &p.shift }, { "range", &p.range }, { "dir", &p.direction }, { "width", &p.width },
      { "monoin", &p.mono_input }, { "dtime", &p.delay }, { "feed", &p.feedback }, { "blur", &p.blur },
      { "mix", &p.mix } };

    p = {};
    compare(path, p, bindings, { "shift and stereo", 1.2, 1.0, {
      { 0.1, [&] { p.shift = 40; } },
      { 0.2, [&] { p.shift = -75; p.range = 300; p.direction = 35; } },
      { 0.3, [&] { p.shift = 100; p.range = 5000; p.direction = -60; p.width = 30; } },
      { 0.4, [&] { p.shift = -100; p.range = 2400; p.direction = 100; p.width = 0; p.mono_input = 1; } },
      { 0.5, [&] { p.shift = 12.5; p.range = 0.1; p.direction = -100; p.width = 100; } },
      { 0.6, [&] { p.shift = 0.0001; p.range = 0.1; p.mono_input = 0; p.mix = 0; } },
      { 0.7, [&] { p.shift = 63; p.range = 1000; p.direction = 50; p.width = 70; p.mix = 25; } },
      { 0.8, [&] { p.shift = -33; p.range = 33; p.direction = -25; p.mix = 50; } },
      { 0.9, [&] { p.shift = 100; p.range = 4900; p.mix = 83.3; } },
      { 1.0, [&] { p.shift = -8; p.range = 22.36; p.direction = 0; p.width = 100; p.mix = 100; } } } });

    p = {};
    p.shift = 45; p.feedback = 50; p.delay = 0.1;
    compare(path, p, bindings, { "feedback", 1.4, 1.0, {
      { 0.2, [&] { p.delay = 0; p.feedback = 80; } },
      { 0.35, [&] { p.delay = 0.001; p.shift = -45; p.range = 500; } },
      { 0.5, [&] { p.delay = 0.3; p.feedback = 100; p.shift = 0; } },
      { 0.75, [&] { p.delay = 0.0207; p.feedback = 60; p.direction = 80; p.mono_input = 1; p.mix = 40; } },
      { 0.9, [&] { p.delay = 5; p.feedback = 99; p.shift = 10; p.width = 40; } },
      { 1.1, [&] { p.delay = 0.25; p.feedback = 25; p.direction = -40; } } } });

    // Blur from the first sample: at 96 and 192 kHz its right taps reach the
    // slider table the script keeps inside BLUR_R before @sample overwrites it.
    p = {};
    p.shift = -20; p.blur = 100;
    compare(path, p, bindings, { "blur", 1.0, 1.0, {
      { 0.3, [&] { p.blur = 30; p.feedback = 70; p.delay = 0.05; } },
      { 0.5, [&] { p.blur = 0.04; } },
      { 0.6, [&] { p.blur = 0.06; p.shift = 80; p.range = 100; } },
      { 0.75, [&] { p.blur = 72; p.direction = -70; p.width = 55; p.mix = 61; } },
      { 0.9, [&] { p.blur = 100; p.feedback = 0; } } } });

    // Signals in the smallest normal binade and below, where EEL2 builds flush assignments.
    p = {};
    p.shift = 30; p.feedback = 90; p.delay = 0.02; p.blur = 20; p.mix = 35;
    compare(path, p, bindings, { "tiny values", 0.5, std::ldexp(1.0, -1021), {
      { 0.25, [&] { p.mono_input = 1; p.shift = -100; p.range = 5000; } } } });
    compare(path, p, bindings, { "denormal values", 0.3, std::ldexp(1.0, -1060), {} });

    // Loud input: nothing in the script clips.
    p = {};
    p.shift = 70; p.range = 150; p.feedback = 95; p.delay = 0.01; p.mix = 12;
    compare(path, p, bindings, { "loud", 0.5, 8.0, {} });
  }
  catch (const std::exception& e) {
    std::cerr << "FAIL " << e.what() << '\n';
    return 1;
  }
  return failures == 0 ? 0 : 1;
}
