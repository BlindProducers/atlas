// SPDX-License-Identifier: GPL-3.0-or-later
// Runs the original Saturator, Overdrive, Dynamic Tube and Multiband Dynamics
// JSFX (the files in this folder) through WDL's EEL2 next to the native ports
// and compares every output sample, at several sample rates and through
// parameter changes that reach every curve, mode and switch.
#include "drive_dynamics.h"
#include "ns-eel.h"
extern "C" {
void NSEEL_HOSTSTUB_EnterMutex() {}
void NSEEL_HOSTSTUB_LeaveMutex() {}
}

#include <algorithm>
#include <cmath>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using namespace atlas::drive_dynamics;

namespace {
  // A minimal offline JSFX host: it compiles the supplied script's @init,
  // @slider, @block and @sample sections, never a second copy of its DSP.
  class Reference {
    public:
      Reference(const std::string& path, double rate) : vm_(NSEEL_VM_alloc()) {
        NSEEL_VM_setramsize(vm_, 4 * 1024 * 1024);
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
          const auto handle = NSEEL_code_compile_ex(vm_, source[name].c_str(), 0,
                              std::string(name) == "init" ? NSEEL_CODE_COMPILE_FLAG_COMMONFUNCS : 0);
          if (!handle) throw std::runtime_error(path + " @" + name + ": " + NSEEL_code_getcodeerror(vm_));
          code_[name] = handle;
        }
        run("init");
      }
      ~Reference() {
        for (auto& entry : code_) NSEEL_code_free(entry.second);
        NSEEL_VM_free(vm_);
      }
      void set(const std::string& name, double value) { *NSEEL_VM_regvar(vm_, name.c_str()) = value; }
      double get(const char* name) { return *NSEEL_VM_regvar(vm_, name); }
      void run(const char* section) { if (code_.count(section)) NSEEL_code_execute(code_[section]); }
      std::pair<double, double> sample(double left, double right) {
        set("spl0", left); set("spl1", right); set("spl2", 0); set("spl3", 0);
        run("sample");
        return { get("spl0"), get("spl1") };
      }
    private:
      NSEEL_VMCTX vm_;
      std::map<std::string, NSEEL_CODEHANDLE> code_;
  };

  using Bindings = std::vector<std::pair<const char*, double*>>;

  // Wideband test material with loud and quiet passages, so the drive stages
  // clip and the dynamics stages cross their thresholds both ways.
  std::pair<double, double> signal(int i, double rate) {
    const double t = i / rate;
    const double swell = 0.02 + 0.98 * std::pow(0.5 + 0.5 * std::sin(2 * 3.14159265358979 * 1.7 * t), 3.0);
    const double left = swell * (0.6 * std::sin(i * 0.0731) + 0.3 * std::sin(i * 0.5113) + 0.1 * std::sin(i * 1.9));
    const double right = swell * (0.5 * std::sin(i * 0.0573 + 1.0) - 0.4 * std::cos(i * 0.2911) + 0.1 * std::sin(i * 2.6));
    return { i == 0 ? 0.9 : left, i == 177 ? -0.8 : right };
  }

  // One run: a list of (time in seconds, change to the parameters) events.
  struct Run {
    std::string name;
    double seconds;
    std::vector<std::pair<double, std::function<void()>>> events;
  };

  template<class Core, class Parameters>
  double compare(const std::string& path, const char* effect, Parameters& p, const Bindings& bindings,
                 const Run& run, double tolerance, int* latency_check = nullptr) {
    double worst = 0;
    for (double rate : { 44100.0, 48000.0, 88200.0, 96000.0, 192000.0 }) {
      const Parameters start = p;
      Reference ref(path, rate);
      // Saturator runs without Atlas's DC blocker here, which is not part of the script.
      Core core = [] { if constexpr (std::is_same_v<Core, Saturator>) return Saturator(false); else return Core(); }();
      core.prepare(rate);
      auto update = [&] {
        for (const auto& binding : bindings) ref.set(binding.first, *binding.second);
        ref.run("slider");
        core.setParameters(p);
      };
      update();
      size_t next = 0;
      const int total = static_cast<int>(run.seconds * rate);
      for (int i = 0; i < total; ++i) {
        bool changed = false;
        while (next < run.events.size() && i == static_cast<int>(run.events[next].first * rate)) {
          run.events[next++].second();
          changed = true;
        }
        if (changed) update();
        if (i % 64 == 0) ref.run("block");
        auto [left, right] = signal(i, rate);
        const auto expected = ref.sample(left, right);
        core.process(left, right);
        for (const double error : { std::abs(left - expected.first), std::abs(right - expected.second) }) {
          if (!std::isfinite(left) || !std::isfinite(right) || !(error <= tolerance))
            throw std::runtime_error(std::string(effect) + " " + run.name + " at " + std::to_string(rate) +
                " Hz, sample " + std::to_string(i) + ": native " + std::to_string(left) + ", " +
                std::to_string(right) + ", JSFX " + std::to_string(expected.first) + ", " +
                std::to_string(expected.second));
          worst = std::max(worst, error);
        }
        if (latency_check && core.latency() != static_cast<int>(ref.get("pdc_delay")))
          throw std::runtime_error(std::string(effect) + " latency differs from pdc_delay");
      }
      p = start;
    }
    std::cout << effect << " " << run.name << ": maximum difference " << worst << '\n';
    return worst;
  }

  void saturatorTests(const std::string& folder) {
    Saturator::Parameters p;
    const Bindings bindings = {
      { "drive", &p.drive }, { "ctype", &p.type }, { "color", &p.color }, { "amtlo", &p.amt_lo },
      { "cfreq", &p.frequency }, { "cwidth", &p.width }, { "amthi", &p.amt_hi }, { "postclip", &p.post_clip },
      { "outdb", &p.output }, { "drywet", &p.dry_wet }, { "bthr", &p.bass_threshold }, { "wsdrive", &p.ws_drive },
      { "wslin", &p.ws_linear }, { "wscurve", &p.ws_curve }, { "wsdamp", &p.ws_damp },
      { "wsperiod", &p.ws_period }, { "wsdepth", &p.ws_depth }, { "dcfilt", &p.dc_filter }, { "hiq", &p.hi_quality } };
    const std::string path = folder + "/Saturator.jsfx";
    int latency = 0;
    for (int type = 0; type < 8; ++type) {
      for (int hiq = 0; hiq < 2; ++hiq) {
        p = {};
        p.type = type; p.hi_quality = hiq; p.drive = 12;
        Run run { "type " + std::to_string(type) + (hiq ? " Hi-Q" : ""), 0.6, {
          { 0.1, [&] { p.drive = 30; p.color = 1; p.amt_lo = 9; p.amt_hi = -7; p.frequency = 2400; p.width = 70; } },
          { 0.2, [&] { p.post_clip = 1; p.output = -6; p.dry_wet = 63; p.bass_threshold = -20; p.dc_filter = 1; } },
          { 0.3, [&] { p.ws_drive = 70; p.ws_linear = 20; p.ws_curve = 40; p.ws_damp = 30; p.ws_period = 60;
                       p.ws_depth = 80; p.post_clip = 2; p.drive = -10; } },
          { 0.4, [&] { p.hi_quality = 1 - p.hi_quality; p.amt_lo = -20; p.amt_hi = 18; p.width = 0; } },
          { 0.5, [&] { p.drive = 36; p.color = 0; p.dry_wet = 100; p.post_clip = 0; } } } };
        compare<Saturator>(path, "Saturator", p, bindings, run, 1.0e-9, &latency);
      }
    }
  }

  void dynamicTubeTests(const std::string& folder) {
    DynamicTube::Parameters p;
    const Bindings bindings = {
      { "drive", &p.drive }, { "ttype", &p.tube_type }, { "bias", &p.bias }, { "envamt", &p.envelope },
      { "attack", &p.attack }, { "release", &p.release }, { "tone", &p.tone }, { "outdb", &p.output },
      { "drywet", &p.dry_wet }, { "hiq", &p.hi_quality } };
    const std::string path = folder + "/DynamicTube.jsfx";
    int latency = 0;
    for (int type = 0; type < 3; ++type) {
      for (int hiq = 0; hiq < 2; ++hiq) {
        p = {};
        p.tube_type = type; p.hi_quality = hiq; p.drive = 6;
        Run run { "tube " + std::string(1, static_cast<char>('A' + type)) + (hiq ? " Hi-Q" : ""), 0.6, {
          { 0.1, [&] { p.drive = 15; p.bias = 60; p.envelope = 2.2; p.attack = 1; p.release = 400; } },
          { 0.2, [&] { p.tone = 0.7; p.output = -4; p.dry_wet = 55; } },
          { 0.3, [&] { p.envelope = -3; p.tone = -1; p.attack = 300; p.release = 3; p.bias = 100; } },
          { 0.4, [&] { p.hi_quality = 1 - p.hi_quality; p.drive = -15; p.tone = 0; } },
          { 0.5, [&] { p.drive = 10; p.bias = 0; p.envelope = 0; p.dry_wet = 100; } } } };
        compare<DynamicTube>(path, "Dynamic Tube", p, bindings, run, 1.0e-9, &latency);
      }
    }
  }

  void overdriveTests(const std::string& folder) {
    Overdrive::Parameters p;
    const Bindings bindings = {
      { "freq", &p.freq }, { "width", &p.width }, { "drive", &p.drive }, { "tone", &p.tone },
      { "dynamics", &p.dynamics }, { "drywet", &p.dry_wet } };
    const std::string path = folder + "/Overdrive.jsfx";
    int latency = 0;
    p = {};
    Run run { "sweep", 1.0, {
      { 0.1, [&] { p.drive = 100; p.dynamics = 0; p.dry_wet = 100; } },
      { 0.25, [&] { p.freq = 180; p.width = 0.5; p.tone = 0; p.dynamics = 100; } },
      { 0.4, [&] { p.freq = 20000; p.width = 9; p.tone = 100; p.drive = 0; } },
      { 0.55, [&] { p.freq = 50; p.dry_wet = 0; p.drive = 73; } },
      { 0.7, [&] { p.freq = 3300; p.width = 3.1; p.dry_wet = 31; p.tone = 42; p.dynamics = 66; } } } };
    compare<Overdrive>(path, "Overdrive", p, bindings, run, 1.0e-9, &latency);
  }

  void multibandTests(const std::string& folder) {
    MultibandDynamics::Parameters p;
    Bindings bindings = {
      { "xo1", &p.split_low_mid }, { "xo2", &p.split_mid_high }, { "lo_on", &p.low_band }, { "hi_on", &p.high_band },
      { "amount", &p.amount }, { "gtime", &p.time }, { "outgain", &p.output }, { "peak", &p.peak }, { "knee", &p.knee } };
    const char* bands[] = { "l_", "m_", "h_" };
    for (int b = 0; b < 3; ++b) {
      auto& band = p.bands[b];
      const std::pair<const char*, double*> fields[] = {
        { "act", &band.active }, { "solo", &band.solo }, { "athr", &band.above_threshold },
        { "arat", &band.above_ratio }, { "bthr", &band.below_threshold }, { "brat", &band.below_ratio },
        { "att", &band.attack }, { "rel", &band.release }, { "in", &band.input }, { "out", &band.output } };
      static std::vector<std::string> names;
      names.reserve(64);
      for (const auto& field : fields) {
        names.push_back(std::string(bands[b]) + field.first);
        bindings.push_back({ names.back().c_str(), field.second });
      }
    }
    const std::string path = folder + "/MultibandDynamics.jsfx";
    for (int peak = 0; peak < 2; ++peak) {
      p = {};
      p.peak = peak;
      for (auto& band : p.bands) { band.above_ratio = -0.75; band.above_threshold = -24; }
      Run run { peak ? "peak" : "RMS", 1.0, {
        { 0.1, [&] { p.bands[0].below_ratio = -3; p.bands[0].below_threshold = -30; p.bands[1].above_ratio = 1;
                     p.bands[2].above_ratio = -1; p.bands[2].input = 12; p.bands[1].output = -9; } },
        { 0.25, [&] { p.knee = 0; p.time = 37; p.amount = 63; p.output = 5; p.split_low_mid = 4000; } },
        { 0.4, [&] { p.bands[1].solo = 1; p.bands[2].active = 0; p.split_low_mid = 250; p.split_mid_high = 900; } },
        { 0.55, [&] { p.low_band = 0; p.bands[1].solo = 0; p.bands[0].solo = 1; p.time = 1000; } },
        { 0.7, [&] { p.low_band = 1; p.high_band = 0; p.bands[0].solo = 0; p.bands[2].active = 1;
                     p.bands[0].attack = 0.1; p.bands[0].release = 5000; p.knee = 1; p.amount = 100; } },
        { 0.85, [&] { p.high_band = 1; p.bands[1].below_ratio = 1; p.bands[1].below_threshold = -10; } } } };
      compare<MultibandDynamics>(path, "Multiband Dynamics", p, bindings, run, 1.0e-9);
    }
  }
}

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: AtlasDriveDynamicsReferenceTests <folder with the JSFX>\n";
    return 2;
  }
  try {
    NSEEL_init();
    saturatorTests(argv[1]);
    dynamicTubeTests(argv[1]);
    overdriveTests(argv[1]);
    multibandTests(argv[1]);
    NSEEL_quit();
    return 0;
  }
  catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
