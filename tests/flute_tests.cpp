// Flute tests: Modeled Flute's concert flute as an oscillator type, through
// the plugin. --reference plays the same MIDI through the plugin and through
// the original plugin's processing of a FluteVoice (ported here without its
// reverb and output gain) and expects the same samples, whatever Atlas's
// voice count, Legato and voice override; the rest covers articulation, the
// oscillator, the envelope modes and the voice hold, host parameters and the
// editor.

#include "JuceHeader.h"
#include "synth_plugin.h"
#include "synth_editor.h"
#include "editor_navigation.h"
#include "parameter_slider.h"
#include "sound_engine.h"
#include "source_types.h"
#include "source_slot_module.h"
#include "flute_module.h"
#include "synth_strings.h"
#include "mf/FluteVoice.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {
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

  // Slot 1 is a Flute oscillator to `destination`, every effect and other
  // oscillator off, Env 1 open with no release, no oversampling.
  void preparePlayback(SynthPlugin& plugin, int destination = vital::constants::kEffects) {
    plugin.setNonRealtime(true);
    plugin.setPlayConfigDetails(0, 2, kRate, kBlock);
    plugin.prepareToPlay(kRate, kBlock);
    setControl(plugin, "oversampling", 0.0f);
    for (int chain = 0; chain <= vital::kNumBuses; ++chain) {
      const auto prefix = chain == 0 ? std::string() : "bus_" + std::to_string(chain) + "_";
      for (int i = 1; i <= vital::constants::kMaxEffectSlots; ++i)
        setControl(plugin, prefix + "fx_slot_" + std::to_string(i) + "_type", 0.0f);
    }
    for (int i = 1; i <= vital::kNumOscillators; ++i)
      setControl(plugin, "osc_" + std::to_string(i) + "_on", 0.0f);
    plugin.parameterChanged("osc_1_type", static_cast<float>(vital::source_types::kFlute));
    setControl(plugin, "osc_1_on", 1.0f);
    setControl(plugin, "osc_1_destination", static_cast<float>(destination));
    setControl(plugin, "env_1_attack", 0.0f);
    setControl(plugin, "env_1_sustain", 1.0f);
    setControl(plugin, "env_1_release", 0.0f);
    plugin.checkOversampling();
    expect(plugin.getEngine()->getSourceSlot(0)->activeType() == vital::source_types::kFlute,
           "slot 1 runs the Flute type");
  }

  // Renders `samples` frames with MIDI events at given samples.
  std::vector<float> render(SynthPlugin& plugin, int samples, const std::vector<MidiEvent>& events,
                            std::vector<float>* right = nullptr, int* peak_voices = nullptr) {
    std::vector<float> result(static_cast<size_t>(samples));
    if (right != nullptr)
      right->assign(static_cast<size_t>(samples), 0.0f);
    if (peak_voices != nullptr)
      *peak_voices = 0;
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
      if (peak_voices != nullptr)
        *peak_voices = std::max(*peak_voices, plugin.getEngine()->getNumActiveVoices());
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

  // Amplitude of the component at `hz` over [start, end) (Goertzel, Hann windowed).
  double toneAmplitude(const std::vector<float>& signal, int start, int end, double hz) {
    const double w = 2.0 * kPi * hz / kRate;
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

  // The frequency near `around` Hz with the most energy over [start, end).
  double strongestNear(const std::vector<float>& signal, int start, int end, double around) {
    double best = 0.0, best_hz = around;
    for (double hz = around * 0.97; hz <= around * 1.03; hz += around * 0.0005) {
      const double amplitude = toneAmplitude(signal, start, end, hz);
      if (amplitude > best) {
        best = amplitude;
        best_hz = hz;
      }
    }
    return best_hz;
  }

  std::vector<MidiEvent> noteEvents(int note, float velocity, int on, int off) {
    return { { on, juce::MidiMessage::noteOn(1, note, velocity) }, { off, juce::MidiMessage::noteOff(1, note, 0.5f) } };
  }

  // A phrase that exercises the player: tongued notes, a slur up and back
  // down (the second key released first), a register-changing slur (C5 to
  // D5), a re-tongued note during a release, the sustain pedal holding a
  // key, and the controllers (the mod wheel adding vibrato, expression,
  // the dynamics controller).
  std::vector<MidiEvent> phrase() {
    const auto on = [](int at, int note, float velocity) { return MidiEvent { at, juce::MidiMessage::noteOn(1, note, velocity) }; };
    const auto off = [](int at, int note) { return MidiEvent { at, juce::MidiMessage::noteOff(1, note, 0.5f) }; };
    const auto cc = [](int at, int number, int value) { return MidiEvent { at, juce::MidiMessage::controllerEvent(1, number, value) }; };
    const int s = kRate;
    std::vector<MidiEvent> events = {
      on(100, 69, 0.7f), off(s / 2 + 37, 69),                           // tongued A4
      on(s / 2 + 2000, 72, 0.8f),                                       // tongued C5
      on(s - 500, 76, 0.6f), off(s + 1000, 76),                         // slur up to E5 and back
      on(s + 2500, 74, 0.9f), off(s + 6000, 72),                        // slur to D5: a register change
      off(2 * s, 74),                                                   // release
      on(2 * s + 2400, 67, 0.5f), off(2 * s + 20000, 67),               // re-tongued during the release
      cc(2 * s + 24000, 64, 127), on(2 * s + 24010, 71, 0.75f), off(2 * s + 30000, 71),  // held by the pedal
      cc(3 * s, 64, 0),                                                 // pedal up: released
      on(3 * s + 4800, 69, 0.7f), cc(3 * s + 9600, 1, 40), cc(3 * s + 14400, 1, 90), cc(3 * s + 19200, 1, 0),
      cc(3 * s + 24000, 11, 70), cc(3 * s + 28800, 11, 127), off(4 * s, 69),
    };
    return events;
  }

  // ---- The original plugin's processing, for the reference ---------------------

  // Modeled Flute's PluginProcessor without its reverb, output gain and meter,
  // at the plugin's default parameters.
  struct Reference {
    static constexpr int kChunk = mf::FluteVoice::kControlBlock;
    mf::FluteVoice voice;
    mf::PerformerParams perf;
    juce::SmoothedValue<float> air, brightness, vibrato_depth, tuning;
    bool sustain_down = false;
    std::array<bool, 128> sustained {};
    int bend_value = 8192;
    float bend_range = 2.0f;
    float air_target = 0.5f, brightness_target = 0.5f, vibrato_target = 0.35f, tuning_target = 440.0f;

    void prepare() {
      voice.prepare(kRate, std::max(kBlock, kChunk));
      for (auto* s : { &air, &brightness, &vibrato_depth, &tuning })
        s->reset(double(kRate), 0.02);
      sustain_down = false;
      sustained.fill(false);
      bend_value = 8192;
      readTargets(true);
      perf = voice.params();
      applyVoiceParams(0);
      voice.reset();
      updateBend();
    }

    void readTargets(bool snap) {
      auto set = [snap](auto& s, float v) {
        if (snap) s.setCurrentAndTargetValue(v);
        else s.setTargetValue(v);
      };
      set(air, air_target);
      set(brightness, brightness_target);
      set(vibrato_depth, vibrato_target);
      set(tuning, tuning_target);
      voice.setRegisterOverlap(true);
    }

    void applyVoiceParams(int advanced) {
      mf::PerformerParams p = perf;
      auto adv = [advanced](auto& s) { return advanced > 0 ? s.skip(advanced) : s.getCurrentValue(); };
      p.tone.air = adv(air);
      p.tone.brightness = adv(brightness);
      p.vibratoDepth = adv(vibrato_depth);
      p.tuningA4 = static_cast<double>(adv(tuning));
      const bool same = p.tuningA4 == perf.tuningA4 && p.tone.air == perf.tone.air &&
                        p.tone.brightness == perf.tone.brightness && p.vibratoDepth == perf.vibratoDepth;
      if (!same || advanced == 0) {
        perf = p;
        voice.setParams(p);
      }
    }

    void renderVoice(float* out, int n) {
      const bool moving = air.isSmoothing() || brightness.isSmoothing() || vibrato_depth.isSmoothing() || tuning.isSmoothing();
      if (!moving) {
        applyVoiceParams(-1);
        voice.process(out, n);
        return;
      }
      for (int pos = 0; pos < n; pos += kChunk) {
        const int count = std::min(kChunk, n - pos);
        applyVoiceParams(count);
        voice.process(out + pos, count);
      }
    }

    void updateBend() {
      const float x = bend_value >= 8192 ? static_cast<float>(bend_value - 8192) / 8191.0f
                                         : static_cast<float>(bend_value - 8192) / 8192.0f;
      voice.setPitchBend(x * bend_range);
    }

    void releaseSustained() {
      const int sounding = voice.lastControls().note;
      for (int n = 0; n < 128; ++n) {
        if (sustained[static_cast<size_t>(n)] && n != sounding) {
          sustained[static_cast<size_t>(n)] = false;
          voice.noteOff(n);
        }
      }
      if (sounding >= 0 && sounding < 128 && sustained[static_cast<size_t>(sounding)]) {
        sustained[static_cast<size_t>(sounding)] = false;
        voice.noteOff(sounding);
      }
    }

    void handleMidi(const juce::MidiMessage& m) {
      if (m.isNoteOn()) {
        sustained[static_cast<size_t>(m.getNoteNumber())] = false;
        voice.noteOn(m.getNoteNumber(), m.getFloatVelocity());
      }
      else if (m.isNoteOff()) {
        if (sustain_down) sustained[static_cast<size_t>(m.getNoteNumber())] = true;
        else voice.noteOff(m.getNoteNumber());
      }
      else if (m.isPitchWheel()) {
        bend_value = m.getPitchWheelValue();
        updateBend();
      }
      else if (m.isController()) {
        const int cc = m.getControllerNumber();
        const float v = static_cast<float>(m.getControllerValue()) / 127.0f;
        if (cc == 1) voice.setDynamics(v);   // CC1 is the dynamics controller by default
        else if (cc == 11) voice.setExpression(v);
        else if (cc == 64) {
          const bool down = m.getControllerValue() >= 64;
          if (sustain_down && !down) releaseSustained();
          sustain_down = down;
        }
        else if (cc == 120 || cc == 123) {
          sustain_down = false;
          sustained.fill(false);
          voice.allNotesOff();
        }
      }
    }

    std::vector<float> render(int samples, const std::vector<MidiEvent>& events) {
      prepare();
      std::vector<float> out(static_cast<size_t>(samples), 0.0f);
      for (int offset = 0; offset < samples; offset += kBlock) {
        const int count = std::min(kBlock, samples - offset);
        readTargets(false);
        std::vector<const MidiEvent*> block;
        for (const MidiEvent& event : events) {
          if (event.sample >= offset && event.sample < offset + count)
            block.push_back(&event);
        }
        std::stable_sort(block.begin(), block.end(), [](const MidiEvent* a, const MidiEvent* b) { return a->sample < b->sample; });
        float* buffer = out.data() + offset;
        int pos = 0;
        for (const MidiEvent* event : block) {
          const int at = event->sample - offset;
          if (at > pos) {
            renderVoice(buffer + pos, at - pos);
            pos = at;
          }
          handleMidi(event->message);
        }
        if (pos < count)
          renderVoice(buffer + pos, count - pos);
      }
      return out;
    }
  };

  // The plugin's output is the reference times one gain (the level and volume
  // path): the best gain, and how far the two differ after it, re the peak.
  struct Match {
    double gain = 0.0, error = 0.0;
    int worst = 0;
  };

  Match match(const std::vector<float>& plugin, const std::vector<float>& reference) {
    double cross = 0.0, power = 0.0;
    for (size_t i = 0; i < plugin.size(); ++i) {
      cross += double(plugin[i]) * reference[i];
      power += double(reference[i]) * reference[i];
    }
    Match m;
    m.gain = power > 0.0 ? cross / power : 0.0;
    double difference = 0.0;
    for (size_t i = 0; i < plugin.size(); ++i) {
      const double d = std::abs(plugin[i] - m.gain * reference[i]);
      if (d > difference) {
        difference = d;
        m.worst = static_cast<int>(i);
      }
    }
    m.error = difference / std::max(1.0e-12, m.gain * peak(reference));
    return m;
  }

  int reference() {
    const int length = 5 * kRate;
    const std::vector<MidiEvent> events = phrase();
    Reference original;
    const auto expected = original.render(length, events);
    expect(finite(expected) && peak(expected) > 0.05, "the original plays the phrase");
    std::cout << "original phrase peak " << 20.0 * std::log10(peak(expected)) << " dBFS, "
              << original.voice.overlapCount() << " register overlaps\n";

    struct Setting {
      const char* name;
      float polyphony, legato, override_;
    };
    const Setting settings[] = {
      { "eight voices", 8.0f, 0.0f, 0.0f },
      { "one voice, Legato off, Kill", 1.0f, 0.0f, 0.0f },
      { "one voice, Legato on, Kill", 1.0f, 1.0f, 0.0f },
      { "one voice, Legato off, Steal", 1.0f, 0.0f, 1.0f },
      { "two voices, Legato on", 2.0f, 1.0f, 0.0f },
    };
    double first_gain = 0.0;
    for (const Setting& setting : settings) {
      SynthPlugin plugin;
      preparePlayback(plugin);
      setControl(plugin, "polyphony", setting.polyphony);
      setControl(plugin, "legato", setting.legato);
      setControl(plugin, "voice_override", setting.override_);
      setControl(plugin, "volume", 6400.0f);   // Atlas's master volume at 0 dB (its default is -6 dB)
      const auto audio = render(plugin, length, events);
      expect(finite(audio), std::string("finite output with ") + setting.name);
      const Match m = match(audio, expected);
      std::cout << setting.name << ": gain " << 20.0 * std::log10(std::max(m.gain, 1.0e-12)) << " dB, largest difference "
                << 20.0 * std::log10(std::max(m.error, 1.0e-12)) << " dB re peak at " << m.worst / double(kRate) << " s\n";
      expect(m.gain > 0.1, std::string("the plugin plays the phrase with ") + setting.name);
      expect(m.error < 3.0e-5, std::string("the plugin matches the original with ") + setting.name);
      if (first_gain == 0.0)
        first_gain = m.gain;
      else
        close(m.gain, first_gain, std::string("the same level with ") + setting.name, 1.0e-6 * first_gain);
    }
    // The slot's level default plays the original at its Output of 0 dB.
    close(20.0 * std::log10(std::max(first_gain, 1.0e-12)), 0.0, "the default level is the original's 0 dB", 0.5);
    return finish("reference");
  }

  // ---- Articulation ---------------------------------------------------------------------

  // One note through a fresh plugin, with controls set first.
  std::vector<float> renderNote(int note, float velocity, int samples, int release,
                                const std::vector<std::pair<std::string, float>>& controls = {},
                                std::vector<float>* right = nullptr) {
    SynthPlugin plugin;
    preparePlayback(plugin);
    for (const auto& [name, value] : controls)
      setControl(plugin, name, value);
    return render(plugin, samples, noteEvents(note, velocity, 0, release), right);
  }

  int articulation() {
    // A key pressed while another is held slurs: the pitch moves and the
    // tone goes on; letting it go slurs back to the held key.
    SynthPlugin plugin;
    preparePlayback(plugin);
    setControl(plugin, "polyphony", 1.0f);
    const auto on = [](int at, int note) { return MidiEvent { at, juce::MidiMessage::noteOn(1, note, 0.7f) }; };
    const auto off = [](int at, int note) { return MidiEvent { at, juce::MidiMessage::noteOff(1, note, 0.5f) }; };
    const int s = kRate;
    const auto slurred = render(plugin, 3 * s, { on(0, 69), on(s, 72), off(2 * s, 72), off(3 * s - 1, 69) });
    expect(finite(slurred), "finite output");
    close(strongestNear(slurred, s / 2, s, 440.0), 440.0, "A4 sounds first", 1.5);
    close(strongestNear(slurred, s + s / 2, 2 * s, 523.25), 523.25, "the slur moves to C5", 1.5);
    close(strongestNear(slurred, 2 * s + s / 2, 3 * s, 440.0), 440.0, "letting go of C5 slurs back to A4", 1.5);
    const double before = rms(slurred, s - 4800, s), across = rms(slurred, s + 480, s + 4800);
    std::cout << "level before the slur " << 20.0 * std::log10(before) << " dB, in the 90 ms after " << 20.0 * std::log10(across) << " dB\n";
    expect(across > 0.5 * before, "the tone goes on through the slur");

    // The same keys with a gap between them tongue the second note: the
    // breath stops and restarts, so the level dips to nothing in between.
    const auto tongued = render(plugin, 3 * s, { on(0, 69), off(s - 12000, 69), on(s, 72), off(2 * s, 72) });
    expect(rms(tongued, s - 2400, s) < 0.05 * rms(tongued, s / 2, s - 12000), "a gap between keys lets the breath stop");
    close(strongestNear(tongued, s + s / 2, 2 * s, 523.25), 523.25, "the tongued note sounds C5", 1.5);

    // Whatever the voice count and Legato, the keys play the same: --reference
    // compares the samples; here the slur holds with eight voices and Legato off.
    SynthPlugin poly;
    preparePlayback(poly);
    setControl(poly, "polyphony", 8.0f);
    setControl(poly, "legato", 0.0f);
    const auto poly_slurred = render(poly, 3 * s, { on(0, 69), on(s, 72), off(2 * s, 72), off(3 * s - 1, 69) });
    double difference = 0.0;
    for (size_t i = 0; i < slurred.size(); ++i)
      difference = std::max(difference, double(std::abs(slurred[i] - poly_slurred[i])));
    expect(difference < 1.0e-5 * peak(slurred), "eight voices without Legato slur the same as one voice");
    return finish("articulation");
  }

  // ---- The oscillator --------------------------------------------------------------------

  int oscillator() {
    std::vector<float> right;
    const auto a4 = renderNote(69, 0.7f, 2 * kRate, kRate, {}, &right);
    expect(finite(a4) && finite(right), "output is finite");
    expect(rms(a4, 4800, 24000) > 1.0e-3, "a blown note sounds");
    const double hz = strongestNear(a4, 4800, kRate, 440.0);
    std::cout << "strongest near A4: " << hz << " Hz\n";
    close(hz, 440.0, "A4 sounds at 440 Hz", 1.0);
    expect(rms(a4, kRate + kRate / 2, 2 * kRate) < 0.01 * rms(a4, kRate / 2, kRate), "the breath stops after release");
    double difference = 0.0;
    for (size_t i = 0; i < a4.size(); ++i)
      difference = std::max(difference, double(std::abs(a4[i] - right[i])));
    expect(difference < 1.0e-6 * std::max(1.0, peak(a4)), "the flute is mono");

    // Velocity: louder blown harder.
    const auto soft = renderNote(60, 0.2f, kRate, kRate / 2);
    const auto loud = renderNote(60, 0.95f, kRate, kRate / 2);
    const double soft_level = rms(soft, 9600, 24000), loud_level = rms(loud, 9600, 24000);
    std::cout << "C4 at velocity 25: " << 20.0 * std::log10(soft_level) << " dB, at 121: " << 20.0 * std::log10(loud_level) << " dB\n";
    expect(loud_level > 2.0 * soft_level, "a harder blow is much louder");

    // Transpose moves the keys by whole semitones; Tuning moves A4.
    const auto up = renderNote(57, 0.7f, kRate, kRate / 2, { { "osc_1_flute_transpose", 12.0f } });
    close(strongestNear(up, 4800, kRate / 2, 440.0), 440.0, "Transpose +12 plays A3 as A4", 1.0);
    const auto tuned = renderNote(69, 0.7f, kRate, kRate / 2, { { "osc_1_flute_tuning", 432.0f } });
    close(strongestNear(tuned, 4800, kRate / 2, 432.0), 432.0, "Tuning 432 Hz retunes A4", 1.0);

    // Air: more breath noise between the harmonics.
    const auto noise_between = [](const std::vector<float>& a) {
      double tone = 0.0, between = 0.0;
      for (int h = 1; h <= 4; ++h) {
        tone += toneAmplitude(a, 9600, 24000, h * 440.0);
        between += toneAmplitude(a, 9600, 24000, (h + 0.5) * 440.0);
      }
      return between / tone;
    };
    const auto clean = renderNote(69, 0.7f, kRate, kRate / 2, { { "osc_1_flute_air", 0.0f } });
    const auto breathy = renderNote(69, 0.7f, kRate, kRate / 2, { { "osc_1_flute_air", 1.0f } });
    std::cout << "noise between the harmonics: Air 0% " << 20.0 * std::log10(noise_between(clean)) << " dB, 100% "
              << 20.0 * std::log10(noise_between(breathy)) << " dB\n";
    expect(noise_between(breathy) > 2.0 * noise_between(clean), "Air adds breath noise");
    return finish("oscillator");
  }

  // ---- Envelope modes and the voice hold ------------------------------------------

  int envelope() {
    // Flute mode: the breath releases after the key and the voice stays
    // until the tone is gone.
    {
      SynthPlugin plugin;
      preparePlayback(plugin);
      const int release = kRate / 2;
      const auto audio = render(plugin, 2 * kRate, noteEvents(69, 0.8f, 0, release));
      expect(rms(audio, release, release + 2400) > 0.3 * rms(audio, release - 2400, release),
             "Flute mode lets the breath release after the key");
      expect(rms(audio, release + kRate / 2, 2 * kRate) < 1.0e-4, "and the tone is gone half a second later");
      expect(plugin.getEngine()->getNumActiveVoices() == 0, "the voice is freed once the tone is gone");
    }
    // Flute mode ignores Env 1: a slow attack changes nothing.
    {
      SynthPlugin plugin;
      preparePlayback(plugin);
      const auto plain = render(plugin, kRate, noteEvents(69, 0.8f, 0, kRate / 2));
      setControl(plugin, "env_1_attack", 1.0f);
      setControl(plugin, "env_1_release", 1.0f);
      const auto shaped = render(plugin, kRate, noteEvents(69, 0.8f, 0, kRate / 2));
      expect(rms(shaped, 2400, 7200) > 0.9 * rms(plain, 2400, 7200), "Flute mode sounds at once whatever Env 1's attack");
    }
    // Envelope 1 mode: Env 1 shapes the note and ends the voice.
    {
      SynthPlugin plugin;
      preparePlayback(plugin);
      setControl(plugin, "osc_1_flute_amp_env", 1.0f);
      const int release = kRate / 2;
      const auto audio = render(plugin, kRate, noteEvents(69, 0.8f, 0, release));
      expect(rms(audio, release + 480, kRate) < 1.0e-4, "Envelope 1 mode with no release stops at the key");
      expect(plugin.getEngine()->getNumActiveVoices() == 0, "the voice is freed with the envelope");
      setControl(plugin, "env_1_attack", 1.0f);
      const auto slow = render(plugin, kRate, noteEvents(69, 0.8f, 0, release));
      expect(rms(slow, 2400, 7200) < 0.2 * rms(audio, 2400, 7200), "Envelope 1 mode follows Env 1's attack");
    }
    return finish("envelope");
  }

  // ---- Parameters -----------------------------------------------------------------

  int parameters() {
    const char* const suffixes[] = { "amp_env", "air", "brightness", "attack", "vibrato_depth", "vibrato_rate",
                                     "vibrato_delay", "humanize", "dynamics_source", "dynamics_cc", "dynamic_range",
                                     "release", "tuning", "overlap", "transpose" };
    juce::MemoryBlock state;
    {
      SynthPlugin plugin;
      preparePlayback(plugin);
      for (int slot = 1; slot <= vital::kNumOscillators; ++slot) {
        const std::string prefix = "osc_" + std::to_string(slot) + "_flute_";
        for (const char* suffix : suffixes) {
          expect(plugin.getControls().count(prefix + suffix) == 1, "control exists: " + prefix + suffix);
          expect(vital::Parameters::getDetails(prefix + suffix).version_added == 0x020133,
                 "new host version: " + prefix + suffix);
        }
      }
      expect(control(plugin, "osc_2_flute_amp_env") == 0.0f, "the Flute envelope by default");
      close(control(plugin, "osc_2_flute_air"), 0.5, "Air 50%", 1e-6);
      close(control(plugin, "osc_2_flute_brightness"), 0.5, "Brightness 50%", 1e-6);
      close(control(plugin, "osc_2_flute_attack"), 0.5, "Attack 50%", 1e-6);
      close(control(plugin, "osc_2_flute_vibrato_depth"), 0.35, "Vibrato depth 35%", 1e-6);
      close(control(plugin, "osc_2_flute_vibrato_rate"), 5.0, "Vibrato rate 5 Hz", 1e-6);
      close(control(plugin, "osc_2_flute_vibrato_delay"), 0.2, "Vibrato delay 0.2 s", 1e-6);
      close(control(plugin, "osc_2_flute_humanize"), 0.5, "Humanize 50%", 1e-6);
      expect(control(plugin, "osc_2_flute_dynamics_source") == 0.0f && control(plugin, "osc_2_flute_dynamics_cc") == 0.0f,
             "velocity then the mod wheel by default");
      close(control(plugin, "osc_2_flute_dynamic_range"), 18.0, "Dynamic range 18 dB", 1e-6);
      close(std::pow(2.0, control(plugin, "osc_2_flute_release")), 0.15, "Release 0.15 s", 1e-4);
      close(control(plugin, "osc_2_flute_tuning"), 440.0, "Tuning 440 Hz", 1e-6);
      expect(control(plugin, "osc_2_flute_overlap") == 1.0f, "Register slur overlap on");
      expect(strings::kOscillatorTypeNames[vital::source_types::kFlute] == "Flute" &&
             strings::kFluteAmpEnvNames[0] == "Flute" && strings::kFluteAmpEnvNames[1] == "Envelope 1",
             "names are registered");
      expect(vital::Parameters::getDetails("osc_1_type").max >= vital::source_types::kFlute,
             "the type menu offers the Flute");

      setControl(plugin, "osc_1_flute_amp_env", 1.0f);
      setControl(plugin, "osc_1_flute_air", 0.8f);
      setControl(plugin, "osc_1_flute_tuning", 432.0f);
      setControl(plugin, "osc_1_flute_dynamics_cc", 2.0f);
      setControl(plugin, "osc_1_flute_transpose", -12.0f);
      plugin.getStateInformation(state);
    }
    {
      SynthPlugin plugin;
      plugin.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
      close(control(plugin, "osc_1_flute_amp_env"), 1.0, "amp env restored", 1e-6);
      close(control(plugin, "osc_1_flute_air"), 0.8, "air restored", 1e-6);
      close(control(plugin, "osc_1_flute_tuning"), 432.0, "tuning restored", 1e-6);
      close(control(plugin, "osc_1_flute_dynamics_cc"), 2.0, "dynamics controller restored", 1e-6);
      close(control(plugin, "osc_1_flute_transpose"), -12.0, "transpose restored", 1e-6);
      expect(static_cast<int>(control(plugin, "osc_1_type") + 0.5f) == vital::source_types::kFlute, "type restored");
    }
    // The dynamics controller: with the breath controller chosen, CC 2 sets the loudness.
    {
      SynthPlugin plugin;
      preparePlayback(plugin);
      setControl(plugin, "osc_1_flute_dynamics_cc", 2.0f);
      std::vector<MidiEvent> events = noteEvents(69, 0.7f, 0, kRate);
      events.insert(events.begin(), { 0, juce::MidiMessage::controllerEvent(1, 2, 127) });
      const auto loud = render(plugin, kRate, events);
      events[0] = { 0, juce::MidiMessage::controllerEvent(1, 2, 10) };
      const auto soft = render(plugin, kRate, events);
      std::cout << "breath controller at 127: " << 20.0 * std::log10(rms(loud, 12000, 24000)) << " dB, at 10: "
                << 20.0 * std::log10(rms(soft, 12000, 24000)) << " dB\n";
      expect(rms(loud, 12000, 24000) > 2.0 * rms(soft, 12000, 24000), "the breath controller sets the loudness");
    }
    return finish("parameters");
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

  Component* sectionNamed(Component& parent, const String& name) {
    if (parent.getProperties()["SectionName"].toString() == name)
      return &parent;
    for (auto* child : parent.getChildren()) {
      if (auto* found = sectionNamed(*child, name))
        return found;
    }
    return nullptr;
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

  int editorTest() {
    SynthPlugin synth;
    synth.parameterChanged("osc_1_type", static_cast<float>(vital::source_types::kFlute));
    SynthEditor editor(synth);
    editor.updateFullGui();
    juce::MessageManager::getInstance()->runDispatchLoopUntil(200);

    auto* section = sectionNamed(editor, "Oscillator 1");
    expect(section != nullptr, "Oscillator 1 section exists");
    if (section != nullptr) {
      auto* envelope = sliderTitled(*section, "1 Flute Amplitude Envelope");
      expect(envelope != nullptr && valueText(envelope) == "Flute" && envelope->getWantsKeyboardFocus() &&
             envelope->getDescription().contains("breath"),
             "the envelope row reads its mode and is described (" + valueText(envelope).toStdString() + ")");
      const char* const titles[] = { "1 Flute Air", "1 Flute Brightness", "1 Flute Attack", "1 Flute Vibrato Depth",
                                     "1 Flute Vibrato Rate", "1 Flute Vibrato Delay", "1 Flute Dynamics Source",
                                     "1 Flute Dynamics Controller", "1 Flute Dynamic Range", "1 Flute Release",
                                     "1 Flute Humanize", "1 Flute Tuning", "1 Flute Amplitude Envelope" };
      int previous_y = -1;
      for (const char* title : titles) {
        auto* row = sliderTitled(*section, title);
        expect(row != nullptr && row->getDescription().isNotEmpty() && row->getWantsKeyboardFocus(),
               std::string("a described row: ") + title);
        if (row != nullptr) {
          expect(row->getScreenY() > previous_y, std::string("rows in the plugin's order: ") + title);
          previous_y = row->getScreenY();
        }
      }
      expect(valueText(sliderTitled(*section, "1 Flute Tuning")).startsWith("440") &&
             valueText(sliderTitled(*section, "1 Flute Air")) == "50%" &&
             valueText(sliderTitled(*section, "1 Flute Dynamics Controller")) == "Mod wheel, CC 1",
             "rows read their values (" + valueText(sliderTitled(*section, "1 Flute Release")).toStdString() + ")");
      // The slur overlap is a switch, between Tuning and the envelope mode.
      auto* overlap = findTitled(*section, [](const String& t) { return t == "1 Flute Register Slur Overlap"; });
      expect(overlap != nullptr && overlap->getDescription().contains("register") && overlap->getWantsKeyboardFocus(),
             "a described Register slur overlap switch");
      auto* tuning = sliderTitled(*section, "1 Flute Tuning");
      expect(overlap != nullptr && tuning != nullptr && envelope != nullptr && overlap->getScreenY() > tuning->getScreenY() &&
             overlap->getScreenY() < envelope->getScreenY(), "the switch sits between Tuning and the envelope mode");
      std::cout << "readouts: Release " << valueText(sliderTitled(*section, "1 Flute Release")) << ", Vibrato delay "
                << valueText(sliderTitled(*section, "1 Flute Vibrato Delay")) << ", Vibrato rate "
                << valueText(sliderTitled(*section, "1 Flute Vibrato Rate")) << ", Dynamic range "
                << valueText(sliderTitled(*section, "1 Flute Dynamic Range")) << ", Transpose "
                << valueText(sliderTitled(*section, "1 Flute Transpose")) << '\n';
      auto* transpose = sliderTitled(*section, "1 Flute Transpose");
      expect(transpose != nullptr && transpose->getDescription().contains("slurs"), "a described Transpose row");
      expect(findTitled(*section, [](const String& t) { return t.contains("Piano") || t.contains("Wave Frame"); }) == nullptr,
             "other types' rows are hidden");
    }
    return finish("editor");
  }

  // ---- A MIDI file through the Flute, for listening --------------------------------

  // Renders a MIDI file through slot 1's Flute, dry, to a WAV file.
  int renderFile(const std::string& midi_path, const std::string& wav_path,
                 const std::vector<std::pair<std::string, float>>& controls) {
    juce::File midi_file(midi_path);
    juce::FileInputStream stream(midi_file);
    juce::MidiFile midi;
    if (!stream.openedOk() || !midi.readFrom(stream)) {
      std::cerr << "cannot read " << midi_path << '\n';
      return 1;
    }
    std::vector<juce::MidiMessage> raw;
    for (int track = 0; track < midi.getNumTracks(); ++track) {
      const juce::MidiMessageSequence& sequence = *midi.getTrack(track);
      for (int i = 0; i < sequence.getNumEvents(); ++i)
        raw.push_back(sequence.getEventPointer(i)->message);
    }
    std::stable_sort(raw.begin(), raw.end(),
                     [](const juce::MidiMessage& a, const juce::MidiMessage& b) { return a.getTimeStamp() < b.getTimeStamp(); });
    const double ppq = std::max(1, static_cast<int>(midi.getTimeFormat()));
    double seconds = 0.0, seconds_per_tick = 0.5 / ppq, last_tick = 0.0, last = 0.0;
    std::vector<MidiEvent> events;
    for (const juce::MidiMessage& message : raw) {
      seconds += (message.getTimeStamp() - last_tick) * seconds_per_tick;
      last_tick = message.getTimeStamp();
      if (message.isTempoMetaEvent())
        seconds_per_tick = message.getTempoSecondsPerQuarterNote() / ppq;
      else if (message.isNoteOnOrOff() || message.isController() || message.isPitchWheel()) {
        events.push_back({ static_cast<int>(seconds * kRate), message });
        last = std::max(last, seconds);
      }
    }

    SynthPlugin plugin;
    preparePlayback(plugin);
    for (const auto& [name, value] : controls)
      setControl(plugin, name, value);
    std::vector<float> right;
    const auto left = render(plugin, static_cast<int>((last + 2.0) * kRate), events, &right);

    juce::File out(wav_path);
    out.deleteFile();
    juce::WavAudioFormat format;
    std::unique_ptr<juce::AudioFormatWriter> writer(format.createWriterFor(new juce::FileOutputStream(out), kRate, 2, 32, {}, 0));
    if (writer == nullptr) {
      std::cerr << "cannot write " << wav_path << '\n';
      return 1;
    }
    juce::AudioBuffer<float> audio(2, static_cast<int>(left.size()));
    audio.copyFrom(0, 0, left.data(), static_cast<int>(left.size()));
    audio.copyFrom(1, 0, right.data(), static_cast<int>(right.size()));
    writer->writeFromAudioSampleBuffer(audio, 0, audio.getNumSamples());
    std::cout << "wrote " << wav_path << ": " << left.size() / double(kRate) << " s, peak " << 20.0 * std::log10(peak(left)) << " dB\n";
    return 0;
  }
}

int main(int argc, char** argv) {
  juce::ScopedJuceInitialiser_GUI juce_init;
  const std::string flag = argc > 1 ? argv[1] : "";
  if (flag == "--render" && argc > 3) {
    std::vector<std::pair<std::string, float>> controls;
    for (int i = 4; i < argc; ++i) {
      const std::string argument = argv[i];
      const size_t equals = argument.find('=');
      if (equals != std::string::npos)
        controls.emplace_back(argument.substr(0, equals), std::stof(argument.substr(equals + 1)));
    }
    return renderFile(argv[2], argv[3], controls);
  }
  if (flag == "--reference") return reference();
  if (flag == "--articulation") return articulation();
  if (flag == "--oscillator") return oscillator();
  if (flag == "--envelope") return envelope();
  if (flag == "--parameters") return parameters();
  if (flag == "--editor") return editorTest();
  std::cerr << "usage: AtlasFluteTests --reference | --articulation | --oscillator | --envelope | --parameters | "
               "--editor | --render <midi file> <wav file> [control=value ...]\n";
  return 2;
}
