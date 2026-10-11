// Executes the supplied BP StepGate source in WDL/EEL2 as the DSP oracle.
#include "trance_gate_module.h"
#include "ns-eel.h"
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
extern "C" {
void NSEEL_HOSTSTUB_EnterMutex() {}
void NSEEL_HOSTSTUB_LeaveMutex() {}
}
class Reference {
public:
  Reference(const char* path) : vm_(NSEEL_VM_alloc()) {
    std::ifstream file(path);
    if (!file) throw std::runtime_error("Missing StepGate reference");
    std::map<std::string, std::string> sections;
    std::string line, section;
    while (std::getline(file, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line.rfind("slider", 0) == 0 && line.find(':') != std::string::npos) {
        auto colon = line.find(':'); set(line.substr(0, colon), std::stod(line.substr(colon + 1)));
      }
      if (!line.empty() && line.front() == '@') { section = line.substr(1); continue; }
      if (!section.empty()) sections[section] += line + "\n";
    }
    // REAPER's slider(index) host function. No changes to the supplied DSP.
    std::string shim = "function slider(n) (";
    for (int n = 1; n <= 103; ++n) shim += "n == " + std::to_string(n) + " ? slider" + std::to_string(n) + " : ";
    shim += "0;);\n";
    sections["init"] = shim + sections["init"];
    for (const auto* name : { "init", "slider", "sample" }) {
      auto handle = NSEEL_code_compile_ex(vm_, sections[name].c_str(), 0,
          std::string(name) == "init" ? NSEEL_CODE_COMPILE_FLAG_COMMONFUNCS : 0);
      if (!handle) throw std::runtime_error(std::string(name) + ": " + NSEEL_code_getcodeerror(vm_));
      code_[name] = handle;
    }
  }
  ~Reference() {
    for (auto* name : { "sample", "slider", "init" }) NSEEL_code_free(code_.at(name));
    NSEEL_VM_free(vm_);
  }
  void set(const std::string& id, double value) { *NSEEL_VM_regvar(vm_, id.c_str()) = value; }
  void run(const char* section) { NSEEL_code_execute(code_.at(section)); }
  double sample(double beat) {
    set("beat_position", beat); set("spl0", 1); set("spl1", -0.5); run("sample");
    return *NSEEL_VM_regvar(vm_, "spl0");
  }
private:
  NSEEL_VMCTX vm_;
  std::map<std::string, NSEEL_CODEHANDLE> code_;
};
int main(int argc, char** argv) {
  if (argc != 2) return 2;
  Reference ref(argv[1]);
  int cases = 0;
  double worst = 0;
  for (int rate : { 44100, 48000, 96000 }) for (int factor : { 1, 2, 4, 8 }) {
    vital::Value bps { 2.25f };
    vital::TranceGateClock clock;
    clock.setPlaying(true);
    vital::TranceGateModule gate(bps.output(), &clock);
    gate.init(); gate.setSampleRate(rate); gate.setOversampleAmount(factor);
    auto controls = gate.getControls();
    auto set = [&](const std::string& id, float value, int slider) {
      controls.at("trance_gate_" + id)->set(value);
      ref.set("slider" + std::to_string(slider), value);
    };
    std::vector<vital::poly_float> input(127 * factor, vital::poly_float(1.0f));
    for (int grid = 0; grid < 8; ++grid) for (int mode = 0; mode < vital::trance_gate::kModes; ++mode) {
      const int variant = grid + mode;
      set("grid", grid, 1);
      set("start", variant % 3 == 0 ? 29 : 2, 2);
      set("stop", variant % 3 == 0 ? 3 : variant % 3 == 1 ? 31 : 2, 3);
      set("attack", variant % 4 == 0 ? 0 : variant % 4 == 1 ? 2 : variant % 4 == 2 ? 123 : 500, 4);
      set("decay", variant % 3 == 0 ? 0 : variant % 3 == 1 ? 57 : 500, 5);
      set("sustain", variant % 3 == 0 ? 0.0f : variant % 3 == 1 ? 0.37f : 1.0f, 6);
      // Release is no longer an Atlas control. Vary it in the unmodified
      // source to verify that removing it leaves the retained shapes identical.
      ref.set("slider7", variant % 3 == 0 ? 0 : variant % 3 == 1 ? 5 : 500);
      for (int i = 0; i < 32; ++i) {
        auto id = "step_" + std::to_string(i + 1) + "_";
        set(id + "value", (i % 5) * 0.5f - 1.0f, 8 + i * 3);
        set(id + "mode", (i + mode) % vital::trance_gate::kModes, 9 + i * 3);
        set(id + "curve", (i % 9) * 0.25f - 1.0f, 10 + i * 3);
      }
      ref.set("srate", rate * factor); ref.set("tempo", 135); ref.set("play_state", 1);
      ref.run("init"); ref.run("slider"); gate.hardReset();
      // Cover all 32 steps and each step's interior, including seeks. Use
      // offsets away from rounding ties at exact mathematical step boundaries.
      const double division = vital::trance_gate::beatsPerStep(grid);
      for (int block = 0; block < 100; ++block) {
        const double seconds = (block * 0.371 + 0.000137) * division / 2.25;
        gate.correctToTime(seconds);
        gate.processWithInput(input.data(), static_cast<int>(input.size()));
        for (int i = 0; i < static_cast<int>(input.size()); ++i) {
          const double expected = ref.sample((seconds + double(i) / (rate * factor)) * 2.25);
          const double actual = gate.output()->buffer[i][0];
          const double error = std::abs(expected - actual);
          worst = std::max(worst, error);
          if (!std::isfinite(actual) || error > 3.0e-6) {
            std::cerr << "Mismatch rate=" << rate << " factor=" << factor << " grid=" << grid
                      << " mode=" << mode << " block=" << block << " sample=" << i
                      << " expected=" << expected << " actual=" << actual << '\n';
            return 1;
          }
        }
      }
      ++cases;
    }
  }
  std::cout << "BP StepGate oracle: " << cases << " configurations, worst absolute error " << worst << '\n';
}
