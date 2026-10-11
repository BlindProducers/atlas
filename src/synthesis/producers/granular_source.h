/* Copyright 2013-2019 Matt Tytel
 *
 * vital is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * vital is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with vital.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "sample_source.h"
#include "slot_modulation.h"

#include <array>

namespace vital {

  class GranularSource : public Processor {
    public:
      static constexpr int kMaxGrains = 64;
      static constexpr mono_float kMaxTranspose = 96.0f;
      static constexpr mono_float kMinTranspose = -96.0f;
      static constexpr mono_float kMaxAmplitude = 1.41421356237f;

      enum {
        kReset,
        kVoiceEvent,
        kMidi,
        kKeytrack,
        kRootKey,
        kLevel,
        kTranspose,
        kTransposeQuantize,
        kTransposeQuantizeKey,
        kTransposeQuantizeScale,
        kTransposeQuantizeMode,
        kTune,
        kStart,
        kEnd,
        kMode,
        kMidiDensity,
        kGrainCount,
        kDensity,
        kGrainSize,
        kSpeed,
        kPosition,
        kPositionMod,
        kPositionModRate,
        kRandomPosition,
        kRandomVolume,
        kRandomPan,
        kRandomPitch,
        kInterval,
        kIntervalChance,
        kDirection,
        kLowCutoff,
        kHighCutoff,
        kPan,
        kNoteCount,
        kWindowShape,
        kTimingJitter,
        kTimingMode,
        kGridRate,
        kSizeMode,
        kGridLength,
        kPositionGrid,
        kBeatsPerSecond,
        kDistortionType,
        kDistortionAmount,
        kSecondaryDistortionType,
        kSecondaryDistortionAmount,
        kNumInputs
      };

      enum WindowShape {
        kHann,
        kSoftPlateau,
        kTriangle,
        kGaussian,
        kPercussive,
        kReversePercussive,
        kNumWindowShapes
      };

      enum TimingMode {
        kFreeTiming,
        kGridTiming,
        kGridDottedTiming,
        kGridTripletTiming,
        kNumTimingModes
      };

      enum SizeMode {
        kMillisecondSize,
        kGridStepSize,
        kNumSizeModes
      };

      // Grid rates from a whole note down to 1/128.
      static constexpr int kNumGridRates = 8;
      // Position grid: off, then 2^n slices of the selected region.
      static constexpr int kNumPositionGrids = 8;
      static constexpr int kWindowResolution = 1024;

      enum {
        kRaw,
        kLevelled,
        kNumOutputs
      };

      GranularSource();
      explicit GranularSource(std::shared_ptr<Sample> sample);

      void process(int num_samples) override;
      Processor* clone() const override { return new GranularSource(*this); }
      void setSeed(int seed) { random_generator_.seed(seed); }
      // Host time at the start of the next block; grid timing follows it so
      // grains land on the song's bar/beat grid, shared by all voice clones.
      void correctToTime(double seconds);
      static mono_float gridStepBeats(int rate_index, int timing_mode);
      Sample* getSample() { return sample_.get(); }
      force_inline Output* getPhaseOutput() const { return phase_output_.get(); }
      SlotModulation& modulation() { return modulation_; }

    private:
      struct Grain {
        bool active = false;
        double source_position = 0.0;
        double source_increment = 1.0;
        int age = 0;
        int length = 1;
        mono_float gain = 1.0f;
        const mono_float* read_buffer = nullptr;
        int read_length = 0;
        int read_scale = 1;
        // Window table read position and per-sample step (replaces per-sample cos).
        mono_float window_position = 0.0f;
        mono_float window_step = 0.0f;
        int window_shape = kHann;
      };

      struct GrainRandomValues {
        mono_float position = 0.0f;
        mono_float pitch = 0.0f;
        mono_float volume = 0.0f;
        mono_float pan = 0.0f;
        mono_float interval_roll = 0.0f;
        mono_float direction_roll = 0.0f;
        mono_float timing = 0.0f;
      };

      struct LaneState {
        double playhead = 0.0;
        double grain_phase = 1.0;
        double modulation_phase = 0.0;
        std::array<Grain, kMaxGrains> grains;
        // Keep original slot order to preserve summation and slot reuse exactly.
        std::array<int, kMaxGrains> active_grains {};
        int active_count = 0;
        // A jittered grain waits here until its delay elapses.
        bool pending = false;
        int pending_delay = 0;
        GrainRandomValues pending_random;
        // Smoothed overlap normalization; per-sample normalization modulated
        // the amplitude at the grain rate and blurred the sound.
        mono_float normalization = 1.0f;
        double last_grid_step = 0.0;
        bool grid_started = false;

        void reset();
      };

      static poly_float onePoleCoefficient(poly_float midi_cutoff, int sample_rate);
      static force_inline poly_float processLowPass(poly_float input, poly_float& state, poly_float coefficient) {
        state += (input - state) * coefficient;
        return state;
      }

      static double wrapPosition(double value, double length);
      mono_float inputValueAt(int input_index, int frame, int lane) const;
      void prepareGrainReader(int lane, Grain& grain) const;
      mono_float readSampleAt(const Grain& grain) const;
      static const mono_float* windowTable(int shape);
      static force_inline mono_float windowValue(const Grain& grain) {
        const mono_float* table = windowTable(grain.window_shape);
        const mono_float position = std::min(grain.window_position, static_cast<mono_float>(kWindowResolution));
        const int index = std::min(static_cast<int>(position), kWindowResolution - 1);
        const mono_float t = position - index;
        return table[index] + (table[index + 1] - table[index]) * t;
      }
      void resetLane(int lane, mono_float start);
      GrainRandomValues nextGrainRandomValues();
      void spawnGrain(int lane, int frame, mono_float start, mono_float end, mono_float normalized_position,
                      mono_float pitch_ratio, const GrainRandomValues& random);
      poly_float snapTranspose(poly_float input_midi, poly_float transpose, int quantize);

      poly_float pan_amplitude_;
      int transpose_quantize_;
      poly_float last_quantized_transpose_;
      poly_float low_cut_state_;
      poly_float high_cut_state_;
      poly_float phase_;
      std::array<LaneState, poly_float::kSize> lanes_;
      std::shared_ptr<cr::Output> phase_output_;
      utils::RandomGenerator random_generator_;
      std::shared_ptr<Sample> sample_;
      std::shared_ptr<double> sync_seconds_;

      SlotModulation modulation_;

      JUCE_LEAK_DETECTOR(GranularSource)
  };
} // namespace vital
