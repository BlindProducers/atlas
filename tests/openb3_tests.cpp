// OpenB3 tests: the tonewheel organ oscillator type, the Scanner Vibrato
// effect and Analogue Rotary's OpenB3 Whirl model, through the engine and the
// plugin (pitch, drawbars, percussion, key click, envelope modes, shared
// wheels, host state, editor), and, when the opt-in reference renderer is
// built, the port against OpenB3's own Beatrix engine.

#include "JuceHeader.h"
#include "synth_plugin.h"
#include "synth_editor.h"
#include "parameter_slider.h"
#include "sound_engine.h"
#include "source_types.h"
#include "source_slot_module.h"
#include "analogue_rotary_module.h"
#include "scanner_vibrato_module.h"
#include "openb3_source.h"
#include "analogue/rotary_engine.h"
#include "openb3/scanner_vibrato.h"
#include "openb3/tonewheel_organ.h"
#include "openb3/tonewheel_tables.h"
#include "openb3/whirl.h"
#include "synth_strings.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {
  namespace openb3 = atlas::openb3;
  namespace rotary = atlas::analogue::rotary;
  constexpr double kPi = 3.14159265358979323846;

  int failures = 0;

  void expect(bool condition, const std::string& message) {
    if (!condition) {
      std::cerr << "FAIL: " << message << '\n';
      ++failures;
    }
  }

  void close(double actual, double expected, const std::string& message, double tolerance) {
    expect(std::isfinite(actual) && std::abs(actual - expected) <= tolerance,
           message + " (actual=" + std::to_string(actual) + ", expected=" + std::to_string(expected) + ")");
  }

  int finish(const char* name) {
    if (failures == 0)
      std::cout << name << ": all tests passed\n";
    return failures == 0 ? 0 : 1;
  }

  struct MidiEvent {
    int sample;
    juce::MidiMessage message;
  };

  void setControl(SynthPlugin& plugin, const std::string& name, float value) {
    auto& controls = plugin.getControls();
    auto found = controls.find(name);
    expect(found != controls.end(), "control exists: " + name);
    if (found != controls.end())
      plugin.valueChanged(name, value);
  }

  float control(SynthPlugin& plugin, const std::string& name) {
    auto& controls = plugin.getControls();
    auto found = controls.find(name);
    return found == controls.end() ? -1.0f : found->second->value();
  }

  constexpr int kRate = 48000;
  constexpr int kBlock = 128;

  // Slot 1 is an OpenB3 oscillator to `destination`, every effect and other
  // oscillator off, Env 1 open with no release. Offline, so effects prepare
  // within the block that selects them.
  void preparePlayback(SynthPlugin& plugin, int rate = kRate, int destination = vital::constants::kEffects) {
    plugin.setNonRealtime(true);
    plugin.setPlayConfigDetails(0, 2, rate, kBlock);
    plugin.prepareToPlay(rate, kBlock);
    setControl(plugin, "oversampling", 0.0f);
    for (int chain = 0; chain <= vital::kNumBuses; ++chain) {
      const auto prefix = chain == 0 ? std::string() : "bus_" + std::to_string(chain) + "_";
      for (int i = 1; i <= vital::constants::kMaxEffectSlots; ++i)
        setControl(plugin, prefix + "fx_slot_" + std::to_string(i) + "_type", 0.0f);
    }
    for (int i = 1; i <= vital::kNumOscillators; ++i)
      setControl(plugin, "osc_" + std::to_string(i) + "_on", 0.0f);
    plugin.parameterChanged("osc_1_type", static_cast<float>(vital::source_types::kOpenB3));
    setControl(plugin, "osc_1_on", 1.0f);
    setControl(plugin, "osc_1_destination", static_cast<float>(destination));
    setControl(plugin, "env_1_attack", 0.0f);
    setControl(plugin, "env_1_sustain", 1.0f);
    setControl(plugin, "env_1_release", 0.0f);
    plugin.checkOversampling();
    expect(plugin.getEngine()->getSourceSlot(0)->activeType() == vital::source_types::kOpenB3,
           "slot 1 runs the OpenB3 type");
  }

  void selectEffect(SynthPlugin& plugin, const std::string& slot_prefix, int effect) {
    plugin.valueChanged(slot_prefix + "type", static_cast<float>(effect + 1));
    plugin.valueChanged(slot_prefix + "enabled", 1.0f);
    plugin.getEngine()->prepareSelectedEffects();
    juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
  }

  // Renders `samples` frames with MIDI events at given samples.
  std::vector<float> render(SynthPlugin& plugin, int samples, const std::vector<MidiEvent>& events,
                            std::vector<float>* right = nullptr) {
    std::vector<float> result(static_cast<size_t>(samples));
    if (right != nullptr)
      right->assign(static_cast<size_t>(samples), 0.0f);
    for (int offset = 0; offset < samples;) {
      const int count = std::min(kBlock, samples - offset);
      juce::AudioBuffer<float> audio(2, count);
      audio.clear();
      juce::MidiBuffer midi;
      for (const MidiEvent& event : events) {
        if (event.sample >= offset && event.sample < offset + count)
          midi.addEvent(event.message, event.sample - offset);
      }
      plugin.processBlock(audio, midi);
      for (int i = 0; i < count; ++i) {
        result[static_cast<size_t>(offset + i)] = audio.getSample(0, i);
        if (right != nullptr)
          (*right)[static_cast<size_t>(offset + i)] = audio.getSample(1, i);
      }
      offset += count;
    }
    return result;
  }

  double rms(const std::vector<float>& signal, int start, int end) {
    double sum = 0.0;
    end = std::min<int>(end, static_cast<int>(signal.size()));
    for (int i = start; i < end; ++i)
      sum += double(signal[static_cast<size_t>(i)]) * signal[static_cast<size_t>(i)];
    return end > start ? std::sqrt(sum / (end - start)) : 0.0;
  }

  double peak(const std::vector<float>& signal) {
    double result = 0.0;
    for (float value : signal)
      result = std::max(result, double(std::abs(value)));
    return result;
  }

  bool finite(const std::vector<float>& signal) {
    for (float value : signal) {
      if (!std::isfinite(value))
        return false;
    }
    return true;
  }

  double db(double ratio) {
    return 20.0 * std::log10(std::max(1.0e-30, ratio));
  }

  double midiToHz(double note) {
    return 440.0 * std::pow(2.0, (note - 69.0) / 12.0);
  }

  // Amplitude of the component at `hz` over [start, end) (Goertzel, Hann windowed).
  double toneAmplitude(const std::vector<float>& signal, int start, int end, int rate, double hz) {
    const double w = 2.0 * kPi * hz / rate;
    const double coefficient = 2.0 * std::cos(w);
    double s1 = 0.0, s2 = 0.0, window_sum = 0.0;
    const int length = end - start;
    for (int i = 0; i < length; ++i) {
      const double window = 0.5 - 0.5 * std::cos(2.0 * kPi * i / (length - 1));
      window_sum += window;
      const double s0 = window * signal[static_cast<size_t>(start + i)] + coefficient * s1 - s2;
      s2 = s1;
      s1 = s0;
    }
    const double power = s1 * s1 + s2 * s2 - coefficient * s1 * s2;
    return 2.0 * std::sqrt(std::max(0.0, power)) / window_sum;
  }

  // The period with the strongest normalised autocorrelation between
  // 1/max_hz and 1/min_hz, refined by parabolic interpolation, as a frequency.
  double pitch(const std::vector<float>& signal, int start, int end, int rate, double min_hz, double max_hz) {
    const int min_lag = static_cast<int>(rate / max_hz);
    const int max_lag = static_cast<int>(rate / min_hz) + 1;
    std::vector<double> scores(static_cast<size_t>(max_lag + 2), 0.0);
    for (int lag = min_lag - 1; lag <= max_lag + 1; ++lag) {
      double sum = 0.0, energy_a = 0.0, energy_b = 0.0;
      for (int i = start; i + lag < end; ++i) {
        const double a = signal[static_cast<size_t>(i)];
        const double b = signal[static_cast<size_t>(i + lag)];
        sum += a * b;
        energy_a += a * a;
        energy_b += b * b;
      }
      scores[static_cast<size_t>(lag)] = sum / std::sqrt(std::max(1.0e-30, energy_a * energy_b));
    }
    int best = min_lag;
    for (int lag = min_lag; lag <= max_lag; ++lag) {
      if (scores[static_cast<size_t>(lag)] > scores[static_cast<size_t>(best)])
        best = lag;
    }
    const double left = scores[static_cast<size_t>(best - 1)];
    const double middle = scores[static_cast<size_t>(best)];
    const double right = scores[static_cast<size_t>(best + 1)];
    const double denominator = left - 2.0 * middle + right;
    const double shift = denominator != 0.0 ? 0.5 * (left - right) / denominator : 0.0;
    return rate / (best + shift);
  }

  const char* const kDrawbars[] = { "drawbar_16", "drawbar_5_1_3", "drawbar_8", "drawbar_4", "drawbar_2_2_3",
                                    "drawbar_2", "drawbar_1_3_5", "drawbar_1_1_3", "drawbar_1" };
  // Each drawbar's pitch against the key's (8'), in tempered semitones: the
  // tonewheels are tuned to the scale, so the fifths and thirds are tempered.
  const int kDrawbarSemitones[] = { -12, 7, 0, 12, 19, 24, 28, 31, 36 };

  void setDrawbars(SynthPlugin& plugin, const std::string& registration) {
    for (int d = 0; d < 9; ++d)
      setControl(plugin, std::string("osc_1_b3_") + kDrawbars[d], static_cast<float>(registration[static_cast<size_t>(d)] - '0'));
  }

  // One note through a fresh plugin with the given registration and controls.
  std::vector<float> renderNote(int note, const std::string& registration,
                                const std::vector<std::pair<std::string, float>>& controls = {},
                                int samples = kRate, int release = -1) {
    SynthPlugin plugin;
    preparePlayback(plugin);
    setDrawbars(plugin, registration);
    for (const auto& [name, value] : controls)
      setControl(plugin, "osc_1_b3_" + name, value);
    std::vector<MidiEvent> events { { 0, juce::MidiMessage::noteOn(1, note, 1.0f) } };
    if (release >= 0)
      events.push_back({ release, juce::MidiMessage::noteOff(1, note) });
    return render(plugin, samples, events);
  }

  // ---- The engine on its own --------------------------------------------------

  // The wiring model's landmarks, the fragment length, and the organ's clock
  // and key compression.
  int selfTest() {
    const openb3::Tables& tables = openb3::tables();
    // Key 0 (C2): 8' is wheel 13 (65.4 Hz); 16' folds back up to the same wheel.
    const auto level = [&](int key, int wheel, int bus) {
      for (const auto& c : tables.contributions[static_cast<size_t>(key)]) {
        if (c.wheel == wheel && c.bus == bus) return c.level;
      }
      return 0.0f;
    };
    // (Wheel 1 still reaches it at -40 dB, as wheel 13's neighbour on the terminal strip.)
    expect(level(0, 13, 2) > 0.3f && level(0, 13, 0) > 0.2f && level(0, 1, 0) < 0.01f,
           "C2's 16' folds back onto its 8' wheel");
    expect(level(12, 13, 0) > 0.3f, "C3's 16' is the wheel an octave below (C2's 8')");
    // Key 60 (C7): 1' would be wheel 109, folded down into the top octave.
    bool top_folded = false;
    for (const auto& c : tables.contributions[60])
      top_folded = top_folded || (c.bus == 8 && c.level > 0.1f && c.wheel > 79 && c.wheel <= 91);
    expect(top_folded, "C7's 1' folds down into the top octave");
    size_t most = 0;
    for (const auto& list : tables.contributions) {
      int wheels = 0, last = -1;
      for (const auto& c : list) {
        if (c.wheel != last) ++wheels;
        last = c.wheel;
      }
      most = std::max(most, static_cast<size_t>(wheels));
    }
    expect(most <= static_cast<size_t>(openb3::kMaxWheelsPerKey), "every key fits a voice's wheels");
    close(tables.frequencies[openb3::kGear60][10], 55.0, "wheel 10 is A at 55 Hz", 1e-9);
    close(1200.0 * std::log2(tables.frequencies[openb3::kGear60][13] / 65.40639), 0.0, "C2 is within a cent", 1.0);
    close(tables.frequencies[openb3::kEqual][13], 65.406391325149658, "equal temperament C2", 1e-9);
    expect(tables.key_compression[0] == 1.0f && tables.key_compression[1] == 1.0f &&
           tables.key_compression[10] < tables.key_compression[2], "key compression lowers the level for more keys");

    expect(openb3::fragmentLength(48000.0) == 64 && openb3::fragmentLength(96000.0) == 128 &&
           openb3::fragmentLength(44100.0) == 59, "a fragment is 64 samples at 48 kHz");

    // The clock: blocks advance it, key compression chases its target over a fragment.
    openb3::Organ organ;
    organ.prepare(48000.0);
    organ.beginBlock(100, 0, 4);
    expect(organ.time() == 0.0, "the clock starts at 0");
    close(organ.keyCompression()[0], 1.0, "key compression starts at unity", 1e-9);
    close(organ.keyCompression()[64], tables.key_compression[4], "it reaches the table's value after a fragment", 1e-5);
    organ.beginBlock(50, 4, 4);
    expect(organ.time() == 100.0, "the clock moves by the block");

    // Percussion: armed with no key held, single trigger otherwise.
    organ.beginBlock(64, 0, 1);
    close(organ.percussionEnvelope()[0], 11.0, "percussion re-arms at osc.perc.gain", 1e-6);
    organ.keyPressed(10);
    close(organ.percussionEnvelope()[10], 11.0, "a key starts the percussion where it lands", 1e-6);
    expect(organ.percussionEnvelope()[63] < organ.percussionEnvelope()[10], "and it decays");
    const float carried = organ.percussionEnvelope()[63];
    organ.beginBlock(64, 1, 2);
    organ.keyPressed(0);
    expect(organ.percussionEnvelope()[0] < carried, "a key played legato does not restart it");
    return finish("self");
  }

  // ---- The oscillator ---------------------------------------------------------

  // Every key sounds at its pitch (the 8' drawbar alone), bounded, also beyond
  // the manual's range.
  int oscillator() {
    for (int note : { 60, 30, 36, 45, 53, 60, 72, 84, 96, 101 }) {
      const auto audio = renderNote(note, "008000000", {}, kRate / 2);
      expect(finite(audio), "finite output, note " + std::to_string(note));
      expect(rms(audio, 0, kRate / 2) > 1.0e-3, "note " + std::to_string(note) + " sounds");
      const double expected = midiToHz(note);
      const double measured = pitch(audio, kRate / 10, kRate / 2, kRate, expected / 1.5, expected * 1.5);
      const double cents = 1200.0 * std::log2(measured / expected);
      std::cout << "note " << note << ": " << measured << " Hz (" << cents << " cents)\n";
      // Gear tuning stays within a cent or two of equal temperament.
      close(cents, 0.0, "note " + std::to_string(note) + " is in tune", 3.0);
    }
    // The defaults: 88 6000 000, one key peaking near 0.9 of full scale before the slot level.
    const auto full = renderNote(60, "886000000", {}, kRate / 2);
    std::cout << "default registration peak " << peak(full) << '\n';
    expect(peak(full) > 0.1 && peak(full) < 1.5, "the default registration is at a sensible level");

    // Transpose and tune move the pitch; the nearest key gives the wiring.
    const auto transposed = renderNote(60, "008000000", { { "transpose", 7.0f }, { "tune", 0.5f } }, kRate / 2);
    const double expected = midiToHz(67.5);
    close(1200.0 * std::log2(pitch(transposed, kRate / 10, kRate / 2, kRate, expected / 1.5, expected * 1.5) / expected),
          0.0, "transpose and tune move the organ", 3.0);

    // Equal temperament: exactly A440 for A4.
    const auto equal = renderNote(69, "008000000", { { "temperament", 2.0f } }, kRate / 2);
    close(1200.0 * std::log2(pitch(equal, kRate / 10, kRate / 2, kRate, 400.0, 480.0) / 440.0), 0.0,
          "equal temperament plays A440", 0.2);

    // Rendering is deterministic.
    expect(renderNote(57, "886000000", {}, kRate / 4) == renderNote(57, "886000000", {}, kRate / 4),
           "the same note renders the same");
    return finish("oscillator");
  }

  // Each drawbar adds its harmonic; levels follow the drawbar linearly; all in is silent.
  int drawbars() {
    const int note = 60;
    const double f0 = midiToHz(note);
    const auto unison = renderNote(note, "008000000", {}, kRate / 2);
    const double unison_level = toneAmplitude(unison, kRate / 10, kRate / 2, kRate, f0);
    for (int d = 0; d < 9; ++d) {
      std::string registration = "000000000";
      registration[static_cast<size_t>(d)] = '8';
      const auto audio = renderNote(note, registration, {}, kRate / 2);
      const double harmonic = toneAmplitude(audio, kRate / 10, kRate / 2, kRate, f0 * std::exp2(kDrawbarSemitones[d] / 12.0));
      const double others = toneAmplitude(audio, kRate / 10, kRate / 2, kRate, f0 * (d == 2 ? 2.0 : 1.0));
      std::cout << kDrawbars[d] << ": harmonic " << db(harmonic / unison_level) << " dB against 8', "
                << db(others / harmonic) << " dB leakage\n";
      expect(harmonic > 0.2 * unison_level, std::string(kDrawbars[d]) + " sounds its harmonic");
      expect(others < 0.05 * harmonic, std::string(kDrawbars[d]) + " is mostly its own harmonic (crosstalk is -40 dB)");
    }
    const auto half = renderNote(note, "004000000", {}, kRate / 2);
    close(db(toneAmplitude(half, kRate / 10, kRate / 2, kRate, f0) / unison_level), db(0.5),
          "a drawbar at 4 is half the level of 8", 0.05);
    const auto silent = renderNote(note, "000000000", {}, kRate / 4);
    expect(peak(silent) == 0.0, "all drawbars in is silent");

    // The lowest octave's 16' folds back: C2's 16' sounds at C2, not C1.
    const auto folded = renderNote(36, "800000000", {}, kRate / 2);
    expect(toneAmplitude(folded, kRate / 10, kRate / 2, kRate, midiToHz(36)) >
               20.0 * toneAmplitude(folded, kRate / 10, kRate / 2, kRate, midiToHz(24)),
           "the bottom octave's 16' folds back an octave");
    return finish("drawbars");
  }

  // Percussion: the chosen harmonic decays at its rate, is single-trigger,
  // mutes the 1' drawbar and, at Normal volume, lowers the drawbars.
  int percussion() {
    const int note = 60;
    const double f0 = midiToHz(note);
    const auto third = renderNote(note, "008000000", { { "percussion", 1.0f } }, kRate);
    const double early = toneAmplitude(third, 64, 64 + 2400, kRate, 3.0 * f0);
    const double late = toneAmplitude(third, kRate / 2, kRate, kRate, 3.0 * f0);
    std::cout << "third percussion: " << db(early / late) << " dB decay to 0.5-1 s\n";
    expect(early > 0.2 * toneAmplitude(third, kRate / 2, kRate, kRate, f0) && early > 30.0 * late,
           "Fast third percussion strikes and dies away");
    const auto second = renderNote(note, "008000000", { { "percussion", 1.0f }, { "percussion_harmonic", 0.0f } }, kRate);
    expect(toneAmplitude(second, 64, 64 + 2400, kRate, 2.0 * f0) > 10.0 * toneAmplitude(second, 64, 64 + 2400, kRate, 3.0 * f0),
           "Second percussion sounds the octave");
    const auto slow = renderNote(note, "008000000", { { "percussion", 1.0f }, { "percussion_decay", 1.0f } }, kRate);
    expect(toneAmplitude(slow, kRate / 4, 9 * kRate / 20, kRate, 3.0 * f0) >
               10.0 * toneAmplitude(third, kRate / 4, 9 * kRate / 20, kRate, 3.0 * f0), "Slow percussion lasts longer");
    const auto soft = renderNote(note, "008000000", { { "percussion", 1.0f }, { "percussion_volume", 1.0f } }, kRate);
    close(db(toneAmplitude(soft, 64, 64 + 2400, kRate, 3.0 * f0) / early), db(0.5012 / 0.60512), "Soft percussion is quieter, without the drawbar drop", 0.5);

    // Normal percussion lowers the drawbars to 0.60512; Soft leaves them.
    const auto plain = renderNote(note, "008000000", {}, kRate);
    close(db(toneAmplitude(third, kRate / 2, kRate, kRate, f0) / toneAmplitude(plain, kRate / 2, kRate, kRate, f0)),
          db(0.60512), "Normal percussion lowers the drawbars", 0.02);
    close(db(toneAmplitude(soft, kRate / 2, kRate, kRate, f0) / toneAmplitude(plain, kRate / 2, kRate, kRate, f0)),
          0.0, "Soft percussion leaves the drawbars", 0.02);

    // The 1' drawbar is the trigger: silent while percussion is on.
    const auto one_foot = renderNote(note, "000000008", { { "percussion", 1.0f } }, kRate);
    expect(rms(one_foot, kRate / 2, kRate) < 1e-3 * rms(renderNote(note, "000000008", {}, kRate), kRate / 2, kRate),
           "the 1' drawbar is silent with percussion on");

    // Single trigger: a key played while another is held gets no percussion;
    // played after the first is released, it does.
    const auto play = [&](bool legato) {
      SynthPlugin plugin;
      preparePlayback(plugin);
      setDrawbars(plugin, "008000000");
      setControl(plugin, "osc_1_b3_percussion", 1.0f);
      std::vector<MidiEvent> events { { 0, juce::MidiMessage::noteOn(1, 60, 1.0f) },
                                      { kRate / 2, juce::MidiMessage::noteOn(1, 67, 1.0f) } };
      if (!legato)
        events.push_back({ kRate / 2 - 4 * kBlock, juce::MidiMessage::noteOff(1, 60) });
      const auto audio = render(plugin, kRate, events);
      return toneAmplitude(audio, kRate / 2 + 64, kRate / 2 + 64 + 2400, kRate, 3.0 * midiToHz(67));
    };
    const double detached = play(false), legato = play(true);
    std::cout << "second key's percussion: detached " << detached << ", legato " << legato << '\n';
    expect(detached > 20.0 * legato, "percussion is single-trigger");
    return finish("percussion");
  }

  // Organ mode: the key shapes the note and ends it at once. Envelope 1 mode:
  // Env 1's release carries it. The key click follows its control.
  int envelope() {
    const int release = kRate / 4;
    const auto organ = renderNote(60, "886000000", {}, kRate / 2, release);
    expect(rms(organ, release - 4800, release) > 0.05, "the note sounds while held");
    expect(rms(organ, release + kRate / 50, kRate / 2) < 1e-6, "Organ mode stops within milliseconds of the release");
    {
      SynthPlugin plugin;
      preparePlayback(plugin);
      setControl(plugin, "osc_1_b3_amp_env", 1.0f);
      setControl(plugin, "env_1_release", 1.0f);
      const auto audio = render(plugin, kRate / 2, { { 0, juce::MidiMessage::noteOn(1, 60, 1.0f) },
                                                    { release, juce::MidiMessage::noteOff(1, 60) } });
      expect(rms(audio, release + kRate / 50, release + kRate / 10) > 0.01, "Envelope 1 mode follows Env 1's release");
    }
    // Organ mode ignores a slow Env 1 attack: the contacts close at once.
    {
      SynthPlugin plugin;
      preparePlayback(plugin);
      setControl(plugin, "env_1_attack", 1.0f);
      const auto audio = render(plugin, kRate / 4, { { 0, juce::MidiMessage::noteOn(1, 60, 1.0f) } });
      expect(rms(audio, 256, 2400) > 0.05, "Organ mode sounds at once whatever Env 1's attack");
    }
    // Key click: the attack's first fragment differs with the click level;
    // the rest of the note does not.
    const auto clean = renderNote(60, "886000000", { { "key_click", 0.0f } }, kRate / 8);
    const auto worn = renderNote(60, "886000000", { { "key_click", 1.0f } }, kRate / 8);
    double attack_difference = 0.0, steady_difference = 0.0;
    for (int i = 0; i < 128; ++i)
      attack_difference = std::max(attack_difference, double(std::abs(clean[static_cast<size_t>(i)] - worn[static_cast<size_t>(i)])));
    for (size_t i = 512; i < clean.size(); ++i)
      steady_difference = std::max(steady_difference, double(std::abs(clean[i] - worn[i])));
    expect(attack_difference > 1e-3 && steady_difference < 1e-6, "the key click shapes only the attack");
    return finish("envelope");
  }

  // Two keys sharing a wheel hear it in phase: C4's 8' and C3's 4' are the
  // same wheel, so together they add up (less key compression's 1.16 dB).
  int coherence() {
    const double f = midiToHz(60);
    const auto upper = renderNote(60, "008000000", {}, kRate / 2);
    const auto lower = renderNote(48, "000800000", {}, kRate / 2);
    SynthPlugin plugin;
    preparePlayback(plugin);
    setDrawbars(plugin, "008800000");
    const auto both = render(plugin, kRate / 2, { { 0, juce::MidiMessage::noteOn(1, 60, 1.0f) },
                                                  { 3001, juce::MidiMessage::noteOn(1, 48, 1.0f) } });
    // C3 on its own, with 8' and 4' out: subtract its 8' wheel's share.
    const double a = toneAmplitude(upper, kRate / 10, kRate / 2, kRate, f);
    const double b = toneAmplitude(lower, kRate / 10, kRate / 2, kRate, f);
    const double together = toneAmplitude(both, kRate / 10, kRate / 2, kRate, f);
    const double compression = openb3::tables().key_compression[2];
    std::cout << "shared wheel: " << a << " + " << b << " -> " << together << " (coherent "
              << (a + b) * compression << ", random " << std::sqrt(a * a + b * b) * compression << ")\n";
    close(together / ((a + b) * compression), 1.0, "a shared wheel adds in phase", 0.02);
    return finish("coherence");
  }

  // ---- Parameters -----------------------------------------------------------------

  int parameters() {
    juce::MemoryBlock state;
    const char* const suffixes[] = { "amp_env", "drawbar_16", "drawbar_5_1_3", "drawbar_8", "drawbar_4",
                                     "drawbar_2_2_3", "drawbar_2", "drawbar_1_3_5", "drawbar_1_1_3", "drawbar_1",
                                     "percussion", "percussion_volume", "percussion_decay", "percussion_harmonic",
                                     "key_click", "temperament", "transpose", "tune" };
    {
      SynthPlugin plugin;
      preparePlayback(plugin);
      for (int slot = 1; slot <= vital::kNumOscillators; ++slot) {
        const std::string prefix = "osc_" + std::to_string(slot) + "_b3_";
        for (const char* suffix : suffixes) {
          expect(plugin.getControls().count(prefix + suffix) == 1, "control exists: " + prefix + suffix);
          expect(vital::Parameters::getDetails(prefix + suffix).version_added == 0x02011d,
                 "new host version: " + prefix + suffix);
        }
      }
      expect(control(plugin, "osc_2_b3_drawbar_16") == 8.0f && control(plugin, "osc_2_b3_drawbar_5_1_3") == 8.0f &&
             control(plugin, "osc_2_b3_drawbar_8") == 6.0f && control(plugin, "osc_2_b3_drawbar_4") == 0.0f &&
             control(plugin, "osc_2_b3_drawbar_1") == 0.0f, "the drawbars default to OpenB3's 88 6000 000");
      expect(control(plugin, "osc_2_b3_percussion") == 0.0f && control(plugin, "osc_2_b3_percussion_volume") == 0.0f &&
             control(plugin, "osc_2_b3_percussion_decay") == 0.0f && control(plugin, "osc_2_b3_percussion_harmonic") == 1.0f,
             "percussion defaults to off, Normal, Fast, Third");
      close(control(plugin, "osc_2_b3_key_click"), 0.5, "key click defaults to 50%", 1e-6);
      expect(control(plugin, "osc_2_b3_temperament") == 0.0f && control(plugin, "osc_2_b3_amp_env") == 0.0f,
             "Gear 60 Hz tuning and the Organ envelope by default");
      for (int chain = 0; chain <= vital::kNumBuses; ++chain) {
        const std::string chain_prefix = chain == 0 ? "" : "bus_" + std::to_string(chain) + "_";
        expect(plugin.getControls().count(chain_prefix + "scanner_vibrato_on") == 1,
               "control exists: " + chain_prefix + "scanner_vibrato_on");
        for (int slot = 1; slot <= vital::constants::kMaxEffectSlots; ++slot) {
          const std::string prefix = chain_prefix + "fx_slot_" + std::to_string(slot) + "_scanner_vibrato_";
          for (const char* suffix : { "mode", "rate" })
            expect(plugin.getControls().count(prefix + suffix) == 1, "control exists: " + prefix + suffix);
        }
      }
      expect(vital::Parameters::getDetails("effect_chain_slot_39").version_added == 0x02011d &&
             vital::Parameters::getDetails("scanner_vibrato_mode").version_added == 0x02011d,
             "the scanner's parameters take a new host version");
      expect(strings::kEffectOrder[vital::constants::kScannerVibrato] == "scanner_vibrato" &&
             strings::kEffectSlotTypeNames[vital::constants::kScannerVibrato + 1] == "Scanner Vibrato" &&
             strings::kOscillatorTypeNames[vital::source_types::kOpenB3] == "OpenB3" &&
             strings::kAnalogueRotaryModelNames[rotary::kWhirl] == "OpenB3 Whirl", "names are registered");
      expect(vital::Parameters::getDetails("analoguerotary_model").max == rotary::kNumModels - 1,
             "Analogue Rotary offers the Whirl");
      expect(control(plugin, "fx_slot_1_scanner_vibrato_mode") == 5.0f, "the scanner defaults to C3");
      close(control(plugin, "fx_slot_1_scanner_vibrato_rate"), 7.25, "the scanner turns at 7.25 Hz", 1e-6);

      setControl(plugin, "osc_1_b3_amp_env", 1.0f);
      setDrawbars(plugin, "838000418");
      setControl(plugin, "osc_1_b3_percussion", 1.0f);
      setControl(plugin, "osc_1_b3_percussion_volume", 1.0f);
      setControl(plugin, "osc_1_b3_percussion_decay", 1.0f);
      setControl(plugin, "osc_1_b3_percussion_harmonic", 0.0f);
      setControl(plugin, "osc_1_b3_key_click", 0.8f);
      setControl(plugin, "osc_1_b3_temperament", 1.0f);
      setControl(plugin, "osc_1_b3_transpose", -12.0f);
      setControl(plugin, "osc_1_b3_tune", 0.25f);
      selectEffect(plugin, "bus_2_fx_slot_7_", vital::constants::kScannerVibrato);
      setControl(plugin, "bus_2_fx_slot_7_scanner_vibrato_mode", 2.0f);
      setControl(plugin, "bus_2_fx_slot_7_scanner_vibrato_rate", 6.5f);
      selectEffect(plugin, "fx_slot_4_", vital::constants::kAnalogueRotary);
      setControl(plugin, "fx_slot_4_analoguerotary_model", 2.0f);
      plugin.getStateInformation(state);
    }
    {
      SynthPlugin plugin;
      preparePlayback(plugin);
      plugin.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
      plugin.checkOversampling();
      expect(plugin.getEngine()->getSourceSlot(0)->activeType() == vital::source_types::kOpenB3,
             "the restored slot runs OpenB3");
      expect(control(plugin, "osc_1_b3_amp_env") == 1.0f && control(plugin, "osc_1_b3_drawbar_16") == 8.0f &&
             control(plugin, "osc_1_b3_drawbar_5_1_3") == 3.0f && control(plugin, "osc_1_b3_drawbar_1_3_5") == 4.0f &&
             control(plugin, "osc_1_b3_drawbar_1") == 8.0f, "the drawbars are restored");
      expect(control(plugin, "osc_1_b3_percussion") == 1.0f && control(plugin, "osc_1_b3_percussion_volume") == 1.0f &&
             control(plugin, "osc_1_b3_percussion_decay") == 1.0f && control(plugin, "osc_1_b3_percussion_harmonic") == 0.0f,
             "percussion is restored");
      close(control(plugin, "osc_1_b3_key_click"), 0.8, "key click is restored", 1e-6);
      expect(control(plugin, "osc_1_b3_temperament") == 1.0f && control(plugin, "osc_1_b3_transpose") == -12.0f,
             "tuning and transpose are restored");
      close(control(plugin, "osc_1_b3_tune"), 0.25, "tune is restored", 1e-6);
      expect(control(plugin, "bus_2_fx_slot_7_type") == vital::constants::kScannerVibrato + 1 &&
             control(plugin, "bus_2_fx_slot_7_scanner_vibrato_mode") == 2.0f, "the scanner slot is restored");
      close(control(plugin, "bus_2_fx_slot_7_scanner_vibrato_rate"), 6.5, "the scanner rate is restored", 1e-6);
      expect(control(plugin, "fx_slot_4_analoguerotary_model") == 2.0f, "the Whirl model is restored");
      const auto audio = render(plugin, kRate / 4, { { 0, juce::MidiMessage::noteOn(1, 60, 1.0f) } });
      expect(finite(audio) && rms(audio, 0, kRate / 4) > 1.0e-3, "the restored patch plays");
    }
    return finish("parameters");
  }

  // ---- The scanner ------------------------------------------------------------------

  std::vector<float> sine(int samples, double hz, double amplitude, int rate = kRate) {
    std::vector<float> signal(static_cast<size_t>(samples));
    for (int i = 0; i < samples; ++i)
      signal[static_cast<size_t>(i)] = static_cast<float>(amplitude * std::sin(2.0 * kPi * hz * i / rate));
    return signal;
  }

  // The deepest swing of a vibrato'd sine's instantaneous frequency, in cents.
  double vibratoDepth(const std::vector<float>& signal, double hz, int rate) {
    std::vector<double> crossings;
    for (size_t i = static_cast<size_t>(rate / 10); i + 1 < signal.size(); ++i) {
      if (signal[i] <= 0.0f && signal[i + 1] > 0.0f)
        crossings.push_back(i + signal[i] / (signal[i] - signal[i + 1]));
    }
    double deepest = 0.0;
    for (size_t i = 1; i < crossings.size(); ++i)
      deepest = std::max(deepest, std::abs(1200.0 * std::log2(rate / (crossings[i] - crossings[i - 1]) / hz)));
    return deepest;
  }

  int scanner() {
    // V1-V3 swing deeper in turn; the depth is the same time at 96 kHz.
    const auto input = sine(kRate, 1000.0, 0.5);
    double depths[3];
    for (int mode = 0; mode < 3; ++mode) {
      openb3::ScannerVibrato scanner;
      scanner.prepare(kRate);
      scanner.setMode(mode);
      std::vector<float> out(input.size());
      const float* in[] = { input.data() };
      float* outs[] = { out.data() };
      scanner.process(in, outs, 1, static_cast<int>(input.size()));
      depths[mode] = vibratoDepth(out, 1000.0, kRate);
    }
    std::cout << "V1/V2/V3 at 1 kHz: +-" << depths[0] << " / " << depths[1] << " / " << depths[2] << " cents\n";
    expect(depths[0] > 2.0 && depths[1] > 1.6 * depths[0] && depths[2] > 1.3 * depths[1], "V1 to V3 deepen");
    {
      const auto fast_input = sine(2 * kRate, 1000.0, 0.5, 2 * kRate);
      openb3::ScannerVibrato scanner;
      scanner.prepare(2 * kRate);
      scanner.setMode(openb3::kVibrato3);
      std::vector<float> out(fast_input.size());
      const float* in[] = { fast_input.data() };
      float* outs[] = { out.data() };
      scanner.process(in, outs, 1, static_cast<int>(fast_input.size()));
      close(vibratoDepth(out, 1000.0, 2 * kRate) / depths[2], 1.0, "V3 is as deep at 96 kHz", 0.1);
    }
    // C modes mix the vibrato with the dry signal at -3 dB each.
    {
      openb3::ScannerVibrato vibrato, chorus;
      vibrato.setMode(openb3::kVibrato2);
      chorus.setMode(openb3::kChorus2);
      std::vector<float> v(input.size()), c(input.size());
      const float* in[] = { input.data() };
      float* vo[] = { v.data() };
      float* co[] = { c.data() };
      vibrato.process(in, vo, 1, static_cast<int>(input.size()));
      chorus.process(in, co, 1, static_cast<int>(input.size()));
      double worst = 0.0;
      for (size_t i = 0; i < input.size(); ++i)
        worst = std::max(worst, std::abs(c[i] - (input[i] + v[i]) * 0.7071067811865475));
      expect(worst < 1e-6, "chorus is the vibrato mixed with the dry signal");
    }
    // The channels run separately.
    {
      openb3::ScannerVibrato scanner;
      std::vector<float> silent(input.size(), 0.0f), left(input.size()), right(input.size());
      const float* in[] = { input.data(), silent.data() };
      float* outs[] = { left.data(), right.data() };
      scanner.process(in, outs, 2, static_cast<int>(input.size()));
      expect(peak(right) == 0.0 && peak(left) > 0.1, "the channels do not mix");
    }
    // In the plugin: the effect runs in main and bus chains and moves the sound.
    for (int chain = 0; chain <= 1; ++chain) {
      SynthPlugin plugin;
      preparePlayback(plugin, kRate, chain == 0 ? vital::constants::kEffects : vital::constants::kBus1);
      setDrawbars(plugin, "008000000");
      const std::string slot = (chain == 0 ? std::string() : "bus_1_") + "fx_slot_2_";
      const auto dry = render(plugin, kRate / 2, { { 0, juce::MidiMessage::noteOn(1, 72, 1.0f) } });
      SynthPlugin wet_plugin;
      preparePlayback(wet_plugin, kRate, chain == 0 ? vital::constants::kEffects : vital::constants::kBus1);
      setDrawbars(wet_plugin, "008000000");
      selectEffect(wet_plugin, slot, vital::constants::kScannerVibrato);
      setControl(wet_plugin, slot + "scanner_vibrato_mode", 2.0f);
      const auto wet = render(wet_plugin, kRate / 2, { { 0, juce::MidiMessage::noteOn(1, 72, 1.0f) } });
      expect(finite(wet) && rms(wet, kRate / 10, kRate / 2) > 0.5 * rms(dry, kRate / 10, kRate / 2),
             "the scanner passes the organ, chain " + std::to_string(chain));
      std::cout << "chain " << chain << ": vibrato depth dry " << vibratoDepth(dry, midiToHz(72), kRate) << ", V3 "
                << vibratoDepth(wet, midiToHz(72), kRate) << " cents\n";
      // The organ's -40 dB crosstalk already wobbles the zero crossings a little.
      expect(vibratoDepth(wet, midiToHz(72), kRate) > vibratoDepth(dry, midiToHz(72), kRate) + 8.0,
             "the scanner bends the pitch, chain " + std::to_string(chain));
    }
    return finish("scanner");
  }

  // ---- The Whirl ------------------------------------------------------------------

  int whirl() {
    const auto run = [](rotary::Engine& engine, double seconds, float* left_out = nullptr) {
      std::vector<float> noise(128), left(128), right(128);
      uint32_t random = 1;
      for (int block = 0; block < static_cast<int>(seconds * 48000.0 / 128); ++block) {
        for (float& sample : noise) {
          random = random * 1664525u + 1013904223u;
          sample = static_cast<float>((random >> 9) / 8388608.0 - 1.0) * 0.1f;
        }
        engine.process(noise.data(), noise.data(), left.data(), right.data(), 128);
        if (left_out != nullptr)
          *left_out = std::max(*left_out, std::abs(left[0]));
      }
    };
    rotary::Engine engine;
    engine.prepare(48000.0);
    rotary::Parameters p;
    p.model = rotary::kWhirl;
    engine.setParameters(p);
    engine.reset();
    // setBfree keeps the speeds in RPM as floats.
    close(engine.hornHz(), 0.672, "the Whirl starts turning at its slow speed (horn)", 1e-6);
    close(engine.drumHz(), 0.6, "the Whirl starts turning at its slow speed (drum)", 1e-6);
    p.speed = rotary::kFast;
    engine.setParameters(p);
    double horn_time = -1.0, drum_time = -1.0;
    for (int step = 1; step <= 400; ++step) {
      run(engine, 0.1);
      if (horn_time < 0.0 && engine.hornHz() > 0.9 * 7.056) horn_time = step * 0.1;
      if (drum_time < 0.0 && engine.drumHz() > 0.9 * 5.955) drum_time = step * 0.1;
    }
    std::cout << "Whirl to 90% of fast: horn " << horn_time << " s, drum " << drum_time << " s\n";
    close(engine.hornHz(), 7.056, "the horn settles at setBfree's fast speed", 1e-6);
    close(engine.drumHz(), 5.955, "the drum settles at setBfree's fast speed", 1e-6);
    expect(horn_time > 0.0 && horn_time < 1.0 && drum_time > 5.0, "the horn spins up fast, the drum slowly");
    p.speed = rotary::kStop;
    engine.setParameters(p);
    run(engine, 20.0);
    expect(engine.hornHz() == 0.0 && engine.drumHz() == 0.0, "Stop brings both rotors to rest");
    p.inertia = 0.0;
    p.speed = rotary::kFast;
    engine.setParameters(p);
    run(engine, 0.01);
    close(engine.hornHz(), 7.056, "without inertia the rotors change speed at once", 1e-6);

    // Drive does nothing on the Whirl; balance and spread do.
    const auto render_with = [](rotary::Parameters parameters) {
      rotary::Engine e;
      e.prepare(48000.0);
      e.setParameters(parameters);
      e.reset();
      const auto input = sine(48000, 330.0, 0.3);
      std::vector<float> left(input.size()), right(input.size());
      for (size_t i = 0; i < input.size(); i += 128)
        e.process(input.data() + i, input.data() + i, left.data() + i, right.data() + i, 128);
      left.insert(left.end(), right.begin(), right.end());
      return left;
    };
    rotary::Parameters base;
    base.model = rotary::kWhirl;
    base.spread = 1.0;
    rotary::Parameters driven = base;
    driven.drive = 1.0;
    expect(render_with(base) == render_with(driven), "Drive does not apply to the Whirl");
    rotary::Parameters drum_only = base;
    drum_only.balance = -1.0;
    rotary::Parameters narrow = base;
    narrow.spread = 0.2;
    expect(render_with(drum_only) != render_with(base) && render_with(narrow) != render_with(base),
           "Balance and Spread move the Whirl");
    const auto out = render_with(base);
    expect(finite(out) && peak(out) < 2.0 && peak(out) > 0.05, "the Whirl renders bounded audio");
    return finish("whirl");
  }

  // ---- Editor ------------------------------------------------------------------------

  Component* findTitled(Component& parent, const std::function<bool(const String&)>& match) {
    if (match(parent.getTitle()))
      return &parent;
    for (auto* child : parent.getChildren()) {
      if (auto* found = findTitled(*child, match))
        return found;
    }
    return nullptr;
  }

  Component* titledExactly(Component& parent, const String& title) {
    return findTitled(parent, [&title](const String& text) { return text == title; });
  }

  Component* sectionNamed(Component& parent, const String& name) {
    if (parent.getProperties()["SectionName"].toString() == name)
      return &parent;
    for (auto* child : parent.getChildren()) {
      if (auto* found = sectionNamed(*child, name))
        return found;
    }
    return nullptr;
  }

  Component* effectSlotComponent(Component& parent, const String& section, int slot) {
    if (parent.getProperties()["SectionName"].toString() == section &&
        parent.getProperties().contains("FxSlot") && static_cast<int>(parent.getProperties()["FxSlot"]) == slot)
      return &parent;
    for (auto* child : parent.getChildren()) {
      if (auto* found = effectSlotComponent(*child, section, slot))
        return found;
    }
    return nullptr;
  }

  bool selectEditorItem(Component& editor, const String& title, const String& text) {
    auto* selector = dynamic_cast<ComboBox*>(titledExactly(editor, title));
    if (selector == nullptr)
      return false;
    for (int i = 0; i < selector->getNumItems(); ++i) {
      if (selector->getItemText(i) == text) {
        selector->setSelectedItemIndex(i, sendNotificationSync);
        return true;
      }
    }
    return false;
  }

  ParameterSlider* sliderTitled(Component& parent, const String& title) {
    if (auto* slider = dynamic_cast<ParameterSlider*>(&parent); slider != nullptr && slider->getTitle() == title)
      return slider;
    for (auto* child : parent.getChildren()) {
      if (auto* found = sliderTitled(*child, title))
        return found;
    }
    return nullptr;
  }

  String valueText(ParameterSlider* slider) {
    if (slider == nullptr)
      return {};
    auto handler = slider->createAccessibilityHandler();
    auto* value = handler != nullptr ? handler->getValueInterface() : nullptr;
    return value != nullptr ? value->getCurrentValueAsString() : String();
  }

  template <typename Predicate>
  void pumpUntil(Predicate done, int timeout_ms = 5000) {
    const uint32 start = juce::Time::getMillisecondCounter();
    while (!done() && juce::Time::getMillisecondCounter() - start < static_cast<uint32>(timeout_ms))
      juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
    juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
  }

  int editorTest() {
    SynthPlugin synth;
    synth.parameterChanged("osc_1_type", static_cast<float>(vital::source_types::kOpenB3));
    synth.valueChanged("fx_slot_2_type", static_cast<float>(vital::constants::kScannerVibrato + 1));
    synth.valueChanged("fx_slot_2_enabled", 1.0f);
    synth.valueChanged("fx_slot_3_type", static_cast<float>(vital::constants::kAnalogueRotary + 1));
    synth.valueChanged("fx_slot_3_enabled", 1.0f);
    synth.valueChanged("fx_slot_3_analoguerotary_model", static_cast<float>(rotary::kWhirl));
    SynthEditor editor(synth);
    editor.updateFullGui();
    pumpUntil([] { return false; }, 200);

    auto* section = sectionNamed(editor, "Oscillator 1");
    expect(section != nullptr, "Oscillator 1 section exists");
    if (section != nullptr) {
      auto* envelope = sliderTitled(*section, "1 OpenB3 Amplitude Envelope");
      expect(envelope != nullptr && valueText(envelope) == "Organ" && envelope->getWantsKeyboardFocus() &&
             envelope->getDescription().contains("key click"),
             "the envelope row reads its mode and is described (" + valueText(envelope).toStdString() + ")");
      const char* const titles[] = { "1 OpenB3 Drawbar 16 Foot", "1 OpenB3 Drawbar 5 1/3 Foot", "1 OpenB3 Drawbar 8 Foot",
                                     "1 OpenB3 Drawbar 4 Foot", "1 OpenB3 Drawbar 2 2/3 Foot", "1 OpenB3 Drawbar 2 Foot",
                                     "1 OpenB3 Drawbar 1 3/5 Foot", "1 OpenB3 Drawbar 1 1/3 Foot", "1 OpenB3 Drawbar 1 Foot" };
      int previous_y = -1;
      for (const char* title : titles) {
        auto* row = sliderTitled(*section, title);
        expect(row != nullptr && row->getDescription().contains("drawbar") && row->getWantsKeyboardFocus(),
               std::string("a described drawbar row: ") + title);
        if (row != nullptr) {
          expect(row->getScreenY() > previous_y, std::string("drawbars in the organ's order: ") + title);
          previous_y = row->getScreenY();
        }
      }
      expect(valueText(sliderTitled(*section, "1 OpenB3 Drawbar 16 Foot")) == "8" &&
             valueText(sliderTitled(*section, "1 OpenB3 Drawbar 8 Foot")) == "6",
             "drawbars read their stop (" + valueText(sliderTitled(*section, "1 OpenB3 Drawbar 8 Foot")).toStdString() + ")");
      for (const auto& [title, value] : std::vector<std::pair<const char*, const char*>> {
               { "1 OpenB3 Percussion Volume", "Normal" }, { "1 OpenB3 Percussion Decay", "Fast" },
               { "1 OpenB3 Percussion Harmonic", "Third" }, { "1 OpenB3 Key Click", "50%" },
               { "1 OpenB3 Tuning", "Gear 60 Hz" } }) {
        auto* row = sliderTitled(*section, title);
        expect(row != nullptr && valueText(row) == value && row->getDescription().isNotEmpty(),
               std::string("a described row reading its value: ") + title + " (" + valueText(row).toStdString() + ")");
      }
      auto* percussion = titledExactly(*section, "1 OpenB3 Percussion");
      expect(percussion != nullptr && percussion->getDescription().contains("single-trigger"), "a percussion switch");
      expect(findTitled(*section, [](const String& t) { return t.contains("OpenWurli") || t.contains("Wave Frame"); }) == nullptr,
             "other types' rows are hidden");
    }

    expect(selectEditorItem(editor, "Editor group", "Effects") && selectEditorItem(editor, "Editor element", "Chain and routing"),
           "the effect chain page opens");
    pumpUntil([] { return false; }, 200);
    auto* slot = effectSlotComponent(editor, "Chain and routing", 1);
    expect(slot != nullptr && slot->getTitle().contains("Scanner Vibrato"), "the slot is titled after the scanner");
    if (slot != nullptr) {
      auto* mode = sliderTitled(*slot, "Mode");
      auto* rate = sliderTitled(*slot, "Rate");
      expect(mode != nullptr && rate != nullptr, "the scanner's rows");
      if (mode != nullptr && rate != nullptr) {
        expect(mode->getScreenY() < rate->getScreenY(), "rows in order: Mode, Rate");
        expect(valueText(mode) == "C3" && valueText(rate).startsWith("7.25"),
               "values read (" + valueText(mode).toStdString() + ", " + valueText(rate).toStdString() + ")");
        expect(mode->getDescription().contains("chorus") && rate->getDescription().contains("hertz"), "the rows are described");
      }
    }
    auto* rotary_slot = effectSlotComponent(editor, "Chain and routing", 2);
    expect(rotary_slot != nullptr, "the rotary slot");
    if (rotary_slot != nullptr) {
      auto* model = titledExactly(*rotary_slot, "Model");
      auto* model_menu = dynamic_cast<ComboBox*>(model);
      const String model_text = model_menu != nullptr ? model_menu->getText()
                              : valueText(dynamic_cast<ParameterSlider*>(model));
      expect(model_text == "OpenB3 Whirl", "the rotary reads the Whirl model (" + model_text.toStdString() + ")");
      expect(sliderTitled(*rotary_slot, "Drive") == nullptr, "the Whirl hides Drive");
      expect(sliderTitled(*rotary_slot, "Balance") != nullptr && sliderTitled(*rotary_slot, "Spread") != nullptr,
             "the Whirl shows Balance and Spread");
    }
    return finish("editor");
  }

  // ---- Against the original ----------------------------------------------------------

  std::string scratchDirectory() {
    if (const char* kept = std::getenv("ATLAS_OPENB3_SCRATCH"))
      return kept;
    const juce::File directory = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("atlas-openb3-tests");
    directory.createDirectory();
    return directory.getFullPathName().toStdString();
  }

  bool run(const std::string& reference, const std::vector<std::string>& arguments) {
    std::string command = "\"" + reference + "\"";
    for (const std::string& argument : arguments)
      command += " \"" + argument + "\"";
    const bool ok = std::system(command.c_str()) == 0;
    expect(ok, "the reference runs: " + command);
    return ok;
  }

  std::vector<float> readFloats(const std::string& path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    const auto size = static_cast<size_t>(stream.tellg());
    std::vector<float> data(size / sizeof(float));
    stream.seekg(0);
    stream.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size() * sizeof(float)));
    return data;
  }

  void writeFloats(const std::string& path, const std::vector<float>& data) {
    std::ofstream(path, std::ios::binary)
        .write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size() * sizeof(float)));
  }

  // The wiring, frequencies and key compression, bit for bit.
  int wiringReference(const std::string& reference) {
    const std::string directory = scratchDirectory();
    if (!run(reference, { "wiring", directory + "/wiring.txt" }))
      return finish("wiring reference");
    std::ifstream wiring(directory + "/wiring.txt");
    std::vector<std::vector<openb3::Contribution>> expected(openb3::kNumKeys);
    int key = 0, wheel = 0, bus = 0;
    std::string level;
    while (wiring >> key >> wheel >> bus >> level)
      expected[static_cast<size_t>(key)].push_back({ static_cast<short>(wheel), static_cast<short>(bus),
                                                     std::strtof(level.c_str(), nullptr) });
    int mismatches = 0, entries = 0;
    for (int k = 0; k < openb3::kNumKeys; ++k) {
      const auto& actual = openb3::tables().contributions[static_cast<size_t>(k)];
      const auto& wanted = expected[static_cast<size_t>(k)];
      expect(actual.size() == wanted.size(), "key " + std::to_string(k) + " has as many contributions");
      for (size_t i = 0; i < std::min(actual.size(), wanted.size()); ++i) {
        ++entries;
        if (actual[i].wheel != wanted[i].wheel || actual[i].bus != wanted[i].bus ||
            std::memcmp(&actual[i].level, &wanted[i].level, sizeof(float)) != 0)
          ++mismatches;
      }
    }
    std::cout << "wiring: " << entries << " contributions, " << mismatches << " differ\n";
    expect(entries > 5000 && mismatches == 0, "every key's wiring is bit-identical");

    for (double rate : { 44100.0, 48000.0 }) {
      if (!run(reference, { "tables", std::to_string(rate), directory + "/tables.txt" }))
        continue;
      std::ifstream tables(directory + "/tables.txt");
      std::string kind;
      int frequency_mismatches = 0, compression_mismatches = 0;
      while (tables >> kind) {
        if (kind == "frequency") {
          int temperament = 0;
          std::string hz, loop;
          tables >> temperament >> wheel >> hz >> loop;
          if (openb3::tables().frequencies[static_cast<size_t>(temperament)][static_cast<size_t>(wheel)] !=
              std::strtod(hz.c_str(), nullptr))
            ++frequency_mismatches;
        }
        else {
          std::string value;
          tables >> key >> value;
          if (openb3::tables().key_compression[static_cast<size_t>(key)] != static_cast<float>(std::strtod(value.c_str(), nullptr)))
            ++compression_mismatches;
        }
      }
      expect(frequency_mismatches == 0, "every wheel frequency is bit-identical");
      expect(compression_mismatches == 0, "the key compression table is bit-identical");
    }
    return finish("wiring reference");
  }

  struct Setup {
    const char* drawbars;
    const char* percussion;  // enabled soft fast first(second harmonic)
  };

  openb3::Registration registrationFor(const Setup& setup) {
    openb3::Registration registration;
    for (int d = 0; d < 9; ++d)
      registration.drawbars[static_cast<size_t>(d)] = static_cast<float>(setup.drawbars[d] - '0');
    registration.percussion = setup.percussion[0] == '1';
    registration.percussion_soft = setup.percussion[1] == '1';
    registration.percussion_fast = setup.percussion[2] == '1';
    registration.percussion_third = setup.percussion[3] != '1';
    return registration;
  }

  const Setup kSetups[] = {
    { "886000000", "0011" }, { "888888888", "0010" }, { "008000000", "1010" }, { "838000418", "1110" },
    { "888000000", "1001" }, { "800000008", "1100" }, { "006050403", "0011" },
  };

  // Each wheel's gains through the drawbars and to the percussion, bit for bit
  // for single keys; chords sum per key here and per wheel in tonegen.c.
  int gainsReference(const std::string& reference) {
    const std::string directory = scratchDirectory();
    int compared = 0;
    for (const Setup& setup : kSetups) {
      const openb3::Registration registration = registrationFor(setup);
      for (const std::vector<int>& keys : std::vector<std::vector<int>> { { 0 }, { 7 }, { 24 }, { 36 }, { 47 }, { 60 },
                                                                          { 12, 24 }, { 24, 28, 31, 36 } }) {
        std::string list;
        for (int key : keys)
          list += (list.empty() ? "" : ",") + std::to_string(key);
        const std::string out = directory + "/gains.txt";
        if (!run(reference, { "gains", "48000", setup.drawbars, setup.percussion, list, out }))
          continue;
        std::map<int, std::pair<double, double>> wanted;
        std::ifstream stream(out);
        int wheel = 0;
        std::string swell, percussion;
        while (stream >> wheel >> swell >> percussion)
          wanted[wheel] = { std::strtod(swell.c_str(), nullptr), std::strtod(percussion.c_str(), nullptr) };

        openb3::Organ organ;
        organ.prepare(48000.0);
        std::vector<float> envelopes(openb3::kNumClickEnvelopes * 64, 1.0f);
        std::map<int, std::pair<double, double>> actual;
        for (int key : keys) {
          openb3::Voice voice;
          voice.noteOn(key, 0.0, registration, envelopes.data(), organ, openb3::tables().frequencies[0].data());
          for (int i = 0; i < voice.numWheels(); ++i) {
            actual[voice.wheelNumber(i)].first += voice.swellGain(i);
            actual[voice.wheelNumber(i)].second += voice.percussionGain(i);
          }
        }
        const std::string label = std::string(setup.drawbars) + "/" + setup.percussion + " keys " + list;
        expect(actual.size() == wanted.size(), label + ": the same wheels sound");
        for (const auto& [number, gains] : wanted) {
          const auto found = actual.find(number);
          if (found == actual.end())
            continue;
          ++compared;
          if (keys.size() == 1) {
            expect(static_cast<float>(found->second.first) == static_cast<float>(gains.first) &&
                   static_cast<float>(found->second.second) == static_cast<float>(gains.second),
                   label + ": wheel " + std::to_string(number) + " is bit-identical");
          }
          else {
            close(found->second.first, gains.first, label + ": wheel " + std::to_string(number) + " swell", 1e-6);
            close(found->second.second, gains.second, label + ": wheel " + std::to_string(number) + " percussion", 1e-6);
          }
        }
      }
    }
    std::cout << "gains: " << compared << " wheels compared\n";
    return finish("gains reference");
  }

  struct Event {
    int fragment;
    bool on;
    int key;
  };

  // Atlas's organ core against oscGenerateFragment, with tonegen.c's loop
  // frequencies and attack envelopes: fragment-aligned events, as tonegen.c
  // applies them. The Atlas output is divided by tonegen.c's 0.07 output trim.
  // Returns the residual's RMS relative to the reference's over fragments
  // from `from` on.
  double compareTonegen(const std::string& reference, const Setup& setup, const std::vector<Event>& events,
                        int fragments, int from, bool same_start) {
    const std::string directory = scratchDirectory();
    std::string list;
    for (const Event& event : events)
      list += (list.empty() ? "" : ",") + std::to_string(event.fragment) + (event.on ? ":on:" : ":off:") +
              std::to_string(event.key);
    if (!run(reference, { "tonegen", "48000", setup.drawbars, setup.percussion, std::to_string(fragments),
                          list.empty() ? "-" : list, directory + "/tonegen.f32", directory + "/envelopes.f32" }) ||
        !run(reference, { "tables", "48000", directory + "/loops.txt" }))
      return 1e9;
    const auto expected = readFloats(directory + "/tonegen.f32");
    const auto envelopes = readFloats(directory + "/envelopes.f32");
    std::vector<double> loops(openb3::kNumWheels + 1, 0.0);
    {
      std::ifstream stream(directory + "/loops.txt");
      std::string kind, hz, loop;
      int temperament = 0, wheel = 0;
      while (stream >> kind) {
        if (kind != "frequency") {
          std::getline(stream, kind);
          continue;
        }
        stream >> temperament >> wheel >> hz >> loop;
        if (temperament == 0)
          loops[static_cast<size_t>(wheel)] = std::strtod(loop.c_str(), nullptr);
      }
    }

    const openb3::Registration registration = registrationFor(setup);
    openb3::Organ organ;
    organ.prepare(48000.0);
    organ.setPercussion(registration.percussion_soft, registration.percussion_fast);
    std::map<int, openb3::Voice> voices;
    std::vector<double> block(64);
    std::vector<float> actual;
    int keys_down = 0;
    for (int fragment = 0; fragment < fragments; ++fragment) {
      const int before = keys_down;
      for (const Event& event : events) {
        if (event.fragment == fragment)
          keys_down += event.on ? 1 : -1;
      }
      organ.beginBlock(64, before, keys_down);
      for (const Event& event : events) {
        if (event.fragment != fragment)
          continue;
        if (event.on) {
          // tonegen.c starts every wheel from its loop's beginning when it
          // first sounds; for keys pressed together that is the same moment.
          voices[event.key].noteOn(event.key, same_start ? 0.0 : organ.time(), registration, envelopes.data(), organ,
                                   loops.data());
          organ.keyPressed(0);
        }
        else
          voices[event.key].noteOff();
      }
      std::fill(block.begin(), block.end(), 0.0);
      for (auto& [key, voice] : voices)
        voice.render(organ, block.data(), 0, 64);
      for (double sample : block)
        actual.push_back(static_cast<float>(sample * 0.07));
    }
    double error = 0.0, energy = 0.0;
    for (size_t i = static_cast<size_t>(from) * 64; i < std::min(actual.size(), expected.size()); ++i) {
      error += (actual[i] - expected[i]) * (actual[i] - expected[i]);
      energy += expected[i] * expected[i];
    }
    return energy > 0.0 ? std::sqrt(error / energy) : std::sqrt(error);
  }

  int tonegenReference(const std::string& reference) {
    // Single keys from the click on: the same wheels, gains, envelopes and
    // phases. The residual is tonegen.c's one-bit dither in its wave tables
    // and the seam where each loop restarts.
    for (const Setup& setup : kSetups) {
      for (int key : { 0, 13, 30, 47, 60 }) {
        const double residual = compareTonegen(reference, setup, { { 1, true, key }, { 300, false, key } }, 320, 1, true);
        std::cout << "tonegen " << setup.drawbars << "/" << setup.percussion << " key " << key << ": residual "
                  << db(residual) << " dB\n";
        expect(residual < 0.01, std::string("key ") + std::to_string(key) + " matches tonegen.c (" + setup.drawbars + ")");
      }
    }
    // A chord pressed together, from the fragment after its clicks: key
    // compression and shared wheels.
    const double chord = compareTonegen(reference, kSetups[0],
                                        { { 1, true, 24 }, { 1, true, 28 }, { 1, true, 31 }, { 1, true, 36 },
                                          { 200, false, 28 } }, 260, 2, true);
    std::cout << "tonegen chord: residual " << db(chord) << " dB\n";
    expect(chord < 0.01, "a chord matches tonegen.c");
    // Percussion through legato playing, percussion alone (drawbars in): the
    // second key enters while the first is held, so no new strike; the third
    // comes after both are released and strikes again. tonegen.c starts the
    // later keys' wheels at other phases, so the comparison is of energy over
    // 40 fragments after each key.
    {
      const Setup setup { "000000000", "1010" };
      const std::vector<Event> events { { 1, true, 12 }, { 100, true, 19 }, { 200, false, 12 }, { 260, false, 19 },
                                        { 300, true, 26 } };
      const std::string directory = scratchDirectory();
      compareTonegen(reference, setup, events, 400, 1, false);
      const auto expected = readFloats(directory + "/tonegen.f32");
      openb3::Organ organ;
      organ.prepare(48000.0);
      const openb3::Registration registration = registrationFor(setup);
      organ.setPercussion(registration.percussion_soft, registration.percussion_fast);
      std::map<int, openb3::Voice> voices;
      std::vector<float> envelopes(openb3::kNumClickEnvelopes * 64, 1.0f);
      std::vector<double> actual;
      int keys_down = 0;
      for (int fragment = 0; fragment < 400; ++fragment) {
        const int before = keys_down;
        for (const Event& event : events) {
          if (event.fragment == fragment)
            keys_down += event.on ? 1 : -1;
        }
        organ.beginBlock(64, before, keys_down);
        for (const Event& event : events) {
          if (event.fragment != fragment)
            continue;
          if (event.on) {
            voices[event.key].noteOn(event.key, organ.time(), registration, envelopes.data(), organ,
                                     openb3::tables().frequencies[0].data());
            organ.keyPressed(0);
          }
          else
            voices[event.key].noteOff();
        }
        double block[64] = {};
        for (auto& [key, voice] : voices)
          voice.render(organ, block, 0, 64);
        for (double sample : block)
          actual.push_back(sample * 0.07);
      }
      const auto energy = [](const auto& signal, int fragment) {
        double sum = 0.0;
        for (int i = fragment * 64; i < (fragment + 40) * 64; ++i)
          sum += double(signal[static_cast<size_t>(i)]) * signal[static_cast<size_t>(i)];
        return std::sqrt(sum / (40 * 64));
      };
      for (int fragment : { 2, 101, 301 }) {
        const double a = energy(actual, fragment), e = energy(expected, fragment);
        std::cout << "percussion after fragment " << fragment << ": Atlas " << db(a) << " dB, tonegen.c " << db(e) << " dB\n";
        close(db(a), db(e), "percussion level after fragment " + std::to_string(fragment), 1.0);
      }
      expect(energy(expected, 101) < 0.1 * energy(expected, 2), "tonegen.c does not strike a legato key");
    }
    return finish("tonegen reference");
  }

  std::vector<float> testSignal(int samples, int rate) {
    // Noise, a sweep and silence, so the delay lines see every state.
    std::vector<float> signal(static_cast<size_t>(samples));
    uint32_t random = 7;
    double phase = 0.0;
    for (int i = 0; i < samples; ++i) {
      random = random * 1664525u + 1013904223u;
      const double noise = (random >> 9) / 8388608.0 - 1.0;
      const double hz = 40.0 * std::pow(2.0, 9.0 * i / samples);
      phase += 2.0 * kPi * hz / rate;
      const double t = static_cast<double>(i) / samples;
      signal[static_cast<size_t>(i)] =
          static_cast<float>(t < 0.4 ? 0.3 * noise : t < 0.9 ? 0.5 * std::sin(phase) : 0.0);
    }
    return signal;
  }

  int scannerReference(const std::string& reference) {
    const std::string directory = scratchDirectory();
    const int rate = 48000;
    const auto input = testSignal(rate * 2, rate);
    writeFloats(directory + "/scanner_in.f32", input);
    const int selects[] = { 0x01, 0x02, 0x03, 0x81, 0x82, 0x83 };
    for (int mode = 0; mode < openb3::kNumScannerModes; ++mode) {
      for (double hz : { 7.25, 4.0, 13.3 }) {
        char select[16];
        std::snprintf(select, sizeof(select), "%d", selects[mode]);
        char rate_text[32];
        std::snprintf(rate_text, sizeof(rate_text), "%.17g", hz);
        if (!run(reference, { "scanner", std::to_string(rate), select, rate_text, directory + "/scanner_in.f32",
                              directory + "/scanner_out.f32" }))
          continue;
        const auto expected = readFloats(directory + "/scanner_out.f32");
        openb3::ScannerVibrato scanner;
        scanner.prepare(rate);
        scanner.setMode(mode);
        scanner.setFrequency(hz);
        std::vector<float> actual(input.size());
        // Uneven blocks: the port does not depend on them.
        for (size_t offset = 0; offset < input.size();) {
          const int count = static_cast<int>(std::min<size_t>(37 + offset % 91, input.size() - offset));
          const float* in[] = { input.data() + offset };
          float* out[] = { actual.data() + offset };
          scanner.process(in, out, 1, count);
          offset += static_cast<size_t>(count);
        }
        size_t differ = 0;
        for (size_t i = 0; i < actual.size(); ++i)
          differ += std::memcmp(&actual[i], &expected[i], sizeof(float)) != 0;
        expect(expected.size() == actual.size() && differ == 0,
               std::string(strings::kScannerVibratoModeNames[mode]) + " at " + rate_text + " Hz is bit-identical (" +
               std::to_string(differ) + " samples differ)");
      }
    }
    return finish("scanner reference");
  }

  // Atlas's speed order: slow, fast, stop. setRevSelect's: slow, stop, fast.
  int revSelect(int speed) {
    return speed == openb3::Whirl::kSlow ? 0 : speed == openb3::Whirl::kStop ? 1 : 2;
  }

  int whirlReference(const std::string& reference) {
    const std::string directory = scratchDirectory();
    for (int rate : { 44100, 48000, 96000 }) {
      const auto input = testSignal(rate * 6, rate);
      writeFloats(directory + "/whirl_in.f32", input);
      // Speed changes at fragment boundaries: spin up from rest to slow,
      // then fast, stop and slow again.
      const int fragments_per_second = rate / 64;
      const std::vector<std::pair<int, int>> changes { { fragments_per_second, openb3::Whirl::kFast },
                                                       { 3 * fragments_per_second, openb3::Whirl::kStop },
                                                       { 5 * fragments_per_second, openb3::Whirl::kSlow } };
      std::string list;
      for (const auto& [fragment, speed] : changes)
        list += (list.empty() ? "" : ",") + std::to_string(fragment) + ":" + std::to_string(revSelect(speed));
      if (!run(reference, { "whirl", std::to_string(rate), list, directory + "/whirl_in.f32", directory + "/whirl_out.f32" }))
        continue;
      const auto expected = readFloats(directory + "/whirl_out.f32");

      openb3::Whirl whirl;
      whirl.prepare(rate);
      std::vector<float> actual;
      std::vector<float> hl(64), hr(64), dl(64), dr(64);
      for (size_t offset = 0, fragment = 0; offset < input.size(); offset += 64, ++fragment) {
        for (const auto& [at, speed] : changes) {
          if (static_cast<size_t>(at) == fragment)
            whirl.setSpeed(speed);
        }
        const int count = static_cast<int>(std::min<size_t>(64, input.size() - offset));
        // Split each fragment unevenly: the speed still changes once per 64 samples.
        const int split = static_cast<int>(fragment % 63) + 1;
        whirl.process(input.data() + offset, hl.data(), hr.data(), dl.data(), dr.data(), std::min(split, count));
        if (split < count)
          whirl.process(input.data() + offset + split, hl.data() + split, hr.data() + split, dl.data() + split,
                        dr.data() + split, count - split);
        for (int i = 0; i < count; ++i) {
          actual.push_back(hl[static_cast<size_t>(i)] + dl[static_cast<size_t>(i)]);
          actual.push_back(hr[static_cast<size_t>(i)] + dr[static_cast<size_t>(i)]);
        }
      }
      size_t differ = 0;
      for (size_t i = 0; i < std::min(actual.size(), expected.size()); ++i)
        differ += std::memcmp(&actual[i], &expected[i], sizeof(float)) != 0;
      std::cout << "whirl at " << rate << " Hz: " << differ << " of " << actual.size() << " samples differ\n";
      expect(actual.size() == expected.size() && differ == 0, "the whirl is bit-identical at " + std::to_string(rate) + " Hz");
    }
    return finish("whirl reference");
  }

  // Analogue Rotary's OpenB3 Whirl model end to end (Spread 100%, Mix 100%,
  // Inertia 50%), against whirlProc3: stopped from the start, then fast, then
  // slow, so both start at rest.
  int rotaryReference(const std::string& reference) {
    const std::string directory = scratchDirectory();
    const int rate = 48000;
    const auto input = testSignal(rate * 4, rate);
    writeFloats(directory + "/rotary_in.f32", input);
    const int fast_at = 100, slow_at = 2000;
    if (!run(reference, { "whirl", std::to_string(rate), "0:1," + std::to_string(fast_at) + ":2," + std::to_string(slow_at) + ":0",
                          directory + "/rotary_in.f32", directory + "/rotary_out.f32" }))
      return finish("rotary reference");
    const auto expected = readFloats(directory + "/rotary_out.f32");
    rotary::Engine engine;
    engine.prepare(rate);
    rotary::Parameters p;
    p.model = rotary::kWhirl;
    p.speed = rotary::kStop;
    p.spread = 1.0;
    p.mix = 1.0;
    engine.setParameters(p);
    engine.reset();
    double worst = 0.0;
    std::vector<float> left(64), right(64);
    for (size_t offset = 0, fragment = 0; offset < input.size(); offset += 64, ++fragment) {
      if (fragment == static_cast<size_t>(fast_at)) { p.speed = rotary::kFast; engine.setParameters(p); }
      if (fragment == static_cast<size_t>(slow_at)) { p.speed = rotary::kSlow; engine.setParameters(p); }
      engine.process(input.data() + offset, input.data() + offset, left.data(), right.data(), 64);
      for (size_t i = 0; i < 64; ++i) {
        worst = std::max(worst, double(std::abs(left[i] - expected[2 * (offset + i)])));
        worst = std::max(worst, double(std::abs(right[i] - expected[2 * (offset + i) + 1])));
      }
    }
    std::cout << "Analogue Rotary OpenB3 Whirl against whirlProc3: worst difference " << worst << '\n';
    expect(worst < 1e-6, "the Whirl model plays OpenB3's rotary speaker");
    return finish("rotary reference");
  }
} // namespace

int main(int argc, char** argv) {
  juce::ScopedJuceInitialiser_GUI juce;
  const juce::String flag = argc > 1 ? juce::String(argv[1]) : juce::String();
  const std::string argument = argc > 2 ? argv[2] : "";
  if (flag == "--self") return selfTest();
  if (flag == "--oscillator") return oscillator();
  if (flag == "--drawbars") return drawbars();
  if (flag == "--percussion") return percussion();
  if (flag == "--envelope") return envelope();
  if (flag == "--coherence") return coherence();
  if (flag == "--parameters") return parameters();
  if (flag == "--scanner") return scanner();
  if (flag == "--whirl") return whirl();
  if (flag == "--editor") return editorTest();
  if (!argument.empty()) {
    if (flag == "--wiring-reference") return wiringReference(argument);
    if (flag == "--gains-reference") return gainsReference(argument);
    if (flag == "--tonegen-reference") return tonegenReference(argument);
    if (flag == "--scanner-reference") return scannerReference(argument);
    if (flag == "--whirl-reference") return whirlReference(argument);
    if (flag == "--rotary-reference") return rotaryReference(argument);
  }
  std::cerr << "usage: AtlasOpenB3Tests --self | --oscillator | --drawbars | --percussion | --envelope | --coherence | "
               "--parameters | --scanner | --whirl | --editor | --wiring-reference <renderer> | --gains-reference <renderer> | "
               "--tonegen-reference <renderer> | --scanner-reference <renderer> | --whirl-reference <renderer> | "
               "--rotary-reference <renderer>\n";
  return 2;
}
