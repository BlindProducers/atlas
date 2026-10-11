#include "JuceHeader.h"
#include "slot_test_ids.h"
#include "resonator/resonator_engine.h"
#include "resonator_module.h"
#include "note_monitor.h"
#include "reorderable_effect_chain.h"
#include "synth_plugin.h"
#include "sound_engine.h"
#include "sample_source.h"
#include "tuning.h"

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <map>
#include <new>
#include <random>
#include <string>
#include <vector>

namespace {
  std::atomic<bool> track_allocations { false };
  std::atomic<size_t> allocations { 0 };
}
void* operator new(std::size_t size) {
  if (track_allocations.load()) ++allocations;
  if (void* p = std::malloc(size)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {
  using Engine = atlas::resonator::Engine;
  using Parameters = atlas::resonator::Parameters;
  constexpr double kRate = 48000.0;

  int failures = 0;
  void expect(bool ok, const std::string& message) {
    if (!ok) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
  }
  bool near(double a, double b, double tolerance) { return std::abs(a - b) <= tolerance; }

  double goertzel(const std::vector<float>& x, int start, int length, double frequency, double rate) {
    const double w = 2.0 * juce::MathConstants<double>::pi * frequency / rate, c = 2.0 * std::cos(w);
    double s1 = 0.0, s2 = 0.0;
    for (int i = 0; i < length; ++i) {
      // A Hann window keeps neighbouring partials from leaking into the scan.
      const double window = 0.5 - 0.5 * std::cos(2.0 * juce::MathConstants<double>::pi * i / (length - 1));
      const double s = x[start + i] * window + c * s1 - s2;
      s2 = s1; s1 = s;
    }
    return std::sqrt(std::max(0.0, s1 * s1 + s2 * s2 - c * s1 * s2));
  }

  double peakFrequency(const std::vector<float>& x, int start, int length, double low, double high,
                       double step, double rate) {
    double best = low, best_level = -1.0;
    for (double f = low; f <= high; f += step) {
      const double level = goertzel(x, start, length, f, rate);
      if (level > best_level) { best_level = level; best = f; }
    }
    return best;
  }

  double rms(const std::vector<float>& x, int start, int length) {
    double sum = 0.0;
    for (int i = start; i < start + length; ++i) sum += double(x[i]) * x[i];
    return std::sqrt(sum / length);
  }

  struct Stereo { std::vector<float> left, right; };

  Stereo render(Engine& engine, const Stereo& input, int block = 128) {
    const int count = static_cast<int>(input.left.size());
    Stereo out { std::vector<float>(count), std::vector<float>(count) };
    for (int i = 0; i < count; i += block) {
      const int size = std::min(block, count - i);
      engine.process(input.left.data() + i, input.right.data() + i, out.left.data() + i, out.right.data() + i, size);
    }
    return out;
  }

  Stereo impulse(int count, float level = 1.0f, int at = 0) {
    Stereo in { std::vector<float>(count), std::vector<float>(count) };
    in.left[at] = in.right[at] = level;
    return in;
  }

  // A fully wet, quiet-bodied String whose partials share one decay time.
  Parameters wetString(double pitch) {
    Parameters p;
    p.model = atlas::resonator::kString;
    p.pitch = pitch;
    p.mix = 1.0;
    p.material = 0.0;
    p.glide = 0.0;
    p.decay = 1.0;
    p.limiter = false;
    return p;
  }

  void prepared(Engine& engine, const Parameters& p, double rate = kRate) {
    engine.prepare(rate);
    engine.setParameters(p);
    engine.reset();
  }

  void testModeTables() {
    Engine engine;
    Parameters p;
    p.quality = atlas::resonator::kHigh;
    const auto ratios = [&](int model) { p.model = model; prepared(engine, p); };

    ratios(atlas::resonator::kBeam);
    const double beam4 = std::pow(4.5 * juce::MathConstants<double>::pi / 4.730040745, 2.0);
    expect(near(engine.modeRatio(0), 1.0, 1e-12) && near(engine.modeRatio(1), 2.756538, 1e-9) &&
           near(engine.modeRatio(2), 5.403917, 1e-9) && near(engine.modeRatio(3), beam4, 1e-9),
           "Beam uses free-free beam roots");
    ratios(atlas::resonator::kMarimba);
    expect(near(engine.modeRatio(1), 4.0, 1e-12) && near(engine.modeRatio(2), 10.0, 1e-12) &&
           near(engine.modeRatio(3), 17.92, 1e-9), "Marimba starts near 1:4:10");
    ratios(atlas::resonator::kString);
    expect(near(engine.modeRatio(39), 40.0, 1e-12) && engine.activeModes() == 40, "String is harmonic");
    p.opening = 0.0; ratios(atlas::resonator::kPipe);
    expect(near(engine.modeRatio(1), 3.0, 1e-12) && near(engine.modeRatio(2), 5.0, 1e-12), "Closed pipe is odd harmonic");
    p.opening = 1.0; ratios(atlas::resonator::kPipe);
    expect(near(engine.modeRatio(1), 2.0, 1e-12) && near(engine.modeRatio(2), 3.0, 1e-12), "Open pipe is harmonic");
    p.ratio = 1.0; ratios(atlas::resonator::kMembrane);
    expect(near(engine.modeRatio(1), std::sqrt(2.5), 1e-12) && engine.modeM(1) == 1 && engine.modeN(1) == 2 &&
           engine.modeM(2) == 2 && engine.modeN(2) == 1, "Membrane selects (1,2) before its (2,1) twin");
    ratios(atlas::resonator::kPlate);
    expect(near(engine.modeRatio(1), 2.5, 1e-12), "Plate uses squared rectangular modes");

    const int eco[] = { 8, 8, 8, 8, 8, 40, 40 }, normal[] = { 20, 20, 20, 20, 20, 40, 40 };
    for (int model = 0; model < atlas::resonator::kNumModels; ++model) {
      p.model = model;
      p.quality = atlas::resonator::kEco; prepared(engine, p);
      const bool eco_ok = engine.activeModes() == eco[model];
      p.quality = atlas::resonator::kNormal; prepared(engine, p);
      expect(eco_ok && engine.activeModes() == normal[model], "Quality selects the script's mode counts");
    }
  }

  void testPitchAndDecay() {
    for (double pitch : { 55.0, 220.0, 1000.0 }) {
      Engine engine;
      prepared(engine, wetString(pitch));
      const auto out = render(engine, impulse(48000));
      const double peak = peakFrequency(out.left, 4800, 24000, pitch * 0.94, pitch * 1.06, 0.05, kRate);
      expect(near(peak, pitch, pitch * 0.0005), "String fundamental matches Tune at " + std::to_string(pitch) + " Hz");
    }

    Parameters p = wetString(220.0);
    p.decay = 0.5;
    Engine engine;
    prepared(engine, p);
    const auto out = render(engine, impulse(24000));
    const double drop = 20.0 * std::log10(rms(out.left, 4800, 2400) / rms(out.left, 16800, 2400));
    expect(near(drop, 30.0, 1.0), "Decay is the time to fall 60 dB (measured drop " + std::to_string(drop) + " dB)");
  }

  void testMixAndLimiter() {
    Parameters p;
    p.mix = 0.0;
    p.limiter = true;
    Engine engine;
    prepared(engine, p);
    Stereo input { std::vector<float>(9600), std::vector<float>(9600) };
    for (int i = 0; i < 9600; ++i) {
      input.left[i] = 1.5f * float(std::sin(2.0 * juce::MathConstants<double>::pi * 130.8128 * i / kRate));
      input.right[i] = -input.left[i];
    }
    const auto out = render(engine, input);
    expect(out.left == input.left && out.right == input.right, "0% Mix is transparent, even above the limiter ceiling");

    p = wetString(130.8128);
    p.decay = 30.0; p.excitation = 16.0; p.wet = 16.0; p.limiter = true;
    prepared(engine, p);
    double peak = 0.0;
    for (int block = 0; block < 20; ++block) {
      const auto burst = render(engine, input);
      for (int i = 0; i < 9600; ++i) peak = std::max(peak, double(std::max(std::abs(burst.left[i]), std::abs(burst.right[i]))));
    }
    expect(peak <= Engine::kLimiterCeiling + 1e-6, "The limiter holds a resonant peak below 0.98 (" + std::to_string(peak) + ")");
  }

  void testStress() {
    std::mt19937 random(1234);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    for (double rate : { 44100.0, 48000.0, 96000.0, 192000.0, 384000.0 }) {
      for (bool limiter : { true, false }) {
        Engine engine;
        engine.prepare(rate);
        Parameters p;
        bool finite = true, bounded = true;
        std::vector<float> in_l(128), in_r(128), out_l(128), out_r(128);
        double phase = 0.0;
        const int blocks = static_cast<int>(rate * 3.0 / 128);
        for (int block = 0; block < blocks; ++block) {
          if (block % 8 == 0) {
            p.model = int(unit(random) * 7); p.quality = int(unit(random) * 3);
            p.pitch = unit(random) < 0.2 ? (unit(random) < 0.5 ? 1.0 : 30000.0) : 20.0 * std::pow(250.0, unit(random));
            p.decay = unit(random) < 0.3 ? 30.0 : 0.02 * std::pow(1500.0, unit(random));
            p.material = 2 * unit(random) - 1; p.brightness = 2 * unit(random) - 1;
            p.inharmonics = 2 * unit(random) - 1; p.ratio = 0.25 + 3.75 * unit(random);
            p.hit = unit(random); p.listen_left = unit(random); p.listen_right = unit(random);
            p.spread = 200 * unit(random) - 100; p.width = unit(random);
            p.radius = unit(random); p.opening = unit(random);
            p.lfo_amount = 24 * unit(random); p.lfo_shape = int(unit(random) * 7);
            p.lfo_frequency = 20 * unit(random); p.lfo_free = unit(random) < 0.5;
            p.lfo_stereo_phase = unit(random); p.lfo_spin = 2 * unit(random) - 1; p.lfo_offset = unit(random);
            p.filter = unit(random) < 0.5; p.filter_frequency = 20 * std::pow(900.0, unit(random));
            p.filter_bandwidth = 0.1 + 5.9 * unit(random); p.bleed = unit(random);
            p.excitation = std::pow(10.0, (48 * unit(random) - 24) / 20); p.wet = std::pow(10.0, (48 * unit(random) - 24) / 20);
            p.output = std::pow(10.0, (36 * unit(random) - 24) / 20); p.mix = unit(random);
            p.glide = 0.5 * unit(random); p.off_decay = unit(random); p.released = unit(random) < 0.3;
            p.limiter = limiter;
          }
          else {
            p.ratio = std::clamp(p.ratio + 0.05 * (unit(random) - 0.5), 0.25, 4.0);
            p.pitch *= std::exp2(0.1 * (unit(random) - 0.5));
          }
          engine.setParameters(p);
          const int kind = (block / 50) % 5;
          for (int i = 0; i < 128; ++i) {
            phase += p.pitch / rate;
            const double sine = std::sin(2.0 * juce::MathConstants<double>::pi * phase);
            const double value = kind == 0 ? 0.0 : kind == 1 ? 2.0 * unit(random) - 1.0
                               : kind == 2 ? sine : kind == 3 ? (i == 0 ? 4.0 : 0.0) : 0.5;
            in_l[i] = float(value); in_r[i] = float(kind == 2 ? -value : value);
          }
          engine.process(in_l.data(), in_r.data(), out_l.data(), out_r.data(), 128);
          for (int i = 0; i < 128; ++i) {
            finite = finite && std::isfinite(out_l[i]) && std::isfinite(out_r[i]);
            const double dry = std::max(std::abs(in_l[i]), std::abs(in_r[i]));
            const double level = std::max(std::abs(out_l[i]), std::abs(out_r[i]));
            // Output gain reaches +12 dB (x4) on dry; the limited wet stays below 0.98.
            bounded = bounded && (limiter ? level <= 4.0 * dry + Engine::kLimiterCeiling + 1e-3 : level < 1.0e4);
          }
        }
        const std::string label = " at " + std::to_string(int(rate)) + (limiter ? " Hz with limiter" : " Hz without limiter");
        expect(finite, "Output stays finite under random extreme settings" + label);
        expect(bounded, "Output stays bounded under random extreme settings" + label);
      }
    }
  }

  void testRandomLfoStereoPhase() {
    for (int shape : { atlas::resonator::kRandomStepped, atlas::resonator::kRandomSmooth }) {
      Parameters p = wetString(220.0);
      p.lfo_shape = shape; p.lfo_amount = 12.0; p.lfo_frequency = 7.0;
      Engine engine;
      prepared(engine, p);
      bool equal = true;
      const auto silence = impulse(32, 0.0f);
      for (int block = 0; block < 1500; ++block) {
        render(engine, silence, 32);
        equal = equal && engine.currentPitch(0) == engine.currentPitch(1);
      }
      p.lfo_stereo_phase = 0.5;
      engine.setParameters(p);
      bool differs = false;
      for (int block = 0; block < 1500; ++block) {
        render(engine, silence, 32);
        differs = differs || engine.currentPitch(0) != engine.currentPitch(1);
      }
      expect(equal, "Random LFO shapes keep both sides equal at 0 degrees Stereo Phase");
      expect(differs, "Random LFO shapes honour Stereo Phase");
    }
  }

  void testGlideKeepsLfoDepth() {
    Parameters p = wetString(220.0);
    p.glide = 0.5; p.lfo_shape = atlas::resonator::kSquare; p.lfo_amount = 12.0; p.lfo_frequency = 4.0;
    Engine engine;
    prepared(engine, p);
    double low = 1e9, high = 0.0;
    const auto silence = impulse(32, 0.0f);
    for (int block = 0; block < 3000; ++block) {
      render(engine, silence, 32);
      low = std::min(low, engine.currentPitch(0)); high = std::max(high, engine.currentPitch(0));
    }
    expect(near(high / low, 4.0, 1e-6), "Glide does not reduce a square LFO's two-octave swing");

    p.lfo_amount = 0.0;
    engine.setParameters(p);
    render(engine, impulse(4800, 0.0f));
    p.pitch = 440.0;
    engine.setParameters(p);
    render(engine, impulse(24000, 0.0f), 32);
    const double octaves = std::log2(engine.currentPitch(0) / 220.0);
    expect(octaves > 0.55 && octaves < 0.7, "Glide still moves between notes over its time constant");
  }

  void testGridModeIdentity() {
    Parameters p = wetString(220.0);
    p.model = atlas::resonator::kMembrane;
    p.ratio = 1.60;
    Engine engine;
    prepared(engine, p);
    render(engine, impulse(2400));
    const auto levels = [&] {
      std::map<std::pair<int, int>, double> result;
      for (int i = 0; i < engine.activeModes(); ++i)
        result[{ engine.modeM(i), engine.modeN(i) }] = engine.modeLevel(0, i);
      return result;
    };
    int first_31 = -1, first_12 = -1;
    for (int i = 0; i < engine.activeModes(); ++i) {
      if (engine.modeM(i) == 3 && engine.modeN(i) == 1) first_31 = i;
      if (engine.modeM(i) == 1 && engine.modeN(i) == 2) first_12 = i;
    }
    const auto before = levels();
    p.ratio = 1.66;
    engine.setParameters(p);
    int second_31 = -1, second_12 = -1;
    for (int i = 0; i < engine.activeModes(); ++i) {
      if (engine.modeM(i) == 3 && engine.modeN(i) == 1) second_31 = i;
      if (engine.modeM(i) == 1 && engine.modeN(i) == 2) second_12 = i;
    }
    expect(first_31 > first_12 && second_31 < second_12, "The test ratios reorder modes (3,1) and (1,2)");
    // Across this change (3,4) leaves the 20 lowest modes and (7,1) enters.
    const auto after = levels();
    int kept = 0;
    bool same = true, entered_silent = true;
    for (const auto& mode : after) {
      const auto previous = before.find(mode.first);
      if (previous != before.end()) { ++kept; same = same && previous->second == mode.second; }
      else entered_silent = entered_silent && mode.second == 0.0;
    }
    expect(kept == 19 && same, "Membrane modes keep their ringing state when Ratio reorders them");
    expect(entered_silent, "A mode entering the selection starts silent");
    expect(before.at({ 1, 2 }) > 100.0 * before.at({ 3, 1 }), "The reordered modes ring at clearly different levels");
  }

  void testIdleSkip() {
    Parameters p = wetString(330.0);
    p.decay = 0.1;
    Engine skipped, fresh;
    prepared(skipped, p);
    render(skipped, impulse(4800));
    const auto silent = render(skipped, impulse(96000, 0.0f));
    bool zero = true;
    for (int i = 48000; i < 96000; ++i) zero = zero && silent.left[i] == 0.0f && silent.right[i] == 0.0f;
    expect(skipped.idle() && zero, "A fully decayed bank goes idle and outputs exact silence");

    prepared(fresh, p);
    const auto input = impulse(9600, 1.0f, 17);
    const auto a = render(skipped, input), b = render(fresh, input);
    double error = 0.0, peak = 0.0;
    for (int i = 0; i < 9600; ++i) {
      error = std::max(error, double(std::abs(a.left[i] - b.left[i])));
      peak = std::max(peak, double(std::abs(b.left[i])));
    }
    expect(peak > 0.0 && error <= peak * 1e-5, "An idle bank wakes with the same response as a fresh one");
  }

  void testEnteringModes() {
    // Raising Quality just before a hit must excite the new upper partials at
    // once, not after the next control tick.
    Parameters p = wetString(100.0);
    p.quality = atlas::resonator::kEco;
    Engine rising, steady;
    prepared(rising, p);
    render(rising, impulse(4800, 0.0f));
    p.quality = atlas::resonator::kHigh;
    rising.setParameters(p);
    prepared(steady, p);
    render(steady, impulse(4800, 0.0f));
    const auto input = impulse(24000, 1.0f, 5);
    const auto a = render(rising, input), b = render(steady, input);
    const double level = goertzel(a.left, 0, 24000, 1500.0, kRate), reference = goertzel(b.left, 0, 24000, 1500.0, kRate);
    expect(reference > 0.0 && near(level / reference, 1.0, 0.1), "Modes added by Quality ring from the next hit");
  }

  void testBlockSizeIndependence() {
    Parameters p = wetString(180.0);
    p.lfo_amount = 5.0; p.lfo_shape = atlas::resonator::kRandomSmooth; p.lfo_frequency = 3.0;
    p.glide = 0.05; p.filter = true; p.bleed = 0.3; p.mix = 0.6; p.limiter = true; p.excitation = 4.0;
    Stereo input { std::vector<float>(20000), std::vector<float>(20000) };
    std::mt19937 random(9);
    std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
    for (int i = 0; i < 20000; ++i) { input.left[i] = unit(random); input.right[i] = unit(random); }
    Engine a, b, c;
    prepared(a, p); prepared(b, p); prepared(c, p);
    const auto one = render(a, input, 1), odd = render(b, input, 97), full = render(c, input, 128);
    expect(one.left == odd.left && odd.left == full.left && one.right == full.right,
           "Output is independent of the host block size");
  }

  void testNoteMonitor() {
    vital::NoteMonitor notes;
    vital::Value range(12.0f), transpose(-3.0f), tune(0.25f);
    notes.setPitchControls(&range, nullptr, nullptr);
    expect(!notes.seen() && !notes.held(), "The monitor starts with no notes");
    notes.noteOn(60, 0); notes.noteOn(64, 1);
    expect(notes.note(vital::NoteMonitor::kLastNote) == 64 && notes.note(vital::NoteMonitor::kLowestNote) == 60,
           "Last and lowest note priorities");
    notes.noteOff(64, 1);
    expect(notes.note(vital::NoteMonitor::kLastNote) == 60, "Releasing the last key returns to a held key");
    notes.noteOff(60, 0);
    expect(!notes.held() && notes.note(vital::NoteMonitor::kLastNote) == 60, "Releasing every key keeps the last pitch");
    notes.noteOn(67, 2); notes.noteOn(67, 2); notes.noteOff(67, 2);
    expect(notes.held(), "A repeated key stays held until each key-down is released");
    notes.noteOff(67, 2);
    notes.sustainOn(3, 3); notes.noteOn(70, 3); notes.noteOff(70, 3);
    expect(notes.held(), "Sustain holds released keys");
    notes.sustainOff(3, 3);
    expect(!notes.held(), "Lifting sustain releases them");
    notes.noteOn(72, 23); notes.noteOn(90, 24);
    expect(notes.note(vital::NoteMonitor::kLastNote) == 72, "Arpeggiator channels count; invalid channels are ignored");
    notes.allNotesOff();
    expect(!notes.held(), "All notes off releases every key");

    // Bends follow VoiceHandler: the zoned wheel of the key's control channel
    // (arpeggiator channels use channel 1) and a 48-semitone MPE bend.
    const auto last = vital::NoteMonitor::kLastNote;
    notes.noteOn(60, 1);
    notes.setZonedPitchWheel(0.5f, 0, 0);
    expect(near(notes.offsetSemitones(last), 0.0, 1e-6), "A wheel on another channel does not bend the key");
    notes.setZonedPitchWheel(0.5f, 1, 1);
    expect(near(notes.offsetSemitones(last), 6.0, 1e-6), "The key's zoned wheel bends by the bend range");
    notes.setLocalPitchWheel(0.25f, 1);
    expect(near(notes.offsetSemitones(last), 18.0, 1e-6), "MPE bend adds a quarter of 48 semitones");
    notes.noteOn(64, 20);
    expect(near(notes.offsetSemitones(last), 6.0, 1e-6), "Arpeggiator keys use the first channel's wheel");
    notes.setPitchControls(&range, &transpose, &tune);
    expect(near(notes.offsetSemitones(last), 3.25, 1e-6), "Voice Transpose and Voice Tune apply");
    notes.setPitchControls(&range, nullptr, nullptr);
    notes.setZonedPitchWheel(0.0f, 0, 15); notes.setLocalPitchWheel(0.0f, 1);

    // All Sound Off releases keys but, like the voices, keeps the pedal down.
    notes.allNotesOff();
    notes.sustainOn(0, 0);
    notes.reset();
    notes.noteOn(62, 0); notes.noteOff(62, 0);
    expect(notes.held(), "Reset keeps the sustain pedal down");
    notes.sustainOff(0, 0);
  }

  struct ModuleFixture {
    vital::Value bpm { 2.0f };
    vital::NoteMonitor notes;
    vital::ResonatorModule module { bpm.output(), &notes };
    vital::control_map controls;
    ModuleFixture(int rate = 48000) {
      module.init(); module.setSampleRate(rate); module.setOversampleAmount(1);
      controls = module.getControls();
      set("model", atlas::resonator::kString); set("mix", 1); set("material", 0); set("glide", 0); set("limiter", 0);
    }
    void set(const std::string& id, float value) { controls.at("resonator_" + id)->set(value); }
    std::vector<vital::poly_float> render(const std::vector<vital::poly_float>& input, bool transport = false,
                                          double start = 0.0) {
      std::vector<vital::poly_float> out(input.size());
      allocations = 0; track_allocations = true;
      for (size_t i = 0; i < input.size(); i += vital::kMaxBufferSize) {
        const int size = static_cast<int>(std::min<size_t>(vital::kMaxBufferSize, input.size() - i));
        if (transport) module.correctToTime(start + double(i) / module.getSampleRate());
        module.processWithInput(input.data() + i, size);
        std::copy_n(module.output()->buffer, size, out.data() + i);
      }
      track_allocations = false;
      expect(allocations == 0, "Resonator processing does not allocate");
      return out;
    }
  };

  std::vector<vital::poly_float> polyImpulse(int count, int at = 0) {
    std::vector<vital::poly_float> input(count, 0.0f);
    input[at] = 1.0f;
    return input;
  }
  std::vector<float> lane(const std::vector<vital::poly_float>& audio, int index) {
    std::vector<float> result(audio.size());
    for (size_t i = 0; i < audio.size(); ++i) result[i] = audio[i][index];
    return result;
  }
  double modulePitch(ModuleFixture& f, double low, double high) {
    f.module.hardReset();
    const auto out = lane(f.render(polyImpulse(48000)), 0);
    return peakFrequency(out, 4800, 24000, low, high, 0.1, kRate);
  }

  void testModulePitch() {
    ModuleFixture f;
    expect(near(modulePitch(f, 120, 140), 130.8128, 0.15), "MIDI Pitch off uses Tune");
    f.set("midi_pitch", 1);
    expect(near(modulePitch(f, 120, 140), 130.8128, 0.15), "MIDI Pitch uses Tune until a key arrives");
    f.notes.noteOn(69, 0);
    expect(near(modulePitch(f, 420, 460), 440.0, 0.15), "MIDI Pitch follows the played key");
    f.set("transpose", 12);
    f.set("fine", 0.5f);
    const double transposed = 880.0 * std::exp2(50.0 / 1200.0);
    expect(near(modulePitch(f, 860, 930), transposed, 0.3), "Transpose and Fine Tune apply to the played key");
    f.set("transpose", 0); f.set("fine", 0);
    vital::Value range(2.0f);
    f.notes.setPitchControls(&range, nullptr, nullptr);
    f.notes.setZonedPitchWheel(1.0f, 0, 15);
    expect(near(modulePitch(f, 470, 520), 440.0 * std::exp2(2.0 / 12.0), 0.2), "Pitch bend uses Atlas's bend range");
    f.notes.setZonedPitchWheel(0.0f, 0, 15);
    Tuning tuning;
    tuning.setReferenceNoteFrequency(69, 432.0f);
    f.notes.setTuning(&tuning);
    expect(near(modulePitch(f, 420, 445), 432.0, 0.15), "MIDI Pitch follows Atlas's loaded tuning");
    f.notes.setTuning(nullptr);

    const auto out = f.render(polyImpulse(4800));
    bool mirrored = true;
    for (const auto& frame : out) mirrored = mirrored && frame[2] == frame[0] && frame[3] == frame[1];
    expect(mirrored, "Lanes 2 and 3 mirror the resonated left and right lanes");

    // Off Decay damps the tail once every key is released.
    f.set("off_decay", 1.0f);
    f.module.hardReset();
    const auto held = lane(f.render(polyImpulse(9600)), 0);
    f.notes.noteOff(69, 0);
    f.module.hardReset();
    const auto released = lane(f.render(polyImpulse(9600)), 0);
    expect(rms(released, 4800, 2400) < 1e-4 * rms(held, 4800, 2400), "Off Decay damps released notes");
  }

  void testTempoSync() {
    // Square LFO, two octaves, synced to quarter notes at 120 BPM. A module
    // that has been running since zero and a fresh module started at the same
    // host time must share the LFO phase, so their responses match.
    const auto configure = [](ModuleFixture& f) {
      f.set("lfo_amount", 12); f.set("lfo_shape", atlas::resonator::kSquare);
      f.set("lfo_sync", 1); f.set("lfo_tempo", 8); f.set("decay", std::log2(0.3));
    };
    ModuleFixture running, fresh, unsynced;
    configure(running); configure(fresh); configure(unsynced);
    const int start = 61440;  // 1.28 s: whole blocks and control periods
    running.render(std::vector<vital::poly_float>(start, 0.0f), true, 0.0);
    const auto a = lane(running.render(polyImpulse(24000), true, start / kRate), 0);
    const auto b = lane(fresh.render(polyImpulse(24000), true, start / kRate), 0);
    const auto c = lane(unsynced.render(polyImpulse(24000), false), 0);
    double difference = 0.0, misaligned = 0.0, level = 0.0;
    for (int i = 0; i < 24000; ++i) {
      difference += double(a[i] - b[i]) * (a[i] - b[i]);
      misaligned += double(a[i] - c[i]) * (a[i] - c[i]);
      level += double(a[i]) * a[i];
    }
    expect(level > 0.0 && difference < 1e-6 * level, "Synced LFO phase follows the host timeline");
    expect(misaligned > 0.1 * level, "Without a host time the synced LFO is not at the timeline phase");
  }

  void testChains() {
    vital::Value bpm(2.0f), keytrack(0.0f);
    vital::NoteMonitor notes;
    for (const std::string prefix : { "", "bus_1_", "bus_2_", "bus_3_" }) {
      vital::ReorderableEffectChain chain(bpm.output(), keytrack.output(), prefix, nullptr, nullptr, nullptr,
                                          nullptr, &notes);
      chain.init();
      chain.setSampleRate(48000);
      auto controls = chain.getControls();
      controls.at(prefix + "fx_slot_1_type")->set(vital::constants::kResonator + 1);
      controls.at(prefix + "fx_slot_1_enabled")->set(1);
      controls.at(prefix + "fx_slot_1_resonator_mix")->set(1);
      controls.at(prefix + "fx_slot_7_type")->set(vital::constants::kResonator + 1);
      controls.at(prefix + "fx_slot_7_enabled")->set(1);
      controls.at(prefix + "fx_slot_7_resonator_mix")->set(0);
      std::vector<vital::poly_float> input(128, 0.0f);
      input[3] = 0.5f;
      chain.processWithInput(input.data(), 128);
      double energy = 0.0;
      for (int i = 0; i < 128; ++i) energy += std::abs(chain.output()->buffer[i][0] - input[i][0]);
      expect(energy > 0.0 && std::isfinite(energy), "Resonator runs in chain '" + prefix + "'");
      controls.at(prefix + "fx_slot_1_enabled")->set(0);
      chain.processWithInput(input.data(), 128);
      bool dry = true;
      for (int i = 0; i < 128; ++i) dry = dry && chain.output()->buffer[i][0] == input[i][0];
      expect(dry, "Bypassing the Resonator slot returns dry audio in chain '" + prefix + "'");
    }
  }

  void testPluginFollowsMidi() {
    SynthPlugin synth;
    synth.setPlayConfigDetails(0, 2, 48000, 128);
    synth.prepareToPlay(48000, 128);
    auto& controls = synth.getControls();
    // The test binary has no factory wavetables: excite with a short PCM burst.
    std::vector<float> pcm(48000, 0.0f);
    std::mt19937 random(5);
    std::uniform_real_distribution<float> unit(-0.4f, 0.4f);
    for (int i = 0; i < 2400; ++i) pcm[i] = unit(random);
    synth.getEngine()->getSample(slot_ids::kSampleSlot)->loadSample(pcm.data(), 48000, 48000);
    for (int osc = 1; osc <= 4; ++osc) controls.at("osc_" + std::to_string(osc) + "_on")->set(0);
    synth.parameterChanged("fx_slot_1_type", vital::constants::kResonator + 1);
    const std::pair<std::string, float> settings[] = {
      { "volume", 6400 }, { slot_ids::id("sample_on"), 1 }, { slot_ids::id("sample_level"), 0.5f }, { slot_ids::id("sample_loop"), 0 },
      { slot_ids::id("sample_destination"), vital::constants::kEffects }, { slot_ids::id("sample_random_phase"), 0 },
      { "env_1_attack", 0.001f }, { "env_1_decay", 0.001f }, { "env_1_sustain", 1 }, { "env_1_release", 0.001f },
      { "fx_slot_1_enabled", 1 }, { "fx_slot_2_enabled", 0 }, { "fx_slot_1_resonator_mix", 1 },
      { "fx_slot_1_resonator_midi_pitch", 1 }, { "fx_slot_1_resonator_model", atlas::resonator::kString },
      { "fx_slot_1_resonator_transpose", 7 }, { "fx_slot_1_resonator_decay", 1 },
      { "fx_slot_1_resonator_material", 0 }, { "fx_slot_1_resonator_limiter", 0 }, { "pitch_bend_range", 2 }
    };
    for (const auto& setting : settings) controls.at(setting.first)->set(setting.second);
    for (int chain = 1; chain <= vital::kNumBuses; ++chain)
      for (int slot = 1; slot <= 2; ++slot)
        controls.at("bus_" + std::to_string(chain) + "_fx_slot_" + std::to_string(slot) + "_enabled")->set(0);

    AudioBuffer<float> audio(2, 128);
    MidiBuffer midi;
    std::vector<float> tail;
    const auto play = [&](int blocks, bool keep) {
      for (int block = 0; block < blocks; ++block) {
        audio.clear();
        synth.processBlock(audio, midi);
        midi.clear();
        if (keep) tail.insert(tail.end(), audio.getReadPointer(0), audio.getReadPointer(0) + 128);
      }
    };
    const auto noteTail = [&](int note, float wheel) {
      midi.addEvent(MidiMessage::pitchWheel(1, juce::jlimit(0, 16383, int(8192 + wheel * 8191))), 0);
      midi.addEvent(MidiMessage::noteOn(1, note, 0.8f), 1);
      play(40, false);
      midi.addEvent(MidiMessage::noteOff(1, note), 0);
      play(40, false);
      tail.clear();
      play(94, true);
      return peakFrequency(tail, 0, 12000, 600.0, 800.0, 0.25, kRate);
    };
    const double fifth = 440.0 * std::exp2(7.0 / 12.0);
    expect(near(noteTail(69, 0.0f), fifth, 0.6), "Atlas's played notes set the Resonator pitch in the plugin");
    expect(near(noteTail(69, 1.0f), fifth * std::exp2(2.0 / 12.0), 0.8), "Atlas's pitch wheel and bend range bend the Resonator");
  }
}

int main() {
  juce::ScopedJuceInitialiser_GUI juce;
  testModeTables();
  testPitchAndDecay();
  testMixAndLimiter();
  testStress();
  testRandomLfoStereoPhase();
  testGlideKeepsLfoDepth();
  testGridModeIdentity();
  testIdleSkip();
  testEnteringModes();
  testBlockSizeIndependence();
  testNoteMonitor();
  testModulePitch();
  testTempoSync();
  testChains();
  testPluginFollowsMidi();
  std::cout << (failures ? "Resonator tests failed: " : "Resonator tests passed") << (failures ? std::to_string(failures) : "") << '\n';
  return failures ? 1 : 0;
}
