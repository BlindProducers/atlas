/* Copyright 2013-2019 Matt Tytel
 *
 * vital is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "random_lfo.h"

#include "futils.h"
#include "synth_constants.h"

#include <algorithm>
#include <cmath>

namespace {
  constexpr float kLorenzInitial1 = 0.0f;
  constexpr float kLorenzInitial2 = 0.0f;
  constexpr float kLorenzInitial3 = 37.6f;
  constexpr float kLorenzA = 10.0f;
  constexpr float kLorenzB = 28.0f;
  constexpr float kLorenzC = 8.0f / 3.0f;
  constexpr float kLorenzSize = 40.0f;
  constexpr float kLorenzScale = 1.0f / kLorenzSize;

  uint64_t splitMix64(uint64_t value) {
    value += 0x9e3779b97f4a7c15ull;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ull;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebull;
    return value ^ (value >> 31);
  }
}

namespace vital {
  uint64_t RandomLfo::next_stream_id_ = 0;

  namespace {
    force_inline poly_float randomLfoRateMultiplier(const poly_float& rate_x10) {
      return utils::maskLoad(poly_float(1.0f), poly_float(10.0f),
                             poly_float::greaterThan(rate_x10, 0.5f));
    }
  }

  RandomLfo::RandomLfo() : Processor(kNumInputs, 1 + kMaxModulationConnections),
      independent_output_active_(std::make_shared<std::array<bool, kMaxModulationConnections>>()),
      stream_seed_(splitMix64(0x41544c4153524e47ull + next_stream_id_++)),
      free_clocks_(std::make_shared<std::array<FreeClock, 1 + kMaxModulationConnections>>()),
      sync_seconds_(std::make_shared<double>(0.0)) {
    free_seed_ = stream_seed_;
    independent_output_active_->fill(false);
  }

  RandomLfo::RandomLfo(const RandomLfo& other) : Processor(other), state_(other.state_),
      independent_states_(other.independent_states_),
      independent_output_active_(other.independent_output_active_),
      stream_seed_(splitMix64(other.stream_seed_ + next_stream_id_++)),
      free_seed_(other.free_seed_), free_clocks_(other.free_clocks_),
      sync_seconds_(other.sync_seconds_), last_process_end_seconds_(other.last_process_end_seconds_),
      have_process_time_(other.have_process_time_) { }

  poly_float RandomLfo::inputAt(int input_index, int sample) const {
    const Output* source = input(input_index)->source;
    if (source->buffer_size > 1)
      return source->buffer[std::min(sample, source->buffer_size - 1)];
    return source->buffer[0];
  }

  int RandomLfo::syncMode() const {
    return utils::iclamp(static_cast<int>(std::round(input(kSync)->at(0)[0])), 0, kNumSyncModes - 1);
  }

  float RandomLfo::randomForCycle(int64_t cycle, int lane, bool mono, uint64_t stream_seed,
                                  bool shared_voices) const {
    // Voices sharing one stream share its left and right values too.
    const int stream = shared_voices ? (mono ? 0 : lane % 2) : (mono ? lane / 2 : lane);
    const uint64_t cycle_bits = static_cast<uint64_t>(cycle) ^ 0x8000000000000000ull;
    const uint64_t bits = splitMix64(stream_seed ^ splitMix64(cycle_bits) ^
                                     (0xd1b54a32d192ed03ull * static_cast<uint64_t>(stream + 1)));
    const uint32_t mantissa = static_cast<uint32_t>(bits >> 40);
    return static_cast<float>(mantissa) * (2.0f / 16777215.0f) - 1.0f;
  }

  void RandomLfo::resetLane(RandomState* state, int lane) {
    state->phase[lane] = 0.0;
    state->cycle[lane] = 0;
    state->offset.set(lane, 0.0f);
  }

  void RandomLfo::catchUpFreeClock(FreeClock* clock, RandomState* state, bool mono, uint64_t stream_seed) {
    if (!clock->started || clock->time != clock->now) {
      if (clock->started) {
        const double elapsed = static_cast<double>(clock->now - clock->time) / getSampleRate();
        const double phase = clock->phase + elapsed * clock->frequency;
        const double wraps = std::floor(phase);
        clock->cycle += static_cast<int64_t>(wraps);
        clock->phase = phase - wraps;
      }
      else {
        for (int lane = 0; lane < 2; ++lane) {
          const double perturbation = randomForCycle(0, lane, mono, stream_seed, true) * 0.01;
          clock->lorenz_end1[lane] = kLorenzInitial1 + perturbation;
          clock->lorenz_end2[lane] = kLorenzInitial2 - perturbation;
          clock->lorenz_end3[lane] = kLorenzInitial3 + perturbation;
        }
      }
      clock->lorenz_start1 = clock->lorenz_end1;
      clock->lorenz_start2 = clock->lorenz_end2;
      clock->lorenz_start3 = clock->lorenz_end3;
      clock->time = clock->now;
      clock->started = true;
    }

    for (int lane = 0; lane < poly_float::kSize; ++lane) {
      state->phase[lane] = clock->phase;
      state->cycle[lane] = clock->cycle;
      state->lorenz_state1[lane] = clock->lorenz_start1[lane % 2];
      state->lorenz_state2[lane] = clock->lorenz_start2[lane % 2];
      state->lorenz_state3[lane] = clock->lorenz_start3[lane % 2];
    }
    state->lorenz_initialized = true;
  }

  void RandomLfo::processRandom(RandomState* state, Output* destination, int num_samples, RandomType type,
                                uint64_t stream_seed, FreeClock* free_clock) {
    const int sync_mode = syncMode();
    const bool sync = sync_mode == kHostSync;
    const bool free_running = sync_mode == kFreeRunning;
    const bool mono = input(kStereo)->at(0)[0] < 0.5f;
    if (free_running)
      catchUpFreeClock(free_clock, state, mono, stream_seed);
    const poly_mask reset_mask = input(kReset)->source->trigger_mask;
    const poly_float reset_value = input(kReset)->source->trigger_value;
    const poly_int reset_offset = input(kReset)->source->trigger_offset;
    const double inverse_sample_rate = 1.0 / getSampleRate();
    poly_float last = 0.5f;

    for (int sample = 0; sample < num_samples; ++sample) {
      poly_float frequency = inputAt(kFrequency, sample) * randomLfoRateMultiplier(inputAt(kRateX10, sample));
      poly_float result = 0.5f;
      for (int lane = 0; lane < poly_float::kSize; ++lane) {
        const double hz = futils::isFinite(frequency[lane]) ?
            std::clamp(static_cast<double>(frequency[lane]), 0.0, getSampleRate() * 0.5) : 0.0;
        frequency.set(lane, static_cast<float>(hz));

        if (!sync && !free_running && reset_mask[lane] && reset_offset[lane] == static_cast<uint32_t>(sample) &&
            static_cast<int>(std::round(reset_value[lane])) == kVoiceOn) {
          resetLane(state, lane);
        }

        double phase = state->phase[lane];
        int64_t cycle = state->cycle[lane];
        if (sync) {
          const double absolute_cycles = (*sync_seconds_ + sample * inverse_sample_rate) * hz;
          const double whole_cycles = std::floor(absolute_cycles);
          cycle = static_cast<int64_t>(whole_cycles);
          phase = absolute_cycles - whole_cycles;
        }

        const float from = randomForCycle(cycle, lane, mono, stream_seed, free_running);
        const float to = randomForCycle(cycle + 1, lane, mono, stream_seed, free_running);
        float value = from;
        if (type == kPerlin)
          value = utils::perlinInterpolate(poly_float(from), poly_float(to), poly_float(phase))[0];
        else if (type == kSinInterpolate)
          value = futils::sinInterpolate(poly_float(from), poly_float(to), poly_float(phase))[0];
        result.set(lane, utils::clamp(value * 0.5f + 0.5f, 0.0f, 1.0f));

        if (!sync) {
          phase += hz * inverse_sample_rate;
          if (phase >= 1.0) {
            const int64_t wraps = static_cast<int64_t>(std::floor(phase));
            cycle += wraps;
            phase -= static_cast<double>(wraps);
          }
          state->phase[lane] = phase;
          state->cycle[lane] = cycle;
        }
        state->offset.set(lane, static_cast<float>(phase));
        state->last_random_value.set(lane, from);
        state->next_random_value.set(lane, to);
      }

      if (!isControlRate())
        destination->buffer[sample] = result;
      last = result;
      if (free_running)
        free_clock->frequency = frequency[0];
    }

    if (isControlRate())
      destination->buffer[0] = last;
    destination->trigger_value = last;
  }

  void RandomLfo::processLorenzAttractor(RandomState* state, Output* destination, int num_samples,
                                         uint64_t stream_seed, bool independent_stream, FreeClock* free_clock) {
    static constexpr float kMaxFrequency = 0.01f;
    const bool mono = input(kStereo)->at(0)[0] < 0.5f;
    const int sync_mode = syncMode();
    const bool sync = sync_mode == kHostSync;
    const bool free_running = sync_mode == kFreeRunning;
    if (free_running)
      catchUpFreeClock(free_clock, state, mono, stream_seed);
    const poly_mask reset_mask = input(kReset)->source->trigger_mask;
    const poly_float reset_value = input(kReset)->source->trigger_value;
    const poly_int reset_offset = input(kReset)->source->trigger_offset;
    const double expected_tolerance = 1.5 / getSampleRate();
    const bool relocated = sync && have_process_time_ &&
        std::abs(*sync_seconds_ - last_process_end_seconds_) > expected_tolerance;

    if (!state->lorenz_initialized) {
      if (independent_stream) {
        for (int lane = 0; lane < poly_float::kSize; ++lane) {
          const double perturbation = randomForCycle(0, lane, mono, stream_seed) * 0.01;
          state->lorenz_state1[lane] += perturbation;
          state->lorenz_state2[lane] -= perturbation;
          state->lorenz_state3[lane] += perturbation;
        }
      }
      state->lorenz_initialized = true;
    }

    poly_float last = 0.5f;
    for (int sample = 0; sample < num_samples; ++sample) {
      poly_float frequency = inputAt(kFrequency, sample) * randomLfoRateMultiplier(inputAt(kRateX10, sample));

      if (mono) {
        state->lorenz_state1[1] = state->lorenz_state1[0];
        state->lorenz_state1[3] = state->lorenz_state1[2];
        state->lorenz_state2[1] = state->lorenz_state2[0];
        state->lorenz_state2[3] = state->lorenz_state2[2];
        state->lorenz_state3[1] = state->lorenz_state3[0];
        state->lorenz_state3[3] = state->lorenz_state3[2];
      }

      for (int lane = 0; lane < poly_float::kSize; ++lane) {
        const bool note_reset = !sync && !free_running && reset_mask[lane] &&
            reset_offset[lane] == static_cast<uint32_t>(sample) &&
            static_cast<int>(std::round(reset_value[lane])) == kVoiceOn;
        if ((relocated && sample == 0) || note_reset) {
          const float perturbation = randomForCycle(0, lane, mono, stream_seed) * 0.01f;
          state->lorenz_state1[lane] = kLorenzInitial1 + perturbation;
          state->lorenz_state2[lane] = kLorenzInitial2 - perturbation;
          state->lorenz_state3[lane] = kLorenzInitial3 + perturbation;
        }

        double state1 = state->lorenz_state1[lane];
        double state2 = state->lorenz_state2[lane];
        double state3 = state->lorenz_state3[lane];
        const double hz = futils::isFinite(frequency[lane]) ?
            std::max(0.0, static_cast<double>(frequency[lane])) : 0.0;
        const double t = std::min(static_cast<double>(kMaxFrequency), hz * (0.5 / getSampleRate()));
        const double delta1 = (state2 - state1) * kLorenzA;
        const double delta2 = (-state3 + kLorenzB) * state1 - state2;
        const double delta3 = state1 * state2 - state3 * kLorenzC;
        state1 += delta1 * t;
        state2 += delta2 * t;
        state3 += delta3 * t;

        if (!futils::isFinite(state1) || !futils::isFinite(state2) || !futils::isFinite(state3)) {
          state1 = kLorenzInitial1;
          state2 = kLorenzInitial2;
          state3 = kLorenzInitial3;
        }
        state->lorenz_state1[lane] = state1;
        state->lorenz_state2[lane] = state2;
        state->lorenz_state3[lane] = state3;
        last.set(lane, utils::clamp(static_cast<float>(state1 * kLorenzScale + 0.5), 0.0f, 1.0f));
      }

      if (!isControlRate())
        destination->buffer[sample] = last;
    }

    if (free_running) {
      for (int lane = 0; lane < 2; ++lane) {
        free_clock->lorenz_end1[lane] = state->lorenz_state1[lane];
        free_clock->lorenz_end2[lane] = state->lorenz_state2[lane];
        free_clock->lorenz_end3[lane] = state->lorenz_state3[lane];
      }
    }

    if (isControlRate())
      destination->buffer[0] = last;
    destination->trigger_value = last;
  }

  void RandomLfo::process(int num_samples) {
    if (num_samples <= 0)
      return;

    const int type = utils::iclamp(static_cast<int>(std::round(input(kStyle)->at(0)[0])),
                                   0, kNumStyles - 1);
    const uint64_t seed = syncMode() == kFreeRunning ? free_seed_ : stream_seed_;
    if (type == kLorenzAttractor)
      processLorenzAttractor(&state_, output(), num_samples, seed, false, &free_clocks_->at(0));
    else
      processRandom(&state_, output(), num_samples, static_cast<RandomType>(type), seed, &free_clocks_->at(0));

    for (int connection = 0; connection < kMaxModulationConnections; ++connection) {
      if (!independent_output_active_->at(connection))
        continue;

      const uint64_t independent_seed = splitMix64(seed ^
          (0x9e3779b97f4a7c15ull * static_cast<uint64_t>(connection + 1)));
      if (type == kLorenzAttractor) {
        processLorenzAttractor(&independent_states_[connection], output(connection + 1), num_samples,
                               independent_seed, true, &free_clocks_->at(connection + 1));
      }
      else {
        processRandom(&independent_states_[connection], output(connection + 1), num_samples,
                      static_cast<RandomType>(type), independent_seed, &free_clocks_->at(connection + 1));
      }
    }

    last_process_end_seconds_ = *sync_seconds_ + static_cast<double>(num_samples) / getSampleRate();
    have_process_time_ = true;
  }

  void RandomLfo::correctToTime(double seconds) {
    *sync_seconds_ = futils::isFinite(seconds) ? seconds : 0.0;
  }
} // namespace vital
