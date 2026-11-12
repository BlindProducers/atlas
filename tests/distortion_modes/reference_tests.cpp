// SPDX-License-Identifier: GPL-3.0-or-later
// Runs the original distortion JSFX through WDL's EEL2 next to the
// port of its modes in Atlas's Distortion effect and requires every output
// sample to be the same double, bit for bit, at several sample rates, in each
// ported mode, through Drive's glide, Mix and Level changes, crossfades
// between modes, the silence suspension and EEL2's flushing of tiny values.
// The script is not part of the repository: CMake's ATLAS_DISTORTION_JSFX
// names it, and the test takes its path as its argument.
#include "distortion_modes.h"
#include "ns-eel.h"
extern "C" {
void NSEEL_HOSTSTUB_EnterMutex() {}
void NSEEL_HOSTSTUB_LeaveMutex() {}
void* NSEEL_PProc_THIS(void* data, int data_size, struct _compileContext* ctx);
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

namespace dm = atlas::distortion_modes;
using dm::Engine;

namespace {
  // A minimal offline JSFX host: it compiles the script's @init, @slider,
  // @block and @sample sections, never a second copy of its DSP. It gives the
  // script the string literals and str_getchar its tables are decoded with,
  // and stubs for slider_show and slider(), which only the drawing of
  // controls and the X-Shaper modes use.
  constexpr double kStringBase = 100000;

  class Reference {
    public:
      // `start` sets slider values before @init runs.
      Reference(const std::string& path, double rate, const std::function<void(Reference&)>& start) :
          vm_(NSEEL_VM_alloc()) {
        NSEEL_VM_setramsize(vm_, 1024 * 1024);
        NSEEL_VM_SetCustomFuncThis(vm_, this);
        NSEEL_VM_SetStringFunc(vm_, &Reference::addString, &Reference::addNamedString);
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
        start(*this);
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

      // What this EEL2 build does where builds differ (dm::Eel2Build), asked
      // of the interpreter itself.
      dm::Eel2Build build() {
        set("probe_tiny", std::ldexp(1.5, -1022));
        set("probe_one", 1.0);
        compile("probe", "probe_flush = probe_tiny * probe_one;");
        run("probe");
        dm::Eel2Build result;
        // The smallest normal binade survives an assignment unless the build flushes it too.
        result.flush_exponent = get("probe_flush") == 0.0 ? 1 : 0;
        return result;
      }

      static void registerFunctions() {
        NSEEL_addfunc_retval("str_getchar", 2, NSEEL_PProc_THIS, &Reference::strGetChar);
        NSEEL_addfunc_retval("slider_show", 2, NSEEL_PProc_THIS, &Reference::sliderShow);
        NSEEL_addfunc_retval("slider", 1, NSEEL_PProc_THIS, &Reference::slider);
      }

    private:
      static EEL_F addString(void* opaque, eelStringSegmentRec* list) {
        auto* self = static_cast<Reference*>(opaque);
        std::string text(static_cast<size_t>(nseel_stringsegments_tobuf(nullptr, 0, list)), '\0');
        text.resize(static_cast<size_t>(nseel_stringsegments_tobuf(text.data(), static_cast<int>(text.size()), list)));
        self->strings_.push_back(std::move(text));
        return kStringBase + static_cast<double>(self->strings_.size() - 1);
      }
      static EEL_F addNamedString(void*, const char*) { return -1; }
      static EEL_F NSEEL_CGEN_CALL strGetChar(void* opaque, EEL_F* string, EEL_F* offset) {
        const auto& strings = static_cast<Reference*>(opaque)->strings_;
        const auto index = static_cast<long>(*string - kStringBase), at = static_cast<long>(*offset);
        if (index < 0 || index >= static_cast<long>(strings.size())) return 0;
        const std::string& text = strings[static_cast<size_t>(index)];
        return at >= 0 && at < static_cast<long>(text.size()) ? static_cast<unsigned char>(text[static_cast<size_t>(at)]) : 0;
      }
      static EEL_F NSEEL_CGEN_CALL sliderShow(void*, EEL_F*, EEL_F*) { return 0; }
      static EEL_F NSEEL_CGEN_CALL slider(void*, EEL_F*) { return 0; }

      NSEEL_VMCTX vm_;
      std::map<std::string, NSEEL_CODEHANDLE> code_;
      std::vector<std::string> strings_;
  };

  bool sameBits(double a, double b) {
    uint64_t x, y;
    std::memcpy(&x, &a, sizeof(x));
    std::memcpy(&y, &b, sizeof(y));
    return x == y;
  }

  // Unrelated left and right material: tones, an asymmetric pulse, a chirp,
  // an impulse on each side, loud and quiet passages, and from `silence_from`
  // for `silence_length` seconds, digital silence.
  struct Signal {
    double scale = 1.0;
    double silence_from = -1, silence_length = 0;
    std::pair<double, double> at(int i, double rate) const {
      const double t = i / rate;
      if (silence_from >= 0 && t >= silence_from && t < silence_from + silence_length) return { 0.0, 0.0 };
      const double swell = 0.02 + 0.98 * std::pow(0.5 + 0.5 * std::sin(2 * 3.14159265358979 * 1.3 * t), 3.0);
      const double pulse = std::fmod(t * 110.0, 1.0) < 0.2 ? 0.8 : -0.2;
      const double left = swell * (0.5 * std::sin(i * 0.0631) + 0.3 * pulse + 0.2 * std::sin(i * 0.4113 + 0.00007 * i * t));
      const double right = swell * (0.45 * std::sin(i * 0.0377 + 1.0) - 0.35 * std::cos(i * 0.2911) + 0.2 * std::sin(i * 2.3));
      return { scale * (i == 0 ? 0.9 : left), scale * (i == 211 ? -0.8 : right) };
    }
  };

  // One run: starting parameters and a list of (time in seconds, change) events.
  struct Run {
    std::string name;
    double seconds;
    Signal signal;
    std::vector<std::pair<double, std::function<void(Engine::Parameters&)>>> events;
  };

  int failures = 0;

  void bind(Reference& ref, const Engine::Parameters& p) {
    ref.set("mode", p.mode); ref.set("drive", p.drive); ref.set("mix", p.mix); ref.set("level", p.level);
  }

  void compare(const std::string& path, const Engine::Parameters& start, const Run& run) {
    const int failures_before = failures;
    for (double rate : { 44100.0, 48000.0, 96000.0, 192000.0 }) {
      Engine::Parameters p = start;
      Reference ref(path, rate, [&](Reference& r) { bind(r, p); });
      Engine core(ref.build(), false);
      core.setParameters(p);
      core.prepare(rate);
      if (Engine::kLatency != static_cast<int>(ref.get("pdc_delay")))
        throw std::runtime_error("the port's latency differs from pdc_delay");
      size_t next = 0;
      long differing = 0;
      const int total = static_cast<int>(run.seconds * rate);
      for (int i = 0; i < total; ++i) {
        bool changed = false;
        while (next < run.events.size() && i == static_cast<int>(run.events[next].first * rate)) {
          run.events[next++].second(p);
          changed = true;
        }
        if (changed) {
          bind(ref, p);
          ref.run("slider");
          core.setParameters(p);
        }
        if (i % 64 == 0) {
          ref.run("block");
          core.block(64);
        }
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
    }
    if (failures == failures_before)
      std::cout << run.name << ": every sample identical\n";
  }

  Engine::Parameters inMode(int mode, double drive = 25) {
    Engine::Parameters p;
    p.mode = mode;
    p.drive = drive;
    return p;
  }
}

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "usage: AtlasDistortionModesReferenceTests <path to the JSFX>\n";
    return 2;
  }
  NSEEL_init();
  Reference::registerFunctions();
  const std::string path = argv[1];
  try {
    {
      Reference probe(path, 48000, [](Reference&) {});
      std::cout << "EEL2 build: assignments flush exponents up to " << probe.build().flush_exponent << '\n';
    }

    const std::pair<int, const char*> modes[] = {
      { dm::kTube, "Tube" }, { dm::kDiode1, "Diode 1" }, { dm::kDiode2, "Diode 2" },
      { dm::kZeroSquare, "Zero-Square" }, { dm::kAsym, "Asym" },
      { dm::kStompBox, "Stomp Box" }, { dm::kTapeSat, "Tape Sat" }, { dm::kSoftSat, "Soft Sat" } };

    // Each mode through Drive's glide (every change glides over 20 ms of
    // 64-sample blocks), Drive 0, Mix and Level.
    for (const auto& mode : modes) {
      compare(path, inMode(mode.first), { mode.second, 0.9, {}, {
        { 0.1, [](Engine::Parameters& p) { p.drive = 0; } },
        { 0.2, [](Engine::Parameters& p) { p.drive = 10; p.level = 0.8; } },
        { 0.3, [](Engine::Parameters& p) { p.drive = 50; p.mix = 50; } },
        { 0.45, [](Engine::Parameters& p) { p.drive = 75.5; p.mix = 0; p.level = 1; } },
        { 0.55, [](Engine::Parameters& p) { p.drive = 100; p.mix = 100; p.level = 0.0003; } },
        { 0.65, [](Engine::Parameters& p) { p.drive = 90; p.mix = 33.3; p.level = 0.25; } },
        { 0.75, [](Engine::Parameters& p) { p.drive = 3.7; p.mix = 100; p.level = 0.5; } } } });
    }

    // Mode changes crossfade over 30 ms; one during a fade waits for it to end.
    compare(path, inMode(dm::kTube, 60), { "mode crossfades", 1.0, {}, {
      { 0.1, [](Engine::Parameters& p) { p.mode = dm::kDiode1; } },
      { 0.11, [](Engine::Parameters& p) { p.mode = dm::kDiode2; } },
      { 0.2, [](Engine::Parameters& p) { p.mode = dm::kZeroSquare; p.drive = 80; } },
      { 0.3, [](Engine::Parameters& p) { p.mode = dm::kAsym; } },
      { 0.4, [](Engine::Parameters& p) { p.mode = dm::kDiode1; p.mix = 70; } },
      { 0.5, [](Engine::Parameters& p) { p.mode = dm::kStompBox; p.drive = 20; } },
      { 0.6, [](Engine::Parameters& p) { p.mode = dm::kTapeSat; } },
      { 0.61, [](Engine::Parameters& p) { p.mode = dm::kSoftSat; } },
      { 0.62, [](Engine::Parameters& p) { p.mode = dm::kTapeSat; } },
      { 0.7, [](Engine::Parameters& p) { p.mode = dm::kSoftSat; p.drive = 100; } },
      { 0.8, [](Engine::Parameters& p) { p.mode = dm::kTube; p.drive = 0; } },
      { 0.9, [](Engine::Parameters& p) { p.mode = dm::kAsym; p.mix = 100; } } } });

    // A script started in the default mode, Tube, fades to the first one set.
    Engine::Parameters tube;
    compare(path, tube, { "fade from the default mode", 0.3, {}, {
      { 0.0, [](Engine::Parameters& p) { p.mode = dm::kStompBox; p.drive = 45; } } } });

    // 0.4 s of digital silence: at every rate the script suspends after 8192
    // samples and freezes its state until the signal returns.
    Signal gap;
    gap.silence_from = 0.3; gap.silence_length = 0.4;
    compare(path, inMode(dm::kZeroSquare, 60), { "silence", 1.2, gap, {
      { 0.25, [](Engine::Parameters& p) { p.mode = dm::kAsym; } } } });

    // Signals in the smallest normal binade and below, and a Drive so small
    // its curve values are, where EEL2 builds flush assignments.
    for (const int exponent : { -1021, -1060 }) {
      Signal tiny;
      tiny.scale = std::ldexp(1.0, exponent);
      compare(path, inMode(dm::kDiode2, 100), { "tiny values 2^" + std::to_string(exponent), 0.4, tiny, {
        { 0.05, [](Engine::Parameters& p) { p.mode = dm::kZeroSquare; } },
        { 0.1, [](Engine::Parameters& p) { p.mode = dm::kSoftSat; } },
        { 0.15, [](Engine::Parameters& p) { p.mode = dm::kStompBox; } },
        { 0.2, [](Engine::Parameters& p) { p.mode = dm::kTapeSat; p.drive = std::ldexp(1.0, -1015); } },
        { 0.25, [](Engine::Parameters& p) { p.mode = dm::kDiode2; p.mix = 60; } },
        { 0.3, [](Engine::Parameters& p) { p.mode = dm::kZeroSquare; } },
        { 0.35, [](Engine::Parameters& p) { p.mode = dm::kAsym; p.drive = 0; } } } });
    }

    // Loud input: only the output's hard clip at +/- 4 limits the dry path.
    Signal loud;
    loud.scale = 8.0;
    Engine::Parameters loud_start = inMode(dm::kDiode2, 85);
    loud_start.mix = 30; loud_start.level = 1;
    compare(path, loud_start, { "loud", 0.5, loud, {
      { 0.25, [](Engine::Parameters& p) { p.mix = 0; p.mode = dm::kDiode1; } } } });
  }
  catch (const std::exception& e) {
    std::cerr << "FAIL " << e.what() << '\n';
    return 1;
  }
  return failures == 0 ? 0 : 1;
}
