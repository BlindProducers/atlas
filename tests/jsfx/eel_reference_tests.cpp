#include "jsfx_ports.h"
#include "ns-eel.h"
extern "C" {
void NSEEL_HOSTSTUB_EnterMutex() {}
void NSEEL_HOSTSTUB_LeaveMutex() {}
}

#include <cmath>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>

using namespace vital::jsfx;

// A minimal offline JSFX lifecycle host. It compiles the supplied files, not a
// hand-written second implementation of their DSP. No REAPER install is needed.
class Reference {
  public:
    Reference(const std::string& path, double rate) : vm(NSEEL_VM_alloc()) {
      std::ifstream file(path);
      if (!file) throw std::runtime_error("Cannot open " + path);
      std::map<std::string, std::string> source;
      std::string line, section;
      while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("slider", 0) == 0 && line.find(':') != std::string::npos) {
          const auto colon = line.find(':');
          set(line.substr(0, colon), std::stod(line.substr(colon + 1)));
        }
        if (!line.empty() && line[0] == '@') { section = line.substr(1); continue; }
        if (!section.empty()) source[section] += line + "\n";
      }
      set("srate", rate); set("tempo", 120); set("samplesblock", 128);
      for (const auto* name : { "init", "slider", "block", "sample" }) {
        if (source[name].empty()) continue;
        const auto handle = NSEEL_code_compile_ex(vm, source[name].c_str(), 0,
                            std::string(name) == "init" ? NSEEL_CODE_COMPILE_FLAG_COMMONFUNCS : 0);
        if (!handle) throw std::runtime_error(path + " @" + name + ": " + NSEEL_code_getcodeerror(vm));
        code[name] = handle;
      }
    }
    ~Reference() {
      for (const auto* name : { "sample", "block", "slider", "init" })
        if (code.count(name)) NSEEL_code_free(code[name]);
      NSEEL_VM_free(vm);
    }
    void set(const std::string& name, double value) { *NSEEL_VM_regvar(vm, name.c_str()) = value; }
    double get(const char* name) { return *NSEEL_VM_regvar(vm, name); }
    void run(const char* section) { if (code.count(section)) NSEEL_code_execute(code[section]); }
    Frame sample(const Frame& input) {
      set("spl0", input[0]); set("spl1", input[1]); run("sample");
      return { get("spl0"), get("spl1"), 0, 0 };
    }
  private:
    NSEEL_VMCTX vm;
    std::map<std::string, NSEEL_CODEHANDLE> code;
};

Frame signal(int sample) {
  const double left = sample == 0 ? 0.8 : 0.17 * std::sin(sample * 0.731) + 0.23 * std::sin(sample * 0.037);
  const double right = sample == 177 ? -0.6 : 0.11 * std::sin(sample * 0.573) - 0.19 * std::cos(sample * 0.019);
  return { left, right, left, right };
}

void compare(const Frame& native, const Frame& reference, double& worst, int sample, const char* effect) {
  for (int lane = 0; lane < 2; ++lane) {
    if (!std::isfinite(native[lane]) || !std::isfinite(reference[lane]))
      throw std::runtime_error(std::string(effect) + " nonfinite output");
    worst = std::max(worst, std::abs(native[lane] - reference[lane]));
    if (worst > 2.0e-6)
      throw std::runtime_error(std::string(effect) + " mismatch at sample " + std::to_string(sample) +
          ", error=" + std::to_string(worst) + ", native=" + std::to_string(native[lane]) +
          ", JSFX=" + std::to_string(reference[lane]));
  }
}

void quoraxTest(const std::string& folder) {
  double worst = 0;
  for (double rate : { 44100.0, 48000.0, 96000.0, 192000.0 }) {
    Quorax core;
    core.prepare(rate);
    Reference ref(folder + "/Quorax_Dimension_Expander.jsfx", rate);
    Quorax::Parameters p;
    auto update = [&] {
      ref.set("slider1", p.size); ref.set("slider2", p.mix); ref.set("slider3", p.output);
      ref.set("slider4", p.motion); ref.set("slider5", p.low_cut); ref.set("slider6", p.high_cut);
      ref.set("slider7", p.slope); ref.run("slider"); core.setParameters(p);
    };
    ref.run("init"); update();
    for (int i = 0; i < static_cast<int>(rate * 1.2); ++i) {
      if (i == static_cast<int>(rate * 0.25)) { p.size = 100; p.mix = 100; p.output = -4.7; p.motion = 1.37; update(); }
      if (i == static_cast<int>(rate * 0.4)) { p.low_cut = 817; p.high_cut = 4310; p.slope = 3; update(); }
      if (i == static_cast<int>(rate * 0.6)) { p.low_cut = 0; p.high_cut = 20000; update(); }
      if (i == static_cast<int>(rate * 0.7)) { p.size = 0; p.low_cut = 10; p.high_cut = 500; p.slope = 0; update(); }
      if (i == static_cast<int>(rate * 0.9)) { p.slope = 2; update(); }
      const auto input = signal(i);
      compare(core.process(input), ref.sample(input), worst, i, "Quorax");
    }
  }
  std::cout << "Quorax maximum error against JSFX: " << worst << '\n';
}

void erosionTest(const std::string& folder) {
  double worst = 0;
  for (double rate : { 44100.0, 48000.0, 96000.0, 192000.0 }) {
    for (int mode : { 0, 1 }) {
      Erosion core;
      core.prepare(rate);
      Reference ref(folder + "/BP A-Erosion.jsfx", rate);
      Erosion::Parameters p;
      p.mode = mode;
      auto update = [&] {
        ref.set("slider1", p.mode); ref.set("slider2", p.frequency); ref.set("slider3", p.width);
        ref.set("slider4", p.amount); ref.set("slider5", p.stereo);
        ref.run("slider"); core.setParameters(p);
      };
      ref.run("init"); update();
      for (int i = 0; i < 24000; ++i) {
        if (i == 5000) { p.frequency = 17000; p.width = 5; p.amount = 200; p.stereo = 0; update(); }
        if (i == 10000) { p.frequency = 20; p.width = 0.1; p.stereo = 37; update(); }
        if (i == 15000) { p.amount = 0; update(); }
        if (i == 20000) { p.frequency = 8000; p.mode = 1 - p.mode; p.amount = 80; update(); }
        const auto input = signal(i);
        const auto expected = ref.sample(input);
        // Feed the exact JSFX random draws to the native noise filter. The
        // shipping effect owns a separate uniform PRNG, never global rand().
        const Frame noise { ref.get("raw_l"), ref.get("raw_r"), ref.get("raw_l"), ref.get("raw_r") };
        compare(core.process(input, &noise), expected, worst, i, "Erosion");
      }
    }
  }
  std::cout << "Erosion maximum error against JSFX (identical random input): " << worst << '\n';
}

void floatyTest(const std::string& folder) {
  double worst = 0;
  for (double rate : { 44100.0, 48000.0, 96000.0 }) {
    for (double playback : { -2.0, -0.5, 0.0, 0.5, 1.0, 2.0 }) {
      Floaty core;
      core.prepare(rate);
      Reference ref(folder + "/floaty_delay", rate);
      Floaty::Parameters p;
      p.delay = 19; p.playback = playback; p.mix = 73; p.warp_rate = 3.1; p.warp = 3.7;
      double bpm = 120;
      auto update = [&] {
        ref.set("slider1", p.delay); ref.set("slider2", p.mix); ref.set("slider3", p.feedback);
        ref.set("slider4", p.warp_rate); ref.set("slider5", p.warp); ref.set("slider6", p.cutoff);
        ref.set("slider7", p.resonance); ref.set("slider8", p.playback); ref.set("slider9", p.sync);
        ref.set("slider10", p.offset); ref.set("slider11", p.clamp); ref.set("tempo", bpm);
        ref.run("block"); ref.run("slider"); core.setParameters(p, bpm);
      };
      ref.run("init"); update();
      for (int i = 0; i < 24000; ++i) {
        if (i == 5000) { p.delay = 7; p.resonance = 140; p.feedback = 80; p.clamp = -9; update(); }
        if (i == 10000) { p.delay = 41; p.cutoff = 0; p.offset = 70; update(); }
        if (i == 15000) { p.sync = 1; p.playback = 1; p.offset = 100; p.cutoff = 83; update(); }
        if (i == 20000) { bpm = 173; p.resonance = 0; update(); }
        const auto input = signal(i);
        compare(core.process(input), ref.sample(input), worst, i, "Floaty");
      }
    }
  }
  std::cout << "Floaty maximum error against JSFX: " << worst << '\n';
}

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  try {
    NSEEL_init();
    quoraxTest(argv[1]); erosionTest(argv[1]); floatyTest(argv[1]);
    NSEEL_quit();
    return 0;
  }
  catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
