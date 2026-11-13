// SPDX-License-Identifier: GPL-3.0-or-later
// Runs the filter JSFX the Band Reject model is ported from through WDL's
// EEL2, on its Bandreject type, next to BandRejectCore, and requires their
// outputs to agree to within rounding at several sample rates, cutoffs,
// widths, drives and resonances, in both regimes of the sections, and after
// a channel restarts. The script glides its sliders per block and takes its
// damping from Reso on its own law, so each run holds its settings still,
// with the script's Reso set to give the core's damping. The comparison
// allows for the last bits of the coefficients: the script's sin and 2^x are
// the C library's, the core's its own series. The script is not part of the
// repository: CMake's ATLAS_BAND_REJECT_JSFX names it, and the test takes its
// path as its argument.
#include "band_reject_core.h"
#include "ns-eel.h"
#include "ns-eel-addfuncs.h"
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
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using vital::BandRejectCore;

namespace {
  // A minimal offline JSFX host: it compiles the script's @init, @slider,
  // @block and @sample sections, never a second copy of its DSP, and stubs
  // slider_show, which only shows or hides a control.
  class Reference {
    public:
      // `start` sets slider values before @init runs, so the script starts
      // on them instead of gliding to them.
      Reference(const std::string& path, double rate, const std::function<void(Reference&)>& start) :
          vm_(NSEEL_VM_alloc()) {
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
        set("srate", rate);
        set("samplesblock", 64);
        start(*this);
        for (const auto* name : { "init", "slider", "block", "sample" }) {
          const auto handle = NSEEL_code_compile_ex(vm_, source[name].c_str(), 0,
                                                    std::string(name) == "init" ? NSEEL_CODE_COMPILE_FLAG_COMMONFUNCS : 0);
          if (!handle) throw std::runtime_error("@" + std::string(name) + ": " + NSEEL_code_getcodeerror(vm_));
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
      void run(const char* section) { NSEEL_code_execute(code_.at(section)); }
      std::pair<double, double> sample(double left, double right) {
        set("spl0", left);
        set("spl1", right);
        run("sample");
        return { get("spl0"), get("spl1") };
      }
      static void registerFunctions() {
        NSEEL_addfunc_retval("slider_show", 2, NSEEL_PProc_THIS, &Reference::sliderShow);
      }

    private:
      static EEL_F NSEEL_CGEN_CALL sliderShow(void*, EEL_F*, EEL_F*) { return 0; }

      NSEEL_VMCTX vm_;
      std::map<std::string, NSEEL_CODEHANDLE> code_;
  };

  // The script's wet gain at Mix 100% and Level 0.5, and its output clip.
  constexpr double kOutputGain = 0.999830894;
  constexpr double kOutputClip = 4.0;
  constexpr int kBandreject = 40;

  struct Settings {
    const char* name;
    double midi, resonance, width, drive;
  };

  // Unrelated left and right material: tones that swell, a chirp-like term
  // and an impulse on each side, loud enough to reach the soft clip.
  std::pair<double, double> signal(int i, double rate) {
    const double t = i / rate;
    const double swell = 0.1 + 0.9 * std::pow(0.5 + 0.5 * std::sin(2 * 3.14159265358979 * 1.3 * t), 2.0);
    const double left = swell * (0.5 * std::sin(i * 0.0731) + 0.3 * std::sin(i * 0.5113 + 0.0001 * i * t) +
                                 0.1 * std::sin(i * 1.9));
    const double right = swell * (0.4 * std::sin(i * 0.0573 + 1.0) - 0.4 * std::cos(i * 0.2911) + 0.1 * std::sin(i * 2.6));
    return { i == 0 ? 0.9 : left, i == 177 ? -0.8 : right };
  }

  double clip(double value) { return std::min(std::max(value, -kOutputClip), kOutputClip); }

  int failures = 0;

  void compare(const std::string& path, const Settings& settings) {
    for (double rate : { 44100.0, 48000.0, 96000.0, 192000.0 }) {
      const double damping = BandRejectCore::damping(settings.resonance);
      // The script's Reso that gives this damping: 2 (1 - (Reso / 100)^(1/4)).
      const double reso = 100.0 * std::pow(1.0 - damping / 2.0, 4.0);
      const double cutoff_hz = BandRejectCore::kMinCutoff * std::pow(2.0, settings.midi / 12.0);
      const double drive = std::pow(1.0 + settings.drive / 100.0, 4.0);
      auto start = [&](Reference& ref) {
        ref.set("type", kBandreject);
        ref.set("fc", cutoff_hz);
        ref.set("reso", reso);
        ref.set("drive", settings.drive);
        ref.set("var", settings.width);
        ref.set("stereo", 50);
        ref.set("mix", 100);
        ref.set("level", 0.5);
      };
      auto ref = std::make_unique<Reference>(path, rate, start);
      BandRejectCore cores[2];
      for (auto& core : cores)
        core.setCutoff(settings.midi, settings.width, rate);
      const int total = static_cast<int>(0.4 * rate);
      double square_error = 0.0, energy = 0.0, worst = 0.0;
      for (int i = 0; i < total; ++i) {
        // Halfway, the channels restart: a fresh script beside reset cores.
        if (i == total / 2) {
          ref = std::make_unique<Reference>(path, rate, start);
          for (auto& core : cores)
            core.reset();
        }
        if (i % 64 == 0)
          ref->run("block");
        const auto [left, right] = signal(i, rate);
        const auto expected = ref->sample(left, right);
        const double actual[] = { clip(kOutputGain * cores[0].tick(left, drive, damping)),
                                  clip(kOutputGain * cores[1].tick(right, drive, damping)) };
        const double wanted[] = { expected.first, expected.second };
        for (int channel = 0; channel < 2; ++channel) {
          if (!std::isfinite(actual[channel]))
            throw std::runtime_error(std::string(settings.name) + ": the core's output is not finite");
          const double error = actual[channel] - wanted[channel];
          square_error += error * error;
          energy += wanted[channel] * wanted[channel];
          worst = std::max(worst, std::abs(error));
        }
      }
      const double relative = std::sqrt(square_error / std::max(energy, 1e-30));
      const bool ok = relative < 1e-9 && worst < 1e-8;
      if (!ok)
        ++failures;
      std::cout << (ok ? "" : "FAIL ") << settings.name << " at " << rate << " Hz: relative RMS error " << relative
                << ", largest " << worst << '\n';
    }
  }
}

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "usage: AtlasBandRejectReferenceTests <path to the filter JSFX>\n";
    return 2;
  }
  NSEEL_init();
  Reference::registerFunctions();
  try {
    // Cutoff as a MIDI note, resonance on Atlas's scale, width and drive as
    // the script's Var and Drive in percent.
    const Settings runs[] = {
      { "no resonance, narrow", 60, 0.0, 0, 0 },
      { "half resonance, driven", 36, 0.5, 25, 35 },
      { "high resonance, hot", 84, 0.9, 60, 100 },
      { "full resonance, widest", 100, 1.0, 100, 0 },
      { "lowpass section at the knob's bottom", 0, 0.3, 0, 50 },
      { "highpass section at the knob's top", 136, 0.6, 50, 0 },
      { "near Nyquist, no resonance", 134, 0.0, 0, 0 },
      { "near Nyquist, some resonance", 134, 0.2, 0, 20 },
      { "narrow and sharp", 48, 0.95, 5, 0 },
    };
    for (const Settings& settings : runs)
      compare(argv[1], settings);
  }
  catch (const std::exception& error) {
    std::cerr << "FAIL " << error.what() << '\n';
    return 1;
  }
  std::cout << "Band Reject reference: " << failures << " failures\n";
  return failures ? 1 : 0;
}
