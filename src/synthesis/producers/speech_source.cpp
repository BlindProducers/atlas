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

  void SpeechSource::followPitch(VoiceState& state, const speech::Phrase& phrase, float frequency) {
    const int count = static_cast<int>(phrase.layers.size());
    if (count < 2 || state.layer < 0 || state.layer >= count || frequency <= 0.0f)
      return;
    const int best = phrase.layerFor(frequency);
    if (best == state.layer)
      return;
    auto distance = [&](int layer) {
      return std::abs(std::log(frequency / phrase.layers[static_cast<size_t>(layer)].frequency));
    };
    // Half a semitone of hysteresis, so a bend or vibrato near the middle
    // of two layers does not flip between them.
    constexpr float kHysteresis = 0.5f * 0.0577622650f;
    if (distance(state.layer) - distance(best) < kHysteresis)
      return;
    const speech::Layer& from = phrase.layers[static_cast<size_t>(state.layer)];
    const speech::Layer& to = phrase.layers[static_cast<size_t>(best)];
    if (state.syllable < 0 || state.syllable >= static_cast<int>(from.syllables.size()) ||
        state.syllable >= static_cast<int>(to.syllables.size()))
      return;
    const speech::Syllable& old_syllable = from.syllables[static_cast<size_t>(state.syllable)];
    const speech::Syllable& new_syllable = to.syllables[static_cast<size_t>(state.syllable)];
    // The same place in the syllable: the hold point while holding, else the
    // same fraction of the way through. Ringing grains finish where they are.
    if (state.holding && new_syllable.hold_point >= 0)
      state.position = new_syllable.hold_point;
    else if (old_syllable.length() > 0)
      state.position *= static_cast<double>(new_syllable.length()) / old_syllable.length();
    state.layer = best;
  }

  void SpeechSource::startSyllable(VoiceState& state, uint32_t generation, int syllable, int last_syllable,
                                   int layer) {
    // Grains of the previous syllable ring out; only new grains change.
    state.generation = generation;
    state.syllable = syllable;
    state.last_syllable = std::max(syllable, last_syllable);
    state.layer = layer;
    state.position = 0.0;
    state.held = true;
    state.holding = false;
    state.singing = syllable >= 0;
    state.kill_remaining = 0;
    state.steady = 0.0f;
    state.countdown = 0.0;
    state.vibrato_time = 0.0;
    state.vibrato_phase = 0.0;
  }

  void SpeechSource::launchGrain(VoiceState& state, const speech::Syllable& syllable,
                                 const GrainSettings& settings) {
    const speech::Mark& mark = nearestMark(syllable.marks, state.position);
    const double step = settings.ratio * settings.formant;
    // Output samples to the next grain. Voiced grains repeat at the note's
    // period (Sung), a little uneven in the steady vowel, or at the
    // recording's period over the note's interval (Spoken); unvoiced ones
    // overlap by half, so noise plays as it was spoken.
    double spacing = 0.0;
    double half_width = mark.half_width;
    if (mark.voiced) {
      spacing = settings.spoken ? mark.half_width / (settings.ratio * settings.pitch_factor) : settings.note_period;
      half_width = std::min(half_width, 0.5 * kMaxGrainPeriods * spacing * step);
    }
    half_width = std::max(half_width, 2.0);
    if (!mark.voiced)
      spacing = half_width / step;

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
    grain.step = step;
    // The countdown overshot by a fraction of a sample: the grain began then.
    grain.time = -state.countdown;
    grain.duration = 2.0 * half_width / step;

    // Voiced grains sit on their mark. Unvoiced ones follow the read
    // position, spread a little when it moves slowly so noise never repeats.
    double centre = mark.position;
    if (!mark.voiced) {
      const double spread = std::max(0.0, 1.0 - settings.speed) * half_width;
      centre = state.position + spread * nextRandom(state);
    }

    const bool steady = state.steady > 0.0f && !syllable.steady_grain.empty();
    grain.audio = syllable.audio.data();
    grain.length = syllable.length();
    grain.start = centre - half_width;
    grain.gain = settings.voice_gain * (steady ? 1.0f - state.steady : 1.0f);
    if (steady) {
      const double steady_half = 0.5 * syllable.steady_grain.size();
      grain.steady = syllable.steady_grain.data();
      grain.steady_length = static_cast<int>(syllable.steady_grain.size());
      grain.steady_start = steady_half - half_width;
      grain.steady_gain = settings.voice_gain * state.steady * (1.0f + kShimmer * nextRandom(state));
      grain.steady_windowed = half_width < steady_half;
    }
    else {
      grain.steady = nullptr;
      grain.steady_gain = 0.0f;
    }

    if (settings.breath_gain > 0.0f && syllable.breath.size() == syllable.audio.size()) {
      // A voiced grain's whisper comes from near its mark, a held one's from
      // anywhere in the vowel's middle, so the noise never repeats. Grains
      // of noise from different places add in power, not amplitude: as loud
      // as the voiced grains, which add in step.
      double breath_centre = centre;
      float gain = settings.breath_gain;
      if (mark.voiced) {
        if (state.holding && syllable.breath_to > syllable.breath_from)
          breath_centre = syllable.breath_from + (0.5 + 0.5 * nextRandom(state)) *
                          (syllable.breath_to - syllable.breath_from);
        else
          breath_centre = centre + half_width * nextRandom(state);
        const double overlap = grain.duration / std::max(1.0, spacing);
        gain *= static_cast<float>(0.5 * overlap / std::sqrt(0.375 * overlap));
      }
      grain.breath = syllable.breath.data();
      grain.breath_start = breath_centre - half_width;
      grain.breath_gain = gain;
    }
    else {
      grain.breath = nullptr;
      grain.breath_gain = 0.0f;
    }

    if (mark.voiced && !settings.spoken)
      spacing *= 1.0 + kJitter * state.steady * nextRandom(state);
    state.countdown += spacing;
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
        if (grain.breath != nullptr)
          sum += grain.breath_gain * gain * readAudio(grain.breath, grain.length, grain.breath_start + offset);
      }
      grain.time += 1.0;
      ++i;
    }
    return sum;
  }

  void SpeechSource::advance(VoiceState& state, const speech::Syllable& syllable, double step, bool can_hold,
                             float steady_in, float steady_out) {
    if (can_hold && state.held && syllable.hold_point >= 0) {
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
    state.position += step;
    if (state.position >= syllable.length())
      state.singing = false;
  }

  void SpeechSource::process(int num_samples) {
    if (num_samples <= 0)
      return;

    const float sample_rate = getSampleRate();
    const poly_float note_input = input(kMidi)->at(0) + input(kTranspose)->at(0) + input(kTune)->at(0);
    const poly_mask reset_mask = getResetMask(kReset);
    const Output* events = input(kRetrigger)->source;
    const bool own_envelope = static_cast<int>(input(kAmpEnvMode)->at(0)[0] + 0.5f) == kVoiceEnvelope;
    const int split = utils::iclamp(static_cast<int>(input(kSplit)->at(0)[0] + 0.5f), 0, speech::kNumSplits - 1);
    const poly_float formant_input = utils::clamp(input(kFormant)->at(0), -24.0f, 24.0f);
    const poly_float breath_input = utils::clamp(input(kBreath)->at(0), 0.0f, 1.0f);
    const poly_float speed_input = utils::clamp(input(kSpeed)->at(0), kMinSpeed, 4.0f) * kBaseSpeed;
    const poly_float vibrato_input = utils::clamp(input(kVibrato)->at(0), 0.0f, 1.0f);
    const poly_float vibrato_rate_input = utils::clamp(input(kVibratoRate)->at(0), -2.0f, 4.0f);
    const float steady_in = 1.0f / std::max(1.0f, kSteadyInSeconds * sample_rate);
    const float steady_out = 1.0f / std::max(1.0f, kSteadyOutSeconds * sample_rate);
    const SlotModulation::Block modulation = modulation_.resolve(*this, kDistortionType, kDistortionAmount,
                                                                 kSecondaryDistortionType,
                                                                 kSecondaryDistortionAmount, true);
    const bool fm_active = modulation.anyFm();

    poly_float* raw = output(kRaw)->buffer;
    poly_float hold = 0.0f;
    const int count = std::min(num_samples, kMaxBufferSize * kMaxOversample);
    const float inverse_count = 1.0f / count;
    const double block_seconds = count / static_cast<double>(sample_rate);
    SpeechSlot& slot = *slot_;

    for (int voice = 0; voice < kVoicesPerProcessor; ++voice) {
      const int lane = 2 * voice;
      VoiceState& state = voices_[static_cast<size_t>(voice)];
      if (reset_mask[lane]) {
        state.num_grains = 0;
        state.syllable = -1;
        state.singing = false;
        state.held = false;
        state.kill_remaining = 0;
      }

      int start_at = -1;
      int release_at = -1;
      int kill_at = -1;
      if (events->trigger_mask[lane]) {
        const int event = static_cast<int>(events->trigger_value[lane]);
        const int offset = std::max(0, std::min(count - 1, static_cast<int>(events->trigger_offset[lane])));
        if (event == kVoiceOn)
          start_at = offset;
        else if (event == kVoiceOff)
          release_at = offset;
        else if (event == kVoiceKill)
          release_at = kill_at = offset;
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

      const float note = note_input[lane];
      // The pitch a note sings (Sung) or the pitch of the voice's own speech
      // moved by the note's interval from middle C (Spoken): which layer
      // serves it, without vibrato.
      auto target_frequency = [note](const speech::Phrase* target) {
        if (target != nullptr && target->spoken) {
          const speech::Layer* reference = target->referenceLayer();
          const float own = reference != nullptr && reference->frequency > 0.0f ? reference->frequency : 120.0f;
          return utils::clamp(own * std::exp2((note - kSpokenRootNote) * (1.0f / kNotesPerOctave)), 8.0f, 8000.0f);
        }
        return utils::clamp(utils::midiNoteToFrequency(note), 8.0f, 8000.0f);
      };

      GrainSettings settings;
      settings.formant = std::exp2(formant_input[lane] * (1.0f / kNotesPerOctave));
      settings.speed = speed_input[lane];
      const float breath = breath_input[lane];
      settings.voice_gain = std::cos(breath * 1.57079632679f);
      settings.breath_gain = std::sin(breath * 1.57079632679f);
      const float vibrato_rate = std::exp2(vibrato_rate_input[lane]);
      const float vibrato_depth = vibrato_input[lane];

      // Sets the pitch of the grains for `target`, the vibrato on top.
      auto set_pitch = [&](const speech::Phrase* target) {
        settings.ratio = target != nullptr ? target->sample_rate / sample_rate : 1.0;
        settings.spoken = target != nullptr && target->spoken;
        const float fade = utils::clamp(static_cast<float>((state.vibrato_time - kVibratoDelaySeconds) /
                                                           kVibratoFadeSeconds), 0.0f, 1.0f);
        const float vibrato = vibrato_depth * fade *
                              std::sin(static_cast<float>(6.28318530718 * state.vibrato_phase));
        const float frequency = target_frequency(target) * std::exp2(vibrato * (1.0f / kNotesPerOctave));
        settings.note_period = sample_rate / frequency;
        settings.pitch_factor = 1.0;
        if (target != nullptr && state.layer >= 0 && state.layer < static_cast<int>(target->layers.size())) {
          const float layer_frequency = target->layers[static_cast<size_t>(state.layer)].frequency;
          if (layer_frequency > 0.0f)
            settings.pitch_factor = frequency / layer_frequency;
        }
      };

      auto current_syllable = [&]() -> const speech::Syllable* {
        if (!state.singing || phrase == nullptr || state.layer < 0 ||
            state.layer >= static_cast<int>(phrase->layers.size()))
          return nullptr;
        const speech::Layer& layer = phrase->layers[static_cast<size_t>(state.layer)];
        if (state.syllable < 0 || state.syllable >= static_cast<int>(layer.syllables.size()))
          return nullptr;
        return &layer.syllables[static_cast<size_t>(state.syllable)];
      };
      if (phrase != nullptr && state.singing)
        followPitch(state, *phrase, target_frequency(phrase));
      set_pitch(phrase);
      const speech::Syllable* syllable = current_syllable();

      for (int i = 0; i < count; ++i) {
        if (i == start_at) {
          const int unit = slot.takeUnit(i / static_cast<double>(sample_rate), split, i);
          const speech::Phrase* next = slot.phraseFor(slot.generation());
          int first = -1;
          int last = -1;
          if (next == nullptr || !next->unitRange(split, unit, first, last))
            first = last = -1;
          startSyllable(state, slot.generation(), first, last,
                        next != nullptr ? next->layerFor(target_frequency(next)) : 0);
          phrase = next;
          if (phrase == nullptr)
            state.singing = false;
          set_pitch(phrase);
          syllable = current_syllable();
        }
        if (i == release_at && state.held) {
          state.held = false;
          state.holding = false;
        }
        if (i == kill_at && state.kill_remaining == 0) {
          state.kill_length = std::max(1, static_cast<int>(kVoiceKillTime * sample_rate));
          state.kill_remaining = state.kill_length;
        }

        if (syllable != nullptr && state.singing) {
          while (state.countdown <= 0.0 && !syllable->marks.empty())
            launchGrain(state, *syllable, settings);
          // Only the note's last syllable holds, and only when sung.
          const bool can_hold = !settings.spoken && state.syllable >= state.last_syllable;
          advance(state, *syllable, settings.ratio * settings.speed, can_hold, steady_in, steady_out);
          // FM speeds up or holds back the next grain; it never runs backwards.
          state.countdown -= fm_active ? std::max(0.0f, modulation.rateScale(i)[lane]) : 1.0;
          if (!state.singing && state.syllable < state.last_syllable) {
            // On to the next syllable of the word or phrase, where this one ended.
            const double past = state.position - syllable->length();
            state.syllable++;
            state.singing = true;
            syllable = current_syllable();
            state.position = syllable != nullptr ? std::max(0.0, past) : 0.0;
            if (syllable == nullptr)
              state.singing = false;
          }
        }

        float value = state.num_grains > 0 ? renderGrains(state) : 0.0f;
        if (state.kill_remaining > 0) {
          value *= static_cast<float>(state.kill_remaining) / state.kill_length;
          if (--state.kill_remaining == 0) {
            // Killed: silent from here, so the voice handler can free it.
            state.num_grains = 0;
            state.singing = false;
            syllable = nullptr;
          }
        }
        raw[i].set(lane, value);
        raw[i].set(lane + 1, value);
      }
      state.vibrato_time += block_seconds;
      state.vibrato_phase += vibrato_rate * block_seconds;
      state.vibrato_phase -= std::floor(state.vibrato_phase);

      // The syllable holds the voice open until it has finished.
      const bool sounding = state.held || state.singing || state.num_grains > 0;
      if (own_envelope && sounding) {
        hold.set(lane, 1.0f);
        hold.set(lane + 1, 1.0f);
      }
    }
    modulation.applyRingMod(raw, count);

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
