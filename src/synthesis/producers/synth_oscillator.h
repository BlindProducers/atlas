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

#include "analog_waveforms.h"
#include "spectral_morph.h"
#include "synth_constants.h"
#include "utils.h"
#include "wave_frame.h"
#include "wavetable.h"

#include <array>

namespace vital {

  class FourierTransform;
  class Wavetable;
  class SpectralMorphChain;

  struct PhaseBuffer {
    poly_int buffer[kMaxBufferSize * kMaxOversample];
  };

  class RandomValues {
    public:
      static constexpr int kSeed = 0x4;
      static constexpr int kMaxSeed = 65535;
      static constexpr int kBufferSize =
          (kRandomAmplitudeStages + 1) * (Wavetable::kNumHarmonics + 1) / poly_float::kSize;

      // Per-oscillator scratch storage, shared only by its sequential voice
      // clones. Allocate up front; changing the seed during playback cannot allocate.
      RandomValues() : data_(std::make_unique<poly_float[]>(kBufferSize)) { }

      static RandomValues* instance() {
        static RandomValues instance(kBufferSize);
        return &instance;
      }

      const poly_float* buffer() { return data_.get(); }

      const poly_float* buffer(int seed) {
        seed = utils::iclamp(seed, 0, kMaxSeed);
        // Old presets have no seed parameter. Preserve their original table exactly.
        if (seed == 0)
          return instance()->buffer();
        if (seed != current_seed_) {
          std::mt19937 generator(seed);
          for (int i = 0; i < kBufferSize; ++i) {
            for (int lane = 0; lane < poly_float::kSize; ++lane) {
              // Explicit conversion makes new seeds reproducible across standard libraries.
              data_[i].set(lane, static_cast<float>(generator() >> 8) * (1.0f / 8388608.0f) - 1.0f);
            }
          }
          current_seed_ = seed;
        }
        return data_.get();
      }

    private:
      RandomValues(int num_poly_floats) {
        data_ = std::make_unique<poly_float[]>(num_poly_floats);
        utils::RandomGenerator generator(-1.0f, 1.0f);
        generator.seed(kSeed);
        for (int i = 0; i < num_poly_floats; ++i)
          data_[i] = generator.polyNext();
      }

      std::unique_ptr<poly_float[]> data_;
      int current_seed_ = -1;
  };

  class SynthOscillator : public Processor {
    public:
      enum {
        kWaveFrame,
        kMidiNote,
        kMidiTrack,
        kSmoothlyInterpolate,
        kTranspose,
        kTransposeQuantize,
        kTransposeQuantizeKey,
        kTransposeQuantizeScale,
        kTransposeQuantizeMode,
        kTune,
        kFixedFrequencyMode,
        kFixedFrequency,
        kAmplitude,
        kPan,
        kUnisonVoices,
        kUnisonDetune,
        kPhase,
        kDistortionPhase,
        kRandomPhase,
        kBlend,
        kStereoSpread,
        kStackStyle,
        kDetunePower,
        kDetuneRange,
        kUnisonFrameSpread,
        kUnisonDistortionSpread,
        kUnisonSpectralMorphSpread,
        kSpectralMorphType,
        kSpectralMorphAmount,
        kSpectralUnison,
        kDistortionType,
        kDistortionAmount,
        kActiveVoices,
        kReset,
        kRetrigger,
        kNotePressed,
        kSecondaryDistortionType,
        kSecondaryDistortionAmount,
        kSecondarySpectralMorphType,
        kSecondarySpectralMorphAmount,
        kSpectralMorphSeed,
        kSecondarySpectralMorphSeed,
        kOscillatorType,
        kAnalogWaveform,
        kAnalogShape,
        kSubLevel,
        kSubOctave,
        kSubWaveform,
        // Unison spread of the secondary warp amounts, like the primary pair's.
        kSecondaryDistortionSpread,
        kSecondarySpectralMorphSpread,
        kNumInputs
      };

      enum OscillatorType {
        kWavetableType,
        kAnalogType,
        kNumOscillatorTypes
      };

      enum {
        kRaw,
        kLevelled,
        kNumOutputs
      };

      enum SpectralMorph {
        kNoSpectralMorph,
        kVocode,
        kFormScale,
        kHarmonicScale,
        kInharmonicScale,
        kSmear,
        kRandomAmplitudes,
        kLowPass,
        kHighPass,
        kPhaseDisperse,
        kShepardTone,
        kSkew,
        kNumSpectralMorphTypes
      };

      enum DistortionType {
        kNone,
        kSync,
        kFormant,
        kQuantize,
        kBend,
        kSqueeze,
        kPulseWidth,
        kFmOscillatorA,
        kFmOscillatorB,
        kFmSample,
        kRmOscillatorA,
        kRmOscillatorB,
        kRmSample,
        kFmOscillatorC,
        kFmGranular,
        kRmOscillatorC,
        kRmGranular,
        kFmNoise,
        kRmNoise,
        kFmSpectral,
        kRmSpectral,
        kNumDistortionTypes
      };

      // DistortionType values are serialized in presets, so they must not be
      // renumbered. This table only controls how modes are presented: native
      // warps, all FM sources, then all ring-mod sources.
      inline static constexpr std::array<int, kNumDistortionTypes> kDistortionPresentationOrder = {{
        kNone,
        kSync,
        kFormant,
        kQuantize,
        kBend,
        kSqueeze,
        kPulseWidth,
        kFmOscillatorA,
        kFmOscillatorB,
        kFmOscillatorC,
        kFmSample,
        kFmGranular,
        kFmNoise,
        kFmSpectral,
        kRmOscillatorA,
        kRmOscillatorB,
        kRmOscillatorC,
        kRmSample,
        kRmGranular,
        kRmNoise,
        kRmSpectral,
      }};

      static int distortionPresentationIndex(int type) {
        for (int i = 0; i < kNumDistortionTypes; ++i) {
          if (kDistortionPresentationOrder[static_cast<size_t>(i)] == type)
            return i;
        }
        return 0;
      }

      static int shiftDistortionType(int type, int shift) {
        const int index = distortionPresentationIndex(type);
        const int next = (index + shift % kNumDistortionTypes + kNumDistortionTypes) %
                         kNumDistortionTypes;
        return kDistortionPresentationOrder[static_cast<size_t>(next)];
      }

      enum UnisonStackType {
        kNormal,
        kCenterDropOctave,
        kCenterDropOctave2,
        kOctave,
        kOctave2,
        kPowerChord,
        kPowerChord2,
        kMajorChord,
        kMinorChord,
        kHarmonicSeries,
        kOddHarmonicSeries,
        kNumUnisonStackTypes
      };


      static bool isAnalogType(float type) { return type >= 0.5f; }

      static constexpr int kMaxUnison = 16;
      static constexpr int kPolyPhasePerVoice = kMaxUnison / poly_float::kSize;
      static constexpr int kNumPolyPhase = kMaxUnison / 2;
      static constexpr int kNumBuffers = kNumPolyPhase * poly_float::kSize;
      static constexpr int kSpectralBufferSize = Wavetable::kWaveformSize * 2 / poly_float::kSize + poly_float::kSize;
      static const mono_float kStackMultipliers[kNumUnisonStackTypes][kNumPolyPhase];

      struct VoiceBlock {
        VoiceBlock();

        bool isStatic() const {
          return memcmp(from_buffers, to_buffers, poly_float::kSize * sizeof(mono_float*)) == 0;
        }

        int start_sample;
        int end_sample;
        int total_samples;

        poly_int phase;
        poly_float phase_inc_mult;
        poly_float from_phase_inc_mult;
        poly_mask shepard_double_mask;
        poly_mask shepard_half_mask;
        poly_int distortion_phase;
        poly_int last_distortion_phase;
        poly_float distortion;
        poly_float last_distortion;
        poly_float secondary_distortion = 0.0f;
        poly_float last_secondary_distortion = 0.0f;
        using PhaseWarp = poly_int (*)(poly_int, poly_float, poly_int, const poly_float*, int);
        using PhaseWindow = poly_float (*)(poly_int, poly_int, poly_float, const poly_float*, int);
        PhaseWarp secondary_phase_warp = nullptr;
        PhaseWindow secondary_window = nullptr;
        const poly_float* secondary_modulation_buffer = nullptr;
        DistortionType secondary_distortion_type = kNone;
        SpectralMorph secondary_spectral_morph = kNoSpectralMorph;
        poly_mask secondary_shepard_double_mask = 0;
        poly_mask secondary_shepard_half_mask = 0;

        int num_buffer_samples;
        poly_int current_buffer_sample;

        bool smoothing_enabled;
        SpectralMorph spectral_morph;
        const poly_float* modulation_buffer;
        const poly_float* phase_inc_buffer;
        const poly_int* phase_buffer;

        const mono_float* from_buffers[poly_float::kSize];
        const mono_float* to_buffers[poly_float::kSize];
      };

      static void setDistortionValues(DistortionType distortion_type, poly_float* values, int num_values, bool spread);
      static void setSpectralMorphValues(SpectralMorph spectral_morph,
                                         poly_float* values, int num_values, bool spread);
      static void runSpectralMorph(SpectralMorph morph_type, float morph_amount,
                                   const Wavetable::WavetableData* wavetable_data,
                                   int wavetable_index, poly_float* dest, FourierTransform* transform);
      static vital::poly_int adjustPhase(DistortionType distortion_type, poly_int phase,
                                         poly_float distortion_amount, poly_int distortion_phase);
      static vital::poly_float getPhaseWindow(DistortionType distortion_type, poly_int phase,
                                              poly_int distorted_phase);
      static poly_float interpolate(const mono_float* buffers, const poly_int indices);
      static bool usesDistortionPhase(DistortionType distortion_type);

      SynthOscillator(Wavetable* wavetable);

      void reset(poly_mask reset_mask, poly_int sample);
      void reset(poly_mask reset_mask) override;
      void setSpectralMorphValues(SpectralMorph spectral_morph, bool secondary = false);
      void setDistortionValues(DistortionType distortion_type);
      void setSecondaryDistortion(DistortionType type, bool left_active, bool right_active);
      void process(int num_samples) override;
      Processor* clone() const override { return new SynthOscillator(*this); }

      // FM, RM and sync read another oscillator slot. Position k is the k-th
      // other slot in ascending order; the legacy type names (Oscillator A/B/C,
      // Sample, Granular, Spectral, Noise) are the positions of the original
      // fixed sources, so old presets keep their routing.
      static constexpr int kNumModulatorPositions = 7;
      static int modulatorPosition(int type) {
        switch (type) {
          case kFmOscillatorA: case kRmOscillatorA: return 0;
          case kFmOscillatorB: case kRmOscillatorB: return 1;
          case kFmOscillatorC: case kRmOscillatorC: return 2;
          case kFmSample: case kRmSample: return 3;
          case kFmGranular: case kRmGranular: return 4;
          case kFmSpectral: case kRmSpectral: return 5;
          case kFmNoise: case kRmNoise: return 6;
          default: return -1;
        }
      }
      static bool isFmType(int type) {
        return type == kFmOscillatorA || type == kFmOscillatorB || type == kFmOscillatorC ||
               type == kFmSample || type == kFmGranular || type == kFmNoise || type == kFmSpectral;
      }
      static bool isRmType(int type) {
        return type == kRmOscillatorA || type == kRmOscillatorB || type == kRmOscillatorC ||
               type == kRmSample || type == kRmGranular || type == kRmNoise || type == kRmSpectral;
      }
      static int fmTypeForPosition(int position) {
        static constexpr int kTypes[kNumModulatorPositions] = {
          kFmOscillatorA, kFmOscillatorB, kFmOscillatorC, kFmSample, kFmGranular, kFmSpectral, kFmNoise };
        return position >= 0 && position < kNumModulatorPositions ? kTypes[position] : kNone;
      }
      static int rmTypeForPosition(int position) {
        static constexpr int kTypes[kNumModulatorPositions] = {
          kRmOscillatorA, kRmOscillatorB, kRmOscillatorC, kRmSample, kRmGranular, kRmSpectral, kRmNoise };
        return position >= 0 && position < kNumModulatorPositions ? kTypes[position] : kNone;
      }
      void setModulatorOutput(int position, Output* output) {
        if (position >= 0 && position < kNumModulatorPositions)
          modulators_[position] = output;
      }

      virtual void setOversampleAmount(int oversample) override {
        Processor::setOversampleAmount(oversample);
        phase_inc_buffer_->ensureBufferSize(oversample * kMaxBufferSize);
      }

    private:
      template<poly_int(*phaseDistort)(poly_int, poly_float, poly_int, const poly_float*, int),
               poly_float(*window)(poly_int, poly_int, poly_float, const poly_float*, int)>
      void processOscillators(int num_samples, DistortionType distortion_type);

      force_inline bool isAnalog() const { return isAnalogType(input(kOscillatorType)->at(0)[0]); }
      void processAnalog(int num_samples);
      template<int waveform>
      void processAnalogChunk(int num_samples, poly_float current_center_amplitude,
                              poly_float current_detuned_amplitude);
      void resetAnalog(poly_mask reset_mask);
      void addAnalogSub(poly_float* audio_out, int num_samples, poly_mask reset_mask);
      void applyMonoOverlapFade(int num_samples, poly_mask reset_mask);
      Output* modulationSource(DistortionType type) const;

      template<poly_int(*phaseDistort)(poly_int, poly_float, poly_int, const poly_float*, int),
               poly_float(*window)(poly_int, poly_int, poly_float, const poly_float*, int)>
      void processChunk(poly_float current_center_amplitude, poly_float current_detuned_amplitude);

      void processBlend(int num_samples, poly_mask reset_mask);

      void loadVoiceBlock(VoiceBlock& voice_block, int index, poly_mask active_mask);

      void resetWavetableBuffers();
      void setActiveOscillators(int new_active_oscillators);
      template<poly_float(*snapTranspose)(poly_float, poly_float, float*)>
      void setPhaseIncBufferSnap(int num_samples, poly_mask reset_mask,
                                 poly_int trigger_sample, poly_mask active_mask, float* snap_buffer);
      void setPhaseIncBuffer(int num_samples, poly_mask reset_mask, poly_int trigger_sample, poly_mask active_mask);
      void setPhaseIncMults();
      void setupShepardWrap(bool secondary = false);
      void clearShepardWrap();
      void doShepardWrap(poly_mask new_buffer_mask, int quantize, bool secondary = false);
      void setAmplitude();
      void setWaveBuffers(poly_float phase_inc, int index);
      template<void(*spectralMorph)(const Wavetable::WavetableData*, int, poly_float*,
                                    FourierTransform*, float, int, const poly_float*)>
      void computeSpectralWaveBufferPair(int phase_update, int index, bool formant_shift,
                                         float phase_adjustment, poly_int wave_index,
                                         poly_float voice_increment, poly_float morph_amount);
      template<void(*spectralMorph)(const Wavetable::WavetableData*, int, poly_float*,
                                    FourierTransform*, float, int, const poly_float*)>
      void setFourierWaveBuffers(poly_float phase_inc, int index, bool formant_shift);
      void stereoBlend(poly_float* audio_out, int num_samples, poly_mask reset_mask);
      void levelOutput(poly_float* audio_out, const poly_float* raw_out, int buffer_size, poly_mask reset_mask);
      void convertVoiceChannels(int buffer_size, poly_float* audio_out, poly_mask active_mask);
      force_inline float getPhaseIncAdjustment() {
        static constexpr int kBaseSampleRate = 44100;
        
        float adjustment = 1.0f;
        int sample_rate_mult = getSampleRate() / kBaseSampleRate;
        while (sample_rate_mult > 1) {
          sample_rate_mult >>= 1;
          adjustment *= 2.0f;
        }
        return adjustment;
      }

      poly_int phases_[kNumPolyPhase];
      poly_float detunings_[kNumPolyPhase];
      poly_float phase_inc_mults_[kNumPolyPhase];
      poly_float from_phase_inc_mults_[kNumPolyPhase];
      poly_int shepard_double_masks_[kNumPolyPhase];
      poly_int shepard_half_masks_[kNumPolyPhase];
      poly_int waiting_shepard_double_masks_[kNumPolyPhase];
      poly_int waiting_shepard_half_masks_[kNumPolyPhase];
      poly_int secondary_shepard_double_masks_[kNumPolyPhase] {};
      poly_int secondary_shepard_half_masks_[kNumPolyPhase] {};
      poly_int secondary_waiting_shepard_double_masks_[kNumPolyPhase] {};
      poly_int secondary_waiting_shepard_half_masks_[kNumPolyPhase] {};
      poly_float pan_amplitude_;
      poly_float center_amplitude_;
      poly_float detuned_amplitude_;
      poly_float midi_note_;
      poly_float distortion_phase_;
      poly_float blend_stereo_multiply_;
      poly_float blend_center_multiply_;
      const mono_float* next_buffers_[kNumBuffers];
      const mono_float* wave_buffers_[kNumBuffers];
      const mono_float* last_buffers_[kNumBuffers];
      poly_float spectral_morph_values_[kNumPolyPhase];
      poly_float last_spectral_morph_values_[kNumPolyPhase];
      poly_float distortion_values_[kNumPolyPhase];
      poly_float last_distortion_values_[kNumPolyPhase];
      poly_float secondary_distortion_values_[kNumPolyPhase] {};
      poly_float last_secondary_distortion_values_[kNumPolyPhase] {};
      poly_float secondary_spectral_morph_values_[kNumPolyPhase] {};
      poly_float last_secondary_spectral_morph_values_[kNumPolyPhase] {};
      VoiceBlock voice_block_;
      analog::LaneState analog_states_[kNumPolyPhase];
      poly_int sub_phase_ = 0;
      utils::RandomGenerator random_generator_;

      int transpose_quantize_;
      poly_float last_quantized_transpose_;
      poly_float last_quantize_ratio_;
      int unison_;
      int active_oscillators_;
      Wavetable* wavetable_;
      int wavetable_version_;
      int wavetable_frames_ = -1;
      Output* modulators_[kNumModulatorPositions];
    
      poly_float fourier_frames1_[kNumBuffers + 1][kSpectralBufferSize];
      poly_float fourier_frames2_[kNumBuffers + 1][kSpectralBufferSize];
      std::shared_ptr<FourierTransform> fourier_transform_;
      std::shared_ptr<SpectralMorphChain> spectral_chain_;
      std::shared_ptr<RandomValues> random_amplitudes_;
      std::shared_ptr<RandomValues> secondary_random_amplitudes_;
      std::shared_ptr<Output> phase_inc_buffer_;
      std::shared_ptr<PhaseBuffer> phase_buffer_;

      JUCE_LEAK_DETECTOR(SynthOscillator)
  };
} // namespace vital
