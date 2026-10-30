// SPDX-License-Identifier: GPL-3.0-or-later
// Runs the original Serum 2 Overdrive JSFX through WDL's EEL2 next to the
// Destroy port and requires every output sample to be the same double, bit
// for bit, at several sample rates and through changes of every slider, the
// stack, both filter positions, the silence suspension and EEL2's flushing of
// tiny values. The port runs without Atlas's DC blocker here, since that is
// not part of the script. The script is not part of the repository: CMake's
// ATLAS_DESTROY_JSFX names it, and the test takes its path as its argument.
#include "destroy.h"
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

using atlas::destroy::Destroy;

namespace {
  // A minimal offline JSFX host: it compiles the script's @init, @slider,
  // @block and @sample sections, never a second copy of its DSP.
  class Reference {
    public:
      Reference(const std::string& path, double rate) : vm_(NSEEL_VM_alloc()) {
        // The drawing code's slider table sits at 30000 and up.
        NSEEL_VM_setramsize(vm_, 1024 * 1024);
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

      // What this EEL2 build does where builds differ (atlas::destroy::Eel2Build),
      // asked of the interpreter itself.
      atlas::destroy::Eel2Build build() {
        set("probe_tiny", std::ldexp(1.5, -1022));
        set("probe_one", 1.0);
        compile("probe", "probe_flush = probe_tiny * probe_one;");
        run("probe");
        atlas::destroy::Eel2Build result;
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

  // Unrelated left and right material: tones, an asymmetric pulse, a chirp,
  // an impulse on each side, loud and quiet passages, and from `silence_from`
  // for `silence_length` seconds, digital silence. right_scale, when set,
  // scales the right side on its own.
  struct Signal {
    double scale = 1.0, right_scale = 0.0;
    double silence_from = -1, silence_length = 0;
    std::pair<double, double> at(int i, double rate) const {
      const double t = i / rate;
      if (silence_from >= 0 && t >= silence_from && t < silence_from + silence_length) return { 0.0, 0.0 };
      const double swell = 0.02 + 0.98 * std::pow(0.5 + 0.5 * std::sin(2 * 3.14159265358979 * 1.3 * t), 3.0);
      const double pulse = std::fmod(t * 110.0, 1.0) < 0.2 ? 0.8 : -0.2;
      const double left = swell * (0.5 * std::sin(i * 0.0631) + 0.3 * pulse + 0.2 * std::sin(i * 0.4113 + 0.00007 * i * t));
      const double right = swell * (0.45 * std::sin(i * 0.0377 + 1.0) - 0.35 * std::cos(i * 0.2911) + 0.2 * std::sin(i * 2.3));
      return { scale * (i == 0 ? 0.9 : left), (right_scale != 0.0 ? right_scale : scale) * (i == 211 ? -0.8 : right) };
    }
  };

  // One run: a list of (time in seconds, change to the parameters) events.
  struct Run {
    std::string name;
    double seconds;
    Signal signal;
    std::vector<std::pair<double, std::function<void()>>> events;
  };

  int failures = 0;

  void compare(const std::string& path, Destroy::Parameters& p, const Bindings& bindings, const Run& run) {
    const int failures_before = failures;
    for (double rate : { 44100.0, 48000.0, 96000.0, 192000.0 }) {
      const Destroy::Parameters start = p;
      Reference ref(path, rate);
      Destroy core(ref.build(), false);
      core.prepare(rate);
      auto update = [&] {
        for (const auto& binding : bindings) ref.set(binding.first, *binding.second);
        ref.run("slider");
        core.setParameters(p);
      };
      update();
      if (Destroy::kLatency != static_cast<int>(ref.get("pdc_delay")))
        throw std::runtime_error("Destroy's latency differs from pdc_delay");
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
        auto [left, right] = run.signal.at(i, rate);
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
    std::cerr << "usage: AtlasDestroyReferenceTests <path to Serum2Overdrive.jsfx>\n";
    return 2;
  }
  NSEEL_init();
  const std::string path = argv[1];
  try {
    {
      Reference probe(path, 48000);
      std::cout << "EEL2 build: assignments flush exponents up to " << probe.build().flush_exponent << '\n';
    }
    Destroy::Parameters p;
    const Bindings bindings = {
      { "drive", &p.drive }, { "stack", &p.stack }, { "filt", &p.filter }, { "freq", &p.frequency },
      { "fq", &p.q }, { "morph", &p.morph }, { "mix", &p.mix }, { "level", &p.level } };

    p = {};
    compare(path, p, bindings, { "drive, mix and level", 1.6, {}, {
      { 0.1, [&] { p.drive = 0; } },
      { 0.2, [&] { p.drive = 10; p.level = 0.8; } },
      { 0.3, [&] { p.drive = 50; p.mix = 50; } },
      { 0.4, [&] { p.drive = 75.5; p.mix = 0; p.level = 1; } },
      { 0.5, [&] { p.drive = 100; p.mix = 100; p.level = 0.0003; } },
      { 0.6, [&] { p.drive = 90; p.mix = 33.3; p.level = 0.25; } },
      { 0.8, [&] { p.drive = 40; p.mix = 100; p.level = 0.5; } },
      { 1.0, [&] { p.drive = 60; p.mix = 87.65; p.level = 0.71; } },
      { 1.3, [&] { p.drive = 25; p.mix = 12; p.level = 0.5; } } } });

    p = {};
    compare(path, p, bindings, { "stack", 1.4, {}, {
      { 0.1, [&] { p.stack = 2; p.drive = 50; } },
      { 0.2, [&] { p.stack = 3; } },
      { 0.3, [&] { p.stack = 4; p.drive = 25; } },
      { 0.45, [&] { p.stack = 8; } },
      { 0.6, [&] { p.stack = 16; p.drive = 100; } },
      { 0.8, [&] { p.stack = 16; p.drive = 15; p.filter = 1; p.frequency = 0.55; p.q = 1.0; } },
      { 1.0, [&] { p.stack = 5; p.drive = 33; p.filter = 0; } },
      { 1.2, [&] { p.stack = 1; } } } });

    p = {};
    p.drive = 0; p.filter = 1;
    compare(path, p, bindings, { "filter", 2.0, {}, {
      { 0.1, [&] { p.frequency = 0.2; p.q = 0.5; } },
      { 0.2, [&] { p.frequency = 0.7; p.q = 0.05; } },
      { 0.3, [&] { p.frequency = 0.9; p.q = 2; p.morph = 25; } },
      { 0.4, [&] { p.frequency = 1; p.q = 5; p.morph = 50; } },
      { 0.5, [&] { p.frequency = 0.8; p.q = 7; p.morph = 75; p.drive = 50; } },
      { 0.6, [&] { p.frequency = 0.3; p.q = 7.6; p.morph = 100; } },
      { 0.7, [&] { p.filter = 2; p.frequency = 0.5; p.q = 1.9; p.morph = 0; p.drive = 25; } },
      { 0.9, [&] { p.frequency = 0.961988; p.q = 0.075; p.morph = 87.239587; p.stack = 8; p.drive = 13.011695; } },
      { 1.1, [&] { p.filter = 1; p.mix = 22.807018; p.level = 0.435673; } },
      { 1.3, [&] { p.frequency = 0; p.q = 3.3; p.morph = 61; p.filter = 2; p.stack = 2; } },
      { 1.5, [&] { p.frequency = 0.45; p.q = 6.5; p.morph = 12.5; p.mix = 100; } },
      { 1.7, [&] { p.filter = 0; } } } });

    // 0.4 s of digital silence: at every rate the script suspends after 8192
    // samples and freezes its state until the signal returns.
    p = {};
    p.drive = 70; p.stack = 3; p.filter = 2; p.frequency = 0.6; p.mix = 80;
    Signal gap;
    gap.silence_from = 0.3; gap.silence_length = 0.4;
    compare(path, p, bindings, { "silence", 1.2, gap, {} });

    // Signals in the smallest normal binade and below, where EEL2 builds flush assignments.
    p = {};
    p.drive = 100; p.stack = 4; p.filter = 1; p.frequency = 0.4; p.q = 4; p.morph = 40;
    Signal tiny;
    tiny.scale = std::ldexp(1.0, -1021);
    compare(path, p, bindings, { "tiny values", 0.3, tiny, {
      { 0.15, [&] { p.filter = 2; p.drive = 0; p.mix = 60; } } } });
    Signal denormal;
    denormal.scale = std::ldexp(1.0, -1060);
    compare(path, p, bindings, { "denormal values", 0.2, denormal, {} });
    // A loud left side and a tiny right one: the shared gain control pulls the
    // right side into denormals between the stages.
    p = {};
    p.drive = 100; p.stack = 3;
    Signal one_side;
    one_side.right_scale = std::ldexp(1.0, -1020);
    compare(path, p, bindings, { "one tiny side", 0.4, one_side, {
      { 0.2, [&] { p.filter = 2; p.frequency = 0.3; p.stack = 1; } } } });

    // Loud input: only the output's hard clip at +/- 4 limits the dry path.
    p = {};
    p.drive = 85; p.mix = 30; p.level = 1;
    Signal loud;
    loud.scale = 8.0;
    compare(path, p, bindings, { "loud", 0.5, loud, {
      { 0.25, [&] { p.mix = 0; p.stack = 6; } } } });
  }
  catch (const std::exception& e) {
    std::cerr << "FAIL " << e.what() << '\n';
    return 1;
  }
  return failures == 0 ? 0 : 1;
}
