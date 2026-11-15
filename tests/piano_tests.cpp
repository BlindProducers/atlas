// Piano tests: the modelled grand as an oscillator type, through the plugin
// (pitch, velocity, the damper, the pedals, the envelope modes, the voice
// hold, host parameters, the editor). The engine itself is checked against
// the original in AtlasPianoEngineTests.

#include "JuceHeader.h"
#include "synth_plugin.h"
#include "synth_editor.h"
#include "editor_navigation.h"
#include "parameter_slider.h"
#include "sound_engine.h"
#include "source_types.h"
#include "source_slot_module.h"
#include "piano_module.h"
#include "piano_model_service.h"
#include "synth_strings.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <functional>
#include <iostream>
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

  // Slot 1 is a Piano oscillator to `destination`, every effect and other
  // oscillator off, Env 1 open with no release. Offline, so the model is
  // built before the first block renders.
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
    plugin.parameterChanged("osc_1_type", static_cast<float>(vital::source_types::kPiano));
    setControl(plugin, "osc_1_on", 1.0f);
    setControl(plugin, "osc_1_destination", static_cast<float>(destination));
    setControl(plugin, "env_1_attack", 0.0f);
    setControl(plugin, "env_1_sustain", 1.0f);
    setControl(plugin, "env_1_release", 0.0f);
    plugin.checkOversampling();
    expect(plugin.getEngine()->getSourceSlot(0)->activeType() == vital::source_types::kPiano,
           "slot 1 runs the Piano type");
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

  // ---- The oscillator ---------------------------------------------------------

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

  int oscillator() {
    std::vector<float> right;
    const auto a4 = renderNote(69, 0.7f, 2 * kRate, kRate, {}, &right);
    expect(finite(a4) && finite(right), "output is finite");
    expect(rms(a4, 2400, 24000) > 1.0e-3, "a struck note sounds");
    const double hz = strongestNear(a4, 2400, kRate, 440.0);
    std::cout << "strongest near A4: " << hz << " Hz\n";
    close(hz, 440.0, "A4 sounds at 440 Hz", 1.0);
    expect(rms(a4, kRate + kRate / 4, kRate + kRate / 2) < 0.02 * rms(a4, kRate / 2, kRate),
           "the damper stops the note after release");
    expect(rms(a4, 2 * kRate - 2400, 2 * kRate) < 1.0e-4, "the note dies away");
    double difference = 0.0;
    for (size_t i = 0; i < a4.size(); ++i)
      difference = std::max(difference, double(std::abs(a4[i] - right[i])));
    expect(difference > 0.01 * peak(a4), "the note has a stereo image");

    // Velocity: louder and brighter struck harder.
    const auto soft = renderNote(60, 0.2f, kRate, kRate / 2);
    const auto loud = renderNote(60, 0.95f, kRate, kRate / 2);
    const double soft_level = rms(soft, 1200, 12000), loud_level = rms(loud, 1200, 12000);
    std::cout << "C4 at velocity 25: " << 20.0 * std::log10(soft_level) << " dB, at 121: " << 20.0 * std::log10(loud_level) << " dB\n";
    expect(loud_level > 4.0 * soft_level, "a harder blow is much louder");
    const double soft_bright = toneAmplitude(soft, 1200, 12000, 4.0 * 261.63) / toneAmplitude(soft, 1200, 12000, 261.63);
    const double loud_bright = toneAmplitude(loud, 1200, 12000, 4.0 * 261.63) / toneAmplitude(loud, 1200, 12000, 261.63);
    expect(loud_bright > soft_bright, "a harder blow is brighter");

    // Transpose moves the note by whole keys.
    const auto up = renderNote(57, 0.7f, kRate, kRate / 2, { { "osc_1_piano_transpose", 12.0f } });
    close(strongestNear(up, 2400, kRate / 2, 440.0), 440.0, "Transpose +12 plays A3 as A4", 1.0);

    // The piano has 88 keys: outside them, silence.
    const auto below = renderNote(20, 0.9f, kRate / 2, kRate / 4);
    const auto above = renderNote(109, 0.9f, kRate / 2, kRate / 4);
    expect(peak(below) < 1.0e-6 && peak(above) < 1.0e-6, "notes below A0 and above C8 stay silent");
    const auto top = renderNote(108, 0.9f, kRate / 2, kRate / 4);
    const auto bottom = renderNote(21, 0.9f, kRate / 2, kRate / 4);
    expect(rms(top, 480, 4800) > 1.0e-3 && rms(bottom, 480, 4800) > 1.0e-3, "A0 and C8 sound");

    // The width control: 0% is mono.
    std::vector<float> mono_right;
    const auto mono = renderNote(69, 0.7f, kRate / 2, kRate / 4, { { "osc_1_piano_width", 0.0f } }, &mono_right);
    difference = 0.0;
    for (size_t i = 0; i < mono.size(); ++i)
      difference = std::max(difference, double(std::abs(mono[i] - mono_right[i])));
    expect(difference < 1.0e-6 * std::max(1.0, peak(mono)), "Stereo width 0% is mono");
    return finish("oscillator");
  }

  // ---- Envelope modes and the voice hold ------------------------------------------

  int envelope() {
    // Piano mode: the note rings after the key is released only as long as the
    // damper lets it, and the voice stays until the note is gone.
    {
      SynthPlugin plugin;
      preparePlayback(plugin);
      const int release = kRate / 2;
      const auto audio = render(plugin, 2 * kRate, noteEvents(60, 0.8f, 0, release));
      // The damper takes 15 to 60 ms to fall and the fundamental's time constant under it is 80 ms.
      expect(rms(audio, release, release + 480) > 0.5 * rms(audio, release - 480, release),
             "Piano mode lets the string ring into the damper after release");
      expect(rms(audio, release + kRate / 4, release + kRate / 2) < 0.02 * rms(audio, release - 2400, release),
             "the damper has stopped it a quarter second later");
      expect(plugin.getEngine()->getNumActiveVoices() == 0, "the voice is freed once the note is gone");
    }
    // Piano mode ignores Env 1: a slow attack and a long release change nothing.
    {
      SynthPlugin plugin;
      preparePlayback(plugin);
      const auto plain = render(plugin, kRate, noteEvents(60, 0.8f, 0, kRate / 2));
      setControl(plugin, "env_1_attack", 1.0f);
      setControl(plugin, "env_1_release", 1.0f);
      const auto shaped = render(plugin, kRate, noteEvents(60, 0.8f, 0, kRate / 2));
      expect(rms(shaped, 0, 2400) > 0.9 * rms(plain, 0, 2400), "Piano mode sounds at once whatever Env 1's attack");
      expect(rms(shaped, kRate - 4800, kRate) < 1.5 * rms(plain, kRate - 4800, kRate) + 1.0e-6,
             "Piano mode ends with the damper whatever Env 1's release");
    }
    // Envelope 1 mode: Env 1 shapes the note and ends the voice.
    {
      SynthPlugin plugin;
      preparePlayback(plugin);
      setControl(plugin, "osc_1_piano_amp_env", 1.0f);
      const int release = kRate / 2;
      const auto audio = render(plugin, kRate, noteEvents(60, 0.8f, 0, release));
      // What remains is the sympathetic strings' ring, which plays outside the voice.
      expect(rms(audio, release + 2400, kRate) < 1.0e-4, "Envelope 1 mode with no release stops at the key");
      expect(plugin.getEngine()->getNumActiveVoices() == 0, "the voice is freed with the envelope");
      setControl(plugin, "env_1_attack", 1.0f);
      const auto slow = render(plugin, kRate, noteEvents(60, 0.8f, 0, release));
      expect(rms(slow, 0, 2400) < 0.2 * rms(audio, 0, 2400), "Envelope 1 mode follows Env 1's attack");
    }
    // Voices run out: a note stolen for another fades instead of clicking.
    {
      SynthPlugin plugin;
      preparePlayback(plugin);
      setControl(plugin, "polyphony", 2.0f);
      std::vector<MidiEvent> events;
      for (int n = 0; n < 6; ++n)
        events.push_back({ n * 2400, juce::MidiMessage::noteOn(1, 48 + n * 4, 0.9f) });
      for (int n = 0; n < 6; ++n)
        events.push_back({ kRate + n * 100, juce::MidiMessage::noteOff(1, 48 + n * 4, 0.5f) });
      const auto audio = render(plugin, 2 * kRate, events);
      expect(finite(audio), "stealing voices keeps the output finite");
      double jump = 0.0;
      for (size_t i = 1; i < audio.size(); ++i)
        jump = std::max(jump, double(std::abs(audio[i] - audio[i - 1])));
      std::cout << "largest step between samples while stealing: " << 20.0 * std::log10(jump / peak(audio)) << " dB re peak\n";
      expect(jump < 0.5 * peak(audio), "no click when a voice is stolen");
    }
    return finish("envelope");
  }

  // ---- Pedals and the body ---------------------------------------------------------

  // One scenario through a fresh plugin, with controls set first.
  std::vector<float> renderEvents(const std::vector<MidiEvent>& events, int samples,
                                  const std::vector<std::pair<std::string, float>>& controls = {},
                                  int* voices_after = nullptr) {
    SynthPlugin plugin;
    preparePlayback(plugin);
    for (const auto& [name, value] : controls)
      setControl(plugin, name, value);
    const auto audio = render(plugin, samples, events);
    if (voices_after != nullptr)
      *voices_after = plugin.getEngine()->getNumActiveVoices();
    return audio;
  }

  int pedals() {
    const int release = kRate / 2;
    const auto dry = renderNote(60, 0.8f, 2 * kRate, release);
    // The sustain pedal holds a released note; lifting it damps it.
    {
      std::vector<MidiEvent> events = noteEvents(60, 0.8f, 0, release);
      events.insert(events.begin(), { 0, juce::MidiMessage::controllerEvent(1, 64, 127) });
      events.push_back({ kRate + kRate / 2, juce::MidiMessage::controllerEvent(1, 64, 0) });
      int voices = -1;
      const auto held = renderEvents(events, 3 * kRate, {}, &voices);
      expect(rms(held, kRate, kRate + 4800) > 5.0 * rms(dry, kRate, kRate + 4800), "the sustain pedal holds a released note");
      expect(rms(held, 2 * kRate - 4800, 2 * kRate) < 0.1 * rms(held, kRate + kRate / 4, kRate + kRate / 2),
             "lifting the pedal damps it");
      expect(voices == 0, "the voice is freed once the pedal lets the note go");
    }
    // Half pedal: the dampers rest lightly, between held and dry.
    {
      std::vector<MidiEvent> events = noteEvents(60, 0.8f, 0, release);
      events.insert(events.begin(), { 0, juce::MidiMessage::controllerEvent(1, 64, 64) });
      const auto half = renderEvents(events, 2 * kRate);
      const double level = rms(half, kRate, kRate + 4800);
      expect(level > 1.5 * rms(dry, kRate, kRate + 4800), "half pedalling lets the note ring on");
    }
    // Sostenuto holds the note down when it is pressed, not one struck after.
    {
      std::vector<MidiEvent> events = noteEvents(60, 0.8f, 0, release);
      events.push_back({ 2400, juce::MidiMessage::controllerEvent(1, 66, 127) });
      events.push_back({ kRate + 2400, juce::MidiMessage::noteOn(1, 67, 0.8f) });
      events.push_back({ kRate + 4800, juce::MidiMessage::noteOff(1, 67, 0.5f) });
      events.push_back({ kRate + kRate / 2, juce::MidiMessage::controllerEvent(1, 66, 0) });
      const auto held = renderEvents(events, 2 * kRate);
      expect(rms(held, kRate, kRate + 2400) > 5.0 * rms(dry, kRate, kRate + 2400), "sostenuto holds the note pressed before it");
      expect(toneAmplitude(held, kRate + 24000, kRate + 36000, 392.0) < 0.05 * toneAmplitude(held, kRate + 2400, kRate + 4800, 392.0),
             "a note struck after the sostenuto is damped on release");
    }
    // The soft pedal makes a gentler blow.
    {
      std::vector<MidiEvent> events = noteEvents(60, 0.8f, 0, release);
      events.insert(events.begin(), { 0, juce::MidiMessage::controllerEvent(1, 67, 127) });
      const auto soft = renderEvents(events, kRate);
      expect(rms(soft, 1200, 12000) < 0.8 * rms(dry, 1200, 12000), "the soft pedal softens the blow");
    }
    // Sympathetic resonance: with the pedal down, the other strings ring with
    // a note and their sound outlives the struck string; at 0% they stay quiet.
    {
      std::vector<MidiEvent> events = noteEvents(48, 0.95f, 0, 1200);
      events.insert(events.begin(), { 0, juce::MidiMessage::controllerEvent(1, 64, 127) });
      const auto open = renderEvents(events, kRate);
      const auto closed = renderEvents(events, kRate, { { "osc_1_piano_sympathetic", 0.0f } });
      double difference = 0.0;
      for (size_t i = 0; i < open.size(); ++i)
        difference = std::max(difference, double(std::abs(open[i] - closed[i])));
      std::cout << "sympathetic strings add " << 20.0 * std::log10(difference / peak(open)) << " dB re peak\n";
      expect(difference > 1.0e-3 * peak(open), "the sympathetic strings sound with the pedal down");
      const auto direct = renderEvents(events, kRate, { { "osc_1_destination", static_cast<float>(vital::constants::kDirectOut) } });
      expect(std::abs(rms(direct, 0, kRate) - rms(open, 0, kRate)) < 0.05 * rms(open, 0, kRate),
             "the sympathetic strings follow the slot's destination");
    }
    return finish("pedals");
  }

  // ---- Parameters -----------------------------------------------------------------

  int parameters() {
    const char* const suffixes[] = { "amp_env", "dynamics", "hardness", "action_noise", "sympathetic", "width",
                                     "tuning", "stretch", "unison", "sustain", "transpose" };
    juce::MemoryBlock state;
    {
      SynthPlugin plugin;
      preparePlayback(plugin);
      for (int slot = 1; slot <= vital::kNumOscillators; ++slot) {
        const std::string prefix = "osc_" + std::to_string(slot) + "_piano_";
        for (const char* suffix : suffixes) {
          expect(plugin.getControls().count(prefix + suffix) == 1, "control exists: " + prefix + suffix);
          expect(vital::Parameters::getDetails(prefix + suffix).version_added == 0x020132,
                 "new host version: " + prefix + suffix);
        }
      }
      expect(control(plugin, "osc_2_piano_amp_env") == 0.0f, "the Piano envelope by default");
      close(control(plugin, "osc_2_piano_dynamics"), 1.0, "Dynamics 100%", 1e-6);
      close(control(plugin, "osc_2_piano_hardness"), 1.0, "Hammer hardness 100%", 1e-6);
      close(control(plugin, "osc_2_piano_action_noise"), 1.0, "Action noise 100%", 1e-6);
      close(control(plugin, "osc_2_piano_sympathetic"), 1.0, "Sympathetic resonance 100%", 1e-6);
      close(control(plugin, "osc_2_piano_width"), 1.0, "Stereo width 100%", 1e-6);
      close(control(plugin, "osc_2_piano_tuning"), 440.0, "Tuning 440 Hz", 1e-6);
      close(control(plugin, "osc_2_piano_stretch"), 1.0, "Octave stretch 100%", 1e-6);
      close(control(plugin, "osc_2_piano_unison"), 1.0, "Unison width 100%", 1e-6);
      close(control(plugin, "osc_2_piano_sustain"), 1.0, "Sustain length 100%", 1e-6);
      expect(strings::kOscillatorTypeNames[vital::source_types::kPiano] == "Piano" &&
             strings::kPianoAmpEnvNames[0] == "Piano" && strings::kPianoAmpEnvNames[1] == "Envelope 1",
             "names are registered");
      expect(vital::Parameters::getDetails("osc_1_type").max >= vital::source_types::kPiano,
             "the type menu offers the Piano");

      setControl(plugin, "osc_1_piano_amp_env", 1.0f);
      setControl(plugin, "osc_1_piano_hardness", 1.6f);
      setControl(plugin, "osc_1_piano_tuning", 432.0f);
      setControl(plugin, "osc_1_piano_sustain", 1.5f);
      setControl(plugin, "osc_1_piano_transpose", -12.0f);
      plugin.getStateInformation(state);
    }
    {
      SynthPlugin plugin;
      plugin.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
      close(control(plugin, "osc_1_piano_amp_env"), 1.0, "amp env restored", 1e-6);
      close(control(plugin, "osc_1_piano_hardness"), 1.6, "hardness restored", 1e-6);
      close(control(plugin, "osc_1_piano_tuning"), 432.0, "tuning restored", 1e-6);
      close(control(plugin, "osc_1_piano_sustain"), 1.5, "sustain restored", 1e-6);
      close(control(plugin, "osc_1_piano_transpose"), -12.0, "transpose restored", 1e-6);
      expect(static_cast<int>(control(plugin, "osc_1_type") + 0.5f) == vital::source_types::kPiano, "type restored");
    }
    // Tuning rebuilds the model: A4 moves to the new pitch.
    {
      SynthPlugin plugin;
      preparePlayback(plugin);
      setControl(plugin, "osc_1_piano_tuning", 432.0f);
      const auto audio = render(plugin, kRate, noteEvents(69, 0.7f, 0, kRate / 2));
      close(strongestNear(audio, 2400, kRate / 2, 432.0), 432.0, "Tuning 432 Hz retunes A4", 1.0);
    }
    // Hammer hardness brightens; Action noise adds to the attack only.
    {
      const auto plain = renderNote(60, 0.7f, kRate / 2, kRate / 4);
      const auto hard = renderNote(60, 0.7f, kRate / 2, kRate / 4, { { "osc_1_piano_hardness", 2.0f } });
      const auto brightness = [](const std::vector<float>& a) {
        return toneAmplitude(a, 1200, 12000, 5.0 * 261.63) / toneAmplitude(a, 1200, 12000, 261.63);
      };
      expect(brightness(hard) > brightness(plain), "harder felt is brighter");
      const auto quiet = renderNote(60, 0.7f, kRate / 2, kRate / 4, { { "osc_1_piano_action_noise", 0.0f } });
      double attack = 0.0, later = 0.0;
      for (int i = 0; i < 480; ++i)
        attack = std::max(attack, double(std::abs(plain[static_cast<size_t>(i)] - quiet[static_cast<size_t>(i)])));
      for (int i = 9600; i < 12000; ++i)
        later = std::max(later, double(std::abs(plain[static_cast<size_t>(i)] - quiet[static_cast<size_t>(i)])));
      expect(attack > 1.0e-3 * peak(plain), "the action's knock is in the attack");
      expect(later < attack, "and fades with the body's ring");
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
    synth.parameterChanged("osc_1_type", static_cast<float>(vital::source_types::kPiano));
    SynthEditor editor(synth);
    editor.updateFullGui();
    juce::MessageManager::getInstance()->runDispatchLoopUntil(200);

    auto* section = sectionNamed(editor, "Oscillator 1");
    expect(section != nullptr, "Oscillator 1 section exists");
    if (section != nullptr) {
      auto* envelope = sliderTitled(*section, "1 Piano Amplitude Envelope");
      expect(envelope != nullptr && valueText(envelope) == "Piano" && envelope->getWantsKeyboardFocus() &&
             envelope->getDescription().contains("damper"),
             "the envelope row reads its mode and is described (" + valueText(envelope).toStdString() + ")");
      const char* const titles[] = { "1 Piano Dynamics", "1 Piano Hammer Hardness", "1 Piano Action Noise",
                                     "1 Piano Sympathetic Resonance", "1 Piano Stereo Width", "1 Piano Tuning",
                                     "1 Piano Octave Stretch", "1 Piano Unison Width", "1 Piano Sustain Length",
                                     "1 Piano Amplitude Envelope" };
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
      expect(valueText(sliderTitled(*section, "1 Piano Tuning")).startsWith("440") &&
             valueText(sliderTitled(*section, "1 Piano Dynamics")) == "100%",
             "rows read their values (" + valueText(sliderTitled(*section, "1 Piano Tuning")).toStdString() + ")");
      auto* transpose = sliderTitled(*section, "1 Piano Transpose");
      expect(transpose != nullptr && transpose->getDescription().contains("A0 to C8"), "a described Transpose row");
      expect(findTitled(*section, [](const String& t) { return t.contains("OpenWurli") || t.contains("Wave Frame"); }) == nullptr,
             "other types' rows are hidden");
    }
    return finish("editor");
  }
}

namespace {
  // ---- A MIDI file through the Piano, for listening --------------------------------

  // Renders a MIDI file through slot 1's Piano, dry, 12 dB under the engine's
  // own level (slot level 50%, the original plugin at Volume -12 dB), to a WAV
  // file. Fortissimo chords peak well above full scale at 0 dB.
  int renderFile(const std::string& midi_path, const std::string& wav_path,
                 const std::vector<std::pair<std::string, float>>& controls) {
    juce::File midi_file(midi_path);
    juce::FileInputStream stream(midi_file);
    juce::MidiFile midi;
    if (!stream.openedOk() || !midi.readFrom(stream)) {
      std::cerr << "cannot read " << midi_path << '\n';
      return 1;
    }
    // Ticks to seconds as the original's renderer does it (tools/render.cpp): the tracks' events in
    // tick order, the seconds summed event by event, so both land every note on the same sample.
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
      else if (message.isNoteOnOrOff() || message.isController()) {
        events.push_back({ static_cast<int>(seconds * kRate), message });
        last = std::max(last, seconds);
      }
    }

    SynthPlugin plugin;
    preparePlayback(plugin);
    setControl(plugin, "osc_1_level", 0.5f);
    setControl(plugin, "polyphony", 32.0f);
    for (const auto& [name, value] : controls)
      setControl(plugin, name, value);
    std::vector<float> right;
    int peak_voices = 0;
    const auto left = render(plugin, static_cast<int>((last + 3.0) * kRate), events, &right, &peak_voices);

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
    std::cout << "wrote " << wav_path << ": " << left.size() / double(kRate) << " s, peak " << 20.0 * std::log10(peak(left))
              << " dB, up to " << peak_voices << " voices of " << static_cast<int>(control(plugin, "polyphony")) << '\n';
    return 0;
  }
}

int main(int argc, char** argv) {
  juce::ScopedJuceInitialiser_GUI juce;
  const juce::String flag = argc > 1 ? juce::String(argv[1]) : juce::String();
  if (flag == "--render" && argc > 3) {
    // Further arguments set controls: osc_1_piano_action_noise=0
    std::vector<std::pair<std::string, float>> controls;
    for (int i = 4; i < argc; ++i) {
      const std::string argument = argv[i];
      const size_t equals = argument.find('=');
      if (equals != std::string::npos)
        controls.push_back({ argument.substr(0, equals), std::stof(argument.substr(equals + 1)) });
    }
    return renderFile(argv[2], argv[3], controls);
  }
  if (flag == "--oscillator") return oscillator();
  if (flag == "--envelope") return envelope();
  if (flag == "--pedals") return pedals();
  if (flag == "--parameters") return parameters();
  if (flag == "--editor") return editorTest();
  std::cerr << "usage: AtlasPianoTests --oscillator | --envelope | --pedals | --parameters | --editor | "
               "--render <midi file> <wav file> [control=value ...]\n";
  return 2;
}
