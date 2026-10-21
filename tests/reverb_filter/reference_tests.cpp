// SPDX-License-Identifier: GPL-3.0-or-later
// Run the unmodified user-supplied JSFX in EEL2, independently of Atlas DSP.
// ns-eel.h includes <windows.h> on Windows; keep its min/max macros out.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "reverb_filter_core.h"
#include "filter_module.h"
#include "synth_constants.h"
#include "value.h"
#include "ns-eel.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
extern "C" {
void NSEEL_HOSTSTUB_EnterMutex() {}
void NSEEL_HOSTSTUB_LeaveMutex() {}
}
class Reference {
public:
  Reference(const char* path, double rate, double cutoff, double resonance, double damp) : vm(NSEEL_VM_alloc()) {
    std::ifstream file(path);
    if (!file) throw std::runtime_error("Missing reference JSFX");
    std::map<std::string, std::string> source;
    std::string line, section;
    while (std::getline(file, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (!line.empty() && line[0] == '@') { section = line.substr(1); continue; }
      if (!section.empty()) source[section] += line + "\n";
    }
    set("srate", rate); set("cutoff_hz", cutoff); set("resonance_pct", resonance * 100);
    set("damp_pct", damp * 100); set("mix_pct", 100); set("output_db", 0);
    for (const auto* name : {"init", "slider", "block", "sample"}) {
      auto handle = NSEEL_code_compile_ex(vm, source[name].c_str(), 0,
          std::string(name) == "init" ? NSEEL_CODE_COMPILE_FLAG_COMMONFUNCS : 0);
      if (!handle) throw std::runtime_error(std::string(name) + ": " + NSEEL_code_getcodeerror(vm));
      code[name] = handle;
    }
    run("init"); run("slider");
  }
  ~Reference() {
    for (auto name : {"sample", "block", "slider", "init"}) NSEEL_code_free(code.at(name));
    NSEEL_VM_free(vm);
  }
  void set(const char* name, double value) { *NSEEL_VM_regvar(vm, name) = value; }
  double get(const char* name) { return *NSEEL_VM_regvar(vm, name); }
  void run(const char* section) { NSEEL_code_execute(code.at(section)); }
private:
  NSEEL_VMCTX vm;
  std::map<std::string, NSEEL_CODEHANDLE> code;
};
void moduleComparison(const char* path) {
  double worst_relative = 0, worst_absolute = 0;
  int cases = 0;
  for (int rate : {44100, 48000, 96000}) for (bool mono : {false, true})
    for (int factor : {1, 2, 4, 8}) for (double resonance : {0., .5, 1.}) {
    const int block = 128;
    const double cutoff = 1000;
    const double damp = resonance == .5 ? .63 : 0;
    const double mix = resonance == .5 ? .37 : 1;
    const double gain_db = resonance == .5 ? -6 : 0;
    Reference ref(path, rate, cutoff, resonance, damp);
    ref.set("mix_pct", mix * 100); ref.set("output_db", gain_db); ref.run("slider");
    vital::FilterModule module(mono ? "filter_fx" : "filter_1");
    vital::Output audio;
    module.setMono(mono); module.plug(&audio, vital::FilterModule::kAudio);
    module.init(); module.setSampleRate(rate); module.setOversampleAmount(factor);
    audio.ensureBufferSize(vital::kMaxBufferSize * factor);
    const std::string prefix = mono ? "filter_fx_" : "filter_1_";
    auto controls = module.getControls();
    controls.at(prefix + "on")->set(1);
    controls.at(prefix + "model")->set(vital::constants::kReverbFilter);
    controls.at(prefix + "reverb_cutoff")->set(std::log2(cutoff));
    controls.at(prefix + "resonance")->set(resonance);
    controls.at(prefix + "reverb_damp")->set(damp);
    controls.at(prefix + "reverb_output")->set(gain_db);
    controls.at(prefix + "mix")->set(mix);
    double error = 0, square_error = 0, ref_energy = 0;
    int position = 0;
    for (int b = -200; b < 200; ++b) {
      // Irregular blocks cover the one-sample processing caused by MIDI events.
      const int count = b < 0 ? block : b % 3 == 0 ? 1 : b % 3 == 1 ? 17 : block;
      double inputs[block];
      for (int i = 0; i < count; ++i) {
        const int n = position + i;
        inputs[i] = b < 0 || b >= 100 ? 0 : float(.1 * std::sin(n * .13) + .07 * std::cos(n * .59));
        // Repeated samples give an exact host-rate reference on every phase.
        // Left and right differ; the remaining lanes stay silent for a voice
        // filter and repeat lanes 0-1 for Filter FX, as in an effect slot.
        const float left = float(inputs[i]), right = float(-.5 * inputs[i]);
        for (int phase = 0; phase < factor; ++phase)
          audio.buffer[i * factor + phase] = mono ? vital::poly_float(left, right, left, right)
                                                  : vital::poly_float(left, right, 0, 0);
      }
      module.process(count * factor);
      ref.set("samplesblock", count); ref.run("block");
      for (int i = 0; i < count; ++i) {
        ref.set("spl0", inputs[i]); ref.set("spl1", -.5 * inputs[i]); ref.run("sample");
        if (b < 0) continue;
        for (int phase = 0; phase < factor; ++phase) for (int lane = 0; lane < 4; ++lane) {
          const double expected = lane < 2 || mono ? ref.get(lane % 2 == 0 ? "spl0" : "spl1") : 0;
          const double actual = module.output()->buffer[i * factor + phase][lane];
          if (!std::isfinite(actual)) throw std::runtime_error("Non-finite module output");
          error = std::max(error, std::abs(actual - expected));
          square_error += (actual - expected) * (actual - expected);
          ref_energy += expected * expected;
        }
      }
      if (b >= 0) position += count;
    }
    const double relative = std::sqrt(square_error / ref_energy);
    worst_relative = std::max(worst_relative, relative); worst_absolute = std::max(worst_absolute, error);
    if (relative > 2e-5 || error > 1e-5)
      throw std::runtime_error(std::string("1000 Hz module mismatch: ") + (mono ? "FX" : "voice") +
          " rate=" + std::to_string(rate) + " oversampling=" + std::to_string(factor) +
          " resonance=" + std::to_string(resonance) + " error=" + std::to_string(error));
    ++cases;
  }
  std::cout << "1000 Hz full filter vs host-rate JSFX: " << cases << " cases; worst absolute error "
            << worst_absolute << "; relative RMS error " << worst_relative << std::endl;
}
int main(int argc, char** argv) {
  if (argc != 2) return 2;
  try {
    moduleComparison(argv[1]);
    double worst = 0; int cases = 0;
    // The core runs four lanes: lanes 0-1 are one JSFX instance's left and
    // right, lanes 2-3 another's at the next cutoff, so the lanes agree on
    // neither cutoff nor input. Halfway through, lanes 0-1 are reset (a new
    // note) and compared with a fresh JSFX while lanes 2-3 ring on.
    const double cutoffs[] {8., 35., 100., 350., 400., 600., 800., 1200., 1600., 3200., 6400., 12800., 20000.};
    constexpr int kCutoffs = sizeof(cutoffs) / sizeof(cutoffs[0]);
    for (double rate : {44100., 48000., 96000., 192000., 384000.})
      for (int which = 0; which < kCutoffs; ++which) {
        const double cutoff = cutoffs[which], other_cutoff = cutoffs[(which + 1) % kCutoffs];
        const double resonance = (cases % 3) * .5, damp = (cases % 4) / 3.0;
        const double other_resonance = ((cases + 1) % 3) * .5, other_damp = ((cases + 2) % 4) / 3.0;
        auto ref = std::make_unique<Reference>(argv[1], rate, cutoff, resonance, damp);
        Reference other(argv[1], rate, other_cutoff, other_resonance, other_damp);
        vital::ReverbFilterCore core;
        core.reserve(rate); core.setSampleRate(rate);
        // Long enough to include the first 8 Hz reflections and feedback.
        const int total = std::min(cutoff, other_cutoff) < 50 ? int(rate * .6) : 4096;
        const int restart = (total / 2 / 127) * 127;
        int since_restart = 0;
        for (int position = 0; position < total;) {
          const int count = std::min(127, total - position);
          if (position == restart) {
            ref = std::make_unique<Reference>(argv[1], rate, cutoff, resonance, damp);
            core.resetLane(0); core.resetLane(1);
            since_restart = 0;
          }
          core.beginBlock(0, resonance, damp, count); core.beginBlock(1, resonance, damp, count);
          core.beginBlock(2, other_resonance, other_damp, count); core.beginBlock(3, other_resonance, other_damp, count);
          ref->set("samplesblock", count); ref->run("block");
          other.set("samplesblock", count); other.run("block");
          for (int i = 0; i < count; ++i) {
            const int n = position + i, m = since_restart++;
            const double input = m == 0 ? .8 : .12 * std::sin(m * .731) + .07 * std::cos(m * .037);
            const double other_input = n == 0 ? -.6 : .1 * std::sin(n * .29) - .05 * std::cos(n * .011);
            ref->set("spl0", input); ref->set("spl1", input * -.5); ref->run("sample");
            other.set("spl0", other_input); other.set("spl1", other_input * .7); other.run("sample");
            alignas(16) const double inputs[4] { input, input * -.5, other_input, other_input * .7 };
            alignas(16) const double lane_cutoffs[4] { cutoff, cutoff, other_cutoff, other_cutoff };
            alignas(16) double actual[4];
            core.tick(inputs, lane_cutoffs, actual);
            const double expected[4] { ref->get("spl0"), ref->get("spl1"), other.get("spl0"), other.get("spl1") };
            for (int lane = 0; lane < 4; ++lane) {
              const double error = std::abs(actual[lane] - expected[lane]);
              worst = std::max(worst, error);
              if (!std::isfinite(actual[lane]) || error > 3e-6)
                throw std::runtime_error("Mismatch rate=" + std::to_string(rate) + " cutoff=" +
                    std::to_string(lane < 2 ? cutoff : other_cutoff) + " lane=" + std::to_string(lane) +
                    " sample=" + std::to_string(n) + " error=" + std::to_string(error));
            }
          }
          position += count;
        }
        ++cases;
      }
    // Automation follows the source's log-cutoff ramp, including FIR boundaries,
    // on every lane at once.
    for (double rate : {44100., 48000.}) {
      Reference ref(argv[1], rate, 100, 0, 0);
      vital::ReverbFilterCore core; core.reserve(rate); core.setSampleRate(rate);
      double log_cut = std::log(100.0);
      for (int block = 0; block < 70; ++block) {
        const int count = block % 3 == 0 ? 1 : block % 3 == 1 ? 17 : 128;
        const double cutoff = std::exp(std::log(8.) + (block % 14) / 13.0 * std::log(2500.));
        const double resonance = (block % 5) / 4.0, damp = (block % 3) / 2.0;
        ref.set("cutoff_hz", cutoff); ref.set("resonance_pct", resonance * 100); ref.set("damp_pct", damp * 100);
        ref.run("slider"); ref.set("samplesblock", count); ref.run("block");
        for (int lane = 0; lane < 4; ++lane) core.beginBlock(lane, resonance, damp, count);
        const double step = (std::log(cutoff) - log_cut) / count;
        for (int i = 0; i < count; ++i) {
          log_cut += step;
          const double input = .13 * std::sin((block * 128 + i) * .17);
          ref.set("spl0", input); ref.set("spl1", -input); ref.run("sample");
          alignas(16) const double inputs[4] { input, -input, input, -input };
          const double lane_cutoff = std::exp(log_cut);
          alignas(16) const double lane_cutoffs[4] { lane_cutoff, lane_cutoff, lane_cutoff, lane_cutoff };
          alignas(16) double actual[4];
          core.tick(inputs, lane_cutoffs, actual);
          for (int lane = 0; lane < 4; ++lane) {
            const double error = std::abs(actual[lane] - ref.get(lane % 2 ? "spl1" : "spl0"));
            worst = std::max(worst, error);
            if (!std::isfinite(actual[lane]) || error > 3e-6) throw std::runtime_error("Automation mismatch");
          }
        }
      }
      ++cases;
    }
    std::cout << "Reverb filter JSFX oracle: " << cases << " cases; worst absolute error " << worst << '\n';
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
