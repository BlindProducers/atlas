// SPDX-License-Identifier: GPL-3.0-or-later
// Tests of the OpenWurli voice port (src/synthesis/effects/openwurli/wurli_voice.h).
//
// runVoiceReferenceTests renders the same notes with the original Rust crate, through the
// reference renderer built by tests/openwurli/build_reference.py, and with the port, and requires
// bit-identical samples. The reference renders in 256-sample chunks; the port must match it at
// chunk sizes 1, 37, 256 and 4096 (the model streams, so chunking must not matter).
// runVoiceSelfTests checks the Atlas extensions and the voice's bookkeeping without a reference.
//
// Header-only, standard library only: include it into one test translation unit.
#pragma once

#include "wurli_voice.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace openwurli_tests {
namespace voice_tests {
  struct VoiceCase {
    int note;
    double velocity;
    double rate;
    uint32_t seed;
    double seconds;
    double off;     // note-off time in seconds; negative = no note-off
    bool mlp;
    // CalibrationConfig and the C-2 fork; Atlas's Condition has no reference.
    openwurli::VoiceSettings settings = {};
  };

  inline std::string formatDouble(double value) {
    char text[64];
    std::snprintf(text, sizeof(text), "%.17g", value);
    return text;
  }

  // %g, for the report.
  inline std::string formatShort(double value) {
    char text[64];
    std::snprintf(text, sizeof(text), "%g", value);
    return text;
  }

  inline std::string shellQuote(const std::string& text) {
#if defined(_WIN32)
    return "\"" + text + "\"";
#else
    std::string quoted = "'";
    for (char c : text) {
      if (c == '\'')
        quoted += "'\\''";
      else
        quoted += c;
    }
    return quoted + "'";
#endif
  }

  inline uint64_t doubleBits(double value) {
    uint64_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
  }

  // `x as usize` for the non-negative products the reference computes.
  inline size_t toSamples(double value) {
    return value > 0.0 ? static_cast<size_t>(value) : 0;
  }

  // The reference's voice command: chunks of `chunk` samples, split at the note-off sample.
  inline std::vector<double> renderVoicePort(const VoiceCase& c, int chunk) {
    const size_t total = toSamples(c.seconds * c.rate);
    const bool has_off = c.off >= 0.0;
    const size_t off = has_off ? toSamples(c.off * c.rate) : 0;
    std::vector<double> out(total, 0.0);

    openwurli::Voice voice;
    voice.noteOn(c.note, c.velocity, c.rate, c.seed, c.mlp, c.settings);
    size_t start = 0;
    while (start < total) {
      size_t end = std::min(start + static_cast<size_t>(chunk), total);
      if (has_off) {
        if (start < off && off < end)
          end = off;
        if (start == off)
          voice.noteOff();
      }
      voice.render(out.data() + start, static_cast<int>(end - start));
      start = end;
    }
    return out;
  }

  inline bool readF64File(const std::string& path, std::vector<double>& samples) {
    std::ifstream file(path, std::ios::binary);
    if (!file)
      return false;
    const std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (bytes.size() % 8 != 0)
      return false;
    samples.resize(bytes.size() / 8);
    for (size_t i = 0; i < samples.size(); ++i) {
      uint64_t bits = 0;
      for (int b = 7; b >= 0; --b)
        bits = (bits << 8) | bytes[i * 8 + static_cast<size_t>(b)];
      std::memcpy(&samples[i], &bits, sizeof(bits));
    }
    return true;
  }

  inline bool renderVoiceReference(const std::string& binary, const VoiceCase& c, const std::string& path) {
    std::string command = shellQuote(binary) + " voice --note " + std::to_string(c.note) + " --velocity " +
                          formatDouble(c.velocity) + " --seconds " + formatDouble(c.seconds) + " --rate " +
                          formatDouble(c.rate) + " --seed " + std::to_string(c.seed) + " --chunk 256";
    if (c.off >= 0.0)
      command += " --off " + formatDouble(c.off);
    if (c.mlp)
      command += " --mlp";
    const openwurli::VoiceSettings defaults;
    if (c.settings.swing_scale != defaults.swing_scale)
      command += " --swing-scale " + formatDouble(c.settings.swing_scale);
    if (c.settings.air_drag_scale != defaults.air_drag_scale)
      command += " --air-drag-scale " + formatDouble(c.settings.air_drag_scale);
    if (c.settings.voicing_slope != defaults.voicing_slope)
      command += " --voicing-slope " + formatDouble(c.settings.voicing_slope);
    if (!c.settings.register_trim)
      command += " --zero-trim";
    if (c.settings.c2_to_emitter)
      command += " --c2-emitter";
    command += " --out " + shellQuote(path);
#if defined(_WIN32)
    // cmd /c strips the outer quotes of a command that starts with one.
    command = "\"" + command + "\"";
#endif
    return std::system(command.c_str()) == 0;
  }

  inline std::vector<VoiceCase> voiceReferenceCases() {
    std::vector<VoiceCase> cases;
    // Every register (bass and treble pickup laws, both sides of each table break, the
    // undamped top keys) at every velocity, the MLP alternating, a note-off at 1 s.
    const int notes[] = { 33, 36, 45, 52, 53, 60, 64, 72, 84, 91, 92, 96 };
    const double velocities[] = { 0.05, 0.3, 0.63, 0.8, 1.0 };
    int index = 0;
    for (int note : notes) {
      for (double velocity : velocities) {
        cases.push_back({ note, velocity, 48000.0, static_cast<uint32_t>(1000 + index), 2.0, 1.0, index % 2 == 1 });
        ++index;
      }
    }
    // Sample rates, with and without the MLP.
    for (int note : { 33, 60, 84, 92, 96 }) {
      for (double rate : { 44100.0, 96000.0 }) {
        cases.push_back({ note, 0.8, rate, 12345u, 2.0, 0.75, false });
        cases.push_back({ note, 0.8, rate, 12345u, 2.0, 0.75, true });
      }
    }
    // Noise seeds.
    for (uint32_t seed : { 0u, 1u, 12345u, 2654435761u, 0xffffffffu }) {
      cases.push_back({ 60, 1.0, 48000.0, seed, 2.0, 1.0, false });
      cases.push_back({ 84, 0.63, 44100.0, seed, 2.0, 1.0, true });
    }
    // Note-off timing: at once, inside the onset ramp and the noise burst, never, near the end;
    // and longer renders through the renormalisation of many 1024-sample periods.
    cases.push_back({ 60, 0.8, 48000.0, 7u, 2.0, 0.0, false });
    cases.push_back({ 45, 0.8, 48000.0, 7u, 2.0, 0.0005, true });
    cases.push_back({ 72, 0.3, 44100.0, 7u, 3.0, -1.0, false });
    cases.push_back({ 36, 1.0, 96000.0, 7u, 3.0, 2.9, true });
    cases.push_back({ 53, 0.63, 44100.0, 7u, 3.0, 0.2, false });
    cases.push_back({ 96, 1.0, 44100.0, 7u, 3.0, 0.1, true });
    // Velocity 0 (silent reed and noise) and a full-range velocity at the keyboard ends.
    cases.push_back({ 60, 0.0, 48000.0, 9u, 1.0, 0.5, false });
    cases.push_back({ 33, 1.0, 44100.0, 9u, 3.0, 2.0, true });
    // Voicing (CalibrationConfig): swing scales within the tables' reach (Atlas holds larger
    // swings at the reach; the Rust does not), air drag off and at its band corners, the
    // voicing slope, the register trim off, and the C-2 fork, each across the keyboard.
    int setting_index = 0;
    for (int note : { 33, 48, 53, 60, 72, 88, 96 }) {
      for (int variant = 0; variant < 8; ++variant) {
        VoiceCase c { note, variant % 2 == 0 ? 1.0 : 0.55, variant < 4 ? 48000.0 : 44100.0,
                      static_cast<uint32_t>(500 + setting_index++), 2.0, 1.2, variant == 5 };
        switch (variant) {
          case 0: c.settings.swing_scale = 0.5; break;
          case 1: c.settings.swing_scale = 1.6; break;
          case 2: c.settings.air_drag_scale = 0.0; break;
          case 3: c.settings.air_drag_scale = 2.236; c.settings.swing_scale = 1.3; break;
          case 4: c.settings.voicing_slope = 0.12; break;
          case 5: c.settings.voicing_slope = -0.2; c.settings.register_trim = false; break;
          case 6: c.settings.c2_to_emitter = true; break;
          default: c.settings.c2_to_emitter = true; c.settings.swing_scale = 0.75; c.settings.air_drag_scale = 0.447;
                   c.settings.register_trim = false; break;
        }
        cases.push_back(c);
      }
    }
    return cases;
  }

  inline int voiceSelfCheck(bool passed, const char* name, const std::string& detail = std::string()) {
    std::printf("%s %s%s%s\n", passed ? "ok  " : "FAIL", name, detail.empty() ? "" : ": ", detail.c_str());
    return passed ? 0 : 1;
  }

  inline std::vector<double> renderVoice(openwurli::Voice& voice, double seconds, double rate, int chunk) {
    const size_t total = toSamples(seconds * rate);
    std::vector<double> out(total, 0.0);
    for (size_t start = 0; start < total; start += static_cast<size_t>(chunk)) {
      const size_t end = std::min(start + static_cast<size_t>(chunk), total);
      voice.render(out.data() + start, static_cast<int>(end - start));
    }
    return out;
  }

  inline bool bitIdentical(const std::vector<double>& a, const std::vector<double>& b) {
    if (a.size() != b.size())
      return false;
    for (size_t i = 0; i < a.size(); ++i) {
      if (doubleBits(a[i]) != doubleBits(b[i]))
        return false;
    }
    return true;
  }

  // Fundamental from interpolated upward zero crossings of samples [begin, end).
  inline double zeroCrossingFrequency(const std::vector<double>& samples, size_t begin, size_t end, double rate) {
    double first = -1.0;
    double last = -1.0;
    int crossings = 0;
    for (size_t i = std::max<size_t>(begin, 1); i < end; ++i) {
      if (samples[i - 1] < 0.0 && samples[i] >= 0.0) {
        const double fraction = -samples[i - 1] / (samples[i] - samples[i - 1]);
        const double time = (static_cast<double>(i - 1) + fraction) / rate;
        if (first < 0.0)
          first = time;
        last = time;
        ++crossings;
      }
    }
    return crossings > 1 ? (crossings - 1) / (last - first) : 0.0;
  }

  inline std::string settingsLabel(const openwurli::VoiceSettings& settings) {
    const openwurli::VoiceSettings defaults;
    std::string label;
    if (settings.swing_scale != defaults.swing_scale)
      label += " swing x" + formatShort(settings.swing_scale);
    if (settings.air_drag_scale != defaults.air_drag_scale)
      label += " drag x" + formatShort(settings.air_drag_scale);
    if (settings.voicing_slope != defaults.voicing_slope)
      label += " slope " + formatShort(settings.voicing_slope);
    if (!settings.register_trim)
      label += " no trim";
    if (settings.c2_to_emitter)
      label += " C-2 emitter";
    return label;
  }

  inline int runReferenceTests(const std::string& reference_binary, const std::string& scratch_dir) {
    const int chunk_sizes[] = { 1, 37, 256, 4096 };
    const std::vector<VoiceCase> cases = voiceReferenceCases();
    int failures = 0;
    int index = 0;
    for (const VoiceCase& c : cases) {
      const std::string label = "voice note " + std::to_string(c.note) + " vel " + formatShort(c.velocity) +
                                " rate " + formatShort(c.rate) + " seed " + std::to_string(c.seed) + " " +
                                formatShort(c.seconds) + " s" +
                                (c.off >= 0.0 ? " off " + formatShort(c.off) + " s" : std::string(" no off")) +
                                (c.mlp ? " mlp" : "") + settingsLabel(c.settings);
      const std::string path = scratch_dir + "/openwurli_voice_" + std::to_string(index++) + ".f64";
      std::vector<double> reference;
      const bool rendered = renderVoiceReference(reference_binary, c, path) && readF64File(path, reference);
      std::remove(path.c_str());
      if (!rendered) {
        std::printf("FAIL %s: the reference did not render\n", label.c_str());
        ++failures;
        continue;
      }

      double peak = 0.0;
      for (double sample : reference)
        peak = std::max(peak, std::abs(sample));

      bool identical = true;
      double max_diff = 0.0;
      int worst_chunk = 0;
      std::string problem;
      for (int chunk : chunk_sizes) {
        const std::vector<double> port = renderVoicePort(c, chunk);
        if (port.size() != reference.size()) {
          problem = "length " + std::to_string(port.size()) + " vs " + std::to_string(reference.size());
          identical = false;
          continue;
        }
        for (size_t i = 0; i < port.size(); ++i) {
          if (doubleBits(port[i]) == doubleBits(reference[i]))
            continue;
          identical = false;
          const double diff = std::abs(port[i] - reference[i]);
          if (!(diff <= max_diff)) {
            max_diff = diff;
            worst_chunk = chunk;
          }
        }
      }

      if (identical && peak > 0.0) {
        std::printf("ok   %s: bit-identical at chunks 1/37/256/4096, peak %.4g V\n", label.c_str(), peak);
      }
      else if (identical) {
        // Only velocity 0 renders silence.
        const bool expected = c.velocity == 0.0;
        std::printf("%s %s: bit-identical silence\n", expected ? "ok  " : "FAIL", label.c_str());
        failures += expected ? 0 : 1;
      }
      else {
        std::printf("FAIL %s: max |diff| %.3g V (chunk %d), peak %.4g V %s\n", label.c_str(), max_diff, worst_chunk,
                    peak, problem.c_str());
        ++failures;
      }
    }
    std::printf("%d of %d voice reference cases failed\n", failures, static_cast<int>(cases.size()));
    return failures;
  }

  inline int runSelfTests() {
    int failures = 0;
    const double rate = 48000.0;

    {
      openwurli::Voice voice;
      std::vector<double> out(64, 1.0);
      voice.render(out.data(), 64);
      const bool zeros = std::all_of(out.begin(), out.end(), [](double x) { return doubleBits(x) == 0; });
      failures += voiceSelfCheck(!voice.active() && voice.isSilent() && zeros, "a new voice is inactive and silent");
    }

    for (bool mlp : { false, true }) {
      // setPitchRatio(1.0) must leave the output bit-identical.
      openwurli::Voice plain;
      openwurli::Voice unity;
      plain.noteOn(64, 0.7, rate, 99u, mlp);
      unity.noteOn(64, 0.7, rate, 99u, mlp);
      std::vector<double> a(static_cast<size_t>(rate)), b(static_cast<size_t>(rate));
      for (size_t start = 0; start < a.size(); start += 512) {
        const int length = static_cast<int>(std::min<size_t>(512, a.size() - start));
        unity.setPitchRatio(1.0);
        plain.render(a.data() + start, length);
        unity.render(b.data() + start, length);
        if (start == 24576) {
          plain.noteOff();
          unity.noteOff();
        }
      }
      failures += voiceSelfCheck(bitIdentical(a, b), mlp ? "setPitchRatio(1) is bit-identical (mlp)"
                                                         : "setPitchRatio(1) is bit-identical");
    }

    {
      // Returning to 1 restores the struck rotation exactly (no drift in the coefficients).
      openwurli::Voice plain;
      openwurli::Voice returned;
      plain.noteOn(77, 0.9, rate, 4u, true);
      returned.noteOn(77, 0.9, rate, 4u, true);
      returned.setPitchRatio(1.5);
      returned.setPitchRatio(0.75);
      returned.setPitchRatio(1.0);
      failures += voiceSelfCheck(bitIdentical(renderVoice(plain, 0.5, rate, 256), renderVoice(returned, 0.5, rate, 256)),
                                 "setPitchRatio back to 1 restores the struck pitch exactly");
    }

    {
      // A semitone up raises the measured fundamental by a semitone.
      const double semitone = std::exp2(1.0 / 12.0);
      openwurli::Voice straight;
      openwurli::Voice bent;
      straight.noteOn(60, 0.5, rate, 5u, false);
      bent.noteOn(60, 0.5, rate, 5u, false);
      bent.setPitchRatio(semitone);
      const std::vector<double> a = renderVoice(straight, 1.2, rate, 256);
      const std::vector<double> b = renderVoice(bent, 1.2, rate, 256);
      const size_t begin = static_cast<size_t>(0.2 * rate);
      const double f_straight = zeroCrossingFrequency(a, begin, a.size(), rate);
      const double f_bent = zeroCrossingFrequency(b, begin, b.size(), rate);
      const double expected = openwurli::midiToFreq(60) * openwurli::freqDetune(60);
      const double ratio_error = std::abs(f_bent / f_straight / semitone - 1.0);
      const bool passed = std::abs(f_straight / expected - 1.0) < 0.005 && ratio_error < 0.005;
      failures += voiceSelfCheck(passed, "setPitchRatio(2^(1/12)) raises note 60 by a semitone",
                                 formatDouble(f_straight) + " Hz -> " + formatDouble(f_bent) + " Hz");
    }

    {
      // A damped mid note goes silent within a few seconds of the note-off.
      openwurli::Voice voice;
      voice.noteOn(60, 0.8, rate, 3u, false);
      std::vector<double> buffer(256);
      for (int i = 0; i < 94; ++i)
        voice.render(buffer.data(), 256);
      const bool sounding = !voice.isSilent();
      voice.noteOff();
      int blocks = 0;
      while (!voice.isSilent() && blocks < 3 * 48000 / 256) {
        voice.render(buffer.data(), 256);
        ++blocks;
      }
      const double seconds = blocks * 256.0 / rate;
      failures += voiceSelfCheck(sounding && voice.isSilent(), "isSilent after a note-off on note 60",
                                 formatDouble(seconds) + " s");
    }

    {
      // A copy carries on exactly as the original.
      openwurli::Voice original;
      original.noteOn(52, 0.9, rate, 11u, true);
      renderVoice(original, 0.3, rate, 256);
      openwurli::Voice copy = original;
      original.noteOff();
      copy.noteOff();
      const std::vector<double> a = renderVoice(original, 1.0, rate, 256);
      const std::vector<double> b = renderVoice(copy, 1.0, rate, 100);
      failures += voiceSelfCheck(bitIdentical(a, b), "a copied voice renders identically");
    }

    {
      // clear() makes the voice inactive and silent.
      openwurli::Voice voice;
      voice.noteOn(60, 0.8, rate, 1u, false);
      renderVoice(voice, 0.1, rate, 256);
      const bool was_active = voice.active();
      voice.clear();
      std::vector<double> out(300, 1.0);
      voice.render(out.data(), 300);
      const bool zeros = std::all_of(out.begin(), out.end(), [](double x) { return doubleBits(x) == 0; });
      failures += voiceSelfCheck(was_active && !voice.active() && voice.isSilent() && zeros,
                                 "clear() makes the voice inactive and renders zeros");
    }

    {
      // Notes outside the keyboard play its end keys.
      openwurli::Voice low, lowest, high, highest;
      low.noteOn(10, 0.8, rate, 2u, false);
      lowest.noteOn(openwurli::kMidiLo, 0.8, rate, 2u, false);
      high.noteOn(127, 0.8, rate, 2u, true);
      highest.noteOn(openwurli::kMidiHi, 0.8, rate, 2u, true);
      const bool passed = bitIdentical(renderVoice(low, 0.2, rate, 256), renderVoice(lowest, 0.2, rate, 256)) &&
                          bitIdentical(renderVoice(high, 0.2, rate, 256), renderVoice(highest, 0.2, rate, 256));
      failures += voiceSelfCheck(passed, "notes outside 33-96 clamp to the keyboard");
    }

    {
      // Condition: scale 1 is OpenWurli's tuning; a wider spread moves each key by its own offset
      // times the scale (note 61's offset is far from zero), measured over one second.
      openwurli::VoiceSettings vintage;
      vintage.detune_scale = 1.0 + 11.0 / 3.0;
      openwurli::Voice plain, fresh, worn;
      plain.noteOn(61, 0.8, rate, 6u, false);
      fresh.noteOn(61, 0.8, rate, 6u, false, openwurli::VoiceSettings());
      worn.noteOn(61, 0.8, rate, 6u, false, vintage);
      const std::vector<double> a = renderVoice(plain, 1.2, rate, 256);
      const std::vector<double> b = renderVoice(fresh, 1.2, rate, 256);
      const std::vector<double> c = renderVoice(worn, 1.2, rate, 256);
      const size_t begin = static_cast<size_t>(0.2 * rate);
      const double f_fresh = zeroCrossingFrequency(b, begin, b.size(), rate);
      const double f_worn = zeroCrossingFrequency(c, begin, c.size(), rate);
      const double unit = openwurli::detuneUnit(61);
      const double cents = 1200.0 * std::log2(f_worn / f_fresh);
      const double expected = 1200.0 * std::log2((1.0 + unit * 0.00173 * vintage.detune_scale) /
                                                 (1.0 + unit * 0.00173));
      char detail[160];
      std::snprintf(detail, sizeof(detail), "offset %.3f, moved %.2f cents, expected %.2f", unit, cents, expected);
      failures += voiceSelfCheck(bitIdentical(a, b) && std::abs(unit) > 0.3 && std::abs(cents - expected) < 0.5,
                                 "Condition widens each reed's tuning offset", detail);
    }

    {
      // Bark past the tables' reach holds each note at the reach: bass at 4.29 mm, treble at
      // 2.89 mm, so a doubled swing on A1 equals a swing of exactly the reach, and stays finite.
      openwurli::VoiceSettings doubled;
      doubled.swing_scale = 2.0;
      openwurli::VoiceSettings at_reach;
      at_reach.swing_scale = openwurli::kSwingReachMmBass / openwurli::keyParams(33).provisional_swing_mm;
      const bool held = openwurli::pickupSwingMm(33, doubled) == openwurli::kSwingReachMmBass &&
                        openwurli::pickupSwingMm(53, doubled) == 2.0 * openwurli::keyParams(53).provisional_swing_mm &&
                        openwurli::pickupSwingMm(33, openwurli::VoiceSettings()) == openwurli::keyParams(33).swing_mm;
      openwurli::Voice voice;
      voice.noteOn(33, 1.0, rate, 8u, false, doubled);
      const std::vector<double> out = renderVoice(voice, 1.0, rate, 256);
      const bool finite = std::all_of(out.begin(), out.end(), [](double x) { return std::isfinite(x); });
      failures += voiceSelfCheck(held && finite && at_reach.swing_scale > 1.0,
                                 "a swing past the pickup table's reach is held at the reach");
    }

    std::printf("%d voice self tests failed\n", failures);
    return failures;
  }
} // namespace voice_tests

  // Runs every case through the reference and the port. Returns the number of failing cases.
  inline int runVoiceReferenceTests(const std::string& reference_binary, const std::string& scratch_dir) {
    return voice_tests::runReferenceTests(reference_binary, scratch_dir);
  }

  // Checks without the reference. Returns the number of failing checks.
  inline int runVoiceSelfTests() {
    return voice_tests::runSelfTests();
  }
} // namespace openwurli_tests
