// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for the OpenWurli amplifier chain port (openwurli::AmpChain,
// src/synthesis/effects/openwurli/wurli_amp.h).
//
// runAmpReferenceTests runs the original Rust crate through the reference
// renderer (tests/openwurli/reference.rs, built by build_reference.py) and
// requires bit-identical output: realistic chords from the reference's own
// voices (up to ff bass chords and a hot copy that clips the power amp) and
// synthetic signals, at 44.1, 48 (oversampled), 88.2 and 96 kHz (not), across
// the volume, tremolo depth and speaker character ranges, with parameter
// changes mid-signal and the C++ side split into calls of 1, 37, 256 and 4096
// samples. runAmpSelfTests needs no reference: reset() and copies reproduce
// the output exactly, bad input stays finite and recovers, and an empty call
// changes nothing.
//
// Header-only, standard library only. Sample files are raw little-endian
// doubles (the host's byte order).
#pragma once

#include "wurli_amp.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace openwurli_tests {
namespace amp_detail {
  enum Parameter { kVolume, kTremolo, kSpeaker };

  struct Settings {
    double volume;
    double tremolo;
    double speaker;
  };

  struct Change {
    double time;  // seconds; both sides apply it at sample floor(time * rate)
    Parameter parameter;
    double value;
  };

  inline std::string quote(const std::string& text) {
    return "\"" + text + "\"";
  }

  inline std::string number(double value) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.17g", value);
    return buffer;
  }

  inline bool writeSamples(const std::string& path, const std::vector<double>& samples) {
    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr)
      return false;
    const size_t written = std::fwrite(samples.data(), sizeof(double), samples.size(), file);
    std::fclose(file);
    return written == samples.size();
  }

  inline bool readSamples(const std::string& path, std::vector<double>& samples) {
    samples.clear();
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr)
      return false;
    double buffer[4096];
    size_t count;
    while ((count = std::fread(buffer, sizeof(double), 4096, file)) > 0)
      samples.insert(samples.end(), buffer, buffer + count);
    std::fclose(file);
    return true;
  }

  inline bool run(const std::string& command) {
    return std::system(command.c_str()) == 0;
  }

  inline const char* parameterName(Parameter parameter) {
    switch (parameter) {
      case kVolume: return "volume";
      case kTremolo: return "tremolo";
      case kSpeaker: return "speaker";
    }
    return "?";
  }

  // One Voice from the reference (pickup volts), delayed by `start` seconds.
  inline bool referenceVoice(const std::string& binary, const std::string& scratch, int note, double velocity,
                             double seconds, double rate, int seed, double start, double off,
                             std::vector<double>& samples) {
    const std::string path = scratch + "/amp_voice.f64";
    std::string command = quote(binary) + " voice --note " + std::to_string(note) + " --velocity "
                          + number(velocity) + " --seconds " + number(seconds - start) + " --rate "
                          + number(rate) + " --seed " + std::to_string(seed) + " --out " + quote(path);
    if (off > 0.0)
      command += " --off " + number(off - start);
    std::vector<double> voice;
    if (!run(command) || !readSamples(path, voice))
      return false;
    samples.assign(static_cast<size_t>(seconds * rate), 0.0);
    const size_t offset = static_cast<size_t>(start * rate);
    for (size_t i = 0; i < voice.size() && offset + i < samples.size(); ++i)
      samples[offset + i] = voice[i];
    return true;
  }

  struct NoteEvent {
    int note;
    double velocity;
    double start;
    double off;  // 0: held to the end
  };

  inline bool referenceChord(const std::string& binary, const std::string& scratch, const std::vector<NoteEvent>& notes,
                             double seconds, double rate, std::vector<double>& sum) {
    sum.assign(static_cast<size_t>(seconds * rate), 0.0);
    int seed = 7;
    for (const NoteEvent& event : notes) {
      std::vector<double> voice;
      if (!referenceVoice(binary, scratch, event.note, event.velocity, seconds, rate, seed++, event.start, event.off,
                          voice))
        return false;
      for (size_t i = 0; i < sum.size(); ++i)
        sum[i] += voice[i];
    }
    return true;
  }

  // ff bass chord, struck together and held.
  inline std::vector<NoteEvent> ffBassChord() {
    return { { 33, 1.0, 0.0, 0.0 }, { 36, 1.0, 0.0, 0.0 }, { 40, 1.0, 0.0, 0.0 },
             { 43, 1.0, 0.0, 0.0 }, { 45, 1.0, 0.0, 0.0 }, { 48, 1.0, 0.0, 0.0 } };
  }

  // A mezzo arpeggio with releases.
  inline std::vector<NoteEvent> mpArpeggio() {
    return { { 60, 0.45, 0.0, 2.0 }, { 64, 0.5, 0.25, 2.2 }, { 67, 0.55, 0.5, 2.4 },
             { 71, 0.5, 0.75, 2.6 }, { 84, 0.6, 1.0, 2.8 } };
  }

  inline std::vector<double> scaled(const std::vector<double>& signal, double gain) {
    std::vector<double> result(signal.size());
    for (size_t i = 0; i < signal.size(); ++i)
      result[i] = signal[i] * gain;
    return result;
  }

  // Logarithmic sine sweep, 20 Hz to 20 kHz (or Nyquist), 50 mV.
  inline std::vector<double> sweep(double seconds, double rate) {
    const size_t total = static_cast<size_t>(seconds * rate);
    const double f0 = 20.0;
    const double f1 = std::min(20000.0, 0.45 * rate);
    const double k = std::log(f1 / f0) / seconds;
    std::vector<double> signal(total);
    for (size_t i = 0; i < total; ++i) {
      const double t = static_cast<double>(i) / rate;
      const double phase = 2.0 * openwurli::kPi * f0 * (std::exp(k * t) - 1.0) / k;
      signal[i] = 0.05 * std::sin(phase);
    }
    return signal;
  }

  // 0.1 V step up at 0.5 s and down at 2 s, single-sample 1 V and -0.5 V impulses.
  inline std::vector<double> stepAndImpulses(double seconds, double rate) {
    const size_t total = static_cast<size_t>(seconds * rate);
    std::vector<double> signal(total, 0.0);
    for (size_t i = static_cast<size_t>(0.5 * rate); i < static_cast<size_t>(2.0 * rate) && i < total; ++i)
      signal[i] = 0.1;
    signal[static_cast<size_t>(1.0 * rate)] += 1.0;
    signal[static_cast<size_t>(2.5 * rate)] -= 0.5;
    return signal;
  }

  // A 3 V, 110 Hz burst on top of a 40x ff chord: hard clipping everywhere.
  inline std::vector<double> veryHot(const std::vector<double>& ff_chord, double rate) {
    std::vector<double> signal = scaled(ff_chord, 40.0);
    const size_t start = static_cast<size_t>(1.5 * rate);
    const size_t end = std::min(signal.size(), static_cast<size_t>(2.5 * rate));
    for (size_t i = start; i < end; ++i)
      signal[i] += 3.0 * std::sin(2.0 * openwurli::kPi * 110.0 * static_cast<double>(i - start) / rate);
    return signal;
  }

  // The chain as Atlas runs it: settings before prepare(), then calls of
  // `call_size` samples, split where a change falls.
  inline std::vector<double> renderCpp(double rate, const Settings& settings, const std::vector<double>& input,
                                       int call_size, const std::vector<Change>& changes) {
    auto chain = std::make_unique<openwurli::AmpChain>();
    chain->setVolume(settings.volume);
    chain->setTremoloDepth(settings.tremolo);
    chain->setSpeakerCharacter(settings.speaker);
    chain->prepare(rate);

    std::vector<Change> pending = changes;
    std::stable_sort(pending.begin(), pending.end(),
                     [rate](const Change& a, const Change& b) {
                       return static_cast<size_t>(a.time * rate) < static_cast<size_t>(b.time * rate);
                     });
    std::vector<double> output(input.size());
    size_t start = 0;
    size_t next = 0;
    while (start < input.size()) {
      while (next < pending.size() && static_cast<size_t>(pending[next].time * rate) <= start) {
        const Change& change = pending[next++];
        if (change.parameter == kVolume)
          chain->setVolume(change.value);
        else if (change.parameter == kTremolo)
          chain->setTremoloDepth(change.value);
        else
          chain->setSpeakerCharacter(change.value);
      }
      size_t end = std::min(input.size(), start + static_cast<size_t>(call_size));
      if (next < pending.size())
        end = std::min(end, std::max(start + 1, static_cast<size_t>(pending[next].time * rate)));
      chain->process(input.data() + start, output.data() + start, static_cast<int>(end - start));
      start = end;
    }
    return output;
  }

  inline bool renderReference(const std::string& binary, const std::string& scratch, double rate,
                              const Settings& settings, const std::vector<double>& input,
                              const std::vector<Change>& changes, int chunk, std::vector<double>& output) {
    const std::string in_path = scratch + "/amp_chain_in.f64";
    const std::string out_path = scratch + "/amp_chain_out.f64";
    if (!writeSamples(in_path, input))
      return false;
    std::string command = quote(binary) + " chain --rate " + number(rate) + " --volume " + number(settings.volume)
                          + " --tremolo " + number(settings.tremolo) + " --speaker " + number(settings.speaker)
                          + " --chunk " + std::to_string(chunk) + " --in " + quote(in_path) + " --out "
                          + quote(out_path);
    if (!changes.empty()) {
      std::string list;
      for (const Change& change : changes) {
        if (!list.empty())
          list += ",";
        list += number(change.time) + ":" + parameterName(change.parameter) + ":" + number(change.value);
      }
      command += " --changes " + list;
    }
    return run(command) && readSamples(out_path, output) && output.size() == input.size();
  }

  struct Comparison {
    bool identical;
    double max_diff;
    double peak;
    bool finite;
  };

  inline Comparison compare(const std::vector<double>& expected, const std::vector<double>& actual) {
    Comparison result { expected.size() == actual.size(), 0.0, 0.0, true };
    const size_t count = std::min(expected.size(), actual.size());
    for (size_t i = 0; i < count; ++i) {
      if (std::memcmp(&expected[i], &actual[i], sizeof(double)) != 0) {
        result.identical = false;
        const double diff = std::fabs(expected[i] - actual[i]);
        result.max_diff = std::isnan(diff) ? std::numeric_limits<double>::infinity()
                                           : std::max(result.max_diff, diff);
      }
      if (!std::isfinite(actual[i]))
        result.finite = false;
      else
        result.peak = std::max(result.peak, std::fabs(actual[i]));
    }
    return result;
  }

  inline double peakOf(const std::vector<double>& signal, size_t begin, size_t end) {
    double peak = 0.0;
    for (size_t i = begin; i < end && i < signal.size(); ++i)
      peak = std::max(peak, std::fabs(signal[i]));
    return peak;
  }

  inline bool allFinite(const std::vector<double>& signal) {
    for (double value : signal) {
      if (!std::isfinite(value))
        return false;
    }
    return true;
  }

  inline bool bitIdentical(const std::vector<double>& a, const std::vector<double>& b) {
    return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(double)) == 0);
  }

  // A synthetic six-note chord with decaying partials, for the self tests.
  inline std::vector<double> syntheticChord(double seconds, double rate, double level) {
    const double notes[] = { 55.0, 82.41, 110.0, 138.59, 164.81, 220.0 };
    const size_t total = static_cast<size_t>(seconds * rate);
    std::vector<double> signal(total, 0.0);
    for (size_t i = 0; i < total; ++i) {
      const double t = static_cast<double>(i) / rate;
      double sum = 0.0;
      for (double f : notes) {
        for (int h = 1; h <= 4; ++h)
          sum += std::exp(-1.5 * t * h) * std::sin(2.0 * openwurli::kPi * f * h * t) / (h * h);
      }
      signal[i] = level * sum;
    }
    return signal;
  }

  // A steady two-tone input (220 and 330 Hz, 5 mV each).
  inline std::vector<double> steadyTone(double seconds, double rate) {
    const size_t total = static_cast<size_t>(seconds * rate);
    std::vector<double> signal(total);
    for (size_t i = 0; i < total; ++i) {
      const double t = static_cast<double>(i) / rate;
      signal[i] = 0.005 * (std::sin(2.0 * openwurli::kPi * 220.0 * t) + std::sin(2.0 * openwurli::kPi * 330.0 * t));
    }
    return signal;
  }

  inline std::vector<double> processInCalls(openwurli::AmpChain& chain, const std::vector<double>& input,
                                            int call_size) {
    std::vector<double> output(input.size());
    for (size_t start = 0; start < input.size(); start += static_cast<size_t>(call_size)) {
      const size_t count = std::min(static_cast<size_t>(call_size), input.size() - start);
      chain.process(input.data() + start, output.data() + start, static_cast<int>(count));
    }
    return output;
  }

  inline int check(bool condition, const char* name) {
    std::printf("amp self test: %s: %s\n", name, condition ? "ok" : "FAILED");
    return condition ? 0 : 1;
  }
} // namespace amp_detail

// Returns the number of failures. `scratch_dir` receives the sample files.
inline int runAmpReferenceTests(const std::string& reference_binary, const std::string& scratch_dir) {
  using namespace amp_detail;
  constexpr double kSeconds = 3.5;
  const double rates[] = { 44100.0, 48000.0, 88200.0, 96000.0 };
  const int call_sizes[] = { 1, 37, 256, 4096 };
  const Settings combos[] = {
    { 0.8, 0.5, 0.0 },   // OpenWurli's defaults
    { 1.0, 1.0, 1.0 }, { 0.05, 0.0, 0.4 }, { 0.5, 1.0, 0.0 }, { 1.0, 0.0, 0.4 },
    { 0.5, 0.5, 1.0 }, { 0.05, 1.0, 1.0 }, { 0.8, 0.0, 1.0 },
  };

  struct Case {
    const char* signal;
    int combo;
  };
  const Case cases[] = {
    { "ff bass chord", 0 }, { "ff bass chord", 1 }, { "ff bass chord", 2 }, { "ff bass chord", 3 },
    { "ff bass chord", 4 }, { "ff bass chord", 5 }, { "ff bass chord", 6 }, { "ff bass chord", 7 },
    { "mp arpeggio", 0 }, { "mp arpeggio", 3 }, { "mp arpeggio", 5 },
    { "silence", 0 }, { "silence", 1 },
    { "sweep", 0 }, { "sweep", 4 },
    { "step+impulses", 1 }, { "step+impulses", 2 },
    { "ff chord x8", 1 }, { "ff chord x8", 6 },
    { "very hot", 0 }, { "very hot", 1 }, { "very hot", 2 }, { "very hot", 6 },
  };

  int failures = 0;
  int case_index = 0;
  for (double rate : rates) {
    std::vector<double> ff_chord;
    std::vector<double> mp_arpeggio;
    if (!referenceChord(reference_binary, scratch_dir, ffBassChord(), kSeconds, rate, ff_chord)
        || !referenceChord(reference_binary, scratch_dir, mpArpeggio(), kSeconds, rate, mp_arpeggio)) {
      std::printf("amp ref %.0f Hz: could not render voices with %s\n", rate, reference_binary.c_str());
      return failures + 1;
    }

    auto signalFor = [&](const std::string& name) {
      if (name == "ff bass chord")
        return ff_chord;
      if (name == "mp arpeggio")
        return mp_arpeggio;
      if (name == "silence")
        return std::vector<double>(static_cast<size_t>(kSeconds * rate), 0.0);
      if (name == "sweep")
        return sweep(kSeconds, rate);
      if (name == "step+impulses")
        return stepAndImpulses(kSeconds, rate);
      if (name == "ff chord x8")
        return scaled(ff_chord, 8.0);
      return veryHot(ff_chord, rate);
    };

    auto runCase = [&](const std::string& label, const std::vector<double>& input, const Settings& settings,
                       const std::vector<Change>& changes, const std::vector<int>& sizes) {
      std::vector<double> expected;
      const int chunk = (case_index % 3 == 0) ? 256 : (case_index % 3 == 1 ? 64 : 1000);
      if (!renderReference(reference_binary, scratch_dir, rate, settings, input, changes, chunk, expected)) {
        std::printf("amp ref %.0f Hz %s: reference failed\n", rate, label.c_str());
        ++failures;
        return;
      }
      for (int size : sizes) {
        const Comparison result = compare(expected, renderCpp(rate, settings, input, size, changes));
        const bool ok = result.identical && result.finite;
        std::printf("amp ref %5.0f Hz  %-14s vol %-4g trem %-3g spk %-3g calls %4d: max|diff| %.3g, peak %.4f%s\n",
                    rate, label.c_str(), settings.volume, settings.tremolo, settings.speaker, size, result.max_diff,
                    result.peak, ok ? "" : "  FAILED");
        if (!ok)
          ++failures;
      }
      ++case_index;
    };

    for (const Case& test : cases) {
      const std::string name = test.signal;
      // The defaults case runs at every call size; the rest rotate through them.
      std::vector<int> sizes;
      if (name == "ff bass chord" && test.combo == 0)
        sizes.assign(std::begin(call_sizes), std::end(call_sizes));
      else
        sizes.push_back(call_sizes[case_index % 4]);
      runCase(name, signalFor(name), combos[test.combo], {}, sizes);
    }

    // Every parameter moved mid-signal, some retargeted within their 5 ms
    // ramp, and one set to its current value (ignored).
    const std::vector<Change> changes = {
      { 0.5, kVolume, 0.2 },    { 0.502, kVolume, 0.6 },  { 1.0, kTremolo, 1.0 },
      { 1.001, kTremolo, 0.2 }, { 1.5, kSpeaker, 0.7 },   { 1.5005, kSpeaker, 0.9 },
      { 1.502, kVolume, 1.0 },  { 2.0, kTremolo, 0.0 },   { 2.25, kSpeaker, 0.0 },
      { 2.5, kVolume, 0.05 },   { 2.501, kTremolo, 0.5 }, { 2.75, kVolume, 0.05 },
      { 3.0, kVolume, 0.8 },
    };
    runCase("ff + changes", ff_chord, combos[0], changes, { 1, 37, 4096 });
    runCase("hot + changes", scaled(ff_chord, 8.0), combos[1], changes, { 256 });
  }
  return failures;
}

// Returns the number of failures.
inline int runAmpSelfTests() {
  using namespace amp_detail;
  using openwurli::AmpChain;
  int failures = 0;

  for (double rate : { 44100.0, 96000.0 }) {
    const std::vector<double> input = syntheticChord(1.0, rate, 0.004);

    auto chain = std::make_unique<AmpChain>();
    chain->setTremoloDepth(1.0);
    chain->setSpeakerCharacter(0.4);
    chain->prepare(rate);
    const std::vector<double> first = processInCalls(*chain, input, 256);
    chain->reset();
    const std::vector<double> after_reset = processInCalls(*chain, input, 256);
    failures += check(bitIdentical(first, after_reset), rate < 50000.0 ? "reset() reproduces prepare() (44.1k)"
                                                                       : "reset() reproduces prepare() (96k)");

    auto fresh = std::make_unique<AmpChain>();
    fresh->setTremoloDepth(1.0);
    fresh->setSpeakerCharacter(0.4);
    fresh->prepare(rate);
    failures += check(bitIdentical(first, processInCalls(*fresh, input, 4096)), "prepare() is deterministic");

    // Call sizes.
    bool same = true;
    for (int size : { 1, 37, 4096 }) {
      chain->reset();
      same = same && bitIdentical(first, processInCalls(*chain, input, size));
    }
    chain->reset();
    std::vector<double> whole(input.size());
    chain->process(input.data(), whole.data(), static_cast<int>(input.size()));
    same = same && bitIdentical(first, whole);
    std::vector<double> in_place = input;
    chain->reset();
    chain->process(in_place.data(), in_place.data(), static_cast<int>(in_place.size()));
    same = same && bitIdentical(first, in_place);
    failures += check(same, "output independent of call size, in place");
  }

  // A copy taken mid-stream continues identically.
  {
    const double rate = 48000.0;
    const std::vector<double> input = syntheticChord(1.0, rate, 0.004);
    const size_t half = input.size() / 2;
    auto chain = std::make_unique<AmpChain>();
    chain->prepare(rate);
    chain->setVolume(1.0);
    std::vector<double> head(input.begin(), input.begin() + static_cast<std::ptrdiff_t>(half));
    processInCalls(*chain, head, 100);
    auto copy = std::make_unique<AmpChain>(*chain);
    const std::vector<double> tail(input.begin() + static_cast<std::ptrdiff_t>(half), input.end());
    failures += check(bitIdentical(processInCalls(*chain, tail, 128), processInCalls(*copy, tail, 333)),
                      "copy mid-stream continues identically");
  }

  // An empty call changes nothing.
  {
    const double rate = 44100.0;
    const std::vector<double> input = syntheticChord(0.3, rate, 0.004);
    auto a = std::make_unique<AmpChain>();
    a->prepare(rate);
    auto b = std::make_unique<AmpChain>(*a);
    b->process(nullptr, nullptr, 0);
    double dummy = 1.0;
    b->process(&dummy, &dummy, 0);
    failures += check(bitIdentical(processInCalls(*a, input, 64), processInCalls(*b, input, 64)) && dummy == 1.0,
                      "process(..., 0) is a no-op");
  }

  // Before prepare(), or after one with an invalid rate: silence.
  {
    AmpChain unprepared;
    std::vector<double> buffer(64, 0.5);
    unprepared.process(buffer.data(), buffer.data(), 64);
    unprepared.reset();
    bool silent = peakOf(buffer, 0, buffer.size()) == 0.0;
    for (double rate : { 0.0, -48000.0, std::numeric_limits<double>::quiet_NaN(),
                         std::numeric_limits<double>::infinity() }) {
      auto chain = std::make_unique<AmpChain>();
      chain->prepare(44100.0);
      chain->prepare(rate);
      std::fill(buffer.begin(), buffer.end(), 0.5);
      chain->process(buffer.data(), buffer.data(), 64);
      silent = silent && peakOf(buffer, 0, buffer.size()) == 0.0;
    }
    failures += check(silent, "zeros before prepare() and after an invalid rate");
  }

  // Non-finite input samples read as 0, as WurliEngine zeroes a non-finite voice sum.
  for (double rate : { 44100.0, 96000.0 }) {
    const std::vector<double> clean = steadyTone(1.0, rate);
    std::vector<double> dirty = clean;
    std::vector<double> zeroed = clean;
    const double inf = std::numeric_limits<double>::infinity();
    const double bad[] = { std::numeric_limits<double>::quiet_NaN(), inf, -inf };
    for (int i = 0; i < 3; ++i) {
      const size_t at = static_cast<size_t>((0.2 + 0.1 * i) * rate);
      dirty[at] = bad[i];
      zeroed[at] = 0.0;
    }
    auto chain = std::make_unique<AmpChain>();
    chain->prepare(rate);
    auto copy = std::make_unique<AmpChain>(*chain);
    failures += check(bitIdentical(processInCalls(*chain, dirty, 512), processInCalls(*copy, zeroed, 512)),
                      rate < 50000.0 ? "NaN/inf input reads as 0 (44.1k)" : "NaN/inf input reads as 0 (96k)");
  }

  // Huge input: the output stays finite and bounded. Up to ~1e10 V the chain
  // plays on within a few seconds (the preamp's capacitors discharge, the amp
  // sits on its rail meanwhile), and +-1e300 is caught by the NaN guards at
  // once. From ~1e15 V the preamp latches for minutes, as upstream: its
  // Newton warm start (Vbe) is left far outside the device's clamp range,
  // where the clamped model is flat, and walks back slowly into a limit
  // cycle. So 1e30 is only checked for finite, bounded output.
  for (double rate : { 44100.0, 96000.0 }) {
    const std::vector<double> clean = steadyTone(6.0, rate);
    auto prepared = std::make_unique<AmpChain>();
    prepared->prepare(rate);
    auto reference = std::make_unique<AmpChain>(*prepared);
    const std::vector<double> expected = processInCalls(*reference, clean, 512);
    const size_t tail = static_cast<size_t>(5.0 * rate);
    const double expected_peak = peakOf(expected, tail, expected.size());
    bool finite = true;
    bool recovered = true;
    for (double huge : { 1e300, -1e300, 1e30, 1e6, 100.0 }) {
      const bool recovers = huge != 1e30;
      std::vector<double> dirty = clean;
      const size_t at = static_cast<size_t>(0.2 * rate);
      for (size_t j = 0; j < 10; ++j)
        dirty[at + j] = huge;
      auto chain = std::make_unique<AmpChain>(*prepared);
      const std::vector<double> output = processInCalls(*chain, dirty, 512);
      const double tail_peak = peakOf(output, tail, output.size());
      finite = finite && allFinite(output) && peakOf(output, 0, output.size()) <= 4.0;
      if (recovers)
        recovered = recovered && tail_peak > 0.5 * expected_peak && tail_peak < 2.0 * expected_peak;
    }
    failures += check(finite, rate < 50000.0 ? "huge input: finite, bounded (44.1k)"
                                             : "huge input: finite, bounded (96k)");
    failures += check(recovered, rate < 50000.0 ? "huge input: recovers (44.1k)" : "huge input: recovers (96k)");
  }

  // Non-finite and extreme settings.
  {
    const double rate = 48000.0;
    const std::vector<double> input = syntheticChord(1.0, rate, 0.004);
    auto chain = std::make_unique<AmpChain>();
    chain->prepare(rate);
    chain->setVolume(std::numeric_limits<double>::quiet_NaN());
    chain->setTremoloDepth(-5.0);
    chain->setSpeakerCharacter(7.0);
    const std::vector<double> bad = processInCalls(*chain, std::vector<double>(input.begin(), input.begin() + 4800), 64);
    chain->setVolume(1e9);
    chain->setTremoloDepth(std::numeric_limits<double>::quiet_NaN());
    chain->setSpeakerCharacter(-1.0);
    const std::vector<double> extreme = processInCalls(*chain, input, 64);
    chain->setVolume(0.8);
    chain->setTremoloDepth(0.5);
    chain->setSpeakerCharacter(0.0);
    const std::vector<double> recovered = processInCalls(*chain, input, 64);
    failures += check(allFinite(bad) && allFinite(extreme) && allFinite(recovered)
                          && peakOf(recovered, recovered.size() / 2, recovered.size()) > 0.0,
                      "NaN/extreme settings: finite and recovers");
  }
  return failures;
}
} // namespace openwurli_tests
