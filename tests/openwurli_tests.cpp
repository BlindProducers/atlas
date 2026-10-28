// OpenWurli tests: the oscillator type and the OpenWurli Amp effect through the
// plugin (pitch, velocity, damper, envelope modes, host state, slots, editor),
// and, when the opt-in reference renderer is built, the C++ port against the
// original Rust sample for sample and Atlas end to end against OpenWurli's
// engine.

#include "JuceHeader.h"
#include "synth_plugin.h"
#include "synth_editor.h"
#include "editor_navigation.h"
#include "parameter_slider.h"
#include "sound_engine.h"
#include "source_types.h"
#include "source_slot_module.h"
#include "openwurli_amp_module.h"
#include "openwurli_source.h"
#include "synth_strings.h"

#include "amp_reference_tests.h"
#include "voice_reference_tests.h"

#include <cmath>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

namespace {
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

  // Slot 1 is an OpenWurli oscillator to `destination`, every effect and other
  // oscillator off, Env 1 open with no release. Offline, so effects prepare
  // within the block that selects them.
  void preparePlayback(SynthPlugin& plugin, int rate = kRate,
                       int destination = vital::constants::kEffects) {
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
    plugin.parameterChanged("osc_1_type", static_cast<float>(vital::source_types::kOpenWurli));
    setControl(plugin, "osc_1_on", 1.0f);
    setControl(plugin, "osc_1_destination", static_cast<float>(destination));
    setControl(plugin, "env_1_attack", 0.0f);
    setControl(plugin, "env_1_sustain", 1.0f);
    setControl(plugin, "env_1_release", 0.0f);
    plugin.checkOversampling();
    expect(plugin.getEngine()->getSourceSlot(0)->activeType() == vital::source_types::kOpenWurli,
           "slot 1 runs the OpenWurli type");
  }

  void selectAmp(SynthPlugin& plugin, const std::string& slot_prefix) {
    plugin.valueChanged(slot_prefix + "type", static_cast<float>(vital::constants::kOpenWurliAmp + 1));
    plugin.valueChanged(slot_prefix + "enabled", 1.0f);
    plugin.getEngine()->prepareSelectedEffects();
    juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
  }

  // Renders `samples` frames with MIDI events at given samples; both channels.
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

  double midiToHz(double note) {
    return 440.0 * std::pow(2.0, (note - 69.0) / 12.0);
  }

  // ---- The oscillator -------------------------------------------------------

  // Every key of the 200A sounds at its pitch, louder with velocity.
  int oscillator() {
    for (int note : { 33, 45, 52, 53, 60, 72, 84, 96 }) {
      SynthPlugin plugin;
      preparePlayback(plugin);
      const auto audio = render(plugin, kRate, { { 0, juce::MidiMessage::noteOn(1, note, 1.0f) } });
      expect(finite(audio), "finite output, note " + std::to_string(note));
      expect(rms(audio, 0, kRate / 2) > 1.0e-3, "note " + std::to_string(note) + " sounds");
      expect(peak(audio) < 2.0, "note " + std::to_string(note) + " stays bounded");
      const double expected = midiToHz(note);
      const double measured = pitch(audio, kRate / 10, kRate / 10 + kRate / 4, kRate, expected / 1.5, expected * 1.5);
      close(measured / expected, 1.0, "note " + std::to_string(note) + " pitch", 0.004);
    }

    SynthPlugin plugin;
    preparePlayback(plugin);
    const auto soft = render(plugin, kRate / 2, { { 0, juce::MidiMessage::noteOn(1, 60, 0.2f) } });
    render(plugin, kRate, { { 0, juce::MidiMessage::noteOff(1, 60) } });
    const auto loud = render(plugin, kRate / 2, { { 0, juce::MidiMessage::noteOn(1, 60, 1.0f) } });
    expect(rms(loud, 0, kRate / 2) > 2.0 * rms(soft, 0, kRate / 2), "velocity strikes harder");

    // Outside the 200A's range the nearest reed plays at the played pitch.
    for (int note : { 24, 108 }) {
      SynthPlugin outside;
      preparePlayback(outside);
      const auto audio = render(outside, kRate / 2, { { 0, juce::MidiMessage::noteOn(1, note, 0.8f) } });
      const double expected = midiToHz(note);
      expect(finite(audio) && rms(audio, 0, kRate / 2) > 1.0e-4, "note " + std::to_string(note) + " sounds");
      close(pitch(audio, kRate / 10, kRate / 2, kRate, expected / 1.5, expected * 1.5) / expected, 1.0,
            "note " + std::to_string(note) + " pitch", 0.006);
    }
    return finish("oscillator");
  }

  // Transpose, tune and pitch bend move the reed; rendering is deterministic.
  int pitchControls() {
    {
      SynthPlugin plugin;
      preparePlayback(plugin);
      setControl(plugin, "osc_1_wurli_transpose", 12.0f);
      setControl(plugin, "osc_1_wurli_tune", 0.5f);
      const auto audio = render(plugin, kRate / 2, { { 0, juce::MidiMessage::noteOn(1, 57, 0.8f) } });
      const double expected = midiToHz(69.5);
      close(pitch(audio, kRate / 10, kRate / 2, kRate, expected / 1.5, expected * 1.5) / expected, 1.0,
            "transpose and tune", 0.004);
    }
    {
      SynthPlugin plugin;
      preparePlayback(plugin);
      const float range = control(plugin, "pitch_bend_range");
      const auto audio = render(plugin, kRate, { { 0, juce::MidiMessage::noteOn(1, 60, 0.8f) },
                                                 { kRate / 2, juce::MidiMessage::pitchWheel(1, 16383) } });
      const double expected = midiToHz(60.0 + range * (8191.0 / 8192.0));
      close(pitch(audio, kRate / 2 + kRate / 10, kRate, kRate, expected / 1.5, expected * 1.5) / expected, 1.0,
            "pitch bend moves the reed", 0.004);
    }
    std::vector<float> first;
    for (int run = 0; run < 2; ++run) {
      SynthPlugin plugin;
      preparePlayback(plugin);
      auto audio = render(plugin, kRate / 2, { { 3, juce::MidiMessage::noteOn(1, 48, 0.9f) },
                                               { 100, juce::MidiMessage::noteOn(1, 67, 0.5f) } });
      if (run == 0)
        first = audio;
      else
        expect(audio == first, "two renders are identical");
    }
    return finish("pitch");
  }

  // Reed mode: the reed shapes the note and the damper ends it; Envelope 1
  // mode: Env 1 does. A held reed decays on its own.
  int envelope() {
    const int off = kRate / 2;
    {
      SynthPlugin plugin;
      preparePlayback(plugin);
      expect(control(plugin, "osc_1_wurli_amp_env") == 0.0f, "Reed mode is the default");
      const auto audio = render(plugin, 2 * kRate, { { 0, juce::MidiMessage::noteOn(1, 48, 0.8f) },
                                                     { off, juce::MidiMessage::noteOff(1, 48) } });
      expect(rms(audio, off + kRate / 500, off + kRate / 100) > 0.1 * rms(audio, off - kRate / 50, off),
             "in Reed mode the damper, not Env 1, ends the note");
      expect(rms(audio, off + kRate / 2, off + kRate) < 1.0e-5, "the damped reed falls silent");
      expect(plugin.getEngine()->getNumActiveVoices() == 0, "the voice ends once the reed is silent");
    }
    {
      SynthPlugin plugin;
      preparePlayback(plugin);
      setControl(plugin, "osc_1_wurli_amp_env", 1.0f);
      const auto audio = render(plugin, kRate, { { 0, juce::MidiMessage::noteOn(1, 48, 0.8f) },
                                                 { off, juce::MidiMessage::noteOff(1, 48) } });
      expect(rms(audio, 0, off) > 1.0e-3, "Envelope 1 mode sounds");
      expect(rms(audio, off + kRate / 100, off + kRate / 50) < 0.02 * rms(audio, off - kRate / 50, off),
             "in Envelope 1 mode Env 1's release ends the note");
    }
    {
      SynthPlugin plugin;
      preparePlayback(plugin);
      const auto audio = render(plugin, 3 * kRate, { { 0, juce::MidiMessage::noteOn(1, 79, 1.0f) } });
      expect(rms(audio, 5 * kRate / 2, 3 * kRate) < 0.5 * rms(audio, kRate / 10, kRate / 2),
             "a held treble reed decays");
    }
    return finish("envelope");
  }

  // Parameters exist on every slot, survive host state, and default as OpenWurli does.
  int parameters() {
    juce::MemoryBlock state;
    {
      SynthPlugin plugin;
      preparePlayback(plugin);
      for (int slot = 1; slot <= vital::kNumOscillators; ++slot) {
        const std::string prefix = "osc_" + std::to_string(slot) + "_wurli_";
        for (const char* suffix : { "amp_env", "mlp", "bark", "damping", "condition", "treble_voicing",
                                    "register_balance", "pickup_low_cut", "transpose", "tune" }) {
          expect(plugin.getControls().count(prefix + suffix) == 1, "control exists: " + prefix + suffix);
          expect(vital::Parameters::getDetails(prefix + suffix).version_added == 0x02011c,
                 "new host version: " + prefix + suffix);
        }
      }
      expect(control(plugin, "osc_2_wurli_mlp") == 0.0f, "MLP corrections are off by default");
      expect(control(plugin, "osc_2_wurli_bark") == 0.5f && control(plugin, "osc_2_wurli_damping") == 1.0f &&
             control(plugin, "osc_2_wurli_condition") == 0.0f && control(plugin, "osc_2_wurli_treble_voicing") == 0.0f &&
             control(plugin, "osc_2_wurli_register_balance") == 1.0f &&
             control(plugin, "osc_2_wurli_pickup_low_cut") == 0.0f, "voicing defaults to OpenWurli's calibration");
      for (int chain = 0; chain <= vital::kNumBuses; ++chain) {
        const std::string chain_prefix = chain == 0 ? "" : "bus_" + std::to_string(chain) + "_";
        // Slots switch with fx_slot_N_enabled; "_on" belongs to each chain's fixed-order set.
        expect(plugin.getControls().count(chain_prefix + "openwurli_amp_on") == 1,
               "control exists: " + chain_prefix + "openwurli_amp_on");
        for (int slot = 1; slot <= vital::constants::kMaxEffectSlots; ++slot) {
          const std::string prefix = chain_prefix + "fx_slot_" + std::to_string(slot) + "_openwurli_amp_";
          for (const char* suffix : { "volume", "tremolo_depth", "speaker" })
            expect(plugin.getControls().count(prefix + suffix) == 1, "control exists: " + prefix + suffix);
        }
      }
      expect(vital::Parameters::getDetails("effect_chain_slot_38").version_added == 0x02011c &&
             vital::Parameters::getDetails("openwurli_amp_volume").version_added == 0x02011c,
             "the amp's parameters take a new host version");
      expect(strings::kEffectOrder[vital::constants::kOpenWurliAmp] == "openwurli_amp" &&
             strings::kEffectSlotTypeNames[vital::constants::kOpenWurliAmp + 1] == "OpenWurli Amp" &&
             strings::kOscillatorTypeNames[vital::source_types::kOpenWurli] == "OpenWurli",
             "names are registered");
      close(control(plugin, "fx_slot_1_openwurli_amp_volume"), 0.8, "volume defaults to 80%", 1.0e-6);
      close(control(plugin, "fx_slot_1_openwurli_amp_tremolo_depth"), 0.5, "tremolo depth defaults to 50%", 1.0e-6);
      close(control(plugin, "fx_slot_1_openwurli_amp_speaker"), 0.0, "speaker character defaults to 0%", 1.0e-6);
      setControl(plugin, "osc_1_wurli_amp_env", 1.0f);
      setControl(plugin, "osc_1_wurli_mlp", 1.0f);
      setControl(plugin, "osc_1_wurli_transpose", -5.0f);
      setControl(plugin, "osc_1_wurli_tune", 0.25f);
      setControl(plugin, "osc_1_wurli_bark", 0.8f);
      setControl(plugin, "osc_1_wurli_damping", 2.0f);
      setControl(plugin, "osc_1_wurli_condition", 0.4f);
      setControl(plugin, "osc_1_wurli_treble_voicing", -3.0f);
      setControl(plugin, "osc_1_wurli_register_balance", 0.0f);
      setControl(plugin, "osc_1_wurli_pickup_low_cut", 1.0f);
      selectAmp(plugin, "bus_2_fx_slot_7_");
      setControl(plugin, "bus_2_fx_slot_7_openwurli_amp_volume", 0.3f);
      setControl(plugin, "bus_2_fx_slot_7_openwurli_amp_tremolo_depth", 0.9f);
      setControl(plugin, "bus_2_fx_slot_7_openwurli_amp_speaker", 0.6f);
      plugin.getStateInformation(state);
    }
    {
      SynthPlugin plugin;
      preparePlayback(plugin);
      plugin.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
      plugin.checkOversampling();
      expect(plugin.getEngine()->getSourceSlot(0)->activeType() == vital::source_types::kOpenWurli,
             "the restored slot runs OpenWurli");
      expect(control(plugin, "osc_1_wurli_amp_env") == 1.0f && control(plugin, "osc_1_wurli_mlp") == 1.0f &&
             control(plugin, "osc_1_wurli_transpose") == -5.0f, "oscillator settings are restored");
      close(control(plugin, "osc_1_wurli_tune"), 0.25, "tune is restored", 1.0e-6);
      close(control(plugin, "osc_1_wurli_bark"), 0.8, "bark is restored", 1.0e-6);
      close(control(plugin, "osc_1_wurli_damping"), 2.0, "air damping is restored", 1.0e-6);
      close(control(plugin, "osc_1_wurli_condition"), 0.4, "condition is restored", 1.0e-6);
      close(control(plugin, "osc_1_wurli_treble_voicing"), -3.0, "treble voicing is restored", 1.0e-6);
      expect(control(plugin, "osc_1_wurli_register_balance") == 0.0f &&
             control(plugin, "osc_1_wurli_pickup_low_cut") == 1.0f, "register balance and pickup low cut are restored");
      expect(control(plugin, "bus_2_fx_slot_7_type") == vital::constants::kOpenWurliAmp + 1, "the amp slot is restored");
      close(control(plugin, "bus_2_fx_slot_7_openwurli_amp_volume"), 0.3, "volume is restored", 1.0e-6);
      close(control(plugin, "bus_2_fx_slot_7_openwurli_amp_tremolo_depth"), 0.9, "depth is restored", 1.0e-6);
      close(control(plugin, "bus_2_fx_slot_7_openwurli_amp_speaker"), 0.6, "character is restored", 1.0e-6);
      const auto audio = render(plugin, kRate / 4, { { 0, juce::MidiMessage::noteOn(1, 60, 1.0f) } });
      expect(finite(audio) && rms(audio, 0, kRate / 4) > 1.0e-3, "the restored patch plays");
    }
    return finish("parameters");
  }

  // Amplitude of the component at `hz` over [start, end) (Goertzel).
  double toneAmplitude(const std::vector<float>& signal, int start, int end, int rate, double hz) {
    const double w = 2.0 * 3.14159265358979323846 * hz / rate;
    const double coefficient = 2.0 * std::cos(w);
    double s1 = 0.0, s2 = 0.0;
    for (int i = start; i < end; ++i) {
      const double s0 = signal[static_cast<size_t>(i)] + coefficient * s1 - s2;
      s2 = s1;
      s1 = s0;
    }
    const double power = s1 * s1 + s2 * s2 - coefficient * s1 * s2;
    return 2.0 * std::sqrt(std::max(0.0, power)) / (end - start);
  }

  // Energy in harmonics `first`-`last` against the fundamental, from 100 to 500 ms.
  double harmonicRatio(const std::vector<float>& signal, int note, int first = 2, int last = 8) {
    const double f0 = midiToHz(note);
    const int start = kRate / 10;
    const int end = kRate / 2;
    double upper = 0.0;
    for (int h = first; h <= last; ++h)
      upper += std::pow(toneAmplitude(signal, start, end, kRate, h * f0), 2.0);
    return upper / std::pow(toneAmplitude(signal, start, end, kRate, f0), 2.0);
  }

  // One note through a fresh plugin with the given oscillator controls.
  std::vector<float> renderVoiced(int note, float velocity, const std::vector<std::pair<std::string, float>>& controls,
                                  int samples = kRate) {
    SynthPlugin plugin;
    preparePlayback(plugin);
    for (const auto& [name, value] : controls)
      setControl(plugin, "osc_1_wurli_" + name, value);
    return render(plugin, samples, { { 0, juce::MidiMessage::noteOn(1, note, velocity) } });
  }

  double db(double ratio) {
    return 20.0 * std::log10(std::max(1.0e-30, ratio));
  }

  // Each voicing control moves the sound as it says, and at its default leaves
  // OpenWurli's calibration untouched.
  int voicingControls() {
    const auto plain = renderVoiced(45, 0.8f, {});
    const auto defaults = renderVoiced(45, 0.8f, { { "bark", 0.5f }, { "damping", 1.0f }, { "condition", 0.0f },
                                                   { "treble_voicing", 0.0f }, { "register_balance", 1.0f },
                                                   { "pickup_low_cut", 0.0f } });
    expect(plain == defaults, "the defaults are OpenWurli's calibration");

    // Bark: a deeper swing into the pickup raises the upper harmonics, and the level with them
    // (OpenWurli levels the keyboard against middle C at the same swing).
    const auto soft_bark = renderVoiced(45, 0.8f, { { "bark", 0.0f } });
    const auto hard_bark = renderVoiced(45, 0.8f, { { "bark", 1.0f } });
    const double ratios[] = { harmonicRatio(soft_bark, 45), harmonicRatio(plain, 45), harmonicRatio(hard_bark, 45) };
    std::cout << "bark 0/50/100%: harmonics to fundamental " << db(std::sqrt(ratios[0])) << " / "
              << db(std::sqrt(ratios[1])) << " / " << db(std::sqrt(ratios[2])) << " dB, level "
              << db(rms(soft_bark, kRate / 10, kRate / 2) / rms(plain, kRate / 10, kRate / 2)) << " / 0 / "
              << db(rms(hard_bark, kRate / 10, kRate / 2) / rms(plain, kRate / 10, kRate / 2)) << " dB\n";
    expect(ratios[0] < ratios[1] && ratios[1] < ratios[2] && ratios[2] > 2.0 * ratios[0], "bark raises the harmonics");
    expect(std::abs(db(rms(hard_bark, kRate / 10, kRate / 2) / rms(plain, kRate / 10, kRate / 2))) < 6.0 &&
           std::abs(db(rms(soft_bark, kRate / 10, kRate / 2) / rms(plain, kRate / 10, kRate / 2))) < 6.0,
           "bark moves the level by a few decibels only");

    // Air damping: a loud bass note loses more in its first second with more drag.
    auto fall = [](const std::vector<float>& audio) {
      return db(rms(audio, kRate / 20, kRate / 5) / rms(audio, 9 * kRate / 10, kRate));
    };
    const auto no_drag = renderVoiced(40, 1.0f, { { "damping", 0.0f } });
    const auto more_drag = renderVoiced(40, 1.0f, { { "damping", 2.5f } });
    std::cout << "air damping 0/100/250%: first-second fall " << fall(no_drag) << " / "
              << fall(renderVoiced(40, 1.0f, {})) << " / " << fall(more_drag) << " dB\n";
    expect(fall(more_drag) > fall(no_drag) + 1.0, "air damping speeds the early decay");

    // Condition: each key moves by its own offset, wider at 100%.
    const int key = 61;
    const auto in_tune = renderVoiced(key, 0.8f, {});
    const auto worn = renderVoiced(key, 0.8f, { { "condition", 1.0f } });
    const double expected_hz = midiToHz(key);
    const double moved = 1200.0 * std::log2(pitch(worn, kRate / 5, kRate, kRate, expected_hz / 1.2, expected_hz * 1.2) /
                                            pitch(in_tune, kRate / 5, kRate, kRate, expected_hz / 1.2, expected_hz * 1.2));
    const double unit = openwurli::detuneUnit(key);
    const double expected = 1200.0 * std::log2((1.0 + unit * 0.00173 * (1.0 + 11.0 / 3.0)) / (1.0 + unit * 0.00173));
    std::cout << "condition 100%: key " << key << " moved " << moved << " cents, expected " << expected << '\n';
    close(moved, expected, "condition widens the key's tuning offset", 1.0);

    // Treble voicing: the top key moves by exactly the decibels set, middle C not at all.
    const auto top = renderVoiced(96, 1.0f, {}, kRate / 2);
    const auto top_up = renderVoiced(96, 1.0f, { { "treble_voicing", 12.0f } }, kRate / 2);
    close(db(rms(top_up, kRate / 10, kRate / 2) / rms(top, kRate / 10, kRate / 2)), 12.0,
          "treble voicing sets the top key's level", 0.05);
    expect(renderVoiced(60, 1.0f, { { "treble_voicing", -12.0f } }, kRate / 4) == renderVoiced(60, 1.0f, {}, kRate / 4),
           "treble voicing leaves middle C alone");

    // Register balance: C6 carries a +3.6 dB trim at full velocity, middle C none.
    const auto c6 = renderVoiced(84, 1.0f, {}, kRate / 2);
    const auto c6_raw = renderVoiced(84, 1.0f, { { "register_balance", 0.0f } }, kRate / 2);
    close(db(rms(c6, kRate / 10, kRate / 2) / rms(c6_raw, kRate / 10, kRate / 2)), 3.6,
          "register balance applies OpenWurli's trim", 0.05);
    expect(renderVoiced(60, 1.0f, { { "register_balance", 0.0f } }, kRate / 4) == renderVoiced(60, 1.0f, {}, kRate / 4),
           "middle C has no register trim");

    // Pickup low cut: the 1436 Hz corner thins middle C's fundamental against its harmonics
    // around 1-2 kHz (everything well below both corners drops alike).
    const auto middle = renderVoiced(60, 0.8f, {});
    const auto bright = renderVoiced(60, 0.8f, { { "pickup_low_cut", 1.0f } });
    const double thinning = db(std::sqrt(harmonicRatio(bright, 60, 4, 8) / harmonicRatio(middle, 60, 4, 8)));
    std::cout << "pickup low cut 1436 Hz: middle C harmonics 4-8 against the fundamental " << thinning << " dB\n";
    expect(thinning > 1.0, "the higher pickup corner thins the fundamental");
    return finish("voicing");
  }

  // ---- The amp ---------------------------------------------------------------

  // The amp runs in a main slot and a bus slot, is mono, follows its controls
  // and keeps a silent input silent after its warm-up.
  int amp() {
    for (int chain = 0; chain <= vital::kNumBuses; ++chain) {
      SynthPlugin plugin;
      const int destination = chain == 0 ? vital::constants::kEffects
                            : chain == 1 ? vital::constants::kBus1
                            : chain == 2 ? vital::constants::kBus2 : vital::constants::kBus3;
      preparePlayback(plugin, kRate, destination);
      const std::string slot = (chain == 0 ? std::string() : "bus_" + std::to_string(chain) + "_") + "fx_slot_3_";
      selectAmp(plugin, slot);
      std::vector<float> right;
      const auto silence = render(plugin, kRate / 4, {}, &right);
      expect(finite(silence) && peak(silence) < 1.0e-3, "a silent input stays quiet, chain " + std::to_string(chain));
      const auto audio = render(plugin, kRate, { { 0, juce::MidiMessage::noteOn(1, 60, 0.8f) },
                                                 { 0, juce::MidiMessage::noteOn(1, 64, 0.8f) } }, &right);
      expect(finite(audio) && rms(audio, 0, kRate) > 1.0e-3, "the amp plays, chain " + std::to_string(chain));
      expect(audio == right, "the amp is mono, chain " + std::to_string(chain));
    }

    // Tremolo depth: the level swings at about 5.6 Hz with depth, not without.
    auto envelopeSwing = [](float depth) {
      SynthPlugin plugin;
      preparePlayback(plugin);
      selectAmp(plugin, "fx_slot_1_");
      setControl(plugin, "fx_slot_1_openwurli_amp_tremolo_depth", depth);
      const auto audio = render(plugin, 2 * kRate, { { 0, juce::MidiMessage::noteOn(1, 41, 1.0f) } });
      // RMS in 20 ms windows over the second second, divided by its trend.
      std::vector<double> windows;
      for (int start = kRate; start + kRate / 50 <= 2 * kRate; start += kRate / 50)
        windows.push_back(rms(audio, start, start + kRate / 50));
      double swing = 0.0;
      for (size_t i = 2; i + 2 < windows.size(); ++i) {
        const double trend = (windows[i - 2] + windows[i + 2]) * 0.5;
        swing = std::max(swing, std::abs(windows[i] - trend) / std::max(1.0e-9, trend));
      }
      return swing;
    };
    const double still = envelopeSwing(0.0f);
    const double deep = envelopeSwing(1.0f);
    std::cout << "tremolo swing: depth 0 " << still << ", depth 1 " << deep << '\n';
    expect(deep > 3.0 * still && deep > 0.05, "tremolo depth modulates the level");

    // Volume: lower volume is quieter.
    auto level = [](float volume) {
      SynthPlugin plugin;
      preparePlayback(plugin);
      selectAmp(plugin, "fx_slot_1_");
      setControl(plugin, "fx_slot_1_openwurli_amp_tremolo_depth", 0.0f);
      setControl(plugin, "fx_slot_1_openwurli_amp_volume", volume);
      const auto audio = render(plugin, kRate / 2, { { 0, juce::MidiMessage::noteOn(1, 60, 0.8f) } });
      return rms(audio, kRate / 10, kRate / 2);
    };
    expect(level(0.3f) < 0.5 * level(0.8f), "volume sets the level");
    return finish("amp");
  }

  // ---- Editor ---------------------------------------------------------------------

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
    return editor_navigation::focusHeading(editor, title, text);
  }

  // The slider titled `title` under `parent` (its row carries the same title).
  ParameterSlider* sliderTitled(Component& parent, const String& title) {
    if (auto* slider = dynamic_cast<ParameterSlider*>(&parent); slider != nullptr && slider->getTitle() == title)
      return slider;
    for (auto* child : parent.getChildren()) {
      if (auto* found = sliderTitled(*child, title))
        return found;
    }
    return nullptr;
  }

  // What a screen reader reads as the slider's value.
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
    synth.parameterChanged("osc_1_type", static_cast<float>(vital::source_types::kOpenWurli));
    synth.valueChanged("fx_slot_2_type", static_cast<float>(vital::constants::kOpenWurliAmp + 1));
    synth.valueChanged("fx_slot_2_enabled", 1.0f);
    SynthEditor editor(synth);
    editor.updateFullGui();
    pumpUntil([] { return false; }, 200);

    auto* section = sectionNamed(editor, "Oscillator 1");
    expect(section != nullptr, "Oscillator 1 section exists");
    if (section != nullptr) {
      // A two-way named choice is an indexed slider that reads its names.
      auto* envelope = sliderTitled(*section, "1 OpenWurli Amplitude Envelope");
      expect(envelope != nullptr, "an amplitude envelope row");
      if (envelope != nullptr) {
        expect(valueText(envelope) == "Reed", "the envelope mode reads by name (" + valueText(envelope).toStdString() + ")");
        expect(envelope->getWantsKeyboardFocus() && envelope->getDescription().contains("damper"),
               "the envelope row is accessible and described");
      }
      expect(titledExactly(*section, "OpenWurli controls") != nullptr, "an OpenWurli controls group");
      for (const char* title : { "1 OpenWurli Bark", "1 OpenWurli Air Damping", "1 OpenWurli Condition",
                                 "1 OpenWurli Treble Voicing", "1 OpenWurli Pickup Low Cut" }) {
        auto* row = sliderTitled(*section, title);
        expect(row != nullptr && row->getDescription().isNotEmpty(), std::string("a described row: ") + title);
      }
      expect(valueText(sliderTitled(*section, "1 OpenWurli Bark")) == "50%" &&
             valueText(sliderTitled(*section, "1 OpenWurli Air Damping")) == "100%" &&
             valueText(sliderTitled(*section, "1 OpenWurli Pickup Low Cut")) == "880 Hz",
             "voicing rows read their values (" + valueText(sliderTitled(*section, "1 OpenWurli Bark")).toStdString() +
             ", " + valueText(sliderTitled(*section, "1 OpenWurli Air Damping")).toStdString() + ", " +
             valueText(sliderTitled(*section, "1 OpenWurli Pickup Low Cut")).toStdString() + ")");
      expect(titledExactly(*section, "1 OpenWurli Register Balance") != nullptr, "a register balance switch");
      auto* mlp = titledExactly(*section, "1 OpenWurli MLP Corrections");
      expect(mlp != nullptr && mlp->getDescription().contains("recordings"), "MLP corrections are described");
      if (auto* pitch_group = titledExactly(*section, "Pitch and unison")) {
        expect(titledExactly(*pitch_group, "1 OpenWurli Transpose") != nullptr &&
               titledExactly(*pitch_group, "1 OpenWurli Tune") != nullptr, "transpose and tune sit with pitch");
      } else {
        expect(false, "a Pitch and unison group");
      }
      expect(findTitled(*section, [](const String& t) { return t.contains("Macro ") || t.contains("Wave Frame"); }) == nullptr,
             "other types' rows are hidden");
      expect(findTitled(*section, [](const String& t) { return t.startsWith("Oscillator 1") &&
                                                                 t.contains("openwurli"); }) != nullptr,
             "the section title names the type");
    }

    expect(selectEditorItem(editor, "Editor group", "Effects") && selectEditorItem(editor, "Editor element", "Chain and routing"),
           "the effect chain page opens");
    pumpUntil([] { return false; }, 200);
    auto* slot = effectSlotComponent(editor, "Chain and routing", 1);
    expect(slot != nullptr && slot->getTitle().contains("OpenWurli Amp"), "the slot is titled after the amp");
    if (slot != nullptr) {
      auto* volume = sliderTitled(*slot, "Volume");
      auto* depth = sliderTitled(*slot, "Tremolo Depth");
      auto* speaker = sliderTitled(*slot, "Speaker Character");
      expect(volume != nullptr && depth != nullptr && speaker != nullptr, "the amp's rows");
      if (volume != nullptr && depth != nullptr && speaker != nullptr) {
        expect(volume->getScreenY() < depth->getScreenY() && depth->getScreenY() < speaker->getScreenY(),
               "rows in order: Volume, Tremolo Depth, Speaker Character");
        expect(valueText(volume) == "80%" && valueText(depth) == "50%" && valueText(speaker) == "0%",
               "values read as percentages (" + valueText(volume).toStdString() + ", " +
               valueText(depth).toStdString() + ", " + valueText(speaker).toStdString() + ")");
        expect(volume->getDescription().contains("volume pot") && depth->getDescription().contains("5.6 hertz") &&
               speaker->getDescription().contains("cabinet"), "the rows are described");
      }
    }
    return finish("editor");
  }

  // ---- Against the original ---------------------------------------------------

  std::string scratchDirectory() {
    if (const char* kept = std::getenv("ATLAS_OPENWURLI_SCRATCH"))
      return kept;
    auto directory = juce::File::getSpecialLocation(juce::File::tempDirectory)
                         .getNonexistentChildFile("atlas-openwurli-reference", "", false);
    directory.createDirectory();
    return directory.getFullPathName().toStdString();
  }

  // Atlas against a reference render, from the second internal block on: a
  // voice's amplitude ramps up from 0 across its first block, for every
  // oscillator type. `gain` is the best fit of actual to expected; `residual`
  // is what that gain leaves, relative to the expected signal.
  struct Match {
    double gain = 0.0;
    double residual = 0.0;
  };

  Match match(const std::vector<float>& actual, const std::vector<float>& expected) {
    double cross = 0.0, energy = 0.0;
    const size_t count = std::min(expected.size(), actual.size());
    for (size_t i = kBlock; i < count; ++i) {
      cross += double(actual[i]) * expected[i];
      energy += double(expected[i]) * expected[i];
    }
    Match result;
    result.gain = cross / std::max(1.0e-30, energy);
    double error = 0.0;
    for (size_t i = kBlock; i < count; ++i)
      error += (double(actual[i]) - result.gain * expected[i]) * (double(actual[i]) - result.gain * expected[i]);
    result.residual = std::sqrt(error / std::max(1.0e-30, energy));
    return result;
  }

  void report(const char* what, int rate, int note, const Match& result) {
    const double gain_db = 20.0 * std::log10(std::max(1.0e-30, result.gain));
    std::cout << what << " rate=" << rate << " note=" << note << " gain=" << gain_db << " dB residual="
              << result.residual << '\n';
    // Atlas's master volume at "0 dB" is a gain of 1.0013 (its dB conversion
    // is approximate); anything else would be a level error in the port.
    expect(std::abs(gain_db) < 0.02, std::string(what) + " level matches OpenWurli");
    expect(result.residual < 1.0e-6, std::string(what) + " matches OpenWurli sample for sample");
  }

  // The oscillator alone at its defaults against OpenWurli's Voice: the pickup
  // voltage times 100, times 0.5 for the default level.
  int oscillatorReference(const std::string& reference) {
    const std::string directory = scratchDirectory();
    for (int rate : { 44100, 48000, 96000 }) {
      for (int note : { 40, 60, 76 }) {
        const float velocity = 100.0f / 127.0f;
        const std::string output = directory + "/voice_" + std::to_string(rate) + "_" + std::to_string(note) + ".f64";
        char velocity_text[32];
        std::snprintf(velocity_text, sizeof(velocity_text), "%.9g", velocity);
        const uint32_t seed = static_cast<uint32_t>(note) * 2654435761u + 1u;
        const std::string command = "\"" + reference + "\" voice --note " + std::to_string(note) + " --velocity " +
                                    velocity_text + " --seconds 2 --rate " + std::to_string(rate) + " --seed " +
                                    std::to_string(seed) + " --out \"" + output + "\"";
        expect(std::system(command.c_str()) == 0, "the reference renders");
        std::ifstream stream(output, std::ios::binary);
        std::vector<double> volts(static_cast<size_t>(2 * rate));
        stream.read(reinterpret_cast<char*>(volts.data()), static_cast<std::streamsize>(volts.size() * sizeof(double)));
        expect(stream.good(), "the reference wrote every sample");
        std::vector<float> expected(volts.size());
        for (size_t i = 0; i < volts.size(); ++i)
          expected[i] = static_cast<float>(volts[i] * vital::OpenWurliSource::kPickupToOutput * 0.5);

        SynthPlugin plugin;
        preparePlayback(plugin, rate);
        setControl(plugin, "volume", 6400.0f);
        const auto actual = render(plugin, 2 * rate, { { 0, juce::MidiMessage::noteOn(1, note, velocity) } });
        std::ofstream(directory + "/atlas_" + std::to_string(rate) + "_" + std::to_string(note) + ".f32", std::ios::binary)
            .write(reinterpret_cast<const char*>(actual.data()), static_cast<std::streamsize>(actual.size() * sizeof(float)));
        report("oscillator", rate, note, match(actual, expected));
      }
    }
    return finish("oscillator reference");
  }

  // Atlas end to end, an OpenWurli oscillator at its defaults into the amp at
  // its defaults, against WurliEngine rendering the same note (with the
  // plugin's +14 dB): one reed, one preamp, the same hammer noise seed.
  int engineReference(const std::string& reference) {
    const std::string directory = scratchDirectory();
    for (int rate : { 44100, 48000, 96000 }) {
      for (int note : { 40, 60, 76 }) {
        const float velocity = 100.0f / 127.0f;
        const std::string output = directory + "/engine_" + std::to_string(rate) + "_" + std::to_string(note) + ".f32";
        char velocity_text[32];
        std::snprintf(velocity_text, sizeof(velocity_text), "%.9g", velocity);
        const std::string command = "\"" + reference + "\" engine --rate " + std::to_string(rate) +
                                    " --seconds 2 --events 0:on:" + std::to_string(note) + ":" + velocity_text +
                                    " --out \"" + output + "\"";
        expect(std::system(command.c_str()) == 0, "the reference renders");
        std::ifstream stream(output, std::ios::binary);
        std::vector<float> expected(static_cast<size_t>(2 * rate));
        stream.read(reinterpret_cast<char*>(expected.data()), static_cast<std::streamsize>(expected.size() * sizeof(float)));
        expect(stream.good(), "the reference wrote every sample");
        for (float& sample : expected)
          sample = static_cast<float>(sample * vital::OpenWurliAmpModule::kOutputGain);

        SynthPlugin plugin;
        preparePlayback(plugin, rate);
        // Atlas's master volume defaults to -6 dB; 80^2 on its square-root scale is 0 dB.
        setControl(plugin, "volume", 6400.0f);
        selectAmp(plugin, "fx_slot_1_");
        const auto actual = render(plugin, 2 * rate, { { 0, juce::MidiMessage::noteOn(1, note, velocity) } });

        report("engine", rate, note, match(actual, expected));
      }
    }
    return finish("engine reference");
  }
} // namespace

int main(int argc, char** argv) {
  juce::ScopedJuceInitialiser_GUI juce;
  const juce::String flag = argc > 1 ? juce::String(argv[1]) : juce::String();
  const std::string argument = argc > 2 ? argv[2] : "";
  if (flag == "--oscillator")
    return oscillator();
  if (flag == "--pitch")
    return pitchControls();
  if (flag == "--envelope")
    return envelope();
  if (flag == "--parameters")
    return parameters();
  if (flag == "--voicing")
    return voicingControls();
  if (flag == "--amp")
    return amp();
  if (flag == "--editor")
    return editorTest();
  if (flag == "--self") {
    failures += openwurli_tests::runVoiceSelfTests();
    failures += openwurli_tests::runAmpSelfTests();
    return finish("self");
  }
  if (flag == "--voice-reference" && !argument.empty()) {
    failures += openwurli_tests::runVoiceReferenceTests(argument, scratchDirectory());
    return finish("voice reference");
  }
  if (flag == "--amp-reference" && !argument.empty()) {
    failures += openwurli_tests::runAmpReferenceTests(argument, scratchDirectory());
    return finish("amp reference");
  }
  if (flag == "--oscillator-reference" && !argument.empty())
    return oscillatorReference(argument);
  if (flag == "--engine-reference" && !argument.empty())
    return engineReference(argument);
  std::cerr << "usage: AtlasOpenWurliTests --oscillator | --pitch | --envelope | --parameters | --voicing | --amp | --editor | "
               "--self | --voice-reference <renderer> | --amp-reference <renderer> | --oscillator-reference <renderer> | "
               "--engine-reference <renderer>\n";
  return 2;
}
