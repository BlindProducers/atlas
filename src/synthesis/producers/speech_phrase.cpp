// SPDX-License-Identifier: GPL-3.0-or-later
#include "speech_phrase.h"

#include "JuceHeader.h"

#include <algorithm>
#include <cmath>

namespace vital {
namespace speech {
  namespace {
    // Pitch range searched for the voice's monotone, in Hz.
    constexpr double kLowestPitch = 45.0;
    constexpr double kHighestPitch = 500.0;
    // A frame is voiced when it repeats this well at its pitch period; in a
    // vowel, which the phonemes say is voiced, a weaker repeat will do.
    constexpr float kVoicedCorrelation = 0.5f;
    constexpr float kVowelCorrelation = 0.2f;
    // ...and is louder than this fraction of the syllable's peak.
    constexpr float kVoicedLevel = 0.02f;
    // The shortest lag within this fraction of the best correlation wins,
    // so a period is never mistaken for two.
    constexpr float kOctaveTolerance = 0.85f;

    // Normalized autocorrelation of audio[start, start + window) at `lag`.
    float correlation(const float* audio, int length, int start, int window, int lag) {
      const int end = std::min(start + window, length - lag);
      if (start < 0 || end - start < lag / 2 + 8)
        return 0.0f;
      double cross = 0.0;
      double first = 0.0;
      double second = 0.0;
      for (int i = start; i < end; ++i) {
        const double a = audio[i];
        const double b = audio[i + lag];
        cross += a * b;
        first += a * a;
        second += b * b;
      }
      const double energy = std::sqrt(first * second);
      return energy > 1.0e-12 ? static_cast<float>(cross / energy) : 0.0f;
    }

    struct Frame {
      bool voiced = false;
      int period = 0;
    };

    // The best period of the frame around `centre`, within [min_lag, max_lag].
    Frame analyseFrame(const float* audio, int length, int centre, int min_lag, int max_lag, float level,
                       float threshold) {
      Frame frame;
      const int window = max_lag + max_lag / 2;
      const int start = std::max(0, centre - window / 2);
      double energy = 0.0;
      const int end = std::min(length, start + window);
      for (int i = start; i < end; ++i)
        energy += static_cast<double>(audio[i]) * audio[i];
      if (end <= start || std::sqrt(energy / (end - start)) < level)
        return frame;
      float best = 0.0f;
      int best_lag = 0;
      for (int lag = min_lag; lag <= max_lag; ++lag) {
        const float value = correlation(audio, length, start, window, lag);
        if (value > best) {
          best = value;
          best_lag = lag;
        }
      }
      frame.voiced = best >= threshold;
      frame.period = best_lag;
      return frame;
    }

    void placeMarks(Syllable& syllable, float period, int vowel_start, int vowel_end) {
      syllable.marks.clear();
      const int length = syllable.length();
      if (length <= 0)
        return;
      const float* audio = syllable.audio.data();
      const int base = std::max(8, static_cast<int>(std::lround(period)));
      const int min_lag = std::max(4, static_cast<int>(base * 0.8f));
      const int max_lag = std::max(min_lag + 1, static_cast<int>(base * 1.25f));

      float peak = 0.0f;
      float positive = 0.0f;
      float negative = 0.0f;
      for (float sample : syllable.audio) {
        peak = std::max(peak, std::abs(sample));
        positive = std::max(positive, sample);
        negative = std::max(negative, -sample);
      }
      // Marks sit on the larger of each period's peaks.
      const float polarity = positive >= negative ? 1.0f : -1.0f;
      const float level = peak * kVoicedLevel;

      // Voicing and period on a grid of half periods.
      const int hop = std::max(4, base / 2);
      std::vector<Frame> frames;
      frames.reserve(static_cast<size_t>(length / hop + 2));
      for (int centre = 0; centre < length + hop; centre += hop) {
        const bool vowel = centre >= vowel_start && centre < vowel_end;
        frames.push_back(analyseFrame(audio, length, centre, min_lag, max_lag, level,
                                      vowel ? kVowelCorrelation : kVoicedCorrelation));
      }
      auto frame_at = [&](int position) -> const Frame& {
        const int index = std::clamp((position + hop / 2) / hop, 0, static_cast<int>(frames.size()) - 1);
        return frames[static_cast<size_t>(index)];
      };
      auto strongest = [&](int from, int to) {
        from = std::clamp(from, 0, length - 1);
        to = std::clamp(to, from + 1, length);
        int best = from;
        for (int i = from; i < to; ++i) {
          if (polarity * audio[i] > polarity * audio[best])
            best = i;
        }
        return best;
      };

      int position = 0;
      bool previous_voiced = false;
      int last_mark = -base;
      while (position < length) {
        const Frame& frame = frame_at(position);
        if (frame.voiced) {
          const int local = frame.period > 0 ? frame.period : base;
          int mark = previous_voiced ? strongest(position - local / 5, position + local / 5 + 1)
                                     : strongest(position, position + local);
          // Never two marks closer than half a period.
          mark = std::max(mark, last_mark + local / 2);
          if (mark >= length)
            break;
          syllable.marks.push_back({ mark, local, true });
          last_mark = mark;
          position = mark + local;
          previous_voiced = true;
        }
        else {
          const int mark = std::max(position, last_mark + base / 2);
          syllable.marks.push_back({ mark, base, false });
          last_mark = mark;
          position = mark + base;
          previous_voiced = false;
        }
      }
      if (syllable.marks.empty())
        syllable.marks.push_back({ 0, base, false });
    }
  } // namespace

  float estimatePeriod(const float* audio, int length, double sample_rate) {
    const int min_lag = std::max(2, static_cast<int>(sample_rate / kHighestPitch));
    const int max_lag = static_cast<int>(sample_rate / kLowestPitch);
    const int window = 2 * max_lag;
    if (audio == nullptr || length < window + max_lag)
      return 0.0f;
    float peak = 0.0f;
    for (int i = 0; i < length; ++i)
      peak = std::max(peak, std::abs(audio[i]));
    if (peak <= 0.0f)
      return 0.0f;

    // Average the correlation of every loud frame, lag by lag.
    std::vector<double> sums(static_cast<size_t>(max_lag + 2), 0.0);
    int frames = 0;
    for (int start = 0; start + window + max_lag <= length; start += window / 2) {
      double energy = 0.0;
      for (int i = start; i < start + window; ++i)
        energy += static_cast<double>(audio[i]) * audio[i];
      if (std::sqrt(energy / window) < peak * 0.1f)
        continue;
      for (int lag = min_lag; lag <= max_lag + 1; ++lag)
        sums[static_cast<size_t>(lag)] += correlation(audio, length, start, window, lag);
      ++frames;
    }
    if (frames == 0)
      return 0.0f;
    double best = 0.0;
    for (int lag = min_lag; lag <= max_lag; ++lag)
      best = std::max(best, sums[static_cast<size_t>(lag)]);
    if (best <= 0.0)
      return 0.0f;
    for (int lag = min_lag + 1; lag <= max_lag; ++lag) {
      const double value = sums[static_cast<size_t>(lag)];
      const bool local_peak = value >= sums[static_cast<size_t>(lag - 1)] && value >= sums[static_cast<size_t>(lag + 1)];
      if (!local_peak || value < best * kOctaveTolerance)
        continue;
      // Parabolic refinement between the neighbouring lags.
      const double left = sums[static_cast<size_t>(lag - 1)];
      const double right = sums[static_cast<size_t>(lag + 1)];
      const double curvature = left - 2.0 * value + right;
      const double offset = curvature < 0.0 ? 0.5 * (left - right) / curvature : 0.0;
      return static_cast<float>(lag + std::clamp(offset, -0.5, 0.5));
    }
    return 0.0f;
  }

  namespace {
    // The vowel as one steady grain: the average magnitude spectrum of its
    // voiced periods with the phase of the grain at the hold point, so the
    // held sound keeps the voice's own pulse shape without the drift of its
    // formants from period to period.
    std::vector<float> buildSteadyGrain(const Syllable& syllable, float period, int from, int to) {
      const int half = std::max(8, static_cast<int>(std::lround(period)));
      const int length = 2 * half;
      int order = 1;
      while ((1 << order) < 2 * length)
        ++order;
      const int size = 1 << order;
      juce::dsp::FFT fft(order);

      const Mark* centre = nullptr;
      for (const Mark& mark : syllable.marks) {
        if (mark.voiced && mark.position == syllable.hold_point)
          centre = &mark;
      }
      if (centre == nullptr)
        return {};

      auto frame = [&](const Mark& mark, std::vector<float>& data) {
        std::fill(data.begin(), data.end(), 0.0f);
        for (int n = 0; n < length; ++n) {
          const int index = mark.position - half + n;
          if (index >= 0 && index < syllable.length()) {
            const float window = 0.5f - 0.5f * std::cos(6.28318530718f * n / length);
            data[static_cast<size_t>(n)] = syllable.audio[static_cast<size_t>(index)] * window;
          }
        }
        fft.performRealOnlyForwardTransform(data.data(), true);
      };

      const int bins = size / 2 + 1;
      std::vector<float> data(static_cast<size_t>(2 * size));
      std::vector<double> magnitude(static_cast<size_t>(bins), 0.0);
      int count = 0;
      for (const Mark& mark : syllable.marks) {
        if (!mark.voiced || mark.position < from || mark.position > to)
          continue;
        frame(mark, data);
        for (int k = 0; k < bins; ++k)
          magnitude[static_cast<size_t>(k)] += std::hypot(data[static_cast<size_t>(2 * k)], data[static_cast<size_t>(2 * k + 1)]);
        ++count;
      }
      if (count == 0)
        return {};

      // The centre grain's phase with the average magnitude.
      frame(*centre, data);
      for (int k = 0; k < bins; ++k) {
        const float re = data[static_cast<size_t>(2 * k)];
        const float im = data[static_cast<size_t>(2 * k + 1)];
        const float current = std::hypot(re, im);
        const float target = static_cast<float>(magnitude[static_cast<size_t>(k)] / count);
        const float scale = current > 1.0e-12f ? target / current : 0.0f;
        data[static_cast<size_t>(2 * k)] = re * scale;
        data[static_cast<size_t>(2 * k + 1)] = im * scale;
      }
      fft.performRealOnlyInverseTransform(data.data());

      std::vector<float> grain(static_cast<size_t>(length));
      double energy = 0.0;
      for (int n = 0; n < length; ++n) {
        const float window = 0.5f - 0.5f * std::cos(6.28318530718f * n / length);
        grain[static_cast<size_t>(n)] = data[static_cast<size_t>(n)] * window;
        energy += static_cast<double>(grain[static_cast<size_t>(n)]) * grain[static_cast<size_t>(n)];
      }
      // As loud as the grain it replaces.
      double frame_energy = 0.0;
      for (int n = 0; n < length; ++n) {
        const int index = centre->position - half + n;
        if (index >= 0 && index < syllable.length()) {
          const float window = 0.5f - 0.5f * std::cos(6.28318530718f * n / length);
          const double value = syllable.audio[static_cast<size_t>(index)] * window;
          frame_energy += value * value;
        }
      }
      if (energy <= 1.0e-20)
        return {};
      const float gain = static_cast<float>(std::sqrt(frame_energy / energy));
      for (float& sample : grain)
        sample *= gain;
      return grain;
    }
  } // namespace

  int Phrase::layerFor(float frequency) const {
    int best = 0;
    float best_distance = 1.0e30f;
    for (int i = 0; i < static_cast<int>(layers.size()); ++i) {
      const float layer_frequency = layers[static_cast<size_t>(i)].frequency;
      if (layer_frequency <= 0.0f || frequency <= 0.0f)
        continue;
      const float distance = std::abs(std::log(frequency / layer_frequency));
      if (distance < best_distance) {
        best_distance = distance;
        best = i;
      }
    }
    return best;
  }

  const Layer* Phrase::referenceLayer() const {
    if (layers.empty())
      return nullptr;
    return &layers[static_cast<size_t>(std::clamp(reference_layer, 0, static_cast<int>(layers.size()) - 1))];
  }

  void analyseLayer(Layer& layer, double sample_rate, const std::vector<int32_t>& nucleus_start,
                    const std::vector<int32_t>& nucleus_end) {
    // espeak-ng speaks in a monotone, so one period serves the whole layer:
    // measure it over the vowels.
    std::vector<float> vowels;
    for (size_t i = 0; i < layer.syllables.size(); ++i) {
      const Syllable& syllable = layer.syllables[i];
      const int start = i < nucleus_start.size() ? nucleus_start[i] : -1;
      const int end = i < nucleus_end.size() ? nucleus_end[i] : -1;
      if (start < 0 || end <= start)
        continue;
      const int from = std::clamp(start, 0, syllable.length());
      const int to = std::clamp(end, from, syllable.length());
      vowels.insert(vowels.end(), syllable.audio.begin() + from, syllable.audio.begin() + to);
    }
    layer.period = estimatePeriod(vowels.data(), static_cast<int>(vowels.size()), sample_rate);
    if (layer.period <= 0.0f)
      layer.period = static_cast<float>(sample_rate / 120.0);
    layer.frequency = static_cast<float>(sample_rate / layer.period);

    for (size_t i = 0; i < layer.syllables.size(); ++i) {
      Syllable& syllable = layer.syllables[i];
      const int start = i < nucleus_start.size() ? nucleus_start[i] : -1;
      const int end = i < nucleus_end.size() ? nucleus_end[i] : -1;
      placeMarks(syllable, layer.period, start, end);
      syllable.hold_point = -1;
      syllable.steady_grain.clear();
      if (start < 0 || end <= start)
        continue;
      // Hold past the vowel's onset, in its first half, so a diphthong holds
      // its first vowel and glides on release. The hold point sits on the
      // voiced mark nearest there.
      const float length = static_cast<float>(end - start);
      const int target = start + static_cast<int>(0.35f * length);
      int best = -1;
      for (const Mark& mark : syllable.marks) {
        if (mark.voiced && mark.position >= start && mark.position < end &&
            (best < 0 || std::abs(mark.position - target) < std::abs(best - target)))
          best = mark.position;
      }
      syllable.hold_point = std::clamp(best >= 0 ? best : target, 0, std::max(0, syllable.length() - 1));
      // The steady grain averages the vowel's middle, away from the
      // consonants on either side.
      syllable.steady_grain = buildSteadyGrain(syllable, layer.period, start + static_cast<int>(0.15f * length),
                                               start + static_cast<int>(0.65f * length));
    }
  }
} // namespace speech
} // namespace vital
