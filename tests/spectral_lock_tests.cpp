#include "JuceHeader.h"
#include "spectral_lock_module.h"
#include "spectral_lock/spectral_lock_engine.h"
#include "spectral_lock/spectral_retune_engine.h"
#include "prepared_effect_module.h"
#include "reorderable_effect_chain.h"
#include "../third_party/spectral_lock/PluginProcessor.h"
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <new>
#include <vector>
#include <cmath>

namespace {
thread_local bool tracking = false;
thread_local size_t allocations = 0;
int failures = 0;
void expect(bool ok, const char* message) {
  if (!ok) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
}
void* operator new(std::size_t size) {
  if (tracking) ++allocations;
  if (void* p = std::malloc(std::max(size, size_t(1)))) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {
const char* atlasIds[] = { "amount", "isolate", "shimmer", "bend", "bend_range", "range_low",
    "range_high", "mute_low", "mute_high", "spray", "tilt", "pitch", "root", "mix" };
const char* sourceIds[] = { "amount", "isolate", "shimmer", "bend", "bendrange", "rangelo",
    "rangehi", "mutelow", "mutehigh", "spray", "tilt", "pitch", "root", "mix" };
void compare(vital::SynthModule& module, int rate, int factor, int maxBlock,
             const std::string& prefix = "", bool reset = false) {
  std::array<SpectralLockAudioProcessor, vital::poly_float::kSize / 2> references;
  std::array<float, 14> p { 1, 0, 0, 0, 2, 65, 2200, 0, 0, 0, 0, 0, 0, 1 };
  auto controls = module.getControls();
  for (int i = 0; i < 14; ++i) controls.at(prefix + "spectral_lock_" + atlasIds[i])->set(p[i]);
  controls.at(prefix + "spectral_lock_scale")->set(0);
  if (reset) module.hardReset();
  for (auto& source : references) source.prepareToPlay(rate * factor, maxBlock);
  std::vector<vital::poly_float> input(maxBlock);
  juce::AudioBuffer<float> buffer(2, maxBlock);
  juce::MidiBuffer midi;
  uint32_t seed = 0x12345678;
  auto random = [&] { seed = seed * 1664525u + 1013904223u; return float(seed >> 8) / 16777216.0f; };
  float worst = 0, peak = 0;
  double atlasSeconds = 0, referenceSeconds = 0;
  int position = 0, scale = 0;
  for (int block = 0; block < 312; ++block) {
    int count = block % 5 == 0 ? 1 : block % 5 == 1 ? std::min(17, maxBlock) : maxBlock;
    if (block >= 12 && block % 12 == 0) {
      scale = ((block / 12) - 1) % 12;
      p = { random(), random(), random(), random() * 2 - 1, float(1 + block % 24),
          60 + random() * 4740, 60 + random() * 4740, float(block % 24 == 0),
          float(block % 36 == 0), random(), random() * 2 - 1,
          float(block % 49 - 24), float(((block / 12) - 1) % 12), random() };
      // Include full/empty-like ranges, full dry, full wet and source extremes.
      if (block % 48 == 0) { p[5] = 60; p[6] = 4800; p[13] = 1; }
      if (block % 60 == 0) { p[5] = 4800; p[6] = 60; p[13] = 0; }
      if (block % 72 == 0) { p[0] = 0; p[1] = 1; p[2] = 1; p[3] = -1; p[4] = 24; }
    }
    for (int i = 0; i < 14; ++i) controls.at(prefix + "spectral_lock_" + atlasIds[i])->set(p[i]);
    controls.at(prefix + "spectral_lock_scale")->set(scale);
    for (int i = 0; i < count; ++i)
      for (int lane = 0; lane < vital::poly_float::kSize; ++lane) {
        float x = .25f * std::sin((position + i) * (lane + 1) * .035f) + .1f * (random() - .5f);
        if (block % 53 > 47) x = 0;
        if (block % 31 == 0) x *= 3;
        input[i].set(lane, x);
      }
    allocations = 0; tracking = true;
    const auto start = std::chrono::steady_clock::now();
    module.processWithInput(input.data(), count);
    atlasSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    tracking = false;
    expect(allocations == 0, "prepared processing and parameter changes allocate no memory");
    for (int pair = 0; pair < vital::poly_float::kSize / 2; ++pair) {
      auto& source = references[pair];
      for (int i = 0; i < 14; ++i) source.apvts.getRawParameterValue(sourceIds[i])->store(p[i]);
      for (int i = 0; i < 12; ++i)
        source.apvts.getRawParameterValue("matrix" + String(i))->store(source.scaleTables[scale][i]);
      for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < count; ++i) buffer.setSample(ch, i, input[i][pair * 2 + ch]);
      auto region = juce::AudioBuffer<float>(buffer.getArrayOfWritePointers(), 2, count);
      const auto before = std::chrono::steady_clock::now();
      source.processBlock(region, midi);
      referenceSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - before).count();
      for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < count; ++i) {
          const float actual = module.output()->buffer[i][pair * 2 + ch], wanted = buffer.getSample(ch, i);
          if (!std::isfinite(actual)) { expect(false, "finite output on every SIMD lane"); return; }
          worst = std::max(worst, std::abs(actual - wanted)); peak = std::max(peak, std::abs(wanted));
        }
    }
    position += count;
  }
  std::cout << "rate=" << rate << " factor=" << factor << " block=" << maxBlock << " error=" << worst
            << " peak=" << peak << " adapter/reference CPU=" << atlasSeconds / referenceSeconds
            << " atlas ms=" << atlasSeconds * 1000.0 << '\n';
  expect(worst < 1.0e-6f && peak > .001f, "audio matches the supplied processor through automation and range changes");
  expect(module.getLatencySamples() == 0, "source filterbank adds no buffering latency");
}
}

namespace {
// Atlas additions: Width scales the per-band stereo placement; Follow keeps the
// re-played tones from outlasting the input.
void testWidthAndFollow() {
  using Engine = atlas::spectral_lock::Engine;
  using Parameters = atlas::spectral_lock::Parameters;
  constexpr int rate = 48000;
  constexpr int total = rate;
  auto render = [&](const Parameters& p, int burst_samples) {
    Engine engine;
    engine.prepare(rate, 512);
    std::vector<float> left(total), right(total);
    for (int i = 0; i < burst_samples; ++i) {
      // Low notes: their bands ring longest after the input stops.
      const float v = 0.4f * (std::sin(2.0f * juce::MathConstants<float>::pi * 110.0f * i / rate) +
                              0.5f * std::sin(2.0f * juce::MathConstants<float>::pi * 164.81f * i / rate));
      left[i] = right[i] = v;
    }
    for (int offset = 0; offset < total; offset += 128) {
      float* channels[] = { left.data() + offset, right.data() + offset };
      juce::AudioBuffer<float> buffer(channels, 2, std::min(128, total - offset));
      engine.process(buffer, p);
    }
    return std::make_pair(left, right);
  };
  auto energy = [](const std::vector<float>& v, int from, int to) {
    double e = 0.0;
    for (int i = from; i < to; ++i) e += double(v[i]) * v[i];
    return e;
  };
  Parameters p;
  p.muteLo = p.muteHi = 1.0f;
  p.width = 1.0f;
  auto wide = render(p, total);
  double side = 0.0, mid = 0.0;
  for (int i = rate / 2; i < total; ++i) {
    side += double(wide.first[i] - wide.second[i]) * (wide.first[i] - wide.second[i]);
    mid += double(wide.first[i] + wide.second[i]) * (wide.first[i] + wide.second[i]);
  }
  expect(side > 1.0e-3 * mid, "Width 100% places bands across the stereo field");
  p.width = 0.0f;
  auto mono = render(p, total);
  bool identical = true;
  for (int i = 0; i < total; ++i) identical = identical && mono.first[i] == mono.second[i];
  expect(identical, "Width 0 is mono: both channels identical");
  expect(energy(mono.first, rate / 2, total) > 1.0e-3, "Width 0 still plays");

  // A burst that stops: with Follow the tail dies with the input.
  p.width = 1.0f; p.follow = 0.0f;
  const int burst = rate / 2;
  auto ringing = render(p, burst);
  p.follow = 1.0f;
  auto followed = render(p, burst);
  const double tail_ringing = energy(ringing.first, burst + rate / 20, burst + rate * 3 / 10);
  const double tail_followed = energy(followed.first, burst + rate / 20, burst + rate * 3 / 10);
  const double body_ringing = energy(ringing.first, rate / 4, burst);
  const double body_followed = energy(followed.first, rate / 4, burst);
  std::cout << "follow: tail energy " << tail_ringing << " -> " << tail_followed
            << ", sustained body " << body_ringing << " -> " << body_followed << '\n';
  expect(tail_ringing > 0.002 * body_ringing, "the source rings on after a low note stops (the tail Follow is for)");
  expect(tail_followed < 0.1 * tail_ringing, "Follow 100% removes the tail after the input stops");
  expect(body_followed > 0.3 * body_ringing, "Follow keeps the sustained sound");
  expect(atlas::spectral_lock::Engine::latencySamples == 0, "Follow adds no latency");

  // Amount above 100% boosts the tones (Atlas extension).
  p = Parameters(); p.muteLo = p.muteHi = 1.0f; p.amount = 1.0f;
  const double unity = energy(render(p, total).first, rate / 2, total);
  p.amount = 2.0f;
  const double doubled = energy(render(p, total).first, rate / 2, total);
  std::cout << "filterbank amount: 100% energy " << unity << ", 200% energy " << doubled << '\n';
  expect(doubled > 3.0 * unity && doubled < 8.0 * unity, "Amount 200% is 6 to 9 dB louder than 100% (the mid band is removed too)");
}
}

namespace {
// Spectral mode: notes move as a whole to the scale, in-scale material passes.
void testSpectralMode() {
  using Engine = atlas::spectral_lock::retune::Engine;
  using Parameters = atlas::spectral_lock::retune::Parameters;
  constexpr int rate = 48000;
  constexpr int total = rate * 2;
  struct Rendered { std::vector<float> left; int notes; int partials; float note_hz; float note_shift; };
  auto render = [&](const Parameters& p, const std::vector<std::pair<float, float>>& tones) {
    Engine engine;
    engine.prepare(rate);
    std::vector<float> in(total), left(total), right(total);
    for (int i = 0; i < total; ++i) {
      float v = 0.0f;
      for (const auto& tone : tones)
        v += tone.second * std::sin(2.0f * juce::MathConstants<float>::pi * tone.first * i / rate);
      in[i] = v;
    }
    for (int offset = 0; offset < total; offset += 128) {
      const int count = std::min(128, total - offset);
      engine.process(in.data() + offset, in.data() + offset, left.data() + offset, right.data() + offset, count, p);
    }
    Rendered r { left, engine.numNotes(), 0, 0.0f, 0.0f };
    if (engine.numNotes() > 0) { r.partials = engine.note(0).partials; r.note_hz = engine.note(0).frequency; r.note_shift = engine.note(0).shift; }
    return r;
  };
  constexpr int kOrder = 15, kSize = 1 << kOrder;
  auto spectrum = [&](const std::vector<float>& signal) {
    juce::dsp::FFT fft(kOrder);
    std::vector<float> data(2 * kSize, 0.0f);
    const int start = int(signal.size()) - kSize;
    for (int i = 0; i < kSize; ++i)
      data[i] = signal[start + i] * (0.5f - 0.5f * std::cos(2.0f * juce::MathConstants<float>::pi * i / kSize));
    fft.performRealOnlyForwardTransform(data.data(), true);
    std::vector<double> db(kSize / 2);
    for (int bin = 0; bin < kSize / 2; ++bin)
      db[bin] = 10.0 * std::log10(data[2 * bin] * data[2 * bin] + data[2 * bin + 1] * data[2 * bin + 1] + 1e-20);
    return db;
  };
  auto peakHz = [&](const std::vector<double>& db, double lo, double hi) {
    int best = int(lo * kSize / rate);
    for (int bin = best; bin < int(hi * kSize / rate); ++bin) if (db[bin] > db[best]) best = bin;
    // Parabolic interpolation for a finer estimate.
    const double a = db[best - 1], b = db[best], c = db[best + 1];
    const double offset = 0.5 * (a - c) / (a - 2.0 * b + c + 1e-12);
    return (best + offset) * double(rate) / kSize;
  };
  auto levelNear = [&](const std::vector<double>& db, double hz) {
    const int centre = int(hz * kSize / rate + 0.5);
    double best = -200.0;
    for (int bin = centre - 4; bin <= centre + 4; ++bin) best = std::max(best, db[bin]);
    return best;
  };
  auto number = [](double v) { char t[32]; std::snprintf(t, sizeof(t), "%.2f", v); return std::string(t); };
  const int kOctaves = 11, kMajor = 1;

  // A lone A4 on an Octaves scale rooted on C moves down to C4 (the source's tables map downward).
  Parameters p;
  p.scale = kOctaves; p.reach = 12.0f;
  auto a4 = render(p, { { 440.0f, 0.5f } });
  auto db = spectrum(a4.left);
  double peak = peakHz(db, 100.0, 1000.0);
  std::cout << "spectral: A4 on Octaves -> " << number(peak) << " Hz\n";
  expect(std::abs(peak - 261.63) < 1.5, "a lone note moves to the scale note the tables choose");
  expect(a4.notes == 1 && std::abs(a4.note_hz - 440.0f) < 2.0f && std::abs(a4.note_shift + 9.0f) < 0.05f,
         "the note is detected at 440 Hz and shifted by -9 semitones");

  // In-scale material passes unchanged: A4 is in C major.
  p.scale = kMajor;
  auto in_scale = render(p, { { 440.0f, 0.5f } });
  db = spectrum(in_scale.left);
  peak = peakHz(db, 100.0, 1000.0);
  const double in_level = levelNear(db, 440.0);
  expect(std::abs(peak - 440.0) < 0.5 && in_level > 10.0 * std::log10(0.25 * 0.25 * kSize * kSize / 64.0) - 2.0,
         ("an in-scale note passes at its pitch and level, " + number(peak) + " Hz").c_str());
  double worst = 0.0;
  {
    std::vector<float> in(total);
    for (int i = 0; i < total; ++i) in[i] = 0.5f * std::sin(2.0f * juce::MathConstants<float>::pi * 440.0f * i / rate);
    for (int i = rate; i < total; ++i) worst = std::max(worst, double(std::abs(in_scale.left[i] - in[i - Engine::latencyForRate(rate)])));
  }
  expect(worst < 0.02, ("in-scale material reconstructs the delayed input closely, worst " + number(worst)).c_str());

  // A bass with harmonics: everything moves by one ratio with its fundamental.
  p.scale = kOctaves; p.rangeLo = 40.0f; p.rangeHi = 300.0f;
  std::vector<std::pair<float, float>> bass;
  for (int h = 1; h <= 8; ++h) bass.push_back({ 55.0f * h, 0.4f / h });
  auto moved = render(p, bass);
  db = spectrum(moved.left);
  const double ratio = 32.703 / 55.0;  // A1 -> C1: the tables map downward
  bool harmonics_follow = true;
  for (int h = 2; h <= 8; ++h) {
    const double expected = 55.0 * h * ratio;
    const double found = peakHz(db, expected * 0.94, expected * 1.06);
    if (std::abs(found - expected) > expected * 0.01) {
      harmonics_follow = false;
      std::cout << "harmonic " << h << ": expected " << number(expected) << " Hz, found " << number(found) << '\n';
    }
  }
  expect(harmonics_follow, "a bass and all its harmonics move together, including those above Range High");
  expect(moved.notes == 1 && moved.partials >= 6, ("the bass is one note with its harmonics grouped, got " +
         std::to_string(moved.notes) + " notes / " + std::to_string(moved.partials) + " partials").c_str());
  expect(levelNear(db, 55.0) < levelNear(db, 65.4) - 20.0, "the old fundamental is gone");
  expect(std::abs(moved.note_shift + 9.0f) < 0.05f, "the bass note is shifted by -9 semitones");

  // A chord with one wrong note: C, E and G stay, F# becomes F.
  p.scale = kMajor; p.rangeLo = 65.0f; p.rangeHi = 2200.0f; p.reach = 3.0f;
  auto chord = render(p, { { 261.63f, 0.3f }, { 329.63f, 0.3f }, { 392.0f, 0.3f }, { 369.99f, 0.3f } });
  db = spectrum(chord.left);
  std::cout << "chord: C " << number(peakHz(db, 250.0, 275.0)) << " E " << number(peakHz(db, 320.0, 340.0))
            << " G " << number(peakHz(db, 380.0, 405.0)) << " Hz\n";
  expect(std::abs(peakHz(db, 250.0, 275.0) - 261.63) < 1.0 && std::abs(peakHz(db, 320.0, 340.0) - 329.63) < 1.0 &&
         std::abs(peakHz(db, 380.0, 405.0) - 392.0) < 1.0, "the in-scale chord tones stay");
  std::cout << "chord: F# level " << number(levelNear(db, 369.99)) << " dB, F level " << number(levelNear(db, 349.23)) << " dB\n";
  expect(levelNear(db, 349.23) > levelNear(db, 369.99) + 15.0, "the F sharp is moved down to F");

  // Amount 200% boosts the retuned partial by 6 dB.
  p.scale = kOctaves; p.reach = 12.0f; p.amount = 2.0f;
  const double boosted = levelNear(spectrum(render(p, { { 440.0f, 0.5f } }).left), 261.63);
  const double plain = levelNear(spectrum(a4.left), 261.63);
  expect(std::abs(boosted - plain - 6.0) < 1.0, ("Amount 200% boosts a retuned note by 6 dB, got " + number(boosted - plain)).c_str());
  p.amount = 1.0f;

  // Reach and Range leave distant or out-of-range notes alone.
  p.scale = kOctaves; p.reach = 3.0f;
  db = spectrum(render(p, { { 440.0f, 0.5f } }).left);
  expect(std::abs(peakHz(db, 100.0, 1000.0) - 440.0) < 0.5, "a note farther than Reach is left alone");
  p.reach = 12.0f; p.rangeHi = 300.0f;
  db = spectrum(render(p, { { 440.0f, 0.5f } }).left);
  expect(std::abs(peakHz(db, 100.0, 1000.0) - 440.0) < 0.5, "a lone partial above Range High is left alone");

  // Mix 0 is the dry signal delayed by exactly the latency.
  p = Parameters(); p.mix = 0.0f;
  auto dry = render(p, { { 440.0f, 0.5f } });
  bool exact = true;
  for (int i = Engine::latencyForRate(rate); i < total; ++i) {
    const float in = 0.5f * std::sin(2.0f * juce::MathConstants<float>::pi * 440.0f * (i - Engine::latencyForRate(rate)) / rate);
    exact = exact && dry.left[i] == in;
  }
  expect(exact, "Mix 0 is the input delayed by the latency");

  // The module switches latency with the mode and stays allocation-free.
  vital::SpectralLockModule module;
  module.init();
  module.setSampleRate(rate);
  module.enable(true);
  expect(module.getLatencySamples() == 0, "Filterbank mode reports no latency");
  module.getControls().at("spectral_lock_mode")->set(1.0f);
  expect(module.getLatencySamples() == 8192, "Spectral mode reports 8192 samples of latency");
  std::vector<vital::poly_float> input(rate / 2);
  for (int i = 0; i < rate / 2; ++i) {
    const float v = 0.4f * std::sin(2.0f * juce::MathConstants<float>::pi * 440.0f * i / rate);
    input[i] = vital::poly_float(v, v, 0.3f * std::sin(2.0f * juce::MathConstants<float>::pi * 110.0f * i / rate), 0.0f);
  }
  bool finite = true;
  for (int offset = 0; offset < rate / 2; offset += 97) {
    const int count = std::min(97, rate / 2 - offset);
    allocations = 0; tracking = true;
    module.processWithInput(input.data() + offset, count);
    tracking = false;
    expect(allocations == 0, "Spectral mode processing allocates no memory");
    for (int i = 0; i < count; ++i)
      for (int lane = 0; lane < 4; ++lane) finite = finite && std::isfinite(module.output()->buffer[i][lane]);
  }
  expect(finite, "finite output on every lane in Spectral mode");

  // A note and its octave move together: the scale snap works on pitch class.
  p = Parameters(); p.scale = kMajor; p.reach = 3.0f; p.rangeLo = 65.0f; p.rangeHi = 2200.0f;
  db = spectrum(render(p, { { 138.59f, 0.3f }, { 277.18f, 0.3f } }).left);
  const double low_note = peakHz(db, 125.0, 155.0), high_note = peakHz(db, 250.0, 310.0);
  std::cout << "octave doubling: C#3 -> " << number(low_note) << " Hz, C#4 -> " << number(high_note) << " Hz\n";
  expect(std::abs(low_note - 138.59) > 3.0 && std::abs(high_note / low_note - 2.0) < 0.01,
         "an octave doubling is retuned by the same interval as the note below it");
}

// The frame follows the processing rate, so oversampling keeps the resolution.
void testSpectralModeRates() {
  auto number = [](double v) { char t[32]; std::snprintf(t, sizeof(t), "%.2f", v); return std::string(t); };
  using Engine = atlas::spectral_lock::retune::Engine;
  using Parameters = atlas::spectral_lock::retune::Parameters;
  expect(Engine::latencyForRate(44100) == 8192 && Engine::latencyForRate(48000) == 8192 &&
         Engine::latencyForRate(96000) == 16384 && Engine::latencyForRate(192000) == 32768 &&
         Engine::latencyForRate(384000) == 32768, "Spectral mode frame: 8192 per 48 kHz, at most 32768");
  for (int rate : { 96000, 192000 }) {
    Parameters p; p.scale = 11; p.reach = 12.0f;
    Engine engine;
    engine.prepare(rate);
    const int total = rate * 2;
    std::vector<float> in(total), left(total), right(total);
    for (int i = 0; i < total; ++i) in[i] = 0.5f * std::sin(2.0f * juce::MathConstants<float>::pi * 440.0f * i / rate);
    for (int offset = 0; offset < total; offset += 512) {
      const int count = std::min(512, total - offset);
      engine.process(in.data() + offset, in.data() + offset, left.data() + offset, right.data() + offset, count, p);
    }
    const int order = 17, size = 1 << order;
    juce::dsp::FFT fft(order);
    std::vector<float> data(2 * size, 0.0f);
    for (int i = 0; i < size; ++i)
      data[i] = left[total - size + i] * (0.5f - 0.5f * std::cos(2.0f * juce::MathConstants<float>::pi * i / size));
    fft.performRealOnlyForwardTransform(data.data(), true);
    int best = int(100.0 * size / rate);
    auto power = [&](int bin) { return data[2 * bin] * data[2 * bin] + data[2 * bin + 1] * data[2 * bin + 1]; };
    for (int bin = best; bin < int(1000.0 * size / rate); ++bin) if (power(bin) > power(best)) best = bin;
    const double hz = double(best) * rate / size;
    std::cout << "spectral at " << rate << " Hz: A4 on Octaves -> " << number(hz) << " Hz, latency "
              << engine.latencySamples() << '\n';
    expect(std::abs(hz - 261.63) < 2.0 * double(rate) / size + 0.5, "the retune lands on the scale note at high rates too");
    vital::SpectralLockModule module;
    module.init(); module.setSampleRate(48000); module.setOversampleAmount(rate / 48000);
    module.getControls().at("spectral_lock_mode")->set(1.0f);
    expect(module.getLatencySamples() == Engine::latencyForRate(rate), "the module reports the frame for its rate");
  }
}

// Switching Mode starts the incoming engine from silence: nothing of its past plays.
void testModeSwitchStartsClean() {
  auto number = [](double v) { char t[32]; std::snprintf(t, sizeof(t), "%.2f", v); return std::string(t); };
  constexpr int rate = 48000;
  vital::SpectralLockModule module;
  module.init(); module.setSampleRate(rate); module.enable(true);
  auto* mode = module.getControls().at("spectral_lock_mode");
  std::vector<vital::poly_float> tone(rate / 2), silence(rate / 2);
  for (int i = 0; i < rate / 2; ++i)
    tone[i] = vital::poly_float(0.4f * std::sin(2.0f * juce::MathConstants<float>::pi * 220.0f * i / rate));
  auto run = [&](const std::vector<vital::poly_float>& input) {
    float peak = 0.0f;
    for (int offset = 0; offset < int(input.size()); offset += 128) {
      const int count = std::min(128, int(input.size()) - offset);
      module.processWithInput(input.data() + offset, count);
      for (int i = 0; i < count; ++i)
        for (int lane = 0; lane < 2; ++lane) peak = std::max(peak, std::abs(module.output()->buffer[i][lane]));
    }
    return peak;
  };
  mode->set(1.0f);
  run(tone);                       // Spectral mode holds a frame of the tone
  mode->set(0.0f);
  run(tone);                       // Filterbank rings with the tone
  run(silence);
  mode->set(1.0f);
  const float spectral_after = run(silence);
  expect(spectral_after == 0.0f, ("Spectral mode replays nothing after a switch, peak " + number(spectral_after)).c_str());
  mode->set(0.0f);
  const float filterbank_after = run(silence);
  expect(filterbank_after < 1.0e-4f, ("Filterbank replays nothing after a switch, peak " + number(filterbank_after)).c_str());
}

// While a prepared Spectral Lock is pending or warming up in real time, the
// dry history must hold Spectral mode's latency.
void testPendingSpectralModeInRealtime() {
  auto worker = std::make_shared<vital::EffectPreparationWorker>();
  const std::string prefix = "fx_slot_1_";
  vital::PreparedEffectModule module(vital::constants::kSpectralLock, prefix,
    [prefix] { return std::make_unique<vital::SpectralLockModule>(prefix); }, worker);
  module.init(); module.setSampleRate(48000); module.setOversampleAmount(4);
  module.getControls().at(prefix + "spectral_lock_mode")->set(1.0f);
  expect(module.getLatencySamples() == 32768, "a prepared Spectral Lock at 4x reports the 192 kHz frame");
  std::vector<vital::poly_float> input(512);
  for (int i = 0; i < 512; ++i) input[i] = vital::poly_float(0.25f * std::sin(0.01f * float(i)));
  bool finite = true;
  vital::RealtimeEffectScope realtime(true);
  for (int block = 0; block < 400; ++block) {
    if (block == 200) module.hardReset();
    module.processWithInput(input.data(), 512);
    for (int i = 0; i < 512; ++i)
      for (int lane = 0; lane < 4; ++lane) finite = finite && std::isfinite(module.output()->buffer[i][lane]);
  }
  expect(finite, "pending and warm-up output of a Spectral-mode Spectral Lock stays finite");
}
}

int main() {
  juce::ScopedJuceInitialiser_GUI juce;
  testWidthAndFollow();
  testSpectralMode();
  testSpectralModeRates();
  testModeSwitchStartsClean();
  testPendingSpectralModeInRealtime();
  for (int rate : { 44100, 48000, 96000, 192000 }) {
    vital::SpectralLockModule module; module.init(); module.setSampleRate(rate);
    compare(module, rate, 1, 127);
  }
  for (int factor : { 2, 4, 8 }) {
    vital::SpectralLockModule module; module.init(); module.setSampleRate(48000); module.setOversampleAmount(factor);
    compare(module, 48000, factor, vital::kMaxBufferSize * factor);
  }
  // Prepared instances have independent state, including on separate busses.
  auto worker = std::make_shared<vital::EffectPreparationWorker>();
  for (const std::string prefix : { "fx_slot_1_", "bus_1_fx_slot_20_", "bus_2_fx_slot_1_", "bus_3_fx_slot_20_" }) {
    vital::PreparedEffectModule module(vital::constants::kSpectralLock, prefix,
      [prefix] { return std::make_unique<vital::SpectralLockModule>(prefix); }, worker);
    module.init(); module.setSampleRate(48000); module.prepareForPlayback();
    compare(module, 48000, 1, 64, prefix);
    module.hardReset(); module.prepareForPlayback();
    compare(module, 48000, 1, 17, prefix);
  }
  std::cout << "Spectral Lock failures: " << failures << '\n';
  return failures ? 1 : 0;
}
