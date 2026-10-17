// SPDX-License-Identifier: GPL-3.0-or-later
#include "speech_source.h"

#include "futils.h"
#include "synth_constants.h"

#include <algorithm>
#include <cmath>

namespace vital {
  namespace {
    constexpr int kHannSize = 1024;

    // A Hann window over [0, 1], kHannSize + 2 points for interpolation.
    const float* hannTable() {
      static const std::array<float, kHannSize + 2> table = [] {
        std::array<float, kHannSize + 2> values {};
        for (int i = 0; i <= kHannSize; ++i)
          values[static_cast<size_t>(i)] = 0.5f - 0.5f * std::cos(6.28318530718f * i / kHannSize);
        values[kHannSize + 1] = 0.0f;
        return values;
      }();
      return table.data();
    }

    force_inline float hann(const float* table, double phase) {
      const double scaled = std::clamp(phase, 0.0, 1.0) * kHannSize;
      const int index = static_cast<int>(scaled);
      const float t = static_cast<float>(scaled - index);
      return table[index] + t * (table[index + 1] - table[index]);
    }

    // Four-point Hermite interpolation, silent outside the audio.
    force_inline float readAudio(const float* audio, int length, double position) {
      const double floor_position = std::floor(position);
      const int index = static_cast<int>(floor_position);
      const float t = static_cast<float>(position - floor_position);
      float y0, y1, y2, y3;
      if (index >= 1 && index + 2 < length) {
        y0 = audio[index - 1];
        y1 = audio[index];
        y2 = audio[index + 1];
        y3 = audio[index + 2];
      }
      else {
        if (index + 2 < 0 || index - 1 >= length)
          return 0.0f;
        auto at = [&](int i) { return i >= 0 && i < length ? audio[i] : 0.0f; };
        y0 = at(index - 1);
        y1 = at(index);
        y2 = at(index + 1);
        y3 = at(index + 2);
      }
      const float c1 = 0.5f * (y2 - y0);
      const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
      const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
      return ((c3 * t + c2) * t + c1) * t + y1;
    }

    // The mark nearest `position`.
    const speech::Mark& nearestMark(const std::vector<speech::Mark>& marks, double position) {
      auto after = std::lower_bound(marks.begin(), marks.end(), position,
                                    [](const speech::Mark& mark, double value) { return mark.position < value; });
      if (after == marks.end())
        return marks.back();
      if (after == marks.begin())
        return *after;
      auto before = after - 1;
      return position - before->position <= after->position - position ? *before : *after;
    }
  } // namespace

  SpeechSource::SpeechSource(std::shared_ptr<SpeechSlot> slot) :
      Processor(kNumInputs, kNumOutputs), slot_(std::move(slot)) {
    hannTable();
    for (int voice = 0; voice < kVoicesPerProcessor; ++voice)
      voices_[static_cast<size_t>(voice)].random = 0x9e3779b9u * static_cast<uint32_t>(voice + 1);
  }

  float SpeechSource::nextRandom(VoiceState& state) {
    // xorshift32, -1 to 1.
    uint32_t x = state.random;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    state.random = x;
    return static_cast<float>(x) * (2.0f / 4294967295.0f) - 1.0f;
  }

  void SpeechSource::startSyllable(VoiceState& state, uint32_t generation, int syllable, int layer) {
    // Grains of the previous syllable ring out; only new grains change.
    state.generation = generation;
    state.syllable = syllable;
    state.layer = layer;
    state.position = 0.0;
    state.held = true;
    state.holding = false;
    state.singing = syllable >= 0;
    state.steady = 0.0f;
    state.countdown = 0.0;
  }

  void SpeechSource::launchGrain(VoiceState& state, const speech::Syllable& syllable, double ratio,
                                 double note_period) {
    const speech::Mark& mark = nearestMark(syllable.marks, state.position);
    double half_width = mark.half_width;
    if (mark.voiced)
      half_width = std::min(half_width, 0.5 * kMaxGrainPeriods * note_period * ratio);
    half_width = std::max(half_width, 2.0);

    if (state.num_grains >= kMaxGrains) {
      // Replace the grain closest to its end.
      int oldest = 0;
      for (int i = 1; i < state.num_grains; ++i) {
        const Grain& grain = state.grains[static_cast<size_t>(i)];
        const Grain& current = state.grains[static_cast<size_t>(oldest)];
        if (grain.duration - grain.time < current.duration - current.time)
          oldest = i;
      }
      state.grains[static_cast<size_t>(oldest)] = state.grains[static_cast<size_t>(--state.num_grains)];
    }
    Grain& grain = state.grains[static_cast<size_t>(state.num_grains++)];
    grain.generation = state.generation;
    grain.step = ratio;
    // The countdown overshot by a fraction of a sample: the grain began then.
    grain.time = -state.countdown;
    grain.duration = 2.0 * half_width / ratio;

    const bool steady = state.steady > 0.0f && !syllable.steady_grain.empty();
    grain.audio = syllable.audio.data();
    grain.length = syllable.length();
    grain.start = mark.position - half_width;
    grain.gain = steady ? 1.0f - state.steady : 1.0f;
    if (steady) {
      const double steady_half = 0.5 * syllable.steady_grain.size();
      grain.steady = syllable.steady_grain.data();
      grain.steady_length = static_cast<int>(syllable.steady_grain.size());
      grain.steady_start = steady_half - half_width;
      grain.steady_gain = state.steady * (1.0f + kShimmer * nextRandom(state));
      grain.steady_windowed = half_width < steady_half;
    }
    else {
      grain.steady = nullptr;
      grain.steady_gain = 0.0f;
    }

    // Voiced grains repeat at the note's period, a little uneven in the
    // steady vowel; unvoiced ones keep the recording's spacing, so noise
    // plays as it was spoken.
    if (mark.voiced)
      state.countdown += note_period * (1.0 + kJitter * state.steady * nextRandom(state));
    else
      state.countdown += mark.half_width / ratio;
  }

  float SpeechSource::renderGrains(VoiceState& state) {
    const float* window = hannTable();
    float sum = 0.0f;
    for (int i = 0; i < state.num_grains;) {
      Grain& grain = state.grains[static_cast<size_t>(i)];
      if (grain.time >= grain.duration) {
        grain = state.grains[static_cast<size_t>(--state.num_grains)];
        continue;
      }
      if (grain.time >= 0.0) {
        const float gain = hann(window, grain.time / grain.duration);
        const double offset = grain.time * grain.step;
        if (grain.gain > 0.0f)
          sum += grain.gain * gain * readAudio(grain.audio, grain.length, grain.start + offset);
        if (grain.steady != nullptr) {
          const float steady = readAudio(grain.steady, grain.steady_length, grain.steady_start + offset);
          sum += grain.steady_gain * (grain.steady_windowed ? gain * steady : steady);
        }
      }
      grain.time += 1.0;
      ++i;
    }
    return sum;
  }

  void SpeechSource::advance(VoiceState& state, const speech::Syllable& syllable, double ratio, float steady_in,
                             float steady_out) {
    if (state.held && syllable.hold_point >= 0) {
      if (!state.holding && state.position >= syllable.hold_point) {
        state.holding = true;
        state.position = syllable.hold_point;
      }
      if (state.holding) {
        if (!syllable.steady_grain.empty())
          state.steady = std::min(1.0f, state.steady + steady_in);
        return;
      }
    }
    state.steady = std::max(0.0f, state.steady - steady_out);
    state.position += ratio;
    if (state.position >= syllable.length())
      state.singing = false;
  }

  void SpeechSource::process(int num_samples) {
    if (num_samples <= 0)
      return;

    const float sample_rate = getSampleRate();
    const poly_float note_input = input(kMidi)->at(0);
    const poly_mask reset_mask = getResetMask(kReset);
    const Output* events = input(kVoiceEvent)->source;
    const bool own_envelope = static_cast<int>(input(kAmpEnvMode)->at(0)[0] + 0.5f) == kVoiceEnvelope;
    const float steady_in = 1.0f / std::max(1.0f, kSteadyInSeconds * sample_rate);
    const float steady_out = 1.0f / std::max(1.0f, kSteadyOutSeconds * sample_rate);

    poly_float* raw = output(kRaw)->buffer;
    poly_float hold = 0.0f;
    const int count = std::min(num_samples, kMaxBufferSize * kMaxOversample);
    const float inverse_count = 1.0f / count;
    SpeechSlot& slot = *slot_;

    for (int voice = 0; voice < kVoicesPerProcessor; ++voice) {
      const int lane = 2 * voice;
      VoiceState& state = voices_[static_cast<size_t>(voice)];
      if (reset_mask[lane]) {
        state.num_grains = 0;
        state.syllable = -1;
        state.singing = false;
        state.held = false;
      }

      int start_at = -1;
      int release_at = -1;
      if (events->trigger_mask[lane]) {
        const int event = static_cast<int>(events->trigger_value[lane]);
        const int offset = std::max(0, std::min(count - 1, static_cast<int>(events->trigger_offset[lane])));
        if (event == kVoiceOn)
          start_at = offset;
        else if (event == kVoiceOff || event == kVoiceKill)
          release_at = offset;
      }

      // A phrase freed since this note started takes its grains with it,
      // as do grains still ringing from a phrase before that.
      const speech::Phrase* phrase = slot.phraseFor(state.generation);
      if (phrase == nullptr)
        state.singing = false;
      for (int g = 0; g < state.num_grains;) {
        if (slot.phraseFor(state.grains[static_cast<size_t>(g)].generation) == nullptr)
          state.grains[static_cast<size_t>(g)] = state.grains[static_cast<size_t>(--state.num_grains)];
        else
          ++g;
      }

      const float frequency = utils::clamp(utils::midiNoteToFrequency(note_input[lane]), 8.0f, 8000.0f);
      const double note_period = sample_rate / frequency;
      double ratio = phrase != nullptr ? phrase->sample_rate / sample_rate : 1.0;
      auto current_syllable = [&]() -> const speech::Syllable* {
        if (!state.singing || phrase == nullptr || state.layer < 0 ||
            state.layer >= static_cast<int>(phrase->layers.size()))
          return nullptr;
        const speech::Layer& layer = phrase->layers[static_cast<size_t>(state.layer)];
        if (state.syllable < 0 || state.syllable >= static_cast<int>(layer.syllables.size()))
          return nullptr;
        return &layer.syllables[static_cast<size_t>(state.syllable)];
      };
      const speech::Syllable* syllable = current_syllable();

      for (int i = 0; i < count; ++i) {
        if (i == start_at) {
          const int index = slot.takeSyllable(i / static_cast<double>(sample_rate));
          const speech::Phrase* next = slot.phraseFor(slot.generation());
          startSyllable(state, slot.generation(), index, next != nullptr ? next->layerFor(frequency) : 0);
          phrase = next;
          ratio = phrase != nullptr ? phrase->sample_rate / sample_rate : 1.0;
          if (phrase == nullptr)
            state.singing = false;
          syllable = current_syllable();
        }
        if (i == release_at && state.held) {
          state.held = false;
          state.holding = false;
        }

        if (syllable != nullptr && state.singing) {
          while (state.countdown <= 0.0 && !syllable->marks.empty())
            launchGrain(state, *syllable, ratio, note_period);
          advance(state, *syllable, ratio, steady_in, steady_out);
          state.countdown -= 1.0;
        }

        const float value = state.num_grains > 0 ? renderGrains(state) : 0.0f;
        raw[i].set(lane, value);
        raw[i].set(lane + 1, value);
      }

      // The syllable holds the voice open until it has finished.
      const bool sounding = state.held || state.singing || state.num_grains > 0;
      if (own_envelope && sounding) {
        hold.set(lane, 1.0f);
        hold.set(lane + 1, 1.0f);
      }
    }
    poly_float* hold_buffer = output(kHold)->buffer;
    for (int i = 0; i < count; ++i)
      hold_buffer[i] = hold;

    // Slot level and pan, as the Sample type applies them.
    const poly_float target_pan = futils::panAmplitude(utils::clamp(input(kPan)->at(0), -1.0f, 1.0f));
    if (!pan_initialized_) {
      pan_amplitude_ = target_pan;
      pan_initialized_ = true;
    }
    poly_float current_pan = utils::maskLoad(pan_amplitude_, target_pan, reset_mask);
    const poly_float delta_pan = (target_pan - current_pan) * inverse_count;
    pan_amplitude_ = target_pan;

    const Output* level_source = input(kLevel)->source;
    const bool audio_rate_level = level_source->buffer_size > 1;
    poly_float* levelled = output(kLevelled)->buffer;
    for (int i = 0; i < count; ++i) {
      current_pan += delta_pan;
      const poly_float level = utils::clamp(level_source->buffer[audio_rate_level ? i : 0], 0.0f, kMaxAmplitude);
      levelled[i] = current_pan * level * level * raw[i];
    }
  }
} // namespace vital
