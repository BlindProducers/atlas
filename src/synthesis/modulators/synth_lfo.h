/* Copyright 2013-2019 Matt Tytel
 *
 * vital is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include "processor.h"
#include "line_generator.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <memory>

namespace vital {

  class SynthLfo : public Processor {
    public:
      enum {
        kFrequency,
        kRateX10,
        kPhase,
        kAmplitude,
        kNoteTrigger,
        kSyncType,
        kSmoothMode,
        kFade,
        kSmoothTime,
        kStereoPhase,
        kDelay,
        kNoteCount,
        kDeform,
        kSwing,
        kNumInputs
      };

      enum {
        kValue,
        kOscPhase,
        kOscFrequency,
        kNumOutputs
      };

      enum SyncType {
        kTrigger,
        kSync,
        kEnvelope,
        kSustainEnvelope,
        kLoopPoint,
        kLoopHold,
        kFreeRunning,
        kNumSyncTypes
      };

      enum {
        kTime,
        kTempo,
        kDottedTempo,
        kTripletTempo,
        kKeytrack,
        kNumSyncOptions
      };

      struct LfoState {
        poly_float delay_time_passed = 0.0f;
        poly_float fade_amplitude = 0.0f;
        poly_float smooth_value = 0.0f;
        poly_float fade_amount = 0.0f;
        poly_float offset = 0.0f;
        poly_float phase = 0.0f;
      };

      // Free Running keeps one phase for every voice: notes join it where it
      // is instead of restarting it. The voice handler counts the samples it
      // renders (now), and the first voice of each block brings the phase up
      // to then at the rate the voices last ran, so it also moves on while no
      // voice sounds. Shared by every voice clone, like sync_seconds_.
      struct FreeClock {
        uint64_t now = 0;
        uint64_t time = 0;
        double phase = 0.0;
        double frequency = 0.0;
        bool started = false;
      };

      static constexpr mono_float kMaxPower = 20.0f;
      static constexpr float kHalfLifeRatio = 0.2f;
      static constexpr float kMinHalfLife = 0.0002f;

      force_inline poly_float getValueAtPhase(mono_float* buffer, poly_float resolution,
                                              poly_int max_index, poly_float phase) {
        poly_float boost = utils::clamp(phase * resolution, 0.0f, resolution);
        poly_int indices = utils::clamp(utils::toInt(boost), 0, max_index);
        poly_float t = boost - utils::toFloat(indices);

        matrix interpolation_matrix = utils::getCatmullInterpolationMatrix(t);
        matrix value_matrix = utils::getValueMatrix(buffer, indices);
        value_matrix.transpose();
        return interpolation_matrix.multiplyAndSumRows(value_matrix);
      }

      force_inline poly_float getValueAtPhase(poly_float phase) {
        int resolution = source_->resolution();
        auto buffer_read = source_->acquireBuffer();
        return getValueAtPhase(buffer_read.get(), resolution, resolution - 1, phase);
      }

      explicit SynthLfo(LineGenerator* source);

      Processor* clone() const override { return new SynthLfo(*this); }
      void process(int num_samples) override;
      void correctToTime(double seconds);
      void advanceFreeClock(int num_samples) { free_clock_->now += static_cast<uint64_t>(num_samples); }
      bool runsFreely() const {
        return static_cast<int>(std::round(input(kSyncType)->at(0)[0])) == kFreeRunning;
      }

    protected:
      void catchUpFreeClock();
      poly_float applySwingToPhase(poly_float phase, bool preserve_end) const;
      void updateLineGeneratorControls();
      poly_float inputAt(int input_index, int sample) const;
      void applyEventAtSample(int sample, int sync_type, const poly_float& frequency,
                              const poly_mask& trigger_mask, const poly_float& trigger_value,
                              const poly_int& trigger_offset);
      void advanceClock(int lane, int sync_type, double delta, double loop_point, bool held);

      LfoState state_;
      poly_mask held_mask_;
      std::array<double, poly_float::kSize> phase_clock_ {};
      LineGenerator* source_;
      // ProcessorRouter clones stateful LFO processors for voice rendering. The
      // host clock is updated on the graph's original module, so every clone
      // must observe the same clock without allocating or locking at runtime.
      std::shared_ptr<double> sync_seconds_;
      std::shared_ptr<FreeClock> free_clock_;

      JUCE_LEAK_DETECTOR(SynthLfo)
  };
} // namespace vital
