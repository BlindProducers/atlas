#include "source_types.h"
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "oscillator_warp_fixture.h"
#include "wavetable_reference_tests.h"
#include "../src/common/wavetable/wavetable_creator.h"
#include "../src/plugin/synth_editor.h"
#include <complex>
#include <fstream>

namespace analog_oscillator_tests {
  using O = vital::SynthOscillator;

  namespace A = vital::analog;

  struct AnalogFixture : oscillator_warp_tests::Fixture<O> {
    AnalogFixture(int rate, int waveform, float shape = 0.0f, int voices = 1, int active = 1) :
        oscillator_warp_tests::Fixture<O>(rate, voices, active) {
      set(O::kOscillatorType, O::kAnalogType);
      set(O::kAnalogWaveform, waveform);
      set(O::kAnalogShape, shape);
      set(O::kSubOctave, A::kSubOctaveDown1);
      set(O::kSubWaveform, A::kSubSquare);
    }

    // The first block starts a note, as the voice handler does. The FM and
    // ring mod source is the usual moving test signal, or a constant.
    bool note_started = false;
    bool constant_modulation = false;
    float modulation_value = 0.0f;
    void process(int block) {
      auto* reset = controls[O::kReset].output();
      if (!note_started)
        reset->trigger(vital::poly_mask(A::kUninitializedStep), vital::kVoiceOn, 0);
      for (int i = 0; i < block; ++i)
        modulation.buffer[i] = constant_modulation ? vital::poly_float(modulation_value) : movingModulation(time + i);
      oscillator->process(block);
      time += block;
      reset->clearTrigger();
      note_started = true;
    }

    static vital::poly_float movingModulation(int sample) {
      const float t = float(sample);
      return vital::poly_float(.6f * std::sin(t * .021f), .4f * std::cos(t * .018f),
                               .5f * std::cos(t * .027f), .7f * std::sin(t * .031f));
    }

    void distortion(int primary, float primary_amount, int secondary = O::kNone, float secondary_amount = .5f) {
      set(O::kDistortionType, primary);
      set(O::kDistortionAmount, primary_amount);
      set(O::kSecondaryDistortionType, secondary);
      set(O::kSecondaryDistortionAmount, secondary_amount);
    }

    void fixedFrequency(float frequency) {
      set(O::kMidiTrack, 0);
      set(O::kFixedFrequencyMode, 1);
      set(O::kFixedFrequency, frequency);
    }

    // Samples of every output lane from the same render, after letting
    // parameter smoothing settle.
    std::array<std::vector<float>, vital::poly_float::kSize> lanes(int samples, int output = O::kRaw, int block = 128) {
      for (int b = 0; b < 16; ++b)
        process(block);
      std::array<std::vector<float>, vital::poly_float::kSize> result;
      while (static_cast<int>(result[0].size()) < samples) {
        process(block);
        for (int i = 0; i < block && static_cast<int>(result[0].size()) < samples; ++i)
          for (int lane = 0; lane < vital::poly_float::kSize; ++lane)
            result[lane].push_back(oscillator->output(output)->buffer[i][lane]);
      }
      return result;
    }

    std::vector<float> lane(int samples, int lane_index = 0, int output = O::kRaw) {
      return lanes(samples, output)[lane_index];
    }
  };

  // Power per DFT bin, for signals that are periodic in the window.
  std::vector<double> powerSpectrum(const std::vector<float>& samples) {
    const int size = static_cast<int>(samples.size());
    std::vector<double> power(size / 2 + 1);
    for (int k = 0; k <= size / 2; ++k) {
      const double w = 2.0 * juce::MathConstants<double>::pi * k / size;
      std::complex<double> step(std::cos(w), -std::sin(w)), rotation(1.0, 0.0), sum(0.0, 0.0);
      for (int n = 0; n < size; ++n) {
        sum += double(samples[n]) * rotation;
        rotation *= step;
        if ((n & 255) == 255)
          rotation /= std::abs(rotation);
      }
      power[k] = std::norm(sum);
    }
    return power;
  }

  // Energy outside the harmonics of `fundamental_bin`, relative to the total, in dB.
  double aliasingDb(const std::vector<float>& samples, int fundamental_bin) {
    const auto power = powerSpectrum(samples);
    double harmonic = 0.0, other = 0.0;
    for (size_t k = 1; k < power.size(); ++k)
      (k % fundamental_bin == 0 ? harmonic : other) += power[k];
    return 10.0 * std::log10(std::max(1e-30, other / std::max(1e-30, harmonic + other)));
  }

  double rms(const std::vector<float>& samples) {
    double sum = 0.0;
    for (float sample : samples) sum += double(sample) * sample;
    return std::sqrt(sum / std::max<size_t>(1, samples.size()));
  }

  double mean(const std::vector<float>& samples) {
    double sum = 0.0;
    for (float sample : samples) sum += sample;
    return sum / std::max<size_t>(1, samples.size());
  }

  double correlation(const std::vector<float>& a, const std::vector<float>& b, int offset = 0) {
    double ab = 0.0, aa = 0.0, bb = 0.0;
    for (size_t i = 0; i + offset < b.size() && i < a.size(); ++i) {
      ab += double(a[i]) * b[i + offset];
      aa += double(a[i]) * a[i];
      bb += double(b[i + offset]) * b[i + offset];
    }
    return ab / std::sqrt(std::max(1e-30, aa * bb));
  }

  double toDb(double ratio) { return 20.0 * std::log10(std::max(1e-30, ratio)); }

  // A falling saw without any band-limiting, for comparison.
  std::vector<float> naiveSaw(float frequency, int rate, int samples) {
    std::vector<float> result(samples);
    double t = 0.0;
    for (int i = 0; i < samples; ++i) {
      t += frequency / rate;
      t -= std::floor(t);
      double u = t + 0.5 - std::floor(t + 0.5);
      result[i] = float(1.0 - 2.0 * u);
    }
    return result;
  }

  void testLevelMatchesDefaultWavetable() {
    vital::Wavetable table(vital::kNumOscillatorWaveFrames);
    WavetableCreator creator(&table);
    creator.loadDefaultCreator();
    creator.render();
    for (float frequency : {55.0f, 110.0f, 440.0f}) {
      AnalogFixture analog(48000, A::kSaw);
      analog.fixedFrequency(frequency);
      AnalogFixture wavetable(48000, A::kSaw);
      wavetable.set(O::kOscillatorType, O::kWavetableType);
      wavetable.oscillator = std::make_unique<O>(&table);
      for (int i = 0; i < O::kNumInputs; ++i) wavetable.oscillator->plug(&wavetable.controls[i], i);
      wavetable.oscillator->setSampleRate(48000);
      wavetable.fixedFrequency(frequency);
      const auto a = analog.lane(9600);
      const auto w = wavetable.lane(9600);
      const double difference = toDb(rms(a) / rms(w));
      const double match = correlation(a, w);
      std::cout << "Saw level vs default wavetable at " << frequency << " Hz: " << difference
                << " dB, correlation " << match << '\n';
      expect(std::abs(difference) < 0.25, "Analog saw matches the default wavetable level at " + std::to_string(frequency) + " Hz");
      expect(match > 0.99, "Analog saw is phase aligned with the default wavetable at " + std::to_string(frequency) + " Hz");
    }
  }

  void testAliasing() {
    struct Case { int rate; float frequency; int bin; };
    // Bins are 10 Hz wide. Frequencies avoid dividing the rate so aliases land between harmonics.
    for (const Case& c : { Case { 48000, 1990.0f, 199 }, Case { 48000, 4430.0f, 443 }, Case { 96000, 4430.0f, 443 } }) {
      const int samples = c.rate / 10;
      const double naive = aliasingDb(naiveSaw(c.frequency, c.rate, samples), c.bin);
      for (int waveform : {A::kSaw, A::kSquare, A::kTriangle, A::kSine}) {
        for (float shape : {0.0f, 0.6f}) {
          AnalogFixture f(c.rate, waveform, shape);
          f.fixedFrequency(c.frequency);
          const double aliasing = aliasingDb(f.lane(samples), c.bin);
          std::cout << "Aliasing rate=" << c.rate << " f=" << c.frequency << " waveform=" << waveform
                    << " shape=" << shape << ": " << aliasing << " dB (naive saw " << naive << " dB)\n";
          expect(aliasing < naive - 15.0, "Band-limiting reduces aliasing well below a naive saw");
        }
      }
    }
  }

  void testPureShapes() {
    const int rate = 48000, samples = 4800;
    auto harmonicDb = [](const std::vector<double>& power, int bin, int harmonic) {
      return 10.0 * std::log10(std::max(1e-30, power[bin * harmonic] / power[bin]));
    };
    {
      AnalogFixture f(rate, A::kSine);
      f.fixedFrequency(440.0f);
      const auto power = powerSpectrum(f.lane(samples));
      double worst = -300.0;
      for (int h = 2; h <= 10; ++h) worst = std::max(worst, harmonicDb(power, 44, h));
      std::cout << "Sine worst harmonic: " << worst << " dB\n";
      expect(worst < -90.0, "Sine at Shape 0 is pure");
    }
    for (int waveform : {A::kSquare, A::kTriangle}) {
      AnalogFixture f(rate, waveform);
      f.fixedFrequency(440.0f);
      const auto samples_out = f.lane(samples);
      const auto power = powerSpectrum(samples_out);
      double worst_even = -300.0;
      for (int h = 2; h <= 20; h += 2) worst_even = std::max(worst_even, harmonicDb(power, 44, h));
      std::cout << "Waveform " << waveform << " worst even harmonic: " << worst_even << " dB, mean " << mean(samples_out) << '\n';
      expect(worst_even < -70.0, "Symmetric waveform at Shape 0 has only odd harmonics");
      expect(std::abs(mean(samples_out)) < 1e-3, "Symmetric waveform has no DC");
    }
    for (int waveform : {A::kSine, A::kTriangle, A::kSaw, A::kSquare}) {
      AnalogFixture f(rate, waveform);
      f.fixedFrequency(55.0f);
      const auto out = f.lane(samples);
      const float peak = *std::max_element(out.begin(), out.end());
      std::cout << "Waveform " << waveform << " peak " << peak << '\n';
      expect(peak > 0.97f && peak < 1.12f, "Waveform peaks at full scale");
    }
    for (float shape : {0.3f, 0.9f}) {
      AnalogFixture f(rate, A::kSquare, shape);
      f.fixedFrequency(110.0f);
      expect(std::abs(mean(f.lane(samples))) < 2e-3, "Narrow pulses have no DC");
    }
    {
      // A saw's second harmonic is half the fundamental; a triangle has none.
      AnalogFixture f(rate, A::kTriangle, 1.0f);
      f.fixedFrequency(440.0f);
      const double second = harmonicDb(powerSpectrum(f.lane(samples)), 44, 2);
      std::cout << "Triangle at full Shape second harmonic " << second << " dB\n";
      expect(std::abs(second + 6.02) < 1.0, "Triangle Shape tilts it into a saw");
    }
  }

  void testNoise() {
    const int rate = 48000, samples = 48000;
    AnalogFixture white(rate, A::kWhiteNoise, 0.0f), dark(rate, A::kWhiteNoise, 1.0f);
    const auto w = white.lane(samples), d = dark.lane(samples);
    const auto high_fraction = [](const std::vector<float>& x) {
      double high = 0.0, total = 0.0;
      for (size_t i = 1; i < x.size(); ++i) {
        const double difference = double(x[i]) - x[i - 1];
        high += difference * difference;
        total += 2.0 * double(x[i]) * x[i];
      }
      return high / std::max(1e-30, total);
    };
    std::cout << "White noise rms " << rms(w) << " dark rms " << rms(d) << " high fraction " << high_fraction(w)
              << " vs " << high_fraction(d) << '\n';
    expect(std::abs(rms(w) - 0.57735) < 0.02, "White noise has the expected level");
    expect(std::abs(toDb(rms(d) / rms(w))) < 1.5, "Dark noise keeps the same level");
    expect(high_fraction(d) < 0.1 * high_fraction(w), "Shape darkens white noise");
    expect(std::abs(correlation(w, std::vector<float>(w.begin() + 1, w.end()))) < 0.05, "Shape 0 is white");

    for (int active : {1, 3}) {
      AnalogFixture mono(rate, A::kWhiteNoise, 0.0f, 1, active), wide(rate, A::kWhiteNoise, 0.0f, 4, active);
      const auto centred = mono.lanes(4096), spread = wide.lanes(4096);
      expect(correlation(centred[0], centred[1]) > 0.9999, "A single noise voice is centred");
      expect(std::abs(correlation(spread[0], spread[1])) < 0.9, "Noise unison spreads across the stereo field");
      if (active == 3)
        expect(std::abs(correlation(centred[0], centred[2])) < 0.1, "Two notes play independent noise");
    }

    for (float frequency : {120.0f, 320.0f}) {
      const int period = int(std::lround(rate / frequency));
      AnalogFixture looped(rate, A::kPitchedNoise, 1.0f), random(rate, A::kPitchedNoise, 0.0f);
      looped.fixedFrequency(frequency);
      random.fixedFrequency(frequency);
      const auto l = looped.lane(8 * period + 64), r = random.lane(8 * period + 64);
      const double looped_match = correlation(l, l, period);
      const double random_match = correlation(r, r, period);
      std::cout << "Pitched noise at " << frequency << " Hz period correlation looped " << looped_match
                << " random " << random_match << " rms " << rms(l) << '\n';
      expect(looped_match > 0.97, "Looped pitched noise repeats every cycle");
      expect(std::abs(random_match) < 0.4, "Random pitched noise does not repeat");
    }
    for (int bin : {44, 199, 443}) {
      AnalogFixture looped(rate, A::kPitchedNoise, 1.0f);
      looped.fixedFrequency(bin * 10.0f);
      const double aliasing = aliasingDb(looped.lane(4800), bin);
      std::cout << "Looped pitched noise inharmonic energy at " << bin * 10 << " Hz: " << aliasing << " dB\n";
      expect(aliasing < -25.0, "Looped pitched noise follows the pitch at " + std::to_string(bin * 10) + " Hz");
    }
  }

  void testSub() {
    const int rate = 48000, samples = 4800;
    for (int octave : {A::kSubOctaveDown1, A::kSubOctaveDown2}) for (int waveform : {A::kSubSine, A::kSubTriangle, A::kSubSquare}) {
      AnalogFixture f(rate, A::kSine);
      f.fixedFrequency(440.0f);
      f.set(O::kSubLevel, 1.0f);
      f.set(O::kSubOctave, octave);
      f.set(O::kSubWaveform, waveform);
      const auto power = powerSpectrum(f.lane(samples));
      const int sub_bin = octave == A::kSubOctaveDown1 ? 22 : 11;
      const double ratio = 10.0 * std::log10(power[sub_bin] / power[44]);
      std::cout << "Sub octave " << octave << " waveform " << waveform << ": sub vs main " << ratio << " dB\n";
      expect(ratio > -3.0, "Sub oscillator plays below the note");
      if (waveform == A::kSubSine)
        expect(std::abs(ratio) < 0.1, "Sub sine at full level matches the main sine");
    }

    // The sub stays centred and follows the oscillator's Level and Pan.
    auto render = [](float sub_level, int output, float pan) {
      AnalogFixture f(48000, A::kSaw, 0.3f, 5, 3);
      f.set(O::kUnisonDetune, .4f);
      f.set(O::kSubLevel, sub_level);
      f.set(O::kAmplitude, .6f);
      f.set(O::kPan, pan);
      std::vector<float> result;
      for (int lane = 0; lane < 4; ++lane) {
        AnalogFixture g(48000, A::kSaw, 0.3f, 5, 3);
        g.set(O::kUnisonDetune, .4f);
        g.set(O::kSubLevel, sub_level);
        g.set(O::kAmplitude, .6f);
        g.set(O::kPan, pan);
        const auto x = g.lane(2048, lane, output);
        result.insert(result.end(), x.begin(), x.end());
      }
      return result;
    };
    const auto with = render(1.0f, O::kRaw, 0.0f), without = render(0.0f, O::kRaw, 0.0f);
    std::vector<float> sub_left(2048), sub_right(2048);
    for (int i = 0; i < 2048; ++i) {
      sub_left[i] = with[i] - without[i];
      sub_right[i] = with[2048 + i] - without[2048 + i];
    }
    expect(rms(sub_left) > 0.3, "Sub is audible");
    expect(correlation(sub_left, sub_right) > 0.99999, "Sub stays centred with stereo unison");

    const auto raw = render(1.0f, O::kRaw, -.4f), levelled = render(1.0f, O::kLevelled, -.4f);
    const auto pan = vital::futils::panAmplitude(vital::poly_float(-.4f));
    double error = 0.0, energy = 0.0;
    for (int lane = 0; lane < 4; ++lane) for (int i = 0; i < 2048; ++i) {
      const double expected = double(raw[lane * 2048 + i]) * .36 * pan[lane];
      energy += expected * expected;
      error += (levelled[lane * 2048 + i] - expected) * (levelled[lane * 2048 + i] - expected);
    }
    expect(std::sqrt(error / energy) < 1e-5, "Level and Pan apply to the whole oscillator, sub included");
  }

  void testVoiceLayoutsAndStability() {
    // One active voice packs four unison oscillators per vector; it must sound
    // the same as when both voices are playing.
    // Loop decisions are random, so pitched noise is compared fully looped.
    for (int waveform : {A::kSaw, A::kSquare, A::kPitchedNoise}) for (int voices : {1, 3, 6, 16}) {
      const float shape = waveform == A::kPitchedNoise ? 1.0f : .4f;
      AnalogFixture one(48000, waveform, shape, voices, 1), both(48000, waveform, shape, voices, 3);
      one.set(O::kUnisonDetune, .3f); both.set(O::kUnisonDetune, .3f);
      const auto a = one.lane(1024, 0), b = both.lane(1024, 0);
      double difference = 0.0;
      for (size_t i = 0; i < a.size(); ++i) difference = std::max(difference, std::abs(double(a[i]) - b[i]));
      expect(difference < 1e-4, "Single and dual voice layouts match, waveform " + std::to_string(waveform) +
                                " voices " + std::to_string(voices) + " difference " + std::to_string(difference));
    }

    for (int rate : {44100, 48000, 96000}) for (int block : {1, 17, 128}) for (int active : {1, 2, 3}) {
      for (int waveform = 0; waveform < A::kNumWaveforms; ++waveform) for (int voices : {1, 4, 16}) {
        AnalogFixture f(rate, waveform, 0.0f, voices, active);
        f.set(O::kSubLevel, .7f);
        f.set(O::kSubWaveform, waveform % A::kNumSubWaveforms);
        float peak = 0.0f;
        bool finite = true;
        static const int primaries[] = { O::kNone, O::kSync, O::kFmOscillatorA, O::kRmOscillatorB, O::kFmSample, O::kBend };
        static const int secondaries[] = { O::kNone, O::kFmOscillatorB, O::kSync, O::kRmOscillatorA };
        for (int b = 0; b < std::max(8, 1024 / block); ++b) {
          f.set(O::kAnalogShape, (b % 11) / 10.0f);
          f.distortion(primaries[(b / 3) % 6], (b % 13) / 12.0f, secondaries[(b / 7) % 4], (b % 5) / 4.0f);
          f.set(O::kMidiNote, 20 + (b * 7) % 100);
          if (b % 5 == 3)
            f.set(O::kOscillatorType, (b / 5) % 2 ? O::kWavetableType : O::kAnalogType);
          tracked_allocations.store(0);
          track_allocations.store(true);
          f.process(block);
          track_allocations.store(false);
          expect(tracked_allocations.load() == 0, "Analog oscillator does not allocate while processing");
          for (int i = 0; i < block; ++i) {
            finite = finite && vital::utils::isFinite(f.oscillator->output(O::kRaw)->buffer[i]);
            const vital::poly_float magnitude = vital::poly_float::abs(f.oscillator->output(O::kRaw)->buffer[i]);
            for (int lane = 0; lane < vital::poly_float::kSize; ++lane)
              peak = std::max(peak, magnitude[lane]);
          }
        }
        expect(finite, "Analog oscillator stays finite");
        // Sixteen in-phase unison voices of a narrow, DC-free pulse peak near 6.4.
        expect(peak < 8.0f, "Analog oscillator stays bounded, waveform " + std::to_string(waveform) + " voices " +
                            std::to_string(voices) + " peak " + std::to_string(peak));
      }
    }
  }

  double maximumDifference(const std::vector<float>& a, const std::vector<float>& b, int offset = 0) {
    double difference = 0.0;
    for (size_t i = 0; i + offset < b.size() && i < a.size(); ++i)
      difference = std::max(difference, std::abs(double(a[i]) - b[i + offset]));
    return difference;
  }

  // A hard-synced falling saw without any band-limiting, for comparison.
  std::vector<float> naiveSyncedSaw(float frequency, float ratio, int rate, int samples) {
    std::vector<float> result(samples);
    for (int i = 0; i < samples; ++i) {
      double master = double(i + 1) * frequency / rate + 0.5;
      master -= std::floor(master);
      double slave = master * ratio;
      slave -= std::floor(slave);
      result[i] = float(1.0 - 2.0 * slave);
    }
    return result;
  }

  void testHardSync() {
    // At a 1:1 ratio (50%) Hard Sync is the free-running oscillator, two samples later.
    for (int waveform : {A::kSine, A::kTriangle, A::kSaw, A::kSquare}) for (float shape : {0.0f, .4f}) {
      AnalogFixture free(48000, waveform, shape), synced(48000, waveform, shape);
      free.fixedFrequency(440.0f);
      synced.fixedFrequency(440.0f);
      synced.distortion(O::kSync, .5f);
      const double difference = maximumDifference(free.lane(4800), synced.lane(4800), 2);
      std::cout << "Hard Sync at 1:1, waveform " << waveform << " shape " << shape << ": difference " << difference << '\n';
      expect(difference < 2e-3, "Hard Sync at 50% matches the free-running waveform");
    }

    const double naive = aliasingDb(naiveSyncedSaw(1990.0f, 3.03f, 48000, 4800), 199);
    for (int waveform : {A::kSine, A::kTriangle, A::kSaw, A::kSquare}) for (float amount : {.7f, .9f}) {
      AnalogFixture synced(48000, waveform);
      synced.fixedFrequency(1990.0f);
      synced.distortion(O::kSync, amount);
      const auto samples = synced.lane(4800);
      const double aliasing = aliasingDb(samples, 199);
      std::cout << "Hard Sync aliasing, waveform " << waveform << " amount " << amount << ": " << aliasing
                << " dB (naive synced saw " << naive << " dB), rms " << rms(samples) << "\n";
      expect(aliasing < naive - 15.0, "Hard Sync is band-limited and keeps the note's pitch");
      expect(rms(samples) > .2, "Hard Sync stays audible");
    }

    // Distortion Spread gives each unison oscillator its own sync ratio.
    AnalogFixture even(48000, A::kSaw, 0.0f, 3, 1), spread(48000, A::kSaw, 0.0f, 3, 1);
    for (auto* f : {&even, &spread}) {
      f->set(O::kUnisonDetune, 0.0f);
      f->distortion(O::kSync, .7f);
    }
    spread.set(O::kUnisonDistortionSpread, .25f);
    expect(maximumDifference(even.lane(2048), spread.lane(2048)) > .1, "Distortion Spread changes the sync across unison");
  }

  void testFmAndRingMod() {
    // FM uses the wavetable depth: a constant 0.5 at 30% moves the phase by 6 x 0.3^2 x 0.5 = 0.27 cycles.
    AnalogFixture free(48000, A::kSine), modulated(48000, A::kSine);
    free.fixedFrequency(480.0f);
    modulated.fixedFrequency(480.0f);
    modulated.constant_modulation = true;
    modulated.modulation_value = .5f;
    modulated.distortion(O::kFmOscillatorA, .3f);
    const double fm_difference = maximumDifference(modulated.lane(4800), free.lane(4800 + 100), 27);
    std::cout << "FM phase offset difference " << fm_difference << '\n';
    expect(fm_difference < 1e-3, "FM depth matches the wavetable oscillator's");

    for (int waveform : {A::kSaw, A::kPitchedNoise}) {
      const float shape = waveform == A::kPitchedNoise ? 1.0f : 0.0f;
      AnalogFixture plain(48000, waveform, shape, 1, 3), zero(48000, waveform, shape, 1, 3);
      zero.distortion(O::kFmOscillatorA, 0.0f);
      expect(maximumDifference(plain.lane(2048), zero.lane(2048)) < 1e-4, "FM at zero amount leaves the sound alone");
    }

    // Ring mod scales the output towards the modulator.
    AnalogFixture plain(48000, A::kSaw, 0.0f, 1, 3), ring(48000, A::kSaw, 0.0f, 1, 3);
    ring.distortion(O::kRmOscillatorB, .6f);
    const auto dry = plain.lanes(2048), wet = ring.lanes(2048);
    double ring_error = 0.0;
    for (int lane = 0; lane < 4; ++lane) for (int i = 0; i < 2048; ++i) {
      const float modulator = AnalogFixture::movingModulation(16 * 128 + i)[lane];
      const double expected = dry[lane][i] * (1.0 + (modulator - 1.0) * .6);
      ring_error = std::max(ring_error, std::abs(expected - wet[lane][i]));
    }
    std::cout << "Ring mod error " << ring_error << '\n';
    expect(ring_error < 1e-4, "Ring mod mixes the modulator in like the wavetable oscillator");

    // Slot order doesn't matter for Analog oscillators.
    AnalogFixture forward(48000, A::kSquare, .3f, 3, 3), backward(48000, A::kSquare, .3f, 3, 3);
    forward.distortion(O::kFmOscillatorA, .4f, O::kRmOscillatorB, .4f);
    backward.distortion(O::kRmOscillatorB, .4f, O::kFmOscillatorA, .4f);
    expect(maximumDifference(forward.lane(2048), backward.lane(2048)) < 1e-5, "Distortion slot order doesn't change Analog");

    // Wavetable-only warps, and Hard Sync on Pitched Noise, do nothing.
    AnalogFixture saw(48000, A::kSaw), bent(48000, A::kSaw);
    bent.distortion(O::kBend, .8f, O::kQuantize, .7f);
    expect(maximumDifference(saw.lane(2048), bent.lane(2048)) == 0.0, "Wavetable warps are off for Analog");
    AnalogFixture noise(48000, A::kPitchedNoise, 1.0f), synced_noise(48000, A::kPitchedNoise, 1.0f);
    synced_noise.distortion(O::kSync, .8f);
    expect(maximumDifference(noise.lane(2048), synced_noise.lane(2048)) == 0.0, "Hard Sync is off for Pitched Noise");
  }

  int runDsp() {
    testHardSync();
    testFmAndRingMod();
    testLevelMatchesDefaultWavetable();
    testAliasing();
    testPureShapes();
    testNoise();
    testSub();
    testVoiceLayoutsAndStability();
    std::cout << "Analog oscillator DSP tests: " << failures << " failures\n";
    return failures ? 1 : 0;
  }

  // Finds a visible control; the editor keeps some hidden legacy controls.
  Component* findTitled(Component& parent, const String& title, bool root = true) {
    if (!root && !parent.isVisible())
      return nullptr;
    if (parent.getTitle().equalsIgnoreCase(title))
      return &parent;
    for (auto* child : parent.getChildren())
      if (auto* found = findTitled(*child, title, false))
        return found;
    return nullptr;
  }

  // The editor lists every section; search only the given oscillator's page.
  Component* findInSection(Component& editor, const String& section, const String& title) {
    std::function<Component*(Component&)> findSection = [&](Component& parent) -> Component* {
      if (parent.getProperties()["SectionName"].toString() == section && parent.getNumChildComponents() > 0)
        return &parent;
      for (auto* child : parent.getChildren())
        if (auto* found = findSection(*child))
          return found;
      return nullptr;
    };
    auto* page = findSection(editor);
    expect(page != nullptr, "Section page exists: " + section.toStdString());
    return page != nullptr ? findTitled(*page, title) : nullptr;
  }

  void pumpEditor() {
    for (int i = 0; i < 4; ++i) {
      Thread::sleep(40);
      Timer::callPendingTimersSynchronously();
    }
  }

  // Renders a held note and returns the RMS of each output channel.
  std::pair<double, double> playNote(SynthPlugin& plugin) {
    AudioSampleBuffer audio(2, 128);
    MidiBuffer midi;
    midi.addEvent(MidiMessage::noteOn(1, 48, .9f), 0);
    double left = 0.0, right = 0.0;
    int count = 0;
    for (int b = 0; b < 80; ++b) {
      audio.clear();
      plugin.processBlock(audio, midi);
      midi.clear();
      if (b < 40)
        continue;
      for (int i = 0; i < 128; ++i, ++count) {
        left += double(audio.getSample(0, i)) * audio.getSample(0, i);
        right += double(audio.getSample(1, i)) * audio.getSample(1, i);
      }
    }
    midi.addEvent(MidiMessage::allNotesOff(1), 0);
    for (int b = 0; b < 200; ++b) {
      audio.clear();
      plugin.processBlock(audio, midi);
      midi.clear();
    }
    return { std::sqrt(left / count), std::sqrt(right / count) };
  }

  int runControls() {
    juce::ScopedJuceInitialiser_GUI gui;
    SynthPlugin plugin;
    plugin.setPlayConfigDetails(0, 2, 48000, 128);
    plugin.prepareToPlay(48000, 128);
    const auto controls = plugin.getControls();
    const std::pair<const char*, float> defaults[] = {
      { "type", O::kWavetableType }, { "analog_waveform", A::kSaw }, { "analog_shape", 0.0f },
      { "sub_level", 0.0f }, { "sub_octave", A::kSubOctaveDown1 }, { "sub_waveform", A::kSubSquare }
    };
    // Slots 5-8 default to the Sample, Granular, Spectral and Noise types they replaced.
    auto defaultFor = [](int osc, const std::pair<const char*, float>& entry) {
      if (std::string(entry.first) == "type" && osc > 4)
        return float(vital::source_types::kLegacySourceTypes[osc - 5]);
      return entry.second;
    };
    for (int osc = 1; osc <= vital::kNumOscillators; ++osc) for (const auto& entry : defaults) {
      const std::string id = "osc_" + std::to_string(osc) + "_" + entry.first;
      expect(controls.count(id) == 1, "Control exists: " + id);
      if (controls.count(id))
        expectClose(controls.at(id)->value(), defaultFor(osc, entry), 0, "Clean default for " + id);
    }

    // Existing host automation IDs keep their positions.
    int legacy_index = 0;
    for (int i = 0; i < vital::Parameters::getNumParameters(); ++i) {
      const auto* details = vital::Parameters::getDetails(i);
      if (details->version_added >= 0x020113 ||
          (!controls.count(details->name) && !controls.count(SynthPlugin::legacyAliasTarget(details->name))))
        continue; // Legacy source aliases keep their number.
      for (auto* parameter : plugin.getParameters()) {
        auto* bridge = dynamic_cast<ValueBridge*>(parameter);
        if (bridge && bridge->getParameterId().toStdString() == details->name)
          expect(bridge->getParameterID() == String(legacy_index), "Original host automation ID remains stable");
      }
      ++legacy_index;
    }

    // Level and Pan control the whole Analog oscillator, sub included.
    plugin.parameterChanged("osc_1_type", O::kAnalogType);
    plugin.parameterChanged("osc_1_sub_level", 1.0f);
    const auto normal = playNote(plugin);
    std::cout << "Analog note rms " << normal.first << " / " << normal.second << '\n';
    expect(normal.first > 1e-3 && normal.second > 1e-3, "Analog oscillator plays through the synth");
    plugin.parameterChanged("osc_1_analog_waveform", A::kSine);
    plugin.parameterChanged("osc_1_sub_waveform", A::kSubSine);
    const auto with_sub = playNote(plugin);
    plugin.parameterChanged("osc_1_sub_level", 0.0f);
    const auto without_sub = playNote(plugin);
    std::cout << "Sine note with sub " << with_sub.first << ", without " << without_sub.first << '\n';
    expectClose(float(toDb(with_sub.first / without_sub.first)), 3.01f, 0.3f, "Sub adds an equal-level sine an octave down");
    plugin.parameterChanged("osc_1_sub_level", 1.0f);
    plugin.parameterChanged("osc_1_level", 0.0f);
    // The first note lets Atlas's usual Level smoothing reach zero.
    playNote(plugin);
    const auto silent = playNote(plugin);
    expect(silent.first < 1e-6 && silent.second < 1e-6, "Level at zero silences the oscillator and its sub");
    const float default_level = vital::Parameters::getDetails("osc_1_level").default_value;
    plugin.parameterChanged("osc_1_level", default_level);
    plugin.parameterChanged("osc_1_pan", -1.0f);
    const auto left = playNote(plugin);
    std::cout << "Hard left pan rms " << left.first << " / " << left.second << '\n';
    expect(left.second < left.first * 1e-3, "Pan moves the oscillator and its sub together");
    plugin.parameterChanged("osc_1_pan", 0.0f);

    // Modulation reaches the new destinations.
    for (const char* id : {"osc_1_analog_shape", "osc_1_sub_level"})
      expect(plugin.connectModulation("macro_control_1", id), std::string("Modulation route to ") + id);

    // State round trip, and old presets load as wavetables.
    plugin.parameterChanged("osc_2_type", O::kAnalogType);
    plugin.parameterChanged("osc_2_analog_waveform", A::kPitchedNoise);
    plugin.parameterChanged("osc_2_analog_shape", .7f);
    plugin.parameterChanged("osc_2_sub_octave", A::kSubOctaveDown2);
    plugin.parameterChanged("osc_2_sub_waveform", A::kSubTriangle);
    MemoryBlock state;
    plugin.getStateInformation(state);
    SynthPlugin restored;
    restored.setStateInformation(state.getData(), int(state.getSize()));
    for (int osc = 1; osc <= vital::kNumOscillators; ++osc) for (const auto& entry : defaults) {
      const std::string id = "osc_" + std::to_string(osc) + "_" + entry.first;
      expectClose(restored.getControls().at(id)->value(), controls.at(id)->value(), 0, "State reload keeps " + id);
    }
    expect(restored.getConnectionIndex("macro_control_1", "osc_1_analog_shape") >= 0, "Shape modulation survives reload");
    MemoryInputStream stream(state.getData(), state.getSize(), false);
    auto legacy = json::parse(stream.readEntireStreamAsString().toStdString());
    for (int osc = 1; osc <= vital::kNumOscillators; ++osc) for (const auto& entry : defaults)
      legacy["settings"].erase("osc_" + std::to_string(osc) + "_" + entry.first);
    legacy["settings"]["modulations"] = json::array();
    const auto text = legacy.dump();
    restored.setStateInformation(text.data(), int(text.size()));
    for (int osc = 1; osc <= vital::kNumOscillators; ++osc) for (const auto& entry : defaults) {
      const std::string id = "osc_" + std::to_string(osc) + "_" + entry.first;
      expectClose(restored.getControls().at(id)->value(), defaultFor(osc, entry), 0, "Old preset loads the default " + id);
    }

    // The accessible editor shows the controls for the current type.
    plugin.parameterChanged("osc_1_type", O::kWavetableType);
    plugin.parameterChanged("osc_1_analog_waveform", A::kSaw);
    SynthEditor editor(plugin);
    pumpEditor();
    const char* analog_rows[] = { "Oscillator 1 Analog Waveform", "Oscillator 1 Analog Shape", "Oscillator 1 Sub Level",
                                  "Oscillator 1 Sub Octave", "Oscillator 1 Sub Waveform" };
    const char* wavetable_rows[] = { "Oscillator 1 Wave Frame", "Load wavetable",
                                     "Oscillator 1 Frequency Morph Type" };
    const char* shared_rows[] = { "Oscillator 1 Type", "Oscillator 1 Level", "Oscillator 1 Pan", "Oscillator 1 Unison Voices",
                                  "Distortion mode", "Secondary distortion mode" };
    auto checkRows = [&](bool analog) {
      const auto& order = editor.getKeyboardFocusOrder();
      for (const char* title : shared_rows) {
        auto* control = findInSection(editor, "Oscillator 1", title);
        expect(control != nullptr, std::string("Row is visible: ") + title);
        if (control)
          expect(std::find(order.begin(), order.end(), control) != order.end(), std::string("Row is in focus order: ") + title);
      }
      for (const char* title : analog_rows) {
        auto* control = findInSection(editor, "Oscillator 1", title);
        expect((control != nullptr) == analog, std::string(analog ? "Analog shows " : "Wavetable hides ") + title);
        if (control && analog)
          expect(std::find(order.begin(), order.end(), control) != order.end(), std::string("Row is in focus order: ") + title);
      }
      for (const char* title : wavetable_rows)
        expect((findInSection(editor, "Oscillator 1", title) != nullptr) != analog,
               std::string(analog ? "Analog hides " : "Wavetable shows ") + title);
      expect(findInSection(editor, "Oscillator 3", "Load wavetable") != nullptr,
             "Other oscillators keep their own type and wavetable actions");
    };
    checkRows(false);

    auto* type = dynamic_cast<Slider*>(findTitled(editor, "Oscillator 1 Type"));
    expect(type != nullptr, "Type is a slider with named choices");
    if (type != nullptr) {
      expect(type->keyPressed(KeyPress(KeyPress::rightKey)), "Arrow keys change the oscillator type");
      expectClose(controls.at("osc_1_type")->value(), O::kAnalogType, 0, "Right arrow chooses Analog");
    }
    pumpEditor();
    checkRows(true);

    // Analog rows sit after Level and Pan, where Wave Frame sits for wavetables.
    {
      const auto& order = editor.getKeyboardFocusOrder();
      auto position = [&](const char* title) {
        auto* control = findInSection(editor, "Oscillator 1", title);
        return int(std::find(order.begin(), order.end(), control) - order.begin());
      };
      expect(position("Oscillator 1 Switch") < position("Oscillator 1 Solo") &&
             position("Oscillator 1 Solo") < position("Oscillator 1 Type"), "Solo follows the switch, then Type");
      expect(position("Oscillator 1 Pan") < position("Oscillator 1 Analog Waveform"), "Analog rows follow Pan");
      expect(position("Oscillator 1 Sub Waveform") < position("Oscillator 1 Unison Voices"), "Analog rows precede unison");
    }

    // Distortion modes for Analog: None, Hard Sync, then FM and ring mod sources.
    auto distortionSlider = [&] { return dynamic_cast<Slider*>(findInSection(editor, "Oscillator 1", "Distortion mode")); };
    if (auto* mode = distortionSlider()) {
      expect(mode->getDescription().contains("Hard Sync"), "Distortion mode explains the Analog modes");
      mode->keyPressed(KeyPress(KeyPress::backspaceKey));
      mode->keyPressed(KeyPress(KeyPress::rightKey));
      expectClose(controls.at("osc_1_distortion_type")->value(), O::kSync, 0, "Hard Sync follows None");
      expect(mode->getTextFromValue(mode->getValue()) == "Hard Sync", "The sync mode reads Hard Sync");
      mode->keyPressed(KeyPress(KeyPress::rightKey));
      expectClose(controls.at("osc_1_distortion_type")->value(), O::kFmOscillatorA, 0, "FM follows Hard Sync, skipping wavetable warps");
      mode->keyPressed(KeyPress(KeyPress::backspaceKey));
    }
    else
      expect(false, "Distortion mode is available for Analog");
    plugin.parameterChanged("osc_1_distortion_type", O::kBend);
    pumpEditor();
    if (auto* mode = distortionSlider())
      expect(mode->getTextFromValue(mode->getValue()) == "None", "A stored wavetable warp reads as None for Analog");
    plugin.parameterChanged("osc_1_distortion_type", O::kNone);
    plugin.parameterChanged("osc_1_analog_waveform", A::kPitchedNoise);
    pumpEditor();
    if (auto* mode = distortionSlider()) {
      mode->keyPressed(KeyPress(KeyPress::rightKey));
      expectClose(controls.at("osc_1_distortion_type")->value(), O::kFmOscillatorA, 0, "Pitched Noise offers FM but not Hard Sync");
      mode->keyPressed(KeyPress(KeyPress::backspaceKey));
    }
    else
      expect(false, "Distortion mode is available for Pitched Noise");
    plugin.parameterChanged("osc_1_analog_waveform", A::kWhiteNoise);
    pumpEditor();
    expect(findInSection(editor, "Oscillator 1", "Distortion mode") == nullptr, "White Noise hides the Distortion rows");
    expect(findInSection(editor, "Oscillator 1", "Oscillator 1 Distortion Amount") == nullptr, "White Noise hides Distortion Amount");
    plugin.parameterChanged("osc_1_analog_waveform", A::kSaw);
    pumpEditor();
    auto* shape = findTitled(editor, "Oscillator 1 Analog Shape");
    expect(shape != nullptr && shape->getDescription().contains("Second saw"), "Shape describes what it does on a saw");
    plugin.parameterChanged("osc_1_analog_waveform", A::kSquare);
    pumpEditor();
    shape = findTitled(editor, "Oscillator 1 Analog Shape");
    expect(shape != nullptr && shape->getDescription().contains("Pulse width"), "Shape describes pulse width on a square");
    expect(findTitled(editor, "Oscillator 1, analog square") != nullptr, "Oscillator page title names the analog waveform");
    if (auto* restored_type = dynamic_cast<Slider*>(findTitled(editor, "Oscillator 1 Type"))) {
      expect(restored_type->keyPressed(KeyPress(KeyPress::backspaceKey)), "Backspace resets the oscillator type");
      expectClose(controls.at("osc_1_type")->value(), O::kWavetableType, 0, "Backspace restores Wavetable");
    }
    pumpEditor();
    checkRows(false);
    std::cout << "Analog oscillator control tests: " << failures << " failures\n";
    return failures ? 1 : 0;
  }

  int benchmark() {
    for (int mode : {int(O::kSync), int(O::kFmOscillatorA), int(O::kRmOscillatorA)}) for (int waveform : {A::kSaw, A::kTriangle}) {
      AnalogFixture f(48000, waveform, .3f, 16, 3);
      f.distortion(mode, .7f);
      for (int b = 0; b < 100; ++b) f.process(128);
      const auto start = std::chrono::steady_clock::now();
      for (int b = 0; b < 4000; ++b) f.process(128);
      const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
      std::cout << "Analog waveform " << waveform << " distortion " << mode
                << " 16 unison, 2 voices: " << ms / 4000.0 * 1000.0 << " us per 128 samples\n";
    }
    for (int type : {O::kWavetableType, O::kAnalogType}) for (int waveform : {A::kSaw, A::kSquare, A::kWhiteNoise, A::kSine})
    for (float shape : {0.0f, .3f}) {
      AnalogFixture f(48000, waveform, shape, 16, 3);
      f.set(O::kOscillatorType, type);
      f.set(O::kSubLevel, .5f);
      for (int b = 0; b < 100; ++b) f.process(128);
      const auto start = std::chrono::steady_clock::now();
      for (int b = 0; b < 4000; ++b) f.process(128);
      const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
      std::cout << (type ? "Analog" : "Wavetable") << " waveform " << waveform << " shape " << shape
                << " 16 unison, 2 voices: " << ms / 4000.0 * 1000.0 << " us per 128 samples\n";
      if (type == O::kWavetableType) break;
    }
    return 0;
  }
}
