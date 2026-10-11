// Executes the unmodified Corpus JSFX in WDL/EEL2 as the Resonator's DSP
// oracle, for every behaviour the port keeps: all models and qualities, body,
// position, filter, gain and mix controls, automation, and the deterministic
// LFO shapes without glide. The port's documented fixes (glide after the LFO,
// random LFO stereo phase, Ratio mode identity, wet-only limiter) are covered
// by resonator_tests.cpp instead, so the limiter stays off here.
#include "resonator/resonator_engine.h"
#include "ns-eel.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>
extern "C" {
void NSEEL_HOSTSTUB_EnterMutex() {}
void NSEEL_HOSTSTUB_LeaveMutex() {}
}

namespace {
  class Reference {
    public:
      explicit Reference(const char* path) : vm_(NSEEL_VM_alloc()) {
        NSEEL_VM_setramsize(vm_, 65536);
        std::ifstream file(path);
        if (!file) throw std::runtime_error("Missing Corpus reference");
        std::string line, section;
        while (std::getline(file, line)) {
          if (!line.empty() && line.back() == '\r') line.pop_back();
          if (line.rfind("slider", 0) == 0 && line.find(':') != std::string::npos) {
            const auto colon = line.find(':');
            defaults_[line.substr(0, colon)] = std::stod(line.substr(colon + 1));
          }
          if (!line.empty() && line.front() == '@') { section = line.substr(1); continue; }
          if (!section.empty()) sections_[section] += line + "\n";
        }
        // REAPER's MIDI host functions; no MIDI reaches the oracle.
        sections_["init"] = "function midirecv(a, b, c, d) ( 0; );\nfunction midisend(a, b, c, d) ( 0; );\n" +
                            sections_["init"];
        for (const auto* name : { "init", "slider", "block", "sample" }) {
          auto handle = NSEEL_code_compile_ex(vm_, sections_[name].c_str(), 0,
              std::string(name) == "init" ? NSEEL_CODE_COMPILE_FLAG_COMMONFUNCS : 0);
          if (!handle) throw std::runtime_error(std::string(name) + ": " + NSEEL_code_getcodeerror(vm_));
          code_[name] = handle;
        }
      }
      ~Reference() {
        for (auto& code : code_) NSEEL_code_free(code.second);
        NSEEL_VM_free(vm_);
      }
      double& var(const std::string& id) { return *NSEEL_VM_regvar(vm_, id.c_str()); }
      void run(const char* section) { NSEEL_code_execute(code_.at(section)); }
      // Script defaults, then the oracle's base values (glide off), then the case.
      void start(double rate, const std::map<int, double>& base, const std::map<int, double>& sliders) {
        for (const auto& slider : defaults_) var(slider.first) = slider.second;
        for (const auto& slider : base) var("slider" + std::to_string(slider.first)) = slider.second;
        for (const auto& slider : sliders) var("slider" + std::to_string(slider.first)) = slider.second;
        var("slider35") = 0.0;  // the limiter differs by design
        var("srate") = rate; var("samplesblock") = 64; var("play_state") = 0;
        var("play_position") = 0; var("beat_position") = 0; var("tempo") = 120; var("midi_bus") = 0;
        run("init"); run("slider");
        // The script starts these two smoothers away from their targets; the
        // port starts every smoother at its target.
        var("filter_k") = var("filter_kt");
        var("bleed_s") = var("bleed_t");
      }
      void change(const std::map<int, double>& sliders) {
        for (const auto& slider : sliders) var("slider" + std::to_string(slider.first)) = slider.second;
        run("slider");
      }
    private:
      NSEEL_VMCTX vm_;
      std::map<std::string, double> defaults_;
      std::map<std::string, std::string> sections_;
      std::map<std::string, NSEEL_CODEHANDLE> code_;
  };

  // The port's parameters for the same JSFX slider values.
  atlas::resonator::Parameters toParameters(const std::map<int, double>& s, const std::map<int, double>& defaults) {
    const auto v = [&](int n) { const auto found = s.find(n); return found != s.end() ? found->second : defaults.at(n); };
    atlas::resonator::Parameters p;
    p.model = int(v(1)); p.quality = int(v(2));
    p.pitch = v(3) * std::exp2(v(6) / 1200.0);
    p.decay = v(7); p.material = v(8) / 100; p.brightness = v(9) / 100; p.inharmonics = v(10) / 100;
    p.ratio = v(11); p.hit = v(12) / 100; p.listen_left = v(13) / 100; p.listen_right = v(14) / 100;
    p.spread = v(15); p.width = v(16) / 100; p.radius = v(17) / 100; p.opening = v(18) / 100;
    p.lfo_amount = v(19); p.lfo_shape = int(v(20)); p.lfo_free = true; p.lfo_frequency = v(22);
    p.lfo_stereo_phase = v(24) / 360; p.lfo_spin = v(25) / 100; p.lfo_offset = v(26) / 360;
    p.filter = v(27) >= 0.5; p.filter_frequency = v(28); p.filter_bandwidth = v(29); p.bleed = v(30) / 100;
    p.excitation = std::pow(10.0, v(31) / 20); p.wet = std::pow(10.0, v(32) / 20); p.mix = v(33) / 100;
    p.output = std::pow(10.0, v(34) / 20); p.limiter = false; p.glide = v(40) / 1000; p.off_decay = v(39) / 100;
    return p;
  }
}

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  Reference ref(argv[1]);
  const std::map<int, double> defaults = {
    { 1, 0 }, { 2, 1 }, { 3, 130.8128 }, { 6, 0 }, { 7, 1.2 }, { 8, -20 }, { 9, 0 }, { 10, 0 }, { 11, 1.3 },
    { 12, 35 }, { 13, 25 }, { 14, 35 }, { 15, 0 }, { 16, 100 }, { 17, 50 }, { 18, 0 }, { 19, 0 }, { 20, 0 },
    { 22, 0.5 }, { 24, 0 }, { 25, 0 }, { 26, 0 }, { 27, 0 }, { 28, 1500 }, { 29, 3 }, { 30, 0 }, { 31, 0 },
    { 32, 0 }, { 33, 35 }, { 34, 0 }, { 39, 0 }, { 40, 0 } };

  struct Case {
    std::string name;
    std::map<int, double> sliders;
    std::map<int, double> automation;  // applied a quarter of the way through
  };
  std::vector<Case> cases;
  const char* models[] = { "Beam", "Marimba", "String", "Membrane", "Plate", "Pipe", "Tube" };
  for (int model = 0; model < 7; ++model) {
    for (int quality = 0; quality < 3; ++quality) {
      cases.push_back({ std::string(models[model]) + " quality " + std::to_string(quality),
                        { { 1, model }, { 2, quality }, { 3, 90.0 + 37 * model }, { 33, 80 },
                          { 8, -60 + 20 * quality }, { 9, 30 - 20 * model % 50 }, { 10, 15 * (model - 3) },
                          { 11, 0.6 + 0.4 * quality }, { 12, 10 + 12 * model }, { 13, 70 - 9 * model },
                          { 14, 5 + 11 * model }, { 17, 20 + 10 * model }, { 18, 15 * quality },
                          { 15, 12 - 8 * quality }, { 16, 40 + 25 * quality }, { 7, 0.3 + 0.9 * quality } },
                        {} });
    }
  }
  cases.push_back({ "High partials fading near Nyquist", { { 1, 2 }, { 2, 2 }, { 3, 2400 }, { 33, 100 } }, {} });
  cases.push_back({ "Filter, bleed and gains", { { 27, 1 }, { 28, 900 }, { 29, 1.2 }, { 30, 40 }, { 31, 6 },
                                                 { 32, -4 }, { 34, 3 }, { 33, 55 } }, {} });
  cases.push_back({ "Automation of body and gains", { { 1, 4 }, { 33, 70 } },
                    { { 7, 4.0 }, { 8, 40 }, { 9, -50 }, { 12, 80 }, { 13, 5 }, { 16, 20 }, { 3, 150 },
                      { 27, 1 }, { 28, 400 }, { 33, 30 }, { 31, -6 } } });
  const char* shapes[] = { "Sine", "Square", "Triangle", "Saw up", "Saw down" };
  for (int shape = 0; shape < 5; ++shape)
    cases.push_back({ std::string("LFO ") + shapes[shape], { { 1, 2 }, { 3, 110 }, { 19, 7.5 }, { 20, shape },
                      { 22, 3.3 }, { 24, 90 }, { 25, 40 }, { 26, 45 }, { 33, 90 } }, {} });

  double worst = 0.0, smallest_peak = 1e9;
  int compared = 0;
  for (double rate : { 44100.0, 48000.0, 96000.0 }) {
    for (const auto& test : cases) {
      ref.start(rate, defaults, test.sliders);
      atlas::resonator::Engine engine;
      engine.prepare(rate);
      auto parameters = toParameters(test.sliders, defaults);
      engine.setParameters(parameters);
      engine.reset();
      std::mt19937 random(7);
      std::uniform_real_distribution<float> unit(-0.5f, 0.5f);
      const int length = static_cast<int>(rate * 1.5), block = 64;
      double peak = 1e-9, error = 0.0;
      float in_l[block], in_r[block], out_l[block], out_r[block];
      for (int start = 0; start < length; start += block) {
        if (!test.automation.empty() && start == (length / 4 / block) * block) {
          ref.change(test.automation);
          auto merged = test.sliders;
          for (const auto& change : test.automation) merged[change.first] = change.second;
          parameters = toParameters(merged, defaults);
        }
        engine.setParameters(parameters);
        ref.run("block");
        // Bursts of noise and a held tone: both ring-up and free decay.
        for (int i = 0; i < block; ++i) {
          const int n = start + i;
          const bool burst = (n / int(rate * 0.25)) % 2 == 0;
          in_l[i] = burst ? unit(random) : 0.0f;
          in_r[i] = burst ? unit(random) : float(0.2 * std::sin(n * 0.013));
        }
        engine.process(in_l, in_r, out_l, out_r, block);
        for (int i = 0; i < block; ++i) {
          ref.var("spl0") = in_l[i]; ref.var("spl1") = in_r[i];
          ref.run("sample");
          const double expected[] = { ref.var("spl0"), ref.var("spl1") };
          const float actual[] = { out_l[i], out_r[i] };
          for (int c = 0; c < 2; ++c) {
            if (!std::isfinite(actual[c])) {
              std::cerr << "Non-finite output in " << test.name << '\n';
              return 1;
            }
            peak = std::max(peak, std::abs(expected[c]));
            error = std::max(error, std::abs(expected[c] - actual[c]));
          }
        }
      }
      const double relative = error / peak;
      worst = std::max(worst, relative);
      smallest_peak = std::min(smallest_peak, peak);
      if (peak < 0.01) {
        std::cerr << "Corpus oracle produced no meaningful signal: " << test.name << '\n';
        return 1;
      }
      if (relative > 2e-5) {
        std::cerr << "Corpus mismatch: " << test.name << " at " << rate << " Hz, error " << error
                  << " for peak " << peak << '\n';
        return 1;
      }
      ++compared;
    }
  }
  std::cout << "Corpus oracle: " << compared << " configurations, worst error " << worst
            << " of peak (smallest peak " << smallest_peak << ")\n";
  return 0;
}
