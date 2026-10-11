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
      void setSeed(uint64_t seed) { stream_seed_ = seed; }
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
      void processRandom(RandomState* state, Output* destination, int num_samples, RandomType type,
                         uint64_t stream_seed);
      void processLorenzAttractor(RandomState* state, Output* destination, int num_samples,
                                  uint64_t stream_seed, bool independent_stream);
      float randomForCycle(int64_t cycle, int lane, bool mono, uint64_t stream_seed) const;
      void resetLane(RandomState* state, int lane);

      RandomState state_;
      std::array<RandomState, kMaxModulationConnections> independent_states_ {};
      std::shared_ptr<std::array<bool, kMaxModulationConnections>> independent_output_active_;
      uint64_t stream_seed_ = 0;
      // Shared by ProcessorRouter voice clones; see SynthLfo::sync_seconds_.
      std::shared_ptr<double> sync_seconds_;
      double last_process_end_seconds_ = 0.0;
      bool have_process_time_ = false;

      static uint64_t next_stream_id_;

      JUCE_LEAK_DETECTOR(RandomLfo)
  };
} // namespace vital
