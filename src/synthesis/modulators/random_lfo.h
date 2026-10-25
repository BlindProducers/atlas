/* Copyright 2013-2019 Matt Tytel
 *
 * vital is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include "processor.h"
#include "synth_constants.h"
#include "utils.h"

#include <array>
#include <cstdint>
#include <memory>

namespace vital {

  class RandomLfo : public Processor {
    public:
      struct RandomState {
        RandomState() {
          lorenz_state1.fill(0.1);
          lorenz_state2.fill(0.0);
          lorenz_state3.fill(0.0);
        }

        poly_float offset = 0.0f;
        poly_float last_random_value = 0.0f;
        poly_float next_random_value = 0.0f;
        std::array<double, poly_float::kSize> phase {};
        std::array<int64_t, poly_float::kSize> cycle {};
        std::array<double, poly_float::kSize> lorenz_state1;
        std::array<double, poly_float::kSize> lorenz_state2;
        std::array<double, poly_float::kSize> lorenz_state3;
        bool lorenz_initialized = false;
      };

      // Free Running keeps one random stream for every voice, as SynthLfo's
      // FreeClock does: the voice handler counts rendered samples (now) and
      // the first voice of each block brings the stream up to then. Lorenz
      // can't be skipped ahead, so it holds still while no voice sounds.
      struct FreeClock {
        uint64_t now = 0;
        uint64_t time = 0;
        double phase = 0.0;
        int64_t cycle = 0;
        double frequency = 0.0;
        std::array<double, 2> lorenz_start1 {}, lorenz_start2 {}, lorenz_start3 {};
        std::array<double, 2> lorenz_end1 {}, lorenz_end2 {}, lorenz_end3 {};
        bool started = false;
      };

      enum {
        kFrequency,
        kRateX10,
        kAmplitude,
        kReset,
        kSync,
        kStyle,
        kRandomType,
        kStereo,
        kNumInputs
      };

      enum SyncMode {
        kNoteTrigger,
        kHostSync,
        kFreeRunning,
        kNumSyncModes
      };

      enum RandomType {
        kPerlin,
        kSampleAndHold,
        kSinInterpolate,
        kLorenzAttractor,
        kNumStyles
      };

      RandomLfo();
      RandomLfo(const RandomLfo& other);

      Processor* clone() const override { return new RandomLfo(*this); }
      void process(int num_samples) override;
      void correctToTime(double seconds);
      void setSeed(uint64_t seed) { stream_seed_ = seed; free_seed_ = seed; }
      bool runsFreely() const { return syncMode() == kFreeRunning; }
      void advanceFreeClock(int num_samples) {
        for (FreeClock& clock : *free_clocks_)
          clock.now += static_cast<uint64_t>(num_samples);
      }
      uint64_t seed() const { return stream_seed_; }
      Output* independentOutput(int connection_index) {
        VITAL_ASSERT(connection_index >= 0 && connection_index < kMaxModulationConnections);
        return output(connection_index + 1);
      }
      void setIndependentOutputActive(int connection_index, bool active) {
        VITAL_ASSERT(connection_index >= 0 && connection_index < kMaxModulationConnections);
        independent_output_active_->at(connection_index) = active;
      }

    protected:
      poly_float inputAt(int input_index, int sample) const;
      int syncMode() const;
      void catchUpFreeClock(FreeClock* clock, RandomState* state, bool mono, uint64_t stream_seed);
      void processRandom(RandomState* state, Output* destination, int num_samples, RandomType type,
                         uint64_t stream_seed, FreeClock* free_clock);
      void processLorenzAttractor(RandomState* state, Output* destination, int num_samples,
                                  uint64_t stream_seed, bool independent_stream, FreeClock* free_clock);
      float randomForCycle(int64_t cycle, int lane, bool mono, uint64_t stream_seed,
                           bool shared_voices = false) const;
      void resetLane(RandomState* state, int lane);

      RandomState state_;
      std::array<RandomState, kMaxModulationConnections> independent_states_ {};
      std::shared_ptr<std::array<bool, kMaxModulationConnections>> independent_output_active_;
      uint64_t stream_seed_ = 0;
      // Voice clones each get their own stream_seed_; Free Running reads this
      // one, which they all copy unchanged.
      uint64_t free_seed_ = 0;
      // Index 0 is the main output, 1 + n independent output n.
      std::shared_ptr<std::array<FreeClock, 1 + kMaxModulationConnections>> free_clocks_;
      // Shared by ProcessorRouter voice clones; see SynthLfo::sync_seconds_.
      std::shared_ptr<double> sync_seconds_;
      double last_process_end_seconds_ = 0.0;
      bool have_process_time_ = false;

      static uint64_t next_stream_id_;

      JUCE_LEAK_DETECTOR(RandomLfo)
  };
} // namespace vital
