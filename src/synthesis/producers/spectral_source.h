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

#include "processor.h"
#include "sample_source.h"
#include "spectral_analysis.h"
#include "slot_modulation.h"

#include <array>

namespace vital {

  class SpectralSource : public Processor {
    public:
      static constexpr mono_float kMaxAmplitude = 1.41421356237f;

      enum {
        kReset,
        kMidi,
        kNotePressed,
        kKeytrack,
        kRootKey,
        kLevel,
        kTranspose,
        kTransposeQuantize,
        kTransposeQuantizeKey,
        kTransposeQuantizeScale,
        kTransposeQuantizeMode,
        kTune,
        kPlayMode,
        kPosition,
        kSpeed,
        kLoopStart,
        kLoopEnd,
        kBinLow,
        kBinHigh,
        kSpectralShift,
        kSpectralStretch,
        kSpectralBlur,
        kSpectralTilt,
        kPhaseRandomize,
        kFreeze,
        kKeepFundamental,
        kPhaseMode,
        kPreserveTransients,
        kSpectralEffect,
        kEffectAmount,
        kPan,
        kActiveVoices,
        kRetrigger,
        kDistortionType,
        kDistortionAmount,
        kSecondaryDistortionType,
        kSecondaryDistortionAmount,
        kNumInputs
      };

      enum {
        kRaw,
        kLevelled,
        kNumOutputs
      };

      enum PlayMode {
        kManual,
        kForward,
        kReverse,
        kLoop,
        kBounce,
        kNumPlayModes
      };

      enum PhaseMode {
        kVocoder,
        kLocked,
        kPeakLocked,
        kNumPhaseModes
      };

      enum SpectralEffect {
        kNoEffect,
        kGate,
        kCompress,
        kExpand,
        kComb,
        kDetune,
        kSpread,
        kShapedTone,
        kNumSpectralEffects
      };

      SpectralSource();
      explicit SpectralSource(std::shared_ptr<Sample> sample);
      SpectralSource(const SpectralSource& other);
      ~SpectralSource() override;

      void process(int num_samples) override;
      void reset(poly_mask reset_mask) override;
      Processor* clone() const override { return new SpectralSource(*this); }
      Sample* getSample() { return sample_.get(); }
      // Non-blocking readiness query for loading UI and deterministic renderers.
      bool isAnalysisReady() const;
      SlotModulation& modulation() { return modulation_; }

    private:
      struct LaneState {
        bool analysis_valid = false;
        double position = 0.0;
        bool bounce_forward = true;
        int write_pos = 0;
        int read_pos = 0;
        int samples_until_frame = 0;
        int last_play_mode = -1;
        int mode_handoff_samples = 0;
        unsigned int random_seed = 1;
        float last_output = 0.0f;
        float mode_handoff_correction = 0.0f;
        bool started = false;
        bool phases_initialized = false;
        int last_transient_frame = -1;
        std::array<float, SpectralAnalysis::kFftSize> overlap {};
        std::array<float, SpectralAnalysis::kNumBins> synth_phase {};
        std::array<float, SpectralAnalysis::kFftSize * 2> work {};
        std::array<float, SpectralAnalysis::kNumBins> magnitude {};
        std::array<float, SpectralAnalysis::kNumBins> frequency {};
        std::array<float, SpectralAnalysis::kNumBins> phase {};
        std::array<float, SpectralAnalysis::kNumBins> scratch {};
        std::array<float, SpectralAnalysis::kNumBins> effect_original {};
        std::array<float, SpectralAnalysis::kNumBins> original_magnitude {};
        std::array<int, SpectralAnalysis::kNumBins> peak_of {};
      };

      static poly_float snapTranspose(poly_float input_midi, poly_float transpose, int quantize);
      static int quantizeScaleMask(int scale, int key);
      static float noteToFrequency(float note);

      struct AnalysisData;
      struct SharedAnalysisCache;

      void refreshAnalysisIfNeeded();
      void resetLane(int lane);
      float control(int index, int sample, int lane, float fallback = 0.0f) const;
      float applyModeHandoff(LaneState& lane, int play_mode, float value) const;
      void advancePosition(LaneState& lane, int play_mode, float position, float speed,
                           float loop_start, float loop_end) const;
      void synthesizeFrame(LaneState& lane, int lane_index, int sample, float pitch_ratio, float root_note,
                           int phase_mode, int spectral_effect);

      std::shared_ptr<Sample> sample_;
      std::shared_ptr<SharedAnalysisCache> analysis_cache_;
      std::shared_ptr<const AnalysisData> active_analysis_;
      int analysis_revision_ = -1;
      std::array<LaneState, poly_float::kSize> lanes_;
      std::array<float, SpectralAnalysis::kFftSize> window_;
      std::shared_ptr<const juce::dsp::FFT> fft_;
      SlotModulation modulation_;

      JUCE_LEAK_DETECTOR(SpectralSource)
  };
} // namespace vital
