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

#include "synth_oscillator.h"

#include "fourier_transform.h"
#include "futils.h"
#include "matrix.h"
#include "wavetable.h"

#include <algorithm>
#include <climits>

namespace vital {
  namespace {
    constexpr int kNumVoicesPerProcess = poly_float::kSize / 2;
    constexpr int kWaveformBits = WaveFrame::kWaveformBits;
    constexpr int kIntermediateBits = 8 * sizeof(uint32_t) - kWaveformBits;
    constexpr int kHalfPhase = INT_MIN;
    constexpr unsigned long long kFullPhase = 1ULL + (unsigned long long)(UINT_MAX);
    constexpr mono_float kPhaseMult = kFullPhase;
    constexpr mono_float kInvPhaseMult = 1.0f / kFullPhase;
    constexpr mono_float kIntermediateMult = 1 << kIntermediateBits;
    const poly_int kIntermediateMask = (1 << kIntermediateBits) - 1;

    constexpr mono_float kPhaseBits = (8 * sizeof(uint32_t));
    constexpr mono_float kDistortBits = kPhaseBits;
    constexpr mono_float kMaxQuantize = 0.85f;

    constexpr mono_float kMaxSqueezePercent = 0.95f;
    constexpr mono_float kMaxPulseWidthRatio = 64.0f;
    constexpr mono_float kMaxAmplitude = 2.0f;

    constexpr mono_float kCenterLowAmplitude = 0.4f;
    constexpr mono_float kDetunedHighAmplitude = 0.6f;
    constexpr mono_float kWavetableFadeTime = 0.007f;

    constexpr int kMaxSyncPower = 4;
    constexpr int kMaxSync = 1 << kMaxSyncPower;

    constexpr mono_float kFifthMult = 1.49830707688f;
    constexpr mono_float kMajorThirdMult = 1.25992104989f;
    constexpr mono_float kMinorThirdMult = 1.189207115f;
    constexpr mono_float kNoMidiTrackDefault = 48.0f;

    constexpr int kTransposeQuantizeCustomScale = 14;
    constexpr int kTransposeQuantizeScaleMasks[] = {
      0,
      (1 << 12) - 1,
      (1 << 0) | (1 << 2) | (1 << 4) | (1 << 5) | (1 << 7) | (1 << 9) | (1 << 11),
      (1 << 0) | (1 << 2) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 8) | (1 << 10),
      (1 << 0) | (1 << 2) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 8) | (1 << 11),
      (1 << 0) | (1 << 2) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 9) | (1 << 11),
      (1 << 0) | (1 << 2) | (1 << 4) | (1 << 7) | (1 << 9),
      (1 << 0) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 10),
      (1 << 0) | (1 << 3) | (1 << 5) | (1 << 6) | (1 << 7) | (1 << 10),
      (1 << 0) | (1 << 2) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 9) | (1 << 10),
      (1 << 0) | (1 << 1) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 8) | (1 << 10),
      (1 << 0) | (1 << 2) | (1 << 4) | (1 << 6) | (1 << 7) | (1 << 9) | (1 << 11),
      (1 << 0) | (1 << 2) | (1 << 4) | (1 << 5) | (1 << 7) | (1 << 9) | (1 << 10),
      (1 << 0) | (1 << 1) | (1 << 3) | (1 << 5) | (1 << 6) | (1 << 8) | (1 << 10),
      -1,
    };

    int rotateTransposeQuantizeMask(int mask, int key) {
      int result = 0;
      key = std::max(0, std::min(kNotesPerOctave - 1, key));
      for (int i = 0; i < kNotesPerOctave; ++i) {
        if ((mask >> i) & 1)
          result |= 1 << ((i + key) % kNotesPerOctave);
      }
      return result;
    }

    int getFrameSpreadPhaseOffset(int index, int active_oscillators, float frame_spread) {
      static constexpr float kMaxFrameSpreadPhaseFanCycles = 0.25f;
      int num_phase_updates = utils::imax(2, active_oscillators / 2);
      float unison_position = index / (num_phase_updates - 1.0f) - 0.5f;
      float spread_amount = std::min(1.0f, std::abs(frame_spread) / (kNumOscillatorWaveFrames * 0.5f));
      return static_cast<int>(unison_position * spread_amount * kMaxFrameSpreadPhaseFanCycles * kPhaseMult);
    }
    
    const poly_float kFmPhaseMult = kPhaseMult / 8.0f;
    const poly_int kMaxFmModulation = 48;

    force_inline poly_int passThroughPhase(poly_int phase, poly_float, poly_int, const poly_float*, int) {
      return phase;
    }

    force_inline poly_int quantizePhase(poly_int phase, poly_float distortion, poly_int distortion_phase,
                                        const poly_float*, int) {
      poly_float normal_phase = utils::toFloat(phase) * distortion * kInvPhaseMult;
      poly_float adjustment = utils::toFloat(distortion_phase) * kInvPhaseMult;
      poly_float floored_phase = utils::trunc(normal_phase + adjustment) - adjustment;
      return utils::toInt((floored_phase / distortion) * kPhaseMult) - distortion_phase;
    }

    force_inline poly_int bendPhase(poly_int phase, poly_float distortion, poly_int distortion_phase,
                                    const poly_float*, int) {
      poly_float float_phase = utils::toFloat(phase - distortion_phase) * (1.0f / kPhaseMult) + 0.5f;

      poly_float distortion_offset = (distortion - distortion * distortion) * 2.0f;
      poly_float float_phase2 = float_phase * float_phase;
      poly_float float_phase3 = float_phase * float_phase2;

      poly_float distortion_scale = distortion * 3.0f;
      poly_float middle_mult1 = distortion_scale + distortion_offset;
      poly_float middle_mult2 = distortion_scale - distortion_offset;
      poly_float middle1 = middle_mult1 * (float_phase2 - float_phase3);
      poly_float middle2 = middle_mult2 * (float_phase - float_phase2 * 2.0f + float_phase3);
      poly_float new_phase = float_phase3 + middle1 + middle2;
      return utils::toInt((new_phase - 0.5f) * kPhaseMult);
    }

    force_inline poly_int squeezePhase(poly_int phase, poly_float distortion, poly_int distortion_phase,
                                       const poly_float*, int) {
      static const poly_float kCenterPhase = kPhaseMult / 4.0f;
      static const poly_float kMaxPhase = kPhaseMult / 2.0f;
      poly_float float_phase = utils::toFloat(phase - distortion_phase);
      poly_mask positive_mask = poly_float::greaterThan(float_phase, 0.0f);
      float_phase = poly_float::abs(float_phase);

      poly_float pivot = distortion * kCenterPhase;
      poly_mask right_half_mask = poly_float::greaterThan(float_phase, pivot);

      poly_float left_phase = float_phase / distortion;
      poly_float right_phase = kMaxPhase - (kMaxPhase - float_phase) / (poly_float(2.0f) - distortion);
      poly_float new_phase = utils::maskLoad(left_phase, right_phase, right_half_mask);
      new_phase = utils::maskLoad(-new_phase, new_phase, positive_mask);
      return utils::toInt(new_phase);
    }

    force_inline poly_int syncPhase(poly_int phase, poly_float distortion, poly_int, const poly_float*, int) {
      poly_float float_val = utils::toFloat(phase + kHalfPhase) * distortion;
      return utils::toInt(float_val) * kMaxSync + kHalfPhase;
    }

    force_inline poly_int pulseWidthPhase(poly_int phase, poly_float distortion, poly_int distortion_phase,
                                          const poly_float*, int) {
      poly_float distorted_phase = utils::toFloat(phase) * distortion;
      poly_float clamped_phase = utils::clamp(distorted_phase, INT_MIN, INT_MAX);
      return utils::toInt(clamped_phase);
    }
    
    force_inline poly_int fmPhase(poly_int phase, poly_float distortion, poly_int,
                                  const poly_float* modulation, int i) {
      poly_float phase_offset = modulation[i] * distortion;
      return phase + utils::toInt(phase_offset * kFmPhaseMult) * kMaxFmModulation;
    }

    force_inline poly_int fmPhaseLeft(poly_int phase, poly_float distortion, poly_int,
                                      const poly_float* modulation, int i) {
      poly_float mod = modulation[i] & constants::kFirstMask;
      mod += utils::swapVoices(mod);
      poly_float phase_offset = mod * distortion;
      return phase + utils::toInt(phase_offset * kFmPhaseMult) * kMaxFmModulation;
    }
    
    force_inline poly_int fmPhaseRight(poly_int phase, poly_float distortion, poly_int,
                                       const poly_float* modulation, int i) {
      poly_float mod = modulation[i] & constants::kSecondMask;
      mod += utils::swapVoices(mod);
      poly_float phase_offset = mod * distortion;
      return phase + utils::toInt(phase_offset * kFmPhaseMult) * kMaxFmModulation;
    }

    force_inline poly_float passThroughWindow(poly_int, poly_int, poly_float, const poly_float*, int) {
      return 1.0f;
    }

    force_inline poly_float pulseWidthWindow(poly_int, poly_int distorted_phase, poly_float, const poly_float*, int) {
      return poly_float(1.0f) & ~poly_int::equal(distorted_phase, INT_MIN) &
             ~poly_int::equal(distorted_phase, INT_MAX);
    }

    force_inline poly_float halfSinWindow(poly_int phase, poly_int, poly_float, const poly_float*, int) {
      poly_float normal_phase = utils::toFloat(phase + INT_MAX) * (kInvPhaseMult / 2.0f);
      return futils::sin(normal_phase + 0.25f);
    }

    force_inline poly_float rmWindow(poly_int, poly_int, poly_float distortion, const poly_float* modulation, int i) {
      return utils::interpolate(1.0f, modulation[i], distortion);
    }

    force_inline poly_float rmWindowLeft(poly_int, poly_int, poly_float distortion,
                                         const poly_float* modulation, int i) {
      poly_float mod = modulation[i] & constants::kFirstMask;
      mod += utils::swapVoices(mod);
      return utils::interpolate(1.0f, mod, distortion);
    }

    force_inline poly_float rmWindowRight(poly_int, poly_int, poly_float distortion, 
                                          const poly_float* modulation, int i) {
      poly_float mod = modulation[i] & constants::kSecondMask;
      mod += utils::swapVoices(mod);
      return utils::interpolate(1.0f, mod, distortion);
    }

    force_inline poly_float noTransposeSnap(poly_float midi, poly_float transpose, float*) {
      return midi + transpose;
    }

    force_inline poly_float localTransposeSnap(poly_float midi, poly_float transpose, float* snap_buffer) {
      static constexpr float kScaleDown = 1.0f / kNotesPerOctave;
      static constexpr float kScaleUp = kNotesPerOctave;

      poly_float note_offset = utils::mod(transpose * kScaleDown) * kScaleUp;
      poly_float octave_snap = transpose - note_offset;
      poly_int int_snap = utils::roundToInt(note_offset);
      poly_float result;
      for (int i = 0; i < poly_float::kSize; ++i)
        result.set(i, snap_buffer[int_snap[i]]);

      return midi + octave_snap + result;
    }

    force_inline poly_float globalTransposeSnap(poly_float midi, poly_float transpose, float* snap_buffer) {
      static constexpr float kScaleDown = 1.0f / kNotesPerOctave;
      static constexpr float kScaleUp = kNotesPerOctave;

      poly_float total = midi + transpose;
      poly_float note_offset = utils::mod(total * kScaleDown) * kScaleUp;
      poly_float octave_snap = total - note_offset;
      poly_int int_snap = utils::roundToInt(note_offset);
      poly_float result;
      for (int i = 0; i < poly_float::kSize; ++i)
        result.set(i, snap_buffer[int_snap[i]]);

      return octave_snap + result;
    }

    force_inline poly_float getInterpolationValues(poly_int indices) {
      return utils::toFloat(indices & kIntermediateMask) * (1.0f / kIntermediateMult);
    }

    force_inline poly_float linearlyInterpolateBuffer(const mono_float* buffer, const poly_int indices) {
      poly_int start_indices = utils::shiftRight<kIntermediateBits>(indices);
      poly_float t = getInterpolationValues(indices);
      matrix interpolation_matrix = utils::getLinearInterpolationMatrix(t);
      matrix value_matrix = utils::getValueMatrix(buffer, start_indices);
      value_matrix.transpose();
      return interpolation_matrix.multiplyAndSumRows(value_matrix);
    }

    force_inline poly_float interpolateBuffers(const mono_float* const* buffers, const poly_int indices) {
      poly_int start_indices = utils::shiftRight<kIntermediateBits>(indices);
      poly_float t = getInterpolationValues(indices);
      matrix interpolation_matrix = utils::getCatmullInterpolationMatrix(t);
      matrix value_matrix = utils::getValueMatrix(buffers, start_indices);
      value_matrix.transpose();
      return interpolation_matrix.multiplyAndSumRows(value_matrix);
    }

    force_inline poly_float interpolateBuffers(const mono_float* const* buffers,
                                               const mono_float* const*,
                                               const poly_int indices, poly_float) {
      return interpolateBuffers(buffers, indices);
    }

    force_inline poly_float interpolateMultipleBuffers(const mono_float* const* buffers_from,
                                                       const mono_float* const* buffers_to,
                                                       const poly_int indices, poly_float buffer_t) {
      poly_int start_indices = utils::shiftRight<kIntermediateBits>(indices);
      poly_float t = getInterpolationValues(indices);
      matrix interpolation_matrix = utils::getCatmullInterpolationMatrix(t);
      matrix value_matrix = utils::getValueMatrix(buffers_from, start_indices);
      value_matrix.interpolateRows(utils::getValueMatrix(buffers_to, start_indices), buffer_t);
      value_matrix.transpose();
      return interpolation_matrix.multiplyAndSumRows(value_matrix);
    }

    force_inline poly_float interpolateShepardBuffers(const mono_float* const* buffers_from,
                                                      const mono_float* const* buffers_to,
                                                      const poly_int indices, poly_float buffer_t,
                                                      poly_mask double_mask, poly_mask half_mask,
                                                      poly_mask double_mask2 = 0, poly_mask half_mask2 = 0) {
      poly_int adjusted_indices = utils::maskLoad(indices, indices * 2, double_mask);
      adjusted_indices = utils::maskLoad(adjusted_indices, utils::shiftRight<1>(adjusted_indices), half_mask);
      adjusted_indices = utils::maskLoad(adjusted_indices, adjusted_indices * 2, double_mask2);
      adjusted_indices = utils::maskLoad(adjusted_indices, utils::shiftRight<1>(adjusted_indices), half_mask2);
      poly_float from = interpolateBuffers(buffers_from, adjusted_indices);
      poly_float to = interpolateBuffers(buffers_to, indices);
      return utils::interpolate(from, to, buffer_t);
    }

    bool hasShepardWrap(const SynthOscillator::VoiceBlock& voice) {
      return (voice.shepard_double_mask | voice.shepard_half_mask |
              voice.secondary_shepard_double_mask | voice.secondary_shepard_half_mask).anyMask();
    }

    poly_int processDetunedShepard(const SynthOscillator::VoiceBlock& voice_block, poly_float* audio_out) {
      const mono_float* const* from_buffers = voice_block.from_buffers;
      const mono_float* const* to_buffers = voice_block.to_buffers;

      int start = voice_block.start_sample;
      poly_float t_inc = 1.0f / voice_block.num_buffer_samples;
      poly_float t = utils::toFloat(voice_block.current_buffer_sample + 1) * t_inc;
      mono_float sample_inc = (1.0f / voice_block.total_samples);

      poly_int phase = voice_block.phase;
      poly_float current_phase_inc_mult = voice_block.from_phase_inc_mult;
      poly_float end_phase_inc_mult = voice_block.phase_inc_mult;
      poly_float delta_phase_inc_mult = (end_phase_inc_mult - current_phase_inc_mult) * sample_inc;
      current_phase_inc_mult += delta_phase_inc_mult * start;

      poly_mask double_mask = voice_block.shepard_double_mask;
      poly_mask half_mask = voice_block.shepard_half_mask;

      const poly_float* phase_inc_buffer = voice_block.phase_inc_buffer + start;
      const poly_int* phase_buffer = voice_block.phase_buffer + start;
      int num_samples = voice_block.end_sample - start;
      for (int i = 0; i < num_samples; ++i) {
        current_phase_inc_mult += delta_phase_inc_mult;
        phase += utils::toInt(phase_inc_buffer[i] * current_phase_inc_mult);
        poly_int adjusted_phase = phase + phase_buffer[i];
        audio_out[i] += interpolateShepardBuffers(from_buffers, to_buffers, adjusted_phase, t, double_mask, half_mask,
            voice_block.secondary_shepard_double_mask, voice_block.secondary_shepard_half_mask);
        t += t_inc;
      }

      return phase;
    }

    template<poly_int(*phaseDistort)(poly_int, poly_float, poly_int, const poly_float*, int),
             poly_float(*window)(poly_int, poly_int, poly_float, const poly_float*, int),
             poly_float(*interpolate)(const mono_float* const*, const mono_float* const*,
                                      const poly_int, poly_float), bool secondary = false>
    poly_int processDetuned(const SynthOscillator::VoiceBlock& voice_block, poly_float* audio_out) {
      const mono_float* const* from_buffers = voice_block.from_buffers;
      const mono_float* const* to_buffers = voice_block.to_buffers;

      int start = voice_block.start_sample;
      poly_float t_inc = 1.0f / voice_block.num_buffer_samples;
      poly_float t = utils::toFloat(voice_block.current_buffer_sample + 1) * t_inc;
      mono_float sample_inc = (1.0f / voice_block.total_samples);

      poly_int phase = voice_block.phase;
      poly_float current_phase_inc_mult = voice_block.from_phase_inc_mult;
      poly_float end_phase_inc_mult = voice_block.phase_inc_mult;
      poly_float delta_phase_inc_mult = (end_phase_inc_mult - current_phase_inc_mult) * sample_inc;
      current_phase_inc_mult += delta_phase_inc_mult * start;

      poly_int current_dist_phase = voice_block.last_distortion_phase;
      poly_int end_dist_phase = voice_block.distortion_phase;
      poly_int delta_dist_phase = utils::toInt(utils::toFloat(end_dist_phase - current_dist_phase) * sample_inc);
      current_dist_phase += delta_dist_phase * start;

      poly_float current_distortion = voice_block.last_distortion;
      poly_float distortion_inc = (voice_block.distortion - current_distortion) * sample_inc;
      current_distortion += distortion_inc * start;

      const bool shepard_wrap = secondary && hasShepardWrap(voice_block);
      poly_float secondary_amount = voice_block.last_secondary_distortion;
      poly_float secondary_delta = 0.0f;
      if constexpr (secondary) {
        secondary_delta = (voice_block.secondary_distortion - secondary_amount) * sample_inc;
        secondary_amount += secondary_delta * start;
      }
      const poly_float* modulation_buffer = voice_block.modulation_buffer + start;
      const poly_float* phase_inc_buffer = voice_block.phase_inc_buffer + start;
      const poly_int* phase_buffer = voice_block.phase_buffer + start;
      int num_samples = voice_block.end_sample - start;
      for (int i = 0; i < num_samples; ++i) {
        current_phase_inc_mult += delta_phase_inc_mult;
        phase += utils::toInt(phase_inc_buffer[i] * current_phase_inc_mult);
        poly_int adjusted_phase = phase + phase_buffer[i];
        current_distortion += distortion_inc;
        current_dist_phase += delta_dist_phase;
        poly_int distorted_phase = phaseDistort(adjusted_phase, current_distortion,
                                                current_dist_phase, modulation_buffer, i);
        poly_float mult = window(adjusted_phase, distorted_phase, current_distortion, modulation_buffer, i);
        poly_int read_phase = distorted_phase + current_dist_phase;
        if constexpr (secondary) {
          secondary_amount += secondary_delta;
          const poly_int next_phase = voice_block.secondary_phase_warp(
              read_phase, secondary_amount, 0, voice_block.secondary_modulation_buffer + start, i);
          mult *= voice_block.secondary_window(read_phase, next_phase, secondary_amount,
                                                voice_block.secondary_modulation_buffer + start, i);
          read_phase = next_phase;
        }
        poly_float result = interpolate(from_buffers, to_buffers, read_phase, t);
        if constexpr (secondary) {
          if (shepard_wrap)
            result = interpolateShepardBuffers(from_buffers, to_buffers, read_phase, t,
                voice_block.shepard_double_mask, voice_block.shepard_half_mask,
                voice_block.secondary_shepard_double_mask, voice_block.secondary_shepard_half_mask);
        }
        audio_out[i] += mult * result;
        t += t_inc;
      }

      return phase;
    }

    template<poly_int(*phaseDistort)(poly_int, poly_float, poly_int, const poly_float*, int),
             poly_float(*window)(poly_int, poly_int, poly_float, const poly_float*, int)>
    force_inline poly_int processDetuned(const SynthOscillator::VoiceBlock& voice_block, poly_float* audio_out) {
      if (voice_block.secondary_distortion_type != SynthOscillator::kNone ||
          (voice_block.secondary_spectral_morph != SynthOscillator::kNoSpectralMorph && hasShepardWrap(voice_block))) {
        if (voice_block.isStatic())
          return processDetuned<phaseDistort, window, interpolateBuffers, true>(voice_block, audio_out);
        return processDetuned<phaseDistort, window, interpolateMultipleBuffers, true>(voice_block, audio_out);
      }
      if (voice_block.isStatic())
        return processDetuned<phaseDistort, window, interpolateBuffers>(voice_block, audio_out);
      if (hasShepardWrap(voice_block))
        return processDetunedShepard(voice_block, audio_out);
      return processDetuned<phaseDistort, window, interpolateMultipleBuffers>(voice_block, audio_out);
    }

    poly_int processCenterShepard(const SynthOscillator::VoiceBlock& voice_block, poly_float* audio_out,
                                  poly_float current_center_amplitude, poly_float delta_center_amplitude,
                                  poly_float current_detuned_amplitude, poly_float delta_detuned_amplitude) {
      const mono_float* const* from_buffers = voice_block.from_buffers;
      const mono_float* const* to_buffers = voice_block.to_buffers;

      int start = voice_block.start_sample;

      poly_float t_inc = 1.0f / voice_block.num_buffer_samples;
      poly_float t = utils::toFloat(voice_block.current_buffer_sample + 1) * t_inc;
      mono_float sample_inc = (1.0f / voice_block.total_samples);

      poly_int phase = voice_block.phase;
      poly_float current_phase_inc_mult = voice_block.from_phase_inc_mult;
      poly_float end_phase_inc_mult = voice_block.phase_inc_mult;
      poly_float delta_phase_inc_mult = (end_phase_inc_mult - current_phase_inc_mult) * sample_inc;
      current_phase_inc_mult += delta_phase_inc_mult * start;

      poly_mask double_mask = voice_block.shepard_double_mask;
      poly_mask half_mask = voice_block.shepard_half_mask;

      const poly_float* phase_inc_buffer = voice_block.phase_inc_buffer + start;
      const poly_int* phase_buffer = voice_block.phase_buffer + start;
      int num_samples = voice_block.end_sample - start;
      for (int i = 0; i < num_samples; ++i) {
        current_phase_inc_mult += delta_phase_inc_mult;
        phase += utils::toInt(phase_inc_buffer[i] * current_phase_inc_mult);
        poly_int adjusted_phase = phase + phase_buffer[i];

        current_center_amplitude += delta_center_amplitude;
        current_detuned_amplitude += delta_detuned_amplitude;
        poly_float read = interpolateShepardBuffers(from_buffers, to_buffers, adjusted_phase,
                                                    t, double_mask, half_mask,
            voice_block.secondary_shepard_double_mask, voice_block.secondary_shepard_half_mask);
        audio_out[i] = current_center_amplitude * read + current_detuned_amplitude * audio_out[i];
        t += t_inc;
      }

      return phase;
    }

    template<poly_int(*phaseDistort)(poly_int, poly_float, poly_int, const poly_float*, int),
             poly_float(*window)(poly_int, poly_int, poly_float, const poly_float*, int),
             poly_float(*interpolate)(const mono_float* const*, const mono_float* const*,
                                      const poly_int, poly_float), bool secondary = false>
    poly_int processCenter(const SynthOscillator::VoiceBlock& voice_block, poly_float* audio_out,
                           poly_float current_center_amplitude, poly_float delta_center_amplitude,
                           poly_float current_detuned_amplitude, poly_float delta_detuned_amplitude) {
      const mono_float* const* from_buffers = voice_block.from_buffers;
      const mono_float* const* to_buffers = voice_block.to_buffers;

      int start = voice_block.start_sample;

      poly_float t_inc = 1.0f / voice_block.num_buffer_samples;
      poly_float t = utils::toFloat(voice_block.current_buffer_sample + 1) * t_inc;
      mono_float sample_inc = (1.0f / voice_block.total_samples);

      poly_int phase = voice_block.phase;
      poly_float current_phase_inc_mult = voice_block.from_phase_inc_mult;
      poly_float end_phase_inc_mult = voice_block.phase_inc_mult;
      poly_float delta_phase_inc_mult = (end_phase_inc_mult - current_phase_inc_mult) * sample_inc;
      current_phase_inc_mult += delta_phase_inc_mult * start;

      poly_int current_dist_phase = voice_block.last_distortion_phase;
      poly_int end_dist_phase = voice_block.distortion_phase;
      poly_int delta_dist_phase = utils::toInt(utils::toFloat(end_dist_phase - current_dist_phase) * sample_inc);
      current_dist_phase += delta_dist_phase * start;

      poly_float current_distortion = voice_block.last_distortion;
      poly_float distortion_inc = (voice_block.distortion - current_distortion) * sample_inc;
      current_distortion += distortion_inc * start;

      const bool shepard_wrap = secondary && hasShepardWrap(voice_block);
      poly_float secondary_amount = voice_block.last_secondary_distortion;
      poly_float secondary_delta = 0.0f;
      if constexpr (secondary) {
        secondary_delta = (voice_block.secondary_distortion - secondary_amount) * sample_inc;
        secondary_amount += secondary_delta * start;
      }
      const poly_float* modulation_buffer = voice_block.modulation_buffer + start;
      const poly_float* phase_inc_buffer = voice_block.phase_inc_buffer + start;
      const poly_int* phase_buffer = voice_block.phase_buffer + start;
      int num_samples = voice_block.end_sample - start;
      for (int i = 0; i < num_samples; ++i) {
        current_phase_inc_mult += delta_phase_inc_mult;
        phase += utils::toInt(phase_inc_buffer[i] * current_phase_inc_mult);
        poly_int adjusted_phase = phase + phase_buffer[i];
        current_distortion += distortion_inc;
        current_dist_phase += delta_dist_phase;
        current_center_amplitude += delta_center_amplitude;
        current_detuned_amplitude += delta_detuned_amplitude;
        poly_int distorted_phase = phaseDistort(adjusted_phase, current_distortion,
                                                current_dist_phase, modulation_buffer, i);
        poly_float mult = window(adjusted_phase, distorted_phase, current_distortion, modulation_buffer, i);
        poly_int read_phase = distorted_phase + current_dist_phase;
        if constexpr (secondary) {
          secondary_amount += secondary_delta;
          const poly_int next_phase = voice_block.secondary_phase_warp(
              read_phase, secondary_amount, 0, voice_block.secondary_modulation_buffer + start, i);
          mult *= voice_block.secondary_window(read_phase, next_phase, secondary_amount,
                                                voice_block.secondary_modulation_buffer + start, i);
          read_phase = next_phase;
        }
        poly_float result = interpolate(from_buffers, to_buffers, read_phase, t);
        if constexpr (secondary) {
          if (shepard_wrap)
            result = interpolateShepardBuffers(from_buffers, to_buffers, read_phase, t,
                voice_block.shepard_double_mask, voice_block.shepard_half_mask,
                voice_block.secondary_shepard_double_mask, voice_block.secondary_shepard_half_mask);
        }
        poly_float read = mult * result;
        poly_float center_value = current_center_amplitude * read;
        audio_out[i] = center_value + current_detuned_amplitude * audio_out[i];
        VITAL_ASSERT(utils::isFinite(audio_out[i]));

        t += t_inc;
      }

      return phase;
    }

    template<poly_int(*phaseDistort)(poly_int, poly_float, poly_int, const poly_float*, int),
             poly_float(*window)(poly_int, poly_int, poly_float, const poly_float*, int)>
    force_inline poly_int processCenter(const SynthOscillator::VoiceBlock& voice_block, poly_float* audio_out,
                                        poly_float current_center_amplitude, poly_float delta_center_amplitude,
                                        poly_float current_detuned_amplitude, poly_float delta_detuned_amplitude) {
      if (voice_block.secondary_distortion_type != SynthOscillator::kNone ||
          (voice_block.secondary_spectral_morph != SynthOscillator::kNoSpectralMorph && hasShepardWrap(voice_block))) {
        if (voice_block.isStatic())
          return processCenter<phaseDistort, window, interpolateBuffers, true>(voice_block, audio_out,
              current_center_amplitude, delta_center_amplitude, current_detuned_amplitude, delta_detuned_amplitude);
        return processCenter<phaseDistort, window, interpolateMultipleBuffers, true>(voice_block, audio_out,
            current_center_amplitude, delta_center_amplitude, current_detuned_amplitude, delta_detuned_amplitude);
      }
      if (voice_block.isStatic()) {
        return processCenter<phaseDistort, window, interpolateBuffers>(
            voice_block, audio_out,
            current_center_amplitude, delta_center_amplitude,
            current_detuned_amplitude, delta_detuned_amplitude);
      }
      if (hasShepardWrap(voice_block)) {
        return processCenterShepard(
            voice_block, audio_out,
            current_center_amplitude, delta_center_amplitude,
            current_detuned_amplitude, delta_detuned_amplitude);
      }
      return processCenter<phaseDistort, window, interpolateMultipleBuffers>(
          voice_block, audio_out,
          current_center_amplitude, delta_center_amplitude,
          current_detuned_amplitude, delta_detuned_amplitude);
    }

    template<class T>
    force_inline T compactAndLoadVoice(T* values, poly_mask active_mask) {
      T one = values[0];
      T two = utils::swapVoices(values[1]);
      return utils::maskLoad(two, one, active_mask);
    }

    template<class T>
    force_inline void expandAndWriteVoice(T* values, T value, poly_mask active_mask) {
      T two = utils::swapVoices(value);
      values[0] = utils::maskLoad(values[0], value, active_mask);
      values[1] = utils::maskLoad(values[1], two, active_mask);
    }

    force_inline void compactAndLoadVoice(const mono_float** dest, const mono_float* const* values,
                                          poly_mask active_mask) {
      const mono_float* const* position1 = values;
      const mono_float* const* position2 = values + poly_float::kSize;
      int index = active_mask[0] ? 0 : 2;
      dest[index] = position1[index];
      dest[index + 1] = position1[index + 1];
      dest[(index + 2) % poly_float::kSize] = position2[index];
      dest[(index + 3) % poly_float::kSize] = position2[index + 1];
    }

    void setPowerDistortionValues(poly_float* values, int num_values, float exponent, bool spread) {
      if (spread) {
        for (int i = 0; i < num_values; ++i)
          values[i] = futils::pow(2.0f, (values[i] - 0.5f) * 2.0f * exponent);
      }
      else {
        poly_float value = futils::pow(2.0f, (values[0] - 0.5f) * 2.0f * exponent);
        for (int i = 0; i < num_values; ++i)
          values[i] = value;
      }
    }
  }

  const mono_float SynthOscillator::kStackMultipliers[kNumUnisonStackTypes][kNumPolyPhase] = {
    { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f },
    { 0.5f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f },
    { 0.25f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f },
    { 1.0f, 2.0f, 1.0f, 2.0f, 1.0f, 2.0f, 1.0f, 2.0f },
    { 1.0f, 2.0f, 3.0f, 1.0f, 2.0f, 3.0f, 1.0f, 2.0f },
    { 1.0f, kFifthMult, 2.0f, 1.0f, kFifthMult, 2.0f, 1.0f, kFifthMult },
    { 1.0f, kFifthMult, 2.0f, 2.0f * kFifthMult, 4.0f, 1.0f, kFifthMult, 2.0f },
    { 1.0f, kMajorThirdMult, kFifthMult, 2.0f, 1.0f, kMajorThirdMult, kFifthMult, 2.0f },
    { 1.0f, kMinorThirdMult, kFifthMult, 2.0f, 1.0f, kMinorThirdMult, kFifthMult, 2.0f },
    { 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f },
    { 1.0f, 3.0f, 5.0f, 7.0f, 9.0f, 11.0f, 13.0f, 15.0f }
  };

  template<class Source>
  void runMorph(SynthOscillator::SpectralMorph morph_type, float morph_amount,
                const Source* wavetable_data, int wavetable_index,
                poly_float* dest, FourierTransform* transform, int h, const poly_float* random_values) {
    switch (morph_type) {
      case SynthOscillator::kVocode:
      case SynthOscillator::kFormScale:
        evenOddVocodeMorph(wavetable_data, wavetable_index, dest, transform, morph_amount, h, nullptr);
        break;
      case SynthOscillator::kHarmonicScale:
        harmonicScaleMorph(wavetable_data, wavetable_index, dest, transform, morph_amount, h, nullptr);
        break;
      case SynthOscillator::kInharmonicScale:
        inharmonicScaleMorph(wavetable_data, wavetable_index, dest, transform, morph_amount, h, nullptr);
        break;
      case SynthOscillator::kSmear:
        smearMorph(wavetable_data, wavetable_index, dest, transform, morph_amount, h, nullptr);
        break;
      case SynthOscillator::kRandomAmplitudes:
        randomAmplitudeMorph(wavetable_data, wavetable_index,
                             dest, transform, morph_amount, h, random_values);
        break;
      case SynthOscillator::kShepardTone:
        shepardMorph(wavetable_data, wavetable_index, dest, transform, morph_amount, h, nullptr);
        break;
      case SynthOscillator::kLowPass:
        lowPassMorph(wavetable_data, wavetable_index, dest, transform, morph_amount, h, nullptr);
        break;
      case SynthOscillator::kHighPass:
        highPassMorph(wavetable_data, wavetable_index, dest, transform, morph_amount, h, nullptr);
        break;
      case SynthOscillator::kPhaseDisperse:
        phaseMorph(wavetable_data, wavetable_index, dest, transform, morph_amount, h, nullptr);
        break;
      case SynthOscillator::kSkew:
        wavetableSkewMorph(wavetable_data, wavetable_index, dest, transform, morph_amount, h, nullptr);
        break;
      default:
        passthroughMorph(wavetable_data, wavetable_index, dest, transform, morph_amount, h, nullptr);
    }
  }

  // Like the FFT workspace, this scratch cache is shared by the oscillator's
  // voice clones, which the voice handler processes sequentially. It is fully
  // allocated at construction and never allocates on the audio thread.
  class SpectralMorphChain {
    public:
      struct Frames {
        SpectralMorphChain* owner;
        int field;
        const poly_float* operator[](int frame) const { return owner->getFrame(frame, field); }
      };
      Frames frequency_amplitudes { this, 0 };
      Frames normalized_frequencies { this, 1 };
      Frames phases { this, 2 };
      int num_frames = 0;

      void prepare(const Wavetable::WavetableData* source, SynthOscillator::SpectralMorph type,
                   float amount, bool need_phases, const poly_float* random_values) {
        source_ = source;
        type_ = type;
        amount_ = amount;
        need_phases_ = need_phases;
        random_values_ = random_values;
        num_frames = source->num_frames;
        clock_ = 0;
        for (auto& frame : cache_) { frame.index = -1; frame.used = 0; }
      }

    private:
      struct Frame {
        int index = -1;
        int used = 0;
        poly_float values[3][Wavetable::kPolyFrequencySize] {};
      };
      const poly_float* getFrame(int index, int field) {
        index = utils::iclamp(index, 0, num_frames - 1);
        Frame* oldest = &cache_[0];
        for (auto& frame : cache_) {
          if (frame.index == index) {
            frame.used = ++clock_;
            return frame.values[field];
          }
          if (frame.used < oldest->used) oldest = &frame;
        }
        oldest->index = index;
        oldest->used = ++clock_;
        memset(spectrum_, 0, sizeof(spectrum_));
        runMorph(type_, amount_, source_, index, spectrum_, nullptr, Wavetable::kWaveformSize / 2, random_values_);
        const float* bins = reinterpret_cast<const float*>(spectrum_ + 1);
        float* amplitudes = reinterpret_cast<float*>(oldest->values[0]);
        float* normalized = reinterpret_cast<float*>(oldest->values[1]);
        float* phases = reinterpret_cast<float*>(oldest->values[2]);
        for (int h = 0; h < Wavetable::kNumHarmonics; ++h) {
          const float real = bins[2 * h];
          const float imag = bins[2 * h + 1];
          const float amplitude = std::sqrt(real * real + imag * imag);
          amplitudes[2 * h] = amplitudes[2 * h + 1] = amplitude;
          const float inverse = amplitude > 0.0f ? 1.0f / amplitude : 0.0f;
          normalized[2 * h] = amplitude > 0.0f ? real * inverse : 1.0f;
          normalized[2 * h + 1] = imag * inverse;
          if (need_phases_)
            phases[2 * h] = phases[2 * h + 1] = std::atan2(imag, real);
        }
        return oldest->values[field];
      }
      Frame cache_[4];
      poly_float spectrum_[SynthOscillator::kSpectralBufferSize] {};
      const Wavetable::WavetableData* source_ = nullptr;
      SynthOscillator::SpectralMorph type_ = SynthOscillator::kNoSpectralMorph;
      float amount_ = 0.0f;
      const poly_float* random_values_ = nullptr;
      bool need_phases_ = false;
      int clock_ = 0;
  };

  SynthOscillator::VoiceBlock::VoiceBlock() : start_sample(0), end_sample(0), total_samples(0),
                                              phase(0), phase_inc_mult(0.0f), from_phase_inc_mult(0.0f),
                                              shepard_double_mask(0), shepard_half_mask(0),
                                              distortion_phase(0), last_distortion_phase(0),
                                              distortion(0.0f), last_distortion(0.0f),
                                              num_buffer_samples(0), current_buffer_sample(0),
                                              smoothing_enabled(false), spectral_morph(kNoSpectralMorph),
                                              modulation_buffer(nullptr) {
    const mono_float* default_buffer = Wavetable::null_waveform();
    for (int i = 0; i < poly_int::kSize; ++i) {
      from_buffers[i] = default_buffer;
      to_buffers[i] = default_buffer;
    }
  }

  SynthOscillator::SynthOscillator(Wavetable* wavetable) :
      Processor(kNumInputs, kNumOutputs), random_generator_(-1.0f, 1.0f),
      transpose_quantize_(0), last_quantized_transpose_(0.0f), last_quantize_ratio_(1.0f),
      unison_(1), active_oscillators_(2), wavetable_(wavetable), wavetable_version_(wavetable->getVersion()),
      modulators_{} {
    pan_amplitude_ = 0.0f;
    center_amplitude_ = 0.0f;
    detuned_amplitude_ = 0.0f;
    distortion_phase_ = 0.0f;
    blend_stereo_multiply_ = 0.0f;
    blend_center_multiply_ = 0.0f;

    for (int i = 0; i < kNumPolyPhase; ++i) {
      // Before the first note, the centre pair shares a generator so a single
      // voice stays centred; notes reseed every lane.
      for (int lane = 0; lane < poly_float::kSize; ++lane)
        analog_states_[i].random.set(lane, analog::mixSeed(i * poly_float::kSize + (i == 0 ? lane | 1 : lane) + 1));
      phases_[i] = 0;
      phase_inc_mults_[i] = 1.0f;
      from_phase_inc_mults_[i] = 1.0f;
      shepard_double_masks_[i] = 0;
      shepard_half_masks_[i] = 0;
      waiting_shepard_double_masks_[i] = 0;
      waiting_shepard_half_masks_[i] = 0;
      detunings_[i] = 1.0f;
      spectral_morph_values_[i] = 0.0f;
      last_spectral_morph_values_[i] = 1.0f;
      distortion_values_[i] = 0.0f;
      last_distortion_values_[i] = 0.0f;
    }

    resetWavetableBuffers();

    fourier_transform_ = std::make_shared<FourierTransform>(kWaveformBits);
    spectral_chain_ = std::make_shared<SpectralMorphChain>();
    random_amplitudes_ = std::make_shared<RandomValues>();
    secondary_random_amplitudes_ = std::make_shared<RandomValues>();
    phase_inc_buffer_ = std::make_shared<Output>();
    phase_buffer_ = std::make_shared<PhaseBuffer>();
    voice_block_.phase_inc_buffer = phase_inc_buffer_->buffer;
    voice_block_.phase_buffer = phase_buffer_->buffer;
    voice_block_.modulation_buffer = null_source_.buffer;
    RandomValues::instance();
  }

  void SynthOscillator::reset(poly_mask reset_mask, poly_int sample) {
    reset(reset_mask);
    voice_block_.current_buffer_sample = utils::maskLoad(voice_block_.current_buffer_sample, -sample, reset_mask);
  }

  void SynthOscillator::reset(poly_mask reset_mask) {
    poly_float random_amount = input(kRandomPhase)->at(0);
    poly_float frame_spread = isAnalog() ? poly_float(0.0f) : input(kUnisonFrameSpread)->at(0);
    last_quantize_ratio_ = utils::maskLoad(last_quantize_ratio_, 1.0f, reset_mask);

    for (int v = 0; v < kNumVoicesPerProcess; ++v) {
      if (reset_mask[v * 2]) {
        for (int i = 0; i < kNumPolyPhase; ++i) {
          uint32_t phase_offset_left = getFrameSpreadPhaseOffset(i, active_oscillators_, frame_spread[2 * v]);
          uint32_t phase_offset_right = getFrameSpreadPhaseOffset(i, active_oscillators_, frame_spread[2 * v + 1]);
          // Through a signed integer, so a negative offset wraps around the cycle on every
          // platform; a negative float converted straight to unsigned is 0 on arm64.
          uint32_t random_phase_left = static_cast<int64_t>(random_generator_.next() * random_amount[2 * v] * INT_MAX);
          uint32_t random_phase_right = static_cast<int64_t>(random_generator_.next() * random_amount[2 * v + 1] * INT_MAX);
          phases_[i].set(2 * v, random_phase_left + phase_offset_left);
          phases_[i].set(2 * v + 1, random_phase_right + phase_offset_right);

          int buffer_index = i * poly_float::kSize + 2 * v;
          last_buffers_[buffer_index] = wave_buffers_[buffer_index];
          last_buffers_[buffer_index + 1] = wave_buffers_[buffer_index + 1];
        }

        if (unison_ < active_oscillators_)
          phases_[0].set(v * 2, phases_[0][v * 2 + 1]);
      }
    }

    for (int i = 0; i < kNumPolyPhase; ++i) {
      last_secondary_distortion_values_[i] = utils::maskLoad(last_secondary_distortion_values_[i], secondary_distortion_values_[i], reset_mask);
      last_secondary_spectral_morph_values_[i] = utils::maskLoad(last_secondary_spectral_morph_values_[i], secondary_spectral_morph_values_[i], reset_mask);
      secondary_shepard_double_masks_[i] &= ~reset_mask;
      secondary_shepard_half_masks_[i] &= ~reset_mask;
      secondary_waiting_shepard_double_masks_[i] &= ~reset_mask;
      secondary_waiting_shepard_half_masks_[i] &= ~reset_mask;
      last_distortion_values_[i] = utils::maskLoad(last_distortion_values_[i], distortion_values_[i], reset_mask);
      last_spectral_morph_values_[i] = utils::maskLoad(last_spectral_morph_values_[i],
                                                       spectral_morph_values_[i], reset_mask);
      from_phase_inc_mults_[i] = utils::maskLoad(from_phase_inc_mults_[i], phase_inc_mults_[i], reset_mask);
      shepard_double_masks_[i] = shepard_double_masks_[i] & ~reset_mask;
      shepard_half_masks_[i] = shepard_half_masks_[i] & ~reset_mask;
      waiting_shepard_double_masks_[i] = waiting_shepard_double_masks_[i] & ~reset_mask;
      waiting_shepard_half_masks_[i] = waiting_shepard_half_masks_[i] & ~reset_mask;
    }
  }

  void SynthOscillator::setPhaseIncMults() {
    poly_float range = input(kDetuneRange)->at(0);
    poly_float cents = range * input(kUnisonDetune)->at(0);
    poly_float power = input(kDetunePower)->at(0);
    int stack_style = std::roundf(input(kStackStyle)->at(0)[0]);
    const mono_float* stack_settings = kStackMultipliers[stack_style];

    mono_float divisor = utils::max(1.0f, unison_ - 1.0f);
    int bump = (unison_ % 2 == 0) ? 1 : 0;

    poly_mask sharp_flat_mask = constants::kLeftMask;
    int num_updates = active_oscillators_ / 2;
    for (int i = 0; i < num_updates; ++i) {
      mono_float t = (2 * i + bump) / divisor;
      poly_float adjusted_t = futils::powerScale(t, power);
      poly_float oscillator_cents = adjusted_t * cents;

      poly_float up_ratio = utils::centsToRatio(oscillator_cents);
      poly_float down_ratio = poly_float(1.0f) / up_ratio;
      detunings_[i] = utils::maskLoad(up_ratio, down_ratio, sharp_flat_mask) * stack_settings[i];
      from_phase_inc_mults_[i] = phase_inc_mults_[i];
      phase_inc_mults_[i] = detunings_[i];

      sharp_flat_mask = ~sharp_flat_mask;
    }
  }

  force_inline void SynthOscillator::setupShepardWrap(bool secondary) {
    const poly_float* spectral_morph_values_ = secondary ? secondary_spectral_morph_values_ : this->spectral_morph_values_;
    const poly_float* last_spectral_morph_values_ = secondary ? last_secondary_spectral_morph_values_ : this->last_spectral_morph_values_;
    poly_int* waiting_shepard_double_masks_ = secondary ? secondary_waiting_shepard_double_masks_ : this->waiting_shepard_double_masks_;
    poly_int* waiting_shepard_half_masks_ = secondary ? secondary_waiting_shepard_half_masks_ : this->waiting_shepard_half_masks_;
    int num_phase_updates = active_oscillators_ / 2;

    poly_float ratio_div = poly_float(1.0f) / last_quantize_ratio_;
    for (int i = 0; i < num_phase_updates; ++i) {
      poly_float spectral_diff = last_spectral_morph_values_[i] - spectral_morph_values_[i];
      poly_float mult = futils::exp2(-spectral_morph_values_[i]);
      phase_inc_mults_[i] *= mult;
      detunings_[i] *= mult;

      poly_mask double_mask = waiting_shepard_double_masks_[i] | poly_float::lessThan(spectral_diff, -0.6f);
      poly_mask half_mask = waiting_shepard_half_masks_[i] | poly_float::greaterThan(spectral_diff, 0.6f);

      phase_inc_mults_[i] = utils::maskLoad(phase_inc_mults_[i], phase_inc_mults_[i] * 2.0f, double_mask);
      phase_inc_mults_[i] = utils::maskLoad(phase_inc_mults_[i], phase_inc_mults_[i] * 0.5f, half_mask);
      poly_float reset_phase_inc_mult = from_phase_inc_mults_[i] * ratio_div;
      from_phase_inc_mults_[i] = utils::maskLoad(from_phase_inc_mults_[i], reset_phase_inc_mult,
                                                 half_mask | double_mask);

      waiting_shepard_double_masks_[i] = double_mask;
      waiting_shepard_half_masks_[i] = half_mask;
    }
  }

  force_inline void SynthOscillator::clearShepardWrap() {
    int num_phase_updates = active_oscillators_ / 2;

    for (int i = 0; i < num_phase_updates; ++i) {
      shepard_double_masks_[i] = 0;
      shepard_half_masks_[i] = 0;
      waiting_shepard_double_masks_[i] = 0;
      waiting_shepard_half_masks_[i] = 0;
    }
  }

  force_inline void SynthOscillator::doShepardWrap(poly_mask new_buffer_mask, int quantize, bool secondary) {
    poly_int* waiting_shepard_double_masks_ = secondary ? secondary_waiting_shepard_double_masks_ : this->waiting_shepard_double_masks_;
    poly_int* waiting_shepard_half_masks_ = secondary ? secondary_waiting_shepard_half_masks_ : this->waiting_shepard_half_masks_;
    poly_int* shepard_double_masks_ = secondary ? secondary_shepard_double_masks_ : this->shepard_double_masks_;
    poly_int* shepard_half_masks_ = secondary ? secondary_shepard_half_masks_ : this->shepard_half_masks_;
    int num_phase_updates = active_oscillators_ / 2;

    if (quantize) {
      for (int i = 0; i < num_phase_updates; ++i) {
        poly_mask double_mask = waiting_shepard_double_masks_[i] & new_buffer_mask;
        poly_mask half_mask = waiting_shepard_half_masks_[i] & new_buffer_mask;
        waiting_shepard_double_masks_[i] = waiting_shepard_double_masks_[i] & ~new_buffer_mask;
        waiting_shepard_half_masks_[i] = waiting_shepard_half_masks_[i] & ~new_buffer_mask;

        phase_inc_mults_[i] = utils::maskLoad(phase_inc_mults_[i], phase_inc_mults_[i] * 0.5f, double_mask);
        phases_[i] = utils::maskLoad(phases_[i], utils::shiftRight<1>(phases_[i]), double_mask);

        phase_inc_mults_[i] = utils::maskLoad(phase_inc_mults_[i], phase_inc_mults_[i] * 2.0f, half_mask);
        phases_[i] = utils::maskLoad(phases_[i], phases_[i] * 2, half_mask);
        from_phase_inc_mults_[i] = utils::maskLoad(from_phase_inc_mults_[i], phase_inc_mults_[i],
                                                   double_mask | half_mask);

        shepard_double_masks_[i] = utils::maskLoad(shepard_double_masks_[i], double_mask, new_buffer_mask);
        shepard_half_masks_[i] = utils::maskLoad(shepard_half_masks_[i], half_mask, new_buffer_mask);
      }
    }
    else {
      for (int i = 0; i < num_phase_updates; ++i) {
        poly_mask double_mask = waiting_shepard_double_masks_[i] & new_buffer_mask;
        poly_mask half_mask = waiting_shepard_half_masks_[i] & new_buffer_mask;
        waiting_shepard_double_masks_[i] = waiting_shepard_double_masks_[i] & ~new_buffer_mask;
        waiting_shepard_half_masks_[i] = waiting_shepard_half_masks_[i] & ~new_buffer_mask;

        phase_inc_mults_[i] = utils::maskLoad(phase_inc_mults_[i], phase_inc_mults_[i] * 0.5f, double_mask);
        from_phase_inc_mults_[i] = utils::maskLoad(from_phase_inc_mults_[i],
                                                   from_phase_inc_mults_[i] * 0.5f, double_mask);
        phases_[i] = utils::maskLoad(phases_[i], utils::shiftRight<1>(phases_[i]), double_mask);

        phase_inc_mults_[i] = utils::maskLoad(phase_inc_mults_[i], phase_inc_mults_[i] * 2.0f, half_mask);
        from_phase_inc_mults_[i] = utils::maskLoad(from_phase_inc_mults_[i], from_phase_inc_mults_[i] * 2, half_mask);
        phases_[i] = utils::maskLoad(phases_[i], phases_[i] * 2.0f, half_mask);

        shepard_double_masks_[i] = utils::maskLoad(shepard_double_masks_[i], double_mask, new_buffer_mask);
        shepard_half_masks_[i] = utils::maskLoad(shepard_half_masks_[i], half_mask, new_buffer_mask);
      }
    }
  }

  force_inline void SynthOscillator::setAmplitude() {
    if (unison_ <= 2) {
      center_amplitude_ = 1.0f;
      detuned_amplitude_ = 0.0f;
      return;
    }

    poly_float blend = input(kBlend)->at(0);
    poly_float center = utils::interpolate(1.0f, kCenterLowAmplitude, blend);
    poly_float detuned_blend = -blend + 1.0f;
    detuned_blend *= detuned_blend;
    poly_float detuned = utils::interpolate(kDetunedHighAmplitude, 0.0f, detuned_blend);

    int half_oscillators = active_oscillators_ / 2;
    poly_float square_sums = center * center + detuned * detuned * (half_oscillators - 1);
    poly_float adjustment = poly_float(1.0f) / utils::sqrt(square_sums);
    center_amplitude_ = adjustment * center;
    detuned_amplitude_ = adjustment * detuned;
  }

  void SynthOscillator::setWaveBuffers(poly_float phase_inc, int index) {
    if (voice_block_.spectral_morph == kShepardTone)
      setFourierWaveBuffers<shepardMorph>(phase_inc, index, false);
    else if (voice_block_.spectral_morph == kVocode)
      setFourierWaveBuffers<evenOddVocodeMorph>(phase_inc, index, true);
    else if (voice_block_.spectral_morph == kFormScale)
      setFourierWaveBuffers<evenOddVocodeMorph>(phase_inc, index, false);
    else if (voice_block_.spectral_morph == kHarmonicScale)
      setFourierWaveBuffers<harmonicScaleMorph>(phase_inc, index, false);
    else if (voice_block_.spectral_morph == kInharmonicScale)
      setFourierWaveBuffers<inharmonicScaleMorph>(phase_inc, index, false);
    else if (voice_block_.spectral_morph == kSmear)
      setFourierWaveBuffers<smearMorph>(phase_inc, index, false);
    else if (voice_block_.spectral_morph == kRandomAmplitudes)
      setFourierWaveBuffers<randomAmplitudeMorph>(phase_inc, index, false);
    else if (voice_block_.spectral_morph == kLowPass)
      setFourierWaveBuffers<lowPassMorph>(phase_inc, index, false);
    else if (voice_block_.spectral_morph == kHighPass)
      setFourierWaveBuffers<highPassMorph>(phase_inc, index, false);
    else if (voice_block_.spectral_morph == kPhaseDisperse)
      setFourierWaveBuffers<phaseMorph>(phase_inc, index, false);
    else if (voice_block_.spectral_morph == kSkew)
      setFourierWaveBuffers<wavetableSkewMorph>(phase_inc, index, false);
    else
      setFourierWaveBuffers<passthroughMorph>(phase_inc, index, false);

    voice_block_.current_buffer_sample.set(index, 0);
    voice_block_.current_buffer_sample.set(index + 1, 0);
  }

  template<void(*spectralMorph)(const Wavetable::WavetableData*, int, poly_float*,
                                FourierTransform*, float, int, const poly_float*)>
  void SynthOscillator::computeSpectralWaveBufferPair(int phase_update, int index, bool formant_shift,
                                                      float phase_adjustment, poly_int wave_index,
                                                      poly_float voice_increment, poly_float morph_amount) {
    for (int i = index; i < index + 2; ++i) {
      mono_float adjust_phase_inc = voice_increment[i] * phase_adjustment;
      mono_float formant_adjustment = voice_increment[i] * Wavetable::kWaveformSize;
      float bin = Wavetable::getFrequencyFloatBin(adjust_phase_inc);
      int buffer_index = phase_update * poly_float::kSize + i;
      last_buffers_[buffer_index] = wave_buffers_[buffer_index];

      poly_float* fourier_buffer = fourier_frames_.first(buffer_index);
      mono_float* destination = ((mono_float*)fourier_buffer) + poly_float::kSize - 1;
      if (destination == wave_buffers_[buffer_index])
        fourier_buffer = fourier_frames_.second(buffer_index);

      float shift = morph_amount[i];
      if (formant_shift)
        shift *= formant_adjustment;
      const Wavetable::WavetableData* wavetable_data = wavetable_->getAllActiveData();
      int table_index = std::min<int>(wave_index[i], wavetable_data->num_frames - 1);

      float bin_shift = Wavetable::kFrequencyBins + 1.0f - bin;
      int last_harmonic = std::max<int>(0, WaveFrame::kWaveformSize * futils::exp2(-bin_shift));
      last_harmonic = std::min(last_harmonic, WaveFrame::kWaveformSize / 2);

      const SpectralMorph secondary = voice_block_.secondary_spectral_morph;
      const poly_float secondary_values = secondary_spectral_morph_values_[phase_update];
      float secondary_amount = secondary_values[i];
      if (secondary == kVocode)
        secondary_amount *= formant_adjustment;
      const poly_float* primary_random = voice_block_.spectral_morph == kRandomAmplitudes
          ? random_amplitudes_->buffer(static_cast<int>(input(kSpectralMorphSeed)->at(0)[0])) : nullptr;
      const poly_float* secondary_random = secondary == kRandomAmplitudes
          ? secondary_random_amplitudes_->buffer(static_cast<int>(input(kSecondarySpectralMorphSeed)->at(0)[0])) : nullptr;
      if (secondary == kNoSpectralMorph) {
        spectralMorph(wavetable_data, table_index, fourier_buffer,
                      fourier_transform_.get(), shift, last_harmonic, primary_random);
      }
      else if (voice_block_.spectral_morph == kNoSpectralMorph) {
        runMorph(secondary, secondary_amount, wavetable_data, table_index, fourier_buffer,
                 fourier_transform_.get(), last_harmonic, secondary_random);
      }
      else {
        spectral_chain_->prepare(wavetable_data, voice_block_.spectral_morph, shift, secondary == kShepardTone,
                                 primary_random);
        runMorph(secondary, secondary_amount, spectral_chain_.get(), table_index, fourier_buffer,
                 fourier_transform_.get(), last_harmonic, secondary_random);
      }
      wave_buffers_[buffer_index] = ((mono_float*)fourier_buffer) + poly_float::kSize - 1;

      if (i == index && morph_amount[i] == morph_amount[i + 1] && wave_index[i] == wave_index[i + 1] &&
          (secondary == kNoSpectralMorph || (secondary_values[i] == secondary_values[i + 1] &&
           ((secondary != kVocode && !formant_shift) || voice_increment[i] == voice_increment[i + 1])))) {
        last_buffers_[buffer_index + 1] = wave_buffers_[buffer_index + 1];
        wave_buffers_[buffer_index + 1] = wave_buffers_[buffer_index];
        return;
      }
    }
  }
  
  template<void(*spectralMorph)(const Wavetable::WavetableData*, int, poly_float*,
                                FourierTransform*, float, int, const poly_float*)>
  void SynthOscillator::setFourierWaveBuffers(poly_float phase_inc, int index, bool formant_shift) {
    poly_float wave_frame = input(kWaveFrame)->at(0);
    poly_float frame_spread = input(kUnisonFrameSpread)->at(0);
    phase_inc = utils::max(0.0f, phase_inc);
    float phase_inc_adjustment = getPhaseIncAdjustment();
    
    DistortionType distortion_type = static_cast<DistortionType>((int)input(kDistortionType)->at(0)[0]);
    poly_mask distortion_frequency_mask = 0;
    mono_float distortion_mult = 1.0f;
    if (distortion_type == kFormant || distortion_type == kSync) {
      distortion_frequency_mask = constants::kFullMask;
      distortion_mult = kMaxSync;
    }

    SpectralMorph spectral_morph = static_cast<SpectralMorph>((int)input(kSpectralMorphType)->at(0)[0]);
    poly_mask spectral_unison_mask = poly_float::notEqual(input(kSpectralUnison)->at(0), 0.0f);
    // A spread on either morph amount gives the unison voices different morphs.
    poly_mask spectral_morph_mask = poly_float::notEqual(spectral_morph_values_[0], spectral_morph_values_[1]) |
                                    poly_float::notEqual(secondary_spectral_morph_values_[0],
                                                         secondary_spectral_morph_values_[1]);
    poly_mask frame_spread_mask = poly_float::notEqual(frame_spread, 0.0f);
    bool spectral_unison = spectral_unison_mask.anyMask() &&
                           (spectral_morph_mask.anyMask() || frame_spread_mask.anyMask() || spectral_morph == kVocode ||
                            voice_block_.secondary_spectral_morph == kVocode);

    const bool secondary_sync = voice_block_.secondary_distortion_type == kSync ||
                                voice_block_.secondary_distortion_type == kFormant;
    int num_phase_updates = active_oscillators_ / 2;
    if (spectral_unison) {
      float t_inc = 1.0f / (utils::imax(2, num_phase_updates) - 1.0f);
      for (int v = 0; v < num_phase_updates; ++v) {
        poly_float frequency_mult = distortion_values_[v] * distortion_mult;
        frequency_mult = utils::maskLoad(1.0f, frequency_mult, distortion_frequency_mask);
        if (secondary_sync) frequency_mult *= secondary_distortion_values_[v] * kMaxSync;

        poly_float morph_amount = spectral_morph_values_[v];
        poly_float voice_increment = phase_inc * detunings_[v] * frequency_mult;
        poly_float t = v * t_inc;
        poly_float frame = wave_frame + t * frame_spread;
        poly_int wave_index = utils::toInt(utils::clamp(frame, 0.0f, kNumOscillatorWaveFrames - 1));

        computeSpectralWaveBufferPair<spectralMorph>(v, index, formant_shift, phase_inc_adjustment,
                                                     wave_index, voice_increment, morph_amount);
      }
    }
    else {
      poly_float frequency_mult = distortion_values_[0] * distortion_mult;
      frequency_mult = utils::maskLoad(1.0f, frequency_mult, distortion_frequency_mask);
      if (secondary_sync) frequency_mult *= secondary_distortion_values_[0] * kMaxSync;

      poly_float morph_amount = spectral_morph_values_[0];
      poly_float voice_increment = phase_inc * detunings_[0] * frequency_mult;
      poly_int wave_index = utils::toInt(utils::clamp(wave_frame, 0.0f, kNumOscillatorWaveFrames - 1));

      computeSpectralWaveBufferPair<spectralMorph>(0, index, formant_shift, phase_inc_adjustment,
                                                   wave_index, voice_increment, morph_amount);

      for (int v = 1; v < num_phase_updates; ++v) {
        for (int i = index; i < index + 2; ++i) {
          int buffer_index = v * poly_float::kSize + i;
          last_buffers_[buffer_index] = wave_buffers_[buffer_index];
          wave_buffers_[buffer_index] = wave_buffers_[i];
        }
      }
    }
  }

  void SynthOscillator::stereoBlend(poly_float* audio_out, int num_samples, poly_mask reset_mask) {
    poly_float stereo_spread = utils::clamp(input(kStereoSpread)->at(0), 0.0f, 1.0f);

    poly_float current_stereo_mult = blend_stereo_multiply_;
    poly_float current_center_mult = blend_center_multiply_;
    blend_stereo_multiply_ = futils::equalPowerFade(stereo_spread * 0.5f + 0.5f);
    blend_center_multiply_ = futils::equalPowerFadeInverse(stereo_spread * 0.5f + 0.5f);

    current_stereo_mult = utils::maskLoad(current_stereo_mult, blend_stereo_multiply_, reset_mask);
    current_center_mult = utils::maskLoad(current_center_mult, blend_center_multiply_, reset_mask);
    poly_float delta_stereo_mult = (blend_stereo_multiply_ - current_stereo_mult) * (1.0f / num_samples);
    poly_float delta_center_mult = (blend_center_multiply_ - current_center_mult) * (1.0f / num_samples);

    if (delta_stereo_mult.sum() + delta_center_mult.sum() == 0.0f && utils::equal(stereo_spread, 1.0f))
      return;

    for (int i = 0; i < num_samples; ++i) {
      current_stereo_mult += delta_stereo_mult;
      current_center_mult += delta_center_mult;
      poly_float val = audio_out[i];
      poly_float swap = utils::swapStereo(val);
      audio_out[i] = val * current_stereo_mult + swap * current_center_mult;
    }
  }

  void SynthOscillator::levelOutput(poly_float* audio_out, const poly_float* raw_out,
                                    int num_samples, poly_mask reset_mask) {
    VITAL_ASSERT(inputMatchesBufferSize(kAmplitude));

    poly_float current_pan_amplitude = pan_amplitude_;
    pan_amplitude_ = futils::panAmplitude(utils::clamp(input(kPan)->at(0), -1.0f, 1.0f));

    current_pan_amplitude = utils::maskLoad(current_pan_amplitude, pan_amplitude_, reset_mask);
    poly_float delta_pan_amplitude = (pan_amplitude_ - current_pan_amplitude) * (1.0f / num_samples);

    const poly_float* amplitude = input(kAmplitude)->source->buffer;
    poly_float zero = 0.0f;
    for (int i = 0; i < num_samples; ++i) {
      poly_float amp = utils::max(amplitude[i], zero);
      current_pan_amplitude += delta_pan_amplitude;
      audio_out[i] = current_pan_amplitude * raw_out[i] * amp * amp;

      VITAL_ASSERT(utils::isFinite(audio_out[i]));
    }
  }

  void SynthOscillator::convertVoiceChannels(int num_samples, poly_float* audio_out, poly_mask active_mask) {
    for (int i = 0; i < num_samples; ++i)
      audio_out[i] += utils::swapVoices(audio_out[i]);
  }

  force_inline void SynthOscillator::resetWavetableBuffers() {
    const mono_float* default_buffer = Wavetable::null_waveform();
    for (int i = 0; i < kNumBuffers; ++i) {
      last_buffers_[i] = default_buffer;
      wave_buffers_[i] = default_buffer;
    }
  }

  force_inline void SynthOscillator::loadVoiceBlock(VoiceBlock& voice_block, int index, poly_mask active_mask) {
    bool single_voice = (~active_mask).anyMask();
    if (single_voice) {
      voice_block.phase = compactAndLoadVoice(phases_ + 2 * index, active_mask);
      voice_block.phase_inc_mult = compactAndLoadVoice(phase_inc_mults_ + 2 * index, active_mask);
      voice_block.from_phase_inc_mult = compactAndLoadVoice(from_phase_inc_mults_ + 2 * index, active_mask);
      voice_block.shepard_double_mask = compactAndLoadVoice(shepard_double_masks_ + 2 * index, active_mask);
      voice_block.shepard_half_mask = compactAndLoadVoice(shepard_half_masks_ + 2 * index, active_mask);
      voice_block.distortion = compactAndLoadVoice(distortion_values_ + 2 * index, active_mask);
      voice_block.last_distortion = compactAndLoadVoice(last_distortion_values_ + 2 * index, active_mask);
      voice_block.secondary_distortion = compactAndLoadVoice(secondary_distortion_values_ + 2 * index, active_mask);
      voice_block.last_secondary_distortion = compactAndLoadVoice(last_secondary_distortion_values_ + 2 * index, active_mask);
      voice_block.secondary_shepard_double_mask = compactAndLoadVoice(secondary_shepard_double_masks_ + 2 * index, active_mask);
      voice_block.secondary_shepard_half_mask = compactAndLoadVoice(secondary_shepard_half_masks_ + 2 * index, active_mask);
      poly_int distortion_phase_swap = utils::swapVoices(voice_block.distortion_phase);
      voice_block.distortion_phase = utils::maskLoad(distortion_phase_swap, voice_block.distortion_phase, active_mask);
      poly_int last_distortion_phase_swap = utils::swapVoices(voice_block.last_distortion_phase);
      voice_block.last_distortion_phase = utils::maskLoad(last_distortion_phase_swap, voice_block.last_distortion_phase,
                                                          active_mask);

      int buffer_index = 2 * index * poly_float::kSize;
      compactAndLoadVoice(voice_block.from_buffers, last_buffers_ + buffer_index, active_mask);
      compactAndLoadVoice(voice_block.to_buffers, wave_buffers_ + buffer_index, active_mask);

      if ((index + 1) * poly_float::kSize > active_oscillators_) {
        int zero_index = active_mask[0] ? 2 : 0;
        voice_block.from_buffers[zero_index] = Wavetable::null_waveform();
        voice_block.from_buffers[zero_index + 1] = Wavetable::null_waveform();
        voice_block.to_buffers[zero_index] = Wavetable::null_waveform();
        voice_block.to_buffers[zero_index + 1] = Wavetable::null_waveform();
      }
    }
    else {
      voice_block.phase = phases_[index];
      voice_block.phase_inc_mult = phase_inc_mults_[index];
      voice_block.from_phase_inc_mult = from_phase_inc_mults_[index];
      voice_block.shepard_double_mask = shepard_double_masks_[index];
      voice_block.shepard_half_mask = shepard_half_masks_[index];
      voice_block.distortion = distortion_values_[index];
      voice_block.last_distortion = last_distortion_values_[index];
      voice_block.secondary_distortion = secondary_distortion_values_[index];
      voice_block.last_secondary_distortion = last_secondary_distortion_values_[index];
      voice_block.secondary_shepard_double_mask = secondary_shepard_double_masks_[index];
      voice_block.secondary_shepard_half_mask = secondary_shepard_half_masks_[index];

      int buffer_index = index * poly_float::kSize;
      memcpy(voice_block.from_buffers, last_buffers_ + buffer_index, poly_float::kSize * sizeof(mono_float*));
      memcpy(voice_block.to_buffers, wave_buffers_ + buffer_index, poly_float::kSize * sizeof(mono_float*));
    }
  }

  void SynthOscillator::setSpectralMorphValues(SpectralMorph spectral_morph, bool secondary) {
    poly_float* spectral_morph_values_ = secondary ? secondary_spectral_morph_values_ : this->spectral_morph_values_;
    poly_float* last_spectral_morph_values_ = secondary ? last_secondary_spectral_morph_values_ : this->last_spectral_morph_values_;
    static constexpr float kModMult = 0.99f;
    poly_float spectral_morph_amount = input(secondary ? kSecondarySpectralMorphAmount : kSpectralMorphAmount)->at(0);
    poly_float morph_spread = input(secondary ? kSecondarySpectralMorphSpread : kUnisonSpectralMorphSpread)->at(0);
    int num_phase_updates = active_oscillators_ / 2;

    for (int v = 0; v < kNumPolyPhase; ++v) {
      poly_float t = (v / (utils::imax(2, num_phase_updates) - 1.0f)) * 2.0f;
      last_spectral_morph_values_[v] = spectral_morph_values_[v];
      spectral_morph_values_[v] = spectral_morph_amount + t * morph_spread;
    }
    
    if (spectral_morph == kShepardTone) {
      for (int v = 0; v < kNumPolyPhase; ++v)
        spectral_morph_values_[v] = utils::mod(spectral_morph_values_[v] * kModMult) * (1.0f / kModMult);
    }
    else {
      for (int v = 0; v < kNumPolyPhase; ++v)
        spectral_morph_values_[v] = utils::clamp(spectral_morph_values_[v], 0.0f, 1.0f);
    }

    bool is_spread = poly_float::notEqual(morph_spread, 0.0f).anyMask() != 0;
    setSpectralMorphValues(spectral_morph, spectral_morph_values_, kNumPolyPhase, is_spread);
    if (spectral_morph == kVocode) {
      static constexpr float kDefaultSampleRate = 88200;
      float sample_rate = wavetable_->getActiveSampleRate();
      if (sample_rate <= 0.0f)
        sample_rate = kDefaultSampleRate;
      float sample_rate_ratio = getSampleRate() / sample_rate;
      float frequency_ratio = sample_rate_ratio * wavetable_->getActiveFrequencyRatio();

      for (int i = 0; i < kNumPolyPhase; ++i)
        spectral_morph_values_[i] *= frequency_ratio;
    }
  }

  void SynthOscillator::setSecondaryDistortion(DistortionType type, bool left_active, bool right_active) {
    const bool changed = type != voice_block_.secondary_distortion_type;
    voice_block_.secondary_distortion_type = type;
    // The secondary amount spreads across unison voices like the primary one.
    const poly_float amount = input(kSecondaryDistortionAmount)->at(0);
    const poly_float spread = input(kSecondaryDistortionSpread)->at(0);
    const int num_phase_updates = active_oscillators_ / 2;
    for (int i = 0; i < kNumPolyPhase; ++i) {
      const poly_float t = (i / (utils::imax(2, num_phase_updates) - 1.0f) * 2.0f);
      last_secondary_distortion_values_[i] = secondary_distortion_values_[i];
      secondary_distortion_values_[i] = utils::clamp(amount + t * spread, 0.0f, 1.0f);
    }
    const bool is_spread = poly_float::notEqual(spread, 0.0f).anyMask() != 0;
    setDistortionValues(type, secondary_distortion_values_, kNumPolyPhase, is_spread);
    for (int i = 0; i < kNumPolyPhase; ++i) {
      if (changed)
        last_secondary_distortion_values_[i] = secondary_distortion_values_[i];
      if (type == kQuantize)
        last_secondary_distortion_values_[i] = utils::max(1.5f, last_secondary_distortion_values_[i]);
    }
    voice_block_.secondary_phase_warp = passThroughPhase;
    voice_block_.secondary_window = passThroughWindow;
    voice_block_.secondary_modulation_buffer = null_source_.buffer;
    Output* source = nullptr;
    bool fm = false;
    bool rm = false;
    switch (type) {
      case kSync: voice_block_.secondary_phase_warp = syncPhase; break;
      case kFormant:
        voice_block_.secondary_phase_warp = syncPhase;
        voice_block_.secondary_window = halfSinWindow;
        break;
      case kQuantize: voice_block_.secondary_phase_warp = quantizePhase; break;
      case kBend: voice_block_.secondary_phase_warp = bendPhase; break;
      case kSqueeze: voice_block_.secondary_phase_warp = squeezePhase; break;
      case kPulseWidth:
        voice_block_.secondary_phase_warp = pulseWidthPhase;
        voice_block_.secondary_window = pulseWidthWindow;
        break;
      default:
        if (const int position = modulatorPosition(type); position >= 0) {
          source = modulators_[position];
          fm = isFmType(type);
          rm = isRmType(type);
        }
        break;
    }
    if (source != nullptr)
      voice_block_.secondary_modulation_buffer = source->buffer;
    if (fm)
      voice_block_.secondary_phase_warp = left_active && right_active ? fmPhase : left_active ? fmPhaseLeft : fmPhaseRight;
    if (rm)
      voice_block_.secondary_window = left_active && right_active ? rmWindow : left_active ? rmWindowLeft : rmWindowRight;
  }

  void SynthOscillator::setDistortionValues(DistortionType distortion_type) {
    poly_float distortion_amount = input(kDistortionAmount)->at(0);
    poly_float distortion_spread = input(kUnisonDistortionSpread)->at(0);
    int num_phase_updates = active_oscillators_ / 2;
    for (int v = 0; v < kNumPolyPhase; ++v) {
      poly_float t = (v / (utils::imax(2, num_phase_updates) - 1.0f) * 2.0f);
      last_distortion_values_[v] = distortion_values_[v];
      distortion_values_[v] = utils::clamp(distortion_amount + t * distortion_spread, 0.0f, 1.0f);
    }

    if (distortion_type == kQuantize) {
      for (int i = 0; i < kNumPolyPhase; ++i)
        last_distortion_values_[i] = utils::max(1.5f, last_distortion_values_[i]);
    }

    bool is_spread = poly_float::notEqual(distortion_spread, 0.0f).anyMask() != 0;
    setDistortionValues(distortion_type, distortion_values_, kNumPolyPhase, is_spread);
  }

  void SynthOscillator::setDistortionValues(DistortionType distortion_type,
                                            poly_float* values, int num_values, bool spread) {
    switch (distortion_type) {
      case kFmOscillatorA:
      case kFmOscillatorB:
      case kFmOscillatorC:
      case kFmSample:
      case kFmGranular:
      case kFmNoise:
      case kFmSpectral:
        for (int i = 0; i < num_values; ++i)
          values[i] *= values[i];
        break;
      case kSync:
      case kFormant:
        setPowerDistortionValues(values, num_values, kMaxSyncPower, spread);
        for (int i = 0; i < num_values; ++i)
          values[i] *= (1.0f / kMaxSync);
        break;
      case kQuantize:
        if (spread) {
          for (int i = 0; i < num_values; ++i) {
            poly_float distortion = poly_float(1.0f) - values[i];
            distortion *= distortion * distortion;
            distortion = distortion * kMaxQuantize;
            values[i] = futils::pow(2.0f, distortion * kDistortBits + 1.0f);
          }
        }
        else {
          poly_float distortion = poly_float(1.0f) - values[0];
          distortion *= distortion * distortion;
          distortion = distortion * kMaxQuantize;
          distortion = futils::pow(2.0f, distortion * kDistortBits + 1.0f);
          for (int i = 0; i < num_values; ++i)
            values[i] = distortion;
        }
        break;
      case kSqueeze:
        for (int i = 0; i < num_values; ++i)
          values[i] = values[i] * 2.0f * kMaxSqueezePercent + (1.0f - kMaxSqueezePercent);
        break;
      case kPulseWidth:
        if (spread) {
          for (int i = 0; i < num_values; ++i) {
            poly_float distortion = utils::max(poly_float(1.0f) - values[i], 1.0f / kMaxPulseWidthRatio);
            values[i] = poly_float(1.0f) / distortion;
          }
        }
        else {
          poly_float distortion = utils::max(poly_float(1.0f) - values[0], 1.0f / kMaxPulseWidthRatio);
          distortion = poly_float(1.0f) / distortion;
          for (int i = 0; i < num_values; ++i)
            values[i] = distortion;
        }
        break;
      default:
        break;
    }
  }

  void SynthOscillator::setSpectralMorphValues(SpectralMorph spectral_morph,
                                               poly_float* values, int num_values, bool spread) {
    switch (spectral_morph) {
      case kVocode:
        setPowerDistortionValues(values, num_values, -kMaxFormantShift, spread);
        break;
      case kFormScale:
        setPowerDistortionValues(values, num_values, -kMaxEvenOddFormantShift, spread);
        break;
      case kHarmonicScale:
        setPowerDistortionValues(values, num_values, kMaxHarmonicScale, spread);
        break;
      case kInharmonicScale:
        setPowerDistortionValues(values, num_values, kMaxInharmonicScale, spread);
        break;
      case kSmear:
        for (int i = 0; i < num_values; ++i) {
          poly_float invert = poly_float(1.0f) - values[i];
          values[i] = poly_float(1.0f) - invert * invert * invert;
        }
        break;
      case kRandomAmplitudes:
        for (int i = 0; i < num_values; ++i)
          values[i] = values[i] * (kRandomAmplitudeStages - 1);
        break;
      case kPhaseDisperse:
        for (int i = 0; i < num_values; ++i)
          values[i] = -(values[i] * 2.0f - 1.0f) * kPhaseDisperseScale;
        break;
      case kSkew:
        for (int i = 0; i < num_values; ++i)
          values[i] = values[i] * values[i] * kSkewScale;
        break;
      case kShepardTone:
        for (int i = 0; i < num_values; ++i)
          values[i] = poly_float(1.0f) - values[i];
        break;
      default:
        break;
    }
  }
  
  void SynthOscillator::runSpectralMorph(SpectralMorph morph_type, float morph_amount,
                                         const Wavetable::WavetableData* wavetable_data,
                                         int wavetable_index, poly_float* dest, FourierTransform* transform) {
    runMorph(morph_type, morph_amount, wavetable_data, wavetable_index, dest, transform,
             Wavetable::kNumHarmonics, RandomValues::instance()->buffer());
  }

  poly_int SynthOscillator::adjustPhase(DistortionType distortion_type, poly_int phase,
                                        poly_float distortion_amount, poly_int distortion_phase) {
    switch (distortion_type) {
      case kSync:
      case kFormant:
        return syncPhase(phase, distortion_amount, distortion_phase, nullptr, 0);
      case kQuantize:
        return quantizePhase(phase, distortion_amount, distortion_phase, nullptr, 0);
      case kBend:
        return bendPhase(phase, distortion_amount, distortion_phase, nullptr, 0);
      case kSqueeze:
        return squeezePhase(phase, distortion_amount, distortion_phase, nullptr, 0);
      case kPulseWidth:
        return pulseWidthPhase(phase, distortion_amount, distortion_phase, nullptr, 0);
      default:
        return phase;
    }
  }

  poly_float SynthOscillator::getPhaseWindow(DistortionType distortion_type, poly_int phase,
                                             poly_int distorted_phase) {
    switch (distortion_type) {
      case kFormant:
        return halfSinWindow(phase, distorted_phase, 0.0f, nullptr, 0);
      case kPulseWidth:
        return pulseWidthWindow(phase, distorted_phase, 0.0f, nullptr, 0);
      default:
        return 1.0f;
    }
  }

  poly_float SynthOscillator::interpolate(const mono_float* buffer, const poly_int indices) {
    return linearlyInterpolateBuffer(buffer, indices);
  }

  bool SynthOscillator::usesDistortionPhase(DistortionType distortion_type) {
    return distortion_type == kSync || distortion_type == kFormant || distortion_type == kQuantize ||
           distortion_type == kBend || distortion_type == kSqueeze || distortion_type == kPulseWidth;
  }

  void SynthOscillator::process(int num_samples) {
    wavetable_->markUsed();
    int wavetable_version = wavetable_->getActiveVersion();
    if (wavetable_version != wavetable_version_) {
      wavetable_version_ = wavetable_version;
      // Every edit publishes a new version, but the frame buffers are owned by
      // this oscillator. Only a frame-count change needs them reset; doing it
      // on every edit faded held notes in from silence while editing.
      const int wavetable_frames = wavetable_->numActiveFrames();
      if (wavetable_frames != wavetable_frames_) {
        wavetable_frames_ = wavetable_frames;
        resetWavetableBuffers();
      }
    }

    poly_float active_voice = input(kActiveVoices)->at(0);
    bool left_active = active_voice[0] == 1.0f;
    bool right_active = active_voice[2] == 1.0f;

    unison_ = utils::clamp(roundf(input(kUnisonVoices)->at(0)[0]), 1.0f, kMaxUnison);
    setActiveOscillators(unison_ + (unison_ % 2));

    SpectralMorph spectral_morph = static_cast<SpectralMorph>((int)input(kSpectralMorphType)->at(0)[0]);
    DistortionType distortion_type = static_cast<DistortionType>((int)input(kDistortionType)->at(0)[0]);
    setSpectralMorphValues(spectral_morph);
    setDistortionValues(distortion_type);
    voice_block_.secondary_spectral_morph = static_cast<SpectralMorph>(
        utils::iclamp(static_cast<int>(input(kSecondarySpectralMorphType)->at(0)[0]), 0, kNumSpectralMorphTypes - 1));
    setSpectralMorphValues(voice_block_.secondary_spectral_morph, true);
    setSecondaryDistortion(static_cast<DistortionType>(utils::iclamp(
        static_cast<int>(input(kSecondaryDistortionType)->at(0)[0]), 0, kNumDistortionTypes - 1)), left_active, right_active);
    voice_block_.phase_inc_buffer = phase_inc_buffer_->buffer;
    voice_block_.spectral_morph = spectral_morph;

    if (isAnalog()) {
      processAnalog(num_samples);
      wavetable_->markUnused();
      return;
    }

    switch (distortion_type) {
      case kSync:
        processOscillators<syncPhase, passThroughWindow>(num_samples, distortion_type);
        break;
      case kFormant:
        processOscillators<syncPhase, halfSinWindow>(num_samples, distortion_type);
        break;
      case kQuantize:
        processOscillators<quantizePhase, passThroughWindow>(num_samples, distortion_type);
        break;
      case kBend:
        processOscillators<bendPhase, passThroughWindow>(num_samples, distortion_type);
        break;
      case kSqueeze:
        processOscillators<squeezePhase, passThroughWindow>(num_samples, distortion_type);
        break;
      case kPulseWidth:
        processOscillators<pulseWidthPhase, pulseWidthWindow>(num_samples, distortion_type);
        break;
      case kFmOscillatorA:
      case kFmOscillatorB:
      case kFmOscillatorC:
      case kFmSample:
      case kFmGranular:
      case kFmNoise:
      case kFmSpectral:
        voice_block_.modulation_buffer = modulators_[modulatorPosition(distortion_type)]->buffer;
        
        if (left_active && right_active)
          processOscillators<fmPhase, passThroughWindow>(num_samples, distortion_type);
        else if (left_active)
          processOscillators<fmPhaseLeft, passThroughWindow>(num_samples, distortion_type);
        else
          processOscillators<fmPhaseRight, passThroughWindow>(num_samples, distortion_type);
        break;
      case kRmOscillatorA:
      case kRmOscillatorB:
      case kRmOscillatorC:
      case kRmSample:
      case kRmGranular:
      case kRmNoise:
      case kRmSpectral:
        voice_block_.modulation_buffer = modulators_[modulatorPosition(distortion_type)]->buffer;

        if (left_active && right_active)
          processOscillators<passThroughPhase, rmWindow>(num_samples, distortion_type);
        else if (left_active)
          processOscillators<passThroughPhase, rmWindowLeft>(num_samples, distortion_type);
        else
          processOscillators<passThroughPhase, rmWindowRight>(num_samples, distortion_type);
        break;
      default:
        processOscillators<passThroughPhase, passThroughWindow>(num_samples, distortion_type);
        break;
    }

    wavetable_->markUnused();
  }

  force_inline void SynthOscillator::setActiveOscillators(int new_active_oscillators) {
    for (int i = active_oscillators_; i < new_active_oscillators; ++i) {
      wave_buffers_[2 * i] = Wavetable::null_waveform();
      wave_buffers_[2 * i + 1] = Wavetable::null_waveform();
    }

    active_oscillators_ = new_active_oscillators;
  }

  template<poly_float(*snapTranspose)(poly_float, poly_float, float*)>
  void SynthOscillator::setPhaseIncBufferSnap(int num_samples, poly_mask reset_mask,
                                              poly_int trigger_sample, poly_mask active_mask, float* snap_buffer) {
    bool midi_track = poly_float::notEqual(input(kMidiTrack)->at(0), 0.0f).anyMask();
    poly_float current_midi = midi_note_;
    midi_note_ = kNoMidiTrackDefault;
    if (midi_track)
      midi_note_ = input(kMidiNote)->at(0);

    float sample_inc = 1.0f / num_samples;
    current_midi = utils::maskLoad(current_midi, midi_note_, reset_mask);
    poly_float delta_midi = (midi_note_ - current_midi) * sample_inc;
    current_midi = utils::maskLoad(utils::swapVoices(current_midi), current_midi, active_mask);
    delta_midi = utils::maskLoad(utils::swapVoices(delta_midi), delta_midi, active_mask);

    const poly_float* transpose_buffer = input(kTranspose)->source->buffer;
    const poly_float* tune_buffer = input(kTune)->source->buffer;
    const poly_float* fixed_frequency_buffer = input(kFixedFrequency)->source->buffer;
    const poly_float* phase_buffer = input(kPhase)->source->buffer;
    const bool fixed_frequency_mode = input(kFixedFrequencyMode)->at(0)[0] >= 0.5f;
    const bool use_fixed_frequency = fixed_frequency_mode && !midi_track;

    poly_float base_midi = current_midi + transpose_buffer[0] + tune_buffer[0];
    poly_float base_frequency = utils::midiNoteToFrequency(base_midi);

    poly_float sample_rate_scale = kPhaseMult / getSampleRate();
    poly_float phase_scale = kPhaseMult;

    poly_float* inc_dest = phase_inc_buffer_->buffer;
    poly_int* phase_dest = phase_buffer_->buffer;

    for (int i = 0; i < num_samples; ++i) {
      poly_float shift_phase = utils::mod(phase_buffer[i]) - 0.5f;
      poly_int phase = utils::toInt(shift_phase * phase_scale);
      phase_dest[i] = utils::maskLoad(utils::swapVoices(phase), phase, active_mask);

      current_midi += delta_midi;

      poly_float midi = snapTranspose(current_midi, transpose_buffer[i], snap_buffer) + tune_buffer[i];
      poly_float frequency = base_frequency * futils::midiOffsetToRatio(midi - base_midi);
      if (use_fixed_frequency)
        frequency = utils::clamp(fixed_frequency_buffer[i], 1.0f, getSampleRate() * 0.45f);
      poly_mask zero_mask = poly_int::lessThan(i, trigger_sample) & reset_mask;
      poly_float result = (frequency * sample_rate_scale) & ~zero_mask;
      inc_dest[i] = utils::maskLoad(utils::swapVoices(result), result, active_mask);
    }
  }

  void SynthOscillator::setPhaseIncBuffer(int num_samples, poly_mask reset_mask, 
                                          poly_int trigger_sample, poly_mask active_mask) {
    int transpose_quantize = static_cast<int>(input(kTransposeQuantize)->at(0)[0]);
    int quantize_scale = static_cast<int>(input(kTransposeQuantizeScale)->at(0)[0]);
    if (quantize_scale != kTransposeQuantizeCustomScale) {
      quantize_scale = std::max(0, std::min(kTransposeQuantizeCustomScale - 1, quantize_scale));
      int key = static_cast<int>(input(kTransposeQuantizeKey)->at(0)[0]);
      int mode = static_cast<int>(input(kTransposeQuantizeMode)->at(0)[0]);
      transpose_quantize = rotateTransposeQuantizeMask(kTransposeQuantizeScaleMasks[quantize_scale], key);
      if (mode >= 1)
        transpose_quantize |= 1 << kNotesPerOctave;
    }

    if (!utils::isTransposeSnapping(transpose_quantize)) {
      setPhaseIncBufferSnap<noTransposeSnap>(num_samples, reset_mask, trigger_sample, active_mask, nullptr);
      return;
    }

    float snap_buffer[kNotesPerOctave + 1];
    utils::fillSnapBuffer(transpose_quantize, snap_buffer);
    if (utils::isTransposeQuantizeGlobal(transpose_quantize))
      setPhaseIncBufferSnap<globalTransposeSnap>(num_samples, reset_mask, trigger_sample, active_mask, snap_buffer);
    else
      setPhaseIncBufferSnap<localTransposeSnap>(num_samples, reset_mask, trigger_sample, active_mask, snap_buffer);
  }

  template<poly_int(*phaseDistort)(poly_int, poly_float, poly_int, const poly_float*, int),
           poly_float(*window)(poly_int, poly_int, poly_float, const poly_float*, int)>
  void SynthOscillator::processOscillators(int num_samples, DistortionType distortion_type) {
    poly_mask active_voice_mask = poly_float::equal(input(kActiveVoices)->at(0), 1.0f);
    poly_float current_center_amplitude = center_amplitude_;
    poly_float current_detuned_amplitude = detuned_amplitude_;
    setAmplitude();

    poly_mask reset_mask = getResetMask(kReset);
    poly_int trigger_offset = input(kReset)->source->trigger_offset;
    poly_mask retrigger_mask = getResetMask(kRetrigger) & ~reset_mask;
    current_center_amplitude = utils::maskLoad(current_center_amplitude, center_amplitude_, reset_mask);
    current_detuned_amplitude = utils::maskLoad(current_detuned_amplitude, detuned_amplitude_, reset_mask);

    setPhaseIncMults();
    setPhaseIncBuffer(num_samples, reset_mask, trigger_offset, active_voice_mask);

    poly_float current_distortion_phase = distortion_phase_;
    distortion_phase_ = 0.0f;
    if (usesDistortionPhase(distortion_type))
      distortion_phase_ = input(kDistortionPhase)->at(0) - 0.5f;
    current_distortion_phase = utils::maskLoad(current_distortion_phase, distortion_phase_, reset_mask);

    voice_block_.last_distortion_phase = utils::toInt(current_distortion_phase * kPhaseMult);
    voice_block_.distortion_phase = utils::toInt(distortion_phase_ * kPhaseMult);

    poly_mask wave_buffer_mask = reset_mask | retrigger_mask;
    poly_float buffer_phase_inc = phase_inc_buffer_->buffer[num_samples - 1] * (1.0f / kPhaseMult);
    if (wave_buffer_mask[0])
      setWaveBuffers(buffer_phase_inc, 0);
    if (wave_buffer_mask[2])
      setWaveBuffers(buffer_phase_inc, 2);

    if (reset_mask.anyMask())
      reset(reset_mask, trigger_offset);

    if (retrigger_mask.anyMask()) {
      for (int i = 0; i < kNumPolyPhase; ++i)
        from_phase_inc_mults_[i] = utils::maskLoad(from_phase_inc_mults_[i], phase_inc_mults_[i], retrigger_mask);
    }

    voice_block_.start_sample = 0;
    voice_block_.total_samples = num_samples;
    int num_buffer_samples = kWavetableFadeTime * getSampleRate();
    if (voice_block_.num_buffer_samples != num_buffer_samples) {
      voice_block_.num_buffer_samples = num_buffer_samples;
      voice_block_.current_buffer_sample = 0;
    }

    bool shepard = voice_block_.spectral_morph == kShepardTone;
    if (shepard)
      setupShepardWrap();
    else
      clearShepardWrap();
    const bool secondary_shepard = voice_block_.secondary_spectral_morph == kShepardTone;
    if (secondary_shepard)
      setupShepardWrap(true);
    else {
      for (int i = 0; i < kNumPolyPhase; ++i) {
        secondary_shepard_double_masks_[i] = secondary_shepard_half_masks_[i] = 0;
        secondary_waiting_shepard_double_masks_[i] = secondary_waiting_shepard_half_masks_[i] = 0;
      }
    }

    voice_block_.current_buffer_sample &= active_voice_mask;
    while (voice_block_.start_sample < num_samples) {
      poly_int remaining_fade_samples = poly_int(voice_block_.num_buffer_samples) - voice_block_.current_buffer_sample;
      int min_remaining_fade_samples = std::min(remaining_fade_samples[0], remaining_fade_samples[2]);
      int samples = std::min(min_remaining_fade_samples, num_samples - voice_block_.start_sample);
      voice_block_.end_sample = voice_block_.start_sample + samples;
      processChunk<phaseDistort, window>(current_center_amplitude, current_detuned_amplitude);

      voice_block_.current_buffer_sample += samples;
      voice_block_.start_sample = voice_block_.end_sample;

      poly_mask new_buffer_mask = poly_int::equal(voice_block_.current_buffer_sample, voice_block_.num_buffer_samples);
      if (shepard && new_buffer_mask.anyMask())
        doShepardWrap(new_buffer_mask, transpose_quantize_);
      if (secondary_shepard && new_buffer_mask.anyMask())
        doShepardWrap(new_buffer_mask, transpose_quantize_, true);

      if (new_buffer_mask[0])
        setWaveBuffers(buffer_phase_inc, 0);
      if (new_buffer_mask[2])
        setWaveBuffers(buffer_phase_inc, 2);

      VITAL_ASSERT((int)voice_block_.current_buffer_sample[0] < voice_block_.num_buffer_samples);
      VITAL_ASSERT((int)voice_block_.current_buffer_sample[2] < voice_block_.num_buffer_samples);
    }

    if (reset_mask.anyMask())
      clearOutputBufferForReset(reset_mask, kReset, kRaw);

    processBlend(num_samples, reset_mask);
    applyMonoOverlapFade(num_samples, reset_mask);
  }

  void SynthOscillator::applyMonoOverlapFade(int num_samples, poly_mask reset_mask) {
    poly_mask overlap_reset_mask = reset_mask & poly_float::greaterThan(input(kNotePressed)->at(0), 1.0f);
    if (overlap_reset_mask.anyMask()) {
      static constexpr mono_float kMonoOverlapFadeSeconds = 0.001f;
      const int fade_samples = std::max(1, static_cast<int>(kMonoOverlapFadeSeconds * getSampleRate()));
      poly_int trigger_offset = input(kReset)->source->trigger_offset;
      poly_float* raw = output(kRaw)->buffer;
      poly_float* levelled = output(kLevelled)->buffer;

      for (int lane = 0; lane < poly_float::kSize; ++lane) {
        if (!overlap_reset_mask[lane])
          continue;

        const int trigger = std::max(0, static_cast<int>(trigger_offset[lane]));
        const int start = std::min(num_samples, trigger);
        const int end = std::min(num_samples, start + fade_samples);
        for (int i = start; i < end; ++i) {
          mono_float fade = (i - start + 1.0f) / fade_samples;
          raw[i].set(lane, raw[i][lane] * fade);
          levelled[i].set(lane, levelled[i][lane] * fade);
        }
      }
    }
  }

  template<poly_int(*phaseDistort)(poly_int, poly_float, poly_int, const poly_float*, int),
           poly_float(*window)(poly_int, poly_int, poly_float, const poly_float*, int)>
  void SynthOscillator::processChunk(poly_float current_center_amplitude, poly_float current_detuned_amplitude) {
    int active_channels = input(kActiveVoices)->at(0).sum();
    if (active_channels < 2)
      return;

    VITAL_ASSERT(active_channels == 2 || active_channels == 4);
    int num_active_voices = active_channels / 2;
    poly_mask active_voice_mask = poly_float::equal(input(kActiveVoices)->at(0), 1.0f);
    int num_samples = voice_block_.end_sample - voice_block_.start_sample;

    poly_float* audio_out = output(kRaw)->buffer + voice_block_.start_sample;
    utils::zeroBuffer(audio_out, num_samples);

    poly_float center_amplitude = center_amplitude_;
    poly_float detuned_amplitude = detuned_amplitude_;

    if (num_active_voices < 2) {
      poly_float current_detuned_swap = utils::swapVoices(current_detuned_amplitude);
      current_detuned_amplitude = utils::maskLoad(current_detuned_swap, current_detuned_amplitude, active_voice_mask);
      current_center_amplitude = utils::maskLoad(current_detuned_amplitude,
                                                 current_center_amplitude, active_voice_mask);

      poly_float detuned_swap = utils::swapVoices(detuned_amplitude);
      detuned_amplitude = utils::maskLoad(detuned_swap, detuned_amplitude, active_voice_mask);
      center_amplitude = utils::maskLoad(detuned_amplitude, center_amplitude, active_voice_mask);

      poly_float distortion_swap = utils::swapVoices(voice_block_.distortion);
      voice_block_.distortion = utils::maskLoad(distortion_swap, voice_block_.distortion, active_voice_mask);

      poly_float last_distortion_swap = utils::swapVoices(voice_block_.last_distortion);
      voice_block_.last_distortion = utils::maskLoad(last_distortion_swap, voice_block_.last_distortion,
                                                     active_voice_mask);
      poly_int swap_buffer_sample = utils::swapVoices(voice_block_.current_buffer_sample);
      voice_block_.current_buffer_sample = utils::maskLoad(swap_buffer_sample, voice_block_.current_buffer_sample,
                                                           active_voice_mask);
    }

    int num_phase_updates = (poly_float::kSize - 1 + num_active_voices * active_oscillators_) / poly_float::kSize;
    for (int p = 1; p < num_phase_updates; ++p) {
      loadVoiceBlock(voice_block_, p, active_voice_mask);

      poly_int phase = processDetuned<phaseDistort, window>(voice_block_, audio_out);
      if (num_active_voices < 2)
        expandAndWriteVoice(phases_ + 2 * p, phase, active_voice_mask);
      else
        phases_[p] = phase;
    }

    loadVoiceBlock(voice_block_, 0, active_voice_mask);

    mono_float sample_inc = 1.0f / voice_block_.total_samples;
    poly_float delta_center_amplitude = (center_amplitude - current_center_amplitude) * sample_inc;
    poly_float delta_detuned_amplitude = (detuned_amplitude - current_detuned_amplitude) * sample_inc;
    current_center_amplitude += delta_center_amplitude * voice_block_.start_sample;
    current_detuned_amplitude += delta_detuned_amplitude * voice_block_.start_sample;
    poly_int center_phase = processCenter<phaseDistort, window>(voice_block_, audio_out,
                                                                current_center_amplitude, delta_center_amplitude,
                                                                current_detuned_amplitude, delta_detuned_amplitude);

    if (num_active_voices < 2) {
      expandAndWriteVoice(phases_, center_phase, active_voice_mask);
      convertVoiceChannels(num_samples, audio_out, active_voice_mask);
    }
    else
      phases_[0] = center_phase;
  }

  void SynthOscillator::processBlend(int num_samples, poly_mask reset_mask) {
    poly_float* audio_out = output(kRaw)->buffer;
    stereoBlend(audio_out, num_samples, reset_mask);
    levelOutput(output(kLevelled)->buffer, audio_out, num_samples, reset_mask);
  }

  namespace {
    // Shape limits for the analog waveforms. Shape 0 is always the pure wave.
    constexpr mono_float kMaxTriangleSkew = 0.49f;
    constexpr mono_float kMaxPulseNarrowing = 0.49f;
    constexpr mono_float kMinSineKnee = 0.02f;
    // Phase distortion keeps its fast half cycle at least this many samples
    // long, so bright settings stay clean on high notes.
    constexpr mono_float kSineKneeSamples = 3.0f;
    constexpr mono_float kNoiseTopCutoff = 20000.0f;
    constexpr mono_float kNoiseBottomCutoff = 40.0f;
    constexpr mono_float kNoiseFadeInShape = 0.05f;

    force_inline analog::LaneState compactAndLoadState(analog::LaneState* states, poly_mask active_mask) {
      analog::LaneState result;
      analog::LaneState::visitFields([active_mask](auto& compact, auto& one, auto& two) {
        compact = utils::maskLoad(utils::swapVoices(two), one, active_mask);
      }, result, states[0], states[1]);
      return result;
    }

    force_inline void expandAndWriteState(analog::LaneState* states, analog::LaneState& value, poly_mask active_mask) {
      analog::LaneState::visitFields([active_mask](auto& compact, auto& one, auto& two) {
        one = utils::maskLoad(one, compact, active_mask);
        two = utils::maskLoad(two, utils::swapVoices(compact), active_mask);
      }, value, states[0], states[1]);
    }

    // One-pole low-pass coefficient for the white noise Shape. Shape 0 is
    // exactly white; 100% darkens it towards brown.
    poly_float noiseColour(poly_float shape, mono_float sample_rate) {
      poly_float coefficient;
      for (int lane = 0; lane < poly_float::kSize; ++lane) {
        const mono_float amount = utils::clamp(shape[lane], 0.0f, 1.0f);
        const mono_float cutoff = kNoiseTopCutoff * std::pow(kNoiseBottomCutoff / kNoiseTopCutoff, amount);
        mono_float value = 1.0f - std::exp(-2.0f * kPi * cutoff / sample_rate);
        value += (1.0f - value) * std::max(0.0f, 1.0f - amount / kNoiseFadeInShape);
        coefficient.set(lane, value);
      }
      return coefficient;
    }

    // Chip-style noise clocked at up to kPitchedNoiseSteps steps per cycle.
    // Each time a cycle ends the pattern repeats with probability `loop`; at
    // 100% one pattern repeats every cycle, giving a pitched tone. High notes
    // use fewer, longer steps so every step lasts at least two samples.
    force_inline poly_float pitchedNoise(analog::LaneState& state, poly_float t, poly_float cycles_per_sample,
                                         poly_float samples_per_cycle, poly_float loop) {
      poly_float steps = static_cast<mono_float>(analog::kPitchedNoiseSteps);
      for (int i = 0; i < 4; ++i)
        steps = utils::maskLoad(steps, steps * 0.5f, poly_float::greaterThan(steps * cycles_per_sample, 0.5f));
      poly_int last_step = utils::toInt(steps) - poly_int(1);
      poly_float position = t * steps;
      poly_float floored = utils::floor(position);
      poly_int step = utils::toInt(floored);
      poly_mask uninitialized = poly_int::equal(state.step, poly_int(analog::kUninitializedStep));
      state.step = utils::maskLoad(state.step, step, uninitialized);
      poly_mask changed = ~poly_int::equal(step, state.step);

      if (changed.anyMask()) {
        auto decide = [&state, loop](poly_mask deciding) {
          state.random = utils::maskLoad(state.random, analog::nextRandom(state.random), deciding);
          poly_mask repeat = deciding & poly_float::lessThan(analog::randomUnipolar(state.random), loop);
          poly_mask fresh = deciding & ~repeat;
          state.lfsr = utils::maskLoad(state.lfsr, state.cycle_seed, repeat);
          state.cycle_seed = utils::maskLoad(state.cycle_seed, state.lfsr, fresh);
        };
        auto produce = [&state](poly_mask producing) {
          state.next = utils::maskLoad(state.next, analog::lfsrOutput(state.lfsr), producing);
          state.lfsr = utils::maskLoad(state.lfsr, analog::clockLfsr(state.lfsr), producing);
        };

        // A cycle can end without visiting the last step on very high notes.
        // Start the new cycle now so looped patterns still follow the pitch.
        poly_mask wrapped = changed & poly_int::lessThan(step, state.step);
        poly_mask missed = wrapped & ~poly_int::equal(state.step, last_step);
        if (missed.anyMask()) {
          decide(missed);
          produce(missed);
        }

        poly_float value = state.next;
        state.last_jump = utils::maskLoad(state.last_jump, value - state.current, changed);
        state.current = utils::maskLoad(state.current, value, changed);

        // Entering the last step: the next value begins a new cycle.
        poly_mask cycle_ending = changed & poly_int::equal(step, last_step);
        if (cycle_ending.anyMask())
          decide(cycle_ending);
        produce(changed);
        state.step = step;
      }

      poly_float samples_per_step = samples_per_cycle / steps;
      poly_float since = position - floored;
      poly_float until = poly_float(1.0f) - since;
      return state.current + (state.next - state.current) * analog::blepResidual(until * samples_per_step) -
             state.last_jump * analog::blepResidual(since * samples_per_step);
    }

    template<int waveform>
    force_inline poly_float analogSample(analog::LaneState& state, poly_float t, poly_float cycles_per_sample,
                                         poly_float samples_per_cycle, poly_float shape, poly_float noise_coefficient,
                                         bool shaped) {
      if constexpr (waveform == analog::kSine) {
        if (!shaped)
          return analog::cosCycles(t);
        // Casio CZ style phase distortion: the first half cycle speeds up.
        poly_float minimum_knee = utils::clamp(cycles_per_sample * kSineKneeSamples, kMinSineKnee, 0.5f);
        poly_float knee = poly_float(0.5f) - shape * (poly_float(0.5f) - minimum_knee);
        poly_float first = t * 0.5f / knee;
        poly_float second = poly_float(0.5f) + (t - knee) * 0.5f / (poly_float(1.0f) - knee);
        return analog::cosCycles(utils::maskLoad(second, first, poly_float::lessThan(t, knee)));
      }
      else if constexpr (waveform == analog::kTriangle) {
        // Peak at the cycle start, trough at `apex`; skewing tilts it into a saw.
        poly_float apex = poly_float(0.5f) + shape * kMaxTriangleSkew;
        poly_float rest = poly_float(1.0f) - apex;
        poly_float falling = poly_float(1.0f) - t * 2.0f / apex;
        poly_float rising = (t - apex) * 2.0f / rest - 1.0f;
        poly_float naive = utils::maskLoad(rising, falling, poly_float::lessThan(t, apex));
        poly_float change = cycles_per_sample * 2.0f / (apex * rest);
        return naive + analog::cornerCorrection(t, 0.0f, samples_per_cycle, -change) +
               analog::cornerCorrection(t, apex, samples_per_cycle, change);
      }
      else if constexpr (waveform == analog::kSaw) {
        // Falling saw, phase aligned with the default wavetable. Shape mixes in
        // a second saw offset by up to half a cycle.
        poly_float position = utils::mod(t + 0.5f);
        if (!shaped)
          return poly_float(1.0f) - position * 2.0f + analog::stepCorrection(t, 0.5f, samples_per_cycle, 2.0f);
        poly_float offset = shape * 0.5f;
        poly_float naive = poly_float(1.0f) - position - utils::mod(position - offset);
        return naive + analog::stepCorrection(t, 0.5f, samples_per_cycle, 1.0f) +
               analog::stepCorrection(t, utils::mod(offset + 0.5f), samples_per_cycle, 1.0f);
      }
      else if constexpr (waveform == analog::kSquare) {
        // Pulse centred on the cycle start, without DC as the width narrows.
        poly_float width = poly_float(0.5f) - shape * kMaxPulseNarrowing;
        poly_float half_width = width * 0.5f;
        poly_float end = poly_float(1.0f) - half_width;
        poly_mask high = poly_float::lessThan(t, half_width) | poly_float::greaterThanOrEqual(t, end);
        poly_float naive = utils::maskLoad(poly_float(-1.0f), poly_float(1.0f), high) - (width * 2.0f - 1.0f);
        return naive + analog::stepCorrection(t, half_width, samples_per_cycle, -2.0f) +
               analog::stepCorrection(t, end, samples_per_cycle, 2.0f);
      }
      else if constexpr (waveform == analog::kWhiteNoise) {
        // Boosting before the filter keeps the RMS level steady, even while
        // the Shape changes quickly.
        poly_float compensation = utils::sqrt((poly_float(2.0f) - noise_coefficient) / noise_coefficient);
        state.random = analog::nextRandom(state.random);
        poly_float input = analog::randomBipolar(state.random) * compensation;
        state.noise_filter += (input - state.noise_filter) * noise_coefficient;
        return state.noise_filter;
      }
      else
        return pitchedNoise(state, t, cycles_per_sample, samples_per_cycle, shape);
    }

    // Naive waveforms for Hard Sync, described by their value and slope at a
    // read position, plus their jumps and corners. Positions are in slave
    // cycles since a sync reset; the read position is half a cycle later, so
    // at a 1:1 ratio a synced oscillator matches the free-running one.
    template<int waveform>
    struct SyncShape;

    template<>
    struct SyncShape<analog::kSine> {
      poly_float knee;
      SyncShape(poly_float shape, poly_float cycles_per_sample) {
        poly_float minimum_knee = utils::clamp(cycles_per_sample * kSineKneeSamples, kMinSineKnee, 0.5f);
        knee = poly_float(0.5f) - shape * (poly_float(0.5f) - minimum_knee);
      }
      force_inline poly_float warp(poly_float q) const {
        poly_float first = q * 0.5f / knee;
        poly_float second = poly_float(0.5f) + (q - knee) * 0.5f / (poly_float(1.0f) - knee);
        return utils::maskLoad(second, first, poly_float::lessThan(q, knee));
      }
      force_inline poly_float value(poly_float q) const { return analog::cosCycles(warp(q)); }
      force_inline poly_float slope(poly_float q) const {
        poly_float rate = utils::maskLoad(poly_float(0.5f) / (poly_float(1.0f) - knee), poly_float(0.5f) / knee,
                                          poly_float::lessThan(q, knee));
        return analog::sinCycles(warp(q)) * rate * (-2.0f * kPi);
      }
      template<class Function> void edges(Function) const { }
      template<class Function> void corners(Function) const { }
    };

    template<>
    struct SyncShape<analog::kTriangle> {
      poly_float apex;
      SyncShape(poly_float shape, poly_float) : apex(poly_float(0.5f) + shape * kMaxTriangleSkew) { }
      force_inline poly_float value(poly_float q) const {
        poly_float falling = poly_float(1.0f) - q * 2.0f / apex;
        poly_float rising = (q - apex) * 2.0f / (poly_float(1.0f) - apex) - 1.0f;
        return utils::maskLoad(rising, falling, poly_float::lessThan(q, apex));
      }
      force_inline poly_float slope(poly_float q) const {
        return utils::maskLoad(poly_float(2.0f) / (poly_float(1.0f) - apex), poly_float(-2.0f) / apex,
                               poly_float::lessThan(q, apex));
      }
      template<class Function> void edges(Function) const { }
      template<class Function> void corners(Function function) const {
        poly_float change = poly_float(2.0f) / (apex * (poly_float(1.0f) - apex));
        function(poly_float(0.5f), -change);
        function(apex - 0.5f, change);
      }
    };

    template<>
    struct SyncShape<analog::kSaw> {
      poly_float offset;
      SyncShape(poly_float shape, poly_float) : offset(shape * 0.5f) { }
      force_inline poly_float value(poly_float q) const {
        poly_float position = utils::mod(q + 0.5f);
        return poly_float(1.0f) - position - utils::mod(position - offset);
      }
      force_inline poly_float slope(poly_float) const { return -2.0f; }
      template<class Function> void edges(Function function) const {
        function(poly_float(0.0f), poly_float(1.0f));
        function(offset, poly_float(1.0f));
      }
      template<class Function> void corners(Function) const { }
    };

    template<>
    struct SyncShape<analog::kSquare> {
      poly_float width;
      SyncShape(poly_float shape, poly_float) : width(poly_float(0.5f) - shape * kMaxPulseNarrowing) { }
      force_inline poly_float value(poly_float q) const {
        poly_float half_width = width * 0.5f;
        poly_mask high = poly_float::lessThan(q, half_width) |
                         poly_float::greaterThanOrEqual(q, poly_float(1.0f) - half_width);
        return utils::maskLoad(poly_float(-1.0f), poly_float(1.0f), high) - (width * 2.0f - 1.0f);
      }
      force_inline poly_float slope(poly_float) const { return 0.0f; }
      template<class Function> void edges(Function function) const {
        function(width * 0.5f + 0.5f, poly_float(-2.0f));
        function(poly_float(0.5f) - width * 0.5f, poly_float(2.0f));
      }
      template<class Function> void corners(Function) const { }
    };

    force_inline poly_float syncReadPosition(poly_float slave) {
      return utils::mod(slave + 0.5f);
    }

    // Band-limited hard sync. `master` is the oscillator's own position since
    // its sync point; the slave runs `ratio` times faster and restarts there.
    // Every jump and corner, including each restart, is found with its exact
    // time, and its correction is spread over a two sample look-ahead.
    template<int waveform>
    force_inline poly_float syncSample(analog::LaneState& state, poly_float master, poly_float ratio, poly_float shape) {
      static constexpr mono_float kTiny = 1.0e-9f;
      // Just before the restart; still distinct once half a cycle is added.
      static constexpr mono_float kBelowOne = 1.0f - 1.0f / 1048576.0f;
      // The synced wave stays below a quarter of the sample rate, so every
      // jump and corner is caught on very high notes with high ratios.
      static constexpr mono_float kMaxSlaveRate = 0.25f;

      poly_mask unset = poly_float::lessThan(state.sync_master, 0.0f);
      if (unset.anyMask()) {
        poly_float slave = utils::mod(master * ratio);
        poly_float start = SyncShape<waveform>(shape, 0.0f).value(syncReadPosition(slave));
        state.sync_slave = utils::maskLoad(state.sync_slave, slave, unset);
        state.sync_history1 = utils::maskLoad(state.sync_history1, start, unset);
        state.sync_history2 = utils::maskLoad(state.sync_history2, start, unset);
        state.sync_correction0 = state.sync_correction0 & ~unset;
        state.sync_correction1 = state.sync_correction1 & ~unset;
        state.sync_correction2 = state.sync_correction2 & ~unset;
        state.sync_correction3 = state.sync_correction3 & ~unset;
        state.sync_master = utils::maskLoad(state.sync_master, master, unset);
      }

      poly_float movement = master - state.sync_master;
      movement -= utils::round(movement);
      poly_mask forward = poly_float::greaterThan(movement, 0.0f);
      poly_mask wrapped = forward & poly_float::lessThan(master, state.sync_master);
      poly_float since_wrap = utils::clamp(master / utils::max(movement, kTiny), 0.0f, kBelowOne) & wrapped;
      poly_float slave_rate = utils::min(utils::max(movement, 0.0f) * ratio, kMaxSlaveRate);
      poly_float safe_rate = utils::max(slave_rate, kTiny);
      poly_float slave = state.sync_slave;
      poly_float end = slave + slave_rate * (poly_float(1.0f) - since_wrap);
      poly_float restarted = slave_rate * since_wrap;
      SyncShape<waveform> sync_shape(shape, slave_rate);

      auto addStep = [&state](poly_mask mask, poly_float since, poly_float height) {
        if (!mask.anyMask())
          return;
        since = utils::clamp(utils::maskLoad(poly_float(0.0f), since, mask), 0.0f, 1.0f);
        height = height & mask;
        state.sync_correction0 += height * analog::blepResidual(poly_float(2.0f) - since);
        state.sync_correction1 += height * analog::blepResidual(poly_float(1.0f) - since);
        state.sync_correction2 -= height * analog::blepResidual(since);
        state.sync_correction3 -= height * analog::blepResidual(since + 1.0f);
      };
      auto addCorner = [&state](poly_mask mask, poly_float since, poly_float change) {
        if (!mask.anyMask())
          return;
        since = utils::clamp(utils::maskLoad(poly_float(0.0f), since, mask), 0.0f, 1.0f);
        change = change & mask;
        state.sync_correction0 += change * analog::blampResidual(poly_float(2.0f) - since);
        state.sync_correction1 += change * analog::blampResidual(poly_float(1.0f) - since);
        state.sync_correction2 += change * analog::blampResidual(since);
        state.sync_correction3 += change * analog::blampResidual(since + 1.0f);
      };

      // Jumps and corners passed before the restart; one that lands exactly on
      // the restart is counted there instead.
      auto crossings = [&](poly_float position, auto add, poly_float amount) {
        poly_float first = utils::maskLoad(position + 1.0f, position, poly_float::greaterThan(position, slave));
        poly_mask crossed = poly_float::lessThan(first, end) |
                            (poly_float::equal(first, end) & ~wrapped);
        add(crossed & forward, poly_float(1.0f) - (first - slave) / safe_rate, amount);
        if (wrapped.anyMask()) {
          poly_mask after = wrapped & poly_float::greaterThan(position, 0.0f) &
                            poly_float::lessThanOrEqual(position, restarted);
          add(after, since_wrap - position / safe_rate, amount);
        }
      };
      sync_shape.edges([&](poly_float position, poly_float height) { crossings(position, addStep, height); });
      sync_shape.corners([&](poly_float position, poly_float change) {
        crossings(position, addCorner, change * slave_rate);
      });

      poly_float next_slave = utils::mod(end);
      if (wrapped.anyMask()) {
        poly_float before = end - utils::floor(end);
        before = utils::maskLoad(before, poly_float(kBelowOne), poly_float::equal(before, 0.0f));
        poly_float read_before = syncReadPosition(before);
        poly_float read_start = syncReadPosition(0.0f);
        addStep(wrapped, since_wrap, sync_shape.value(read_start) - sync_shape.value(read_before));
        addCorner(wrapped, since_wrap, (sync_shape.slope(read_start) - sync_shape.slope(read_before)) * slave_rate);
        next_slave = utils::maskLoad(next_slave, utils::mod(restarted), wrapped);
      }

      state.sync_slave = next_slave;
      state.sync_master = master;
      poly_float result = state.sync_history2 + state.sync_correction0;
      state.sync_history2 = state.sync_history1;
      state.sync_history1 = sync_shape.value(syncReadPosition(next_slave));
      state.sync_correction0 = state.sync_correction1;
      state.sync_correction1 = state.sync_correction2;
      state.sync_correction2 = state.sync_correction3;
      state.sync_correction3 = 0.0f;
      return result;
    }

    // How an Analog oscillator uses a Distortion slot: FM drives its phase,
    // Hard Sync restarts it with its own note, and ring modulation scales its
    // output. Warps that only make sense for wavetables are off.
    enum AnalogModulation {
      kAnalogNoModulation,
      kAnalogSync,
      kAnalogFm,
      kAnalogRm
    };

    AnalogModulation analogModulation(SynthOscillator::DistortionType type, int waveform) {
      if (waveform == analog::kWhiteNoise)
        return kAnalogNoModulation;
      switch (type) {
        case SynthOscillator::kSync:
          return waveform == analog::kPitchedNoise ? kAnalogNoModulation : kAnalogSync;
        case SynthOscillator::kFmOscillatorA:
        case SynthOscillator::kFmOscillatorB:
        case SynthOscillator::kFmOscillatorC:
        case SynthOscillator::kFmSample:
        case SynthOscillator::kFmGranular:
        case SynthOscillator::kFmNoise:
        case SynthOscillator::kFmSpectral:
          return kAnalogFm;
        case SynthOscillator::kRmOscillatorA:
        case SynthOscillator::kRmOscillatorB:
        case SynthOscillator::kRmOscillatorC:
        case SynthOscillator::kRmSample:
        case SynthOscillator::kRmGranular:
        case SynthOscillator::kRmNoise:
        case SynthOscillator::kRmSpectral:
          return kAnalogRm;
        default:
          return kAnalogNoModulation;
      }
    }

    // Wavetable FM offsets the phase by up to 48 / 8 cycles at full amount.
    constexpr mono_float kAnalogFmCycles = 6.0f;

    // Analog aging. Each control sets its own amount, and pitch depths grow
    // with its square, so low settings stay subtle. Depths are at full
    // amount, in cents. Drift is a slow wander from temperature plus a fast
    // jitter from supply noise, both Ornstein-Uhlenbeck processes with unit
    // variance. Voice Spread gives each voice card fixed pitch and level
    // offsets.
    constexpr mono_float kWanderCents = 80.0f;
    constexpr mono_float kWanderSeconds = 3.0f;
    constexpr mono_float kWanderSmoothSeconds = 0.4f;
    constexpr mono_float kJitterCents = 15.0f;
    constexpr mono_float kJitterSeconds = 0.008f;
    constexpr mono_float kSpreadCents = 100.0f;
    constexpr mono_float kSpreadDb = 8.0f;
    // Each unison oscillator wanders on its own by this much more.
    constexpr mono_float kUnisonWanderCents = 30.0f;
    constexpr mono_float kSpreadSubDb = 6.0f;
    // Supply Sag: pitch and level droop as more voices load the supply. Load
    // approaches 1 with many voices and follows the count like a power supply.
    constexpr mono_float kSagCents = 80.0f;
    constexpr mono_float kSagLevel = 0.4f;
    constexpr mono_float kSagVoices = 6.0f;
    constexpr mono_float kSagSeconds = 0.06f;
    // Wear: a fixed shape offset per voice card plus a little that wanders with
    // the drift (pulse width and triangle slopes), uneven halves of the wave
    // (even harmonics), and an edge softened by a fixed time constant, like a
    // slow saw reset or lazy comparator, so high notes lose the most.
    constexpr mono_float kWearShape = 0.35f;
    constexpr mono_float kWearShapeWander = 0.15f;
    constexpr mono_float kWearAsymmetry = 0.5f;
    constexpr mono_float kWearEdgeSeconds = 0.00015f;
    constexpr mono_float kWearMeanSeconds = 0.05f;
    // Noise is hiss, with crackle from dirty contacts above 50%. Hum is mains
    // hum (rectifier ripple, mostly the second harmonic). Levels are at full amount.
    constexpr mono_float kHissLevel = 0.08f;
    constexpr mono_float kHumLevel = 0.05f;
    constexpr mono_float kCrackleStart = 0.5f;
    constexpr mono_float kCracklesPerSecond = 60.0f;
    constexpr mono_float kCrackleLevel = 0.3f;
    constexpr mono_float kCrackleSeconds = 0.0004f;
    // Uniform noise in [-1, 1] times this has unit variance.
    constexpr mono_float kUniformToUnitVariance = 1.7320508f;

    force_inline mono_float cardBipolar(uint32_t value) {
      return static_cast<mono_float>(value >> 8) * (2.0f / 16777216.0f) - 1.0f;
    }

    template<int sub_waveform>
    force_inline poly_float subSample(poly_float t, poly_float cycles_per_sample, poly_float samples_per_cycle) {
      if constexpr (sub_waveform == analog::kSubSine)
        return analog::cosCycles(t);
      else if constexpr (sub_waveform == analog::kSubTriangle) {
        poly_float naive = poly_float::abs(t - 0.5f) * 4.0f - 1.0f;
        poly_float change = cycles_per_sample * 8.0f;
        return naive + analog::cornerCorrection(t, 0.0f, samples_per_cycle, -change) +
               analog::cornerCorrection(t, 0.5f, samples_per_cycle, change);
      }
      else {
        poly_mask high = poly_float::lessThan(t, 0.25f) | poly_float::greaterThanOrEqual(t, 0.75f);
        poly_float naive = utils::maskLoad(poly_float(-1.0f), poly_float(1.0f), high);
        return naive + analog::stepCorrection(t, 0.25f, samples_per_cycle, -2.0f) +
               analog::stepCorrection(t, 0.75f, samples_per_cycle, 2.0f);
      }
    }

    template<int sub_waveform>
    void renderSub(poly_float* audio_out, int num_samples, const poly_float* phase_inc_buffer,
                   const poly_float* level, poly_int& sub_phase, int octave_shift, poly_mask pre_trigger_mask,
                   poly_int trigger_offset, poly_float gain) {
      for (int i = 0; i < num_samples; ++i) {
        poly_int increment = utils::toInt(phase_inc_buffer[i]);
        increment = octave_shift == 1 ? utils::shiftRight<1>(increment) : utils::shiftRight<2>(increment);
        sub_phase += increment;
        poly_float cycles_per_sample = utils::max(utils::toFloat(increment) * analog::kPhaseToCycles,
                                                  analog::kMinCyclesPerSample);
        poly_float value = subSample<sub_waveform>(analog::phaseToCycles(sub_phase), cycles_per_sample,
                                                   poly_float(1.0f) / cycles_per_sample);
        poly_float amplitude = utils::max(level[i], 0.0f);
        poly_mask silent = pre_trigger_mask & poly_int::greaterThan(trigger_offset, poly_int(i));
        audio_out[i] += (value * amplitude * amplitude * gain) & ~silent;
      }
    }
  }

  Output* SynthOscillator::modulationSource(DistortionType type) const {
    const int position = modulatorPosition(type);
    return position >= 0 ? modulators_[position] : nullptr;
  }

  void SynthOscillator::processAnalog(int num_samples) {
    poly_float current_center_amplitude = center_amplitude_;
    poly_float current_detuned_amplitude = detuned_amplitude_;
    setAmplitude();

    poly_mask active_voice_mask = poly_float::equal(input(kActiveVoices)->at(0), 1.0f);
    poly_mask reset_mask = getResetMask(kReset);
    poly_int trigger_offset = input(kReset)->source->trigger_offset;
    poly_mask retrigger_mask = getResetMask(kRetrigger) & ~reset_mask;
    current_center_amplitude = utils::maskLoad(current_center_amplitude, center_amplitude_, reset_mask);
    current_detuned_amplitude = utils::maskLoad(current_detuned_amplitude, detuned_amplitude_, reset_mask);

    setPhaseIncMults();
    setPhaseIncBuffer(num_samples, reset_mask, trigger_offset, active_voice_mask);

    if (reset_mask.anyMask()) {
      reset(reset_mask, trigger_offset);
      resetAnalog(reset_mask);
    }

    if (retrigger_mask.anyMask()) {
      for (int i = 0; i < kNumPolyPhase; ++i)
        from_phase_inc_mults_[i] = utils::maskLoad(from_phase_inc_mults_[i], phase_inc_mults_[i], retrigger_mask);
    }

    ageAnalogPitch(num_samples, reset_mask, active_voice_mask);

    const int waveform = utils::iclamp(static_cast<int>(std::roundf(input(kAnalogWaveform)->at(0)[0])),
                                       0, analog::kNumWaveforms - 1);
    switch (waveform) {
      case analog::kSine:
        processAnalogChunk<analog::kSine>(num_samples, current_center_amplitude, current_detuned_amplitude);
        break;
      case analog::kTriangle:
        processAnalogChunk<analog::kTriangle>(num_samples, current_center_amplitude, current_detuned_amplitude);
        break;
      case analog::kSquare:
        processAnalogChunk<analog::kSquare>(num_samples, current_center_amplitude, current_detuned_amplitude);
        break;
      case analog::kWhiteNoise:
        processAnalogChunk<analog::kWhiteNoise>(num_samples, current_center_amplitude, current_detuned_amplitude);
        break;
      case analog::kPitchedNoise:
        processAnalogChunk<analog::kPitchedNoise>(num_samples, current_center_amplitude, current_detuned_amplitude);
        break;
      default:
        processAnalogChunk<analog::kSaw>(num_samples, current_center_amplitude, current_detuned_amplitude);
        break;
    }

    if (reset_mask.anyMask())
      clearOutputBufferForReset(reset_mask, kReset, kRaw);

    // The sub joins after stereo spread, so it stays centred, and before the
    // level and pan, so the oscillator Level and Pan include it.
    poly_float* raw = output(kRaw)->buffer;
    stereoBlend(raw, num_samples, reset_mask);
    ageAnalogWave(raw, num_samples);
    addAnalogSub(raw, num_samples, reset_mask);
    addAnalogNoise(raw, num_samples, reset_mask);
    ageAnalogLevel(raw, num_samples, reset_mask);
    levelOutput(output(kLevelled)->buffer, raw, num_samples, reset_mask);
    applyMonoOverlapFade(num_samples, reset_mask);
  }

  template<int waveform>
  void SynthOscillator::processAnalogChunk(int num_samples, poly_float current_center_amplitude,
                                           poly_float current_detuned_amplitude) {
    poly_float* audio_out = output(kRaw)->buffer;
    utils::zeroBuffer(audio_out, num_samples);

    int active_channels = input(kActiveVoices)->at(0).sum();
    if (active_channels < 2)
      return;

    const int num_active_voices = active_channels / 2;
    const bool single_voice = num_active_voices < 2;
    poly_mask active_voice_mask = poly_float::equal(input(kActiveVoices)->at(0), 1.0f);
    poly_float center_amplitude = center_amplitude_;
    poly_float detuned_amplitude = detuned_amplitude_;

    // With one voice, each vector holds four unison oscillators of that voice.
    if (single_voice) {
      poly_float current_detuned_swap = utils::swapVoices(current_detuned_amplitude);
      current_detuned_amplitude = utils::maskLoad(current_detuned_swap, current_detuned_amplitude, active_voice_mask);
      current_center_amplitude = utils::maskLoad(current_detuned_amplitude,
                                                 current_center_amplitude, active_voice_mask);
      poly_float detuned_swap = utils::swapVoices(detuned_amplitude);
      detuned_amplitude = utils::maskLoad(detuned_swap, detuned_amplitude, active_voice_mask);
      center_amplitude = utils::maskLoad(detuned_amplitude, center_amplitude, active_voice_mask);
    }

    auto voiceValue = [single_voice, active_voice_mask](poly_float value) {
      return single_voice ? utils::maskLoad(utils::swapVoices(value), value, active_voice_mask) : value;
    };

    const poly_float* shape_buffer = input(kAnalogShape)->source->buffer;
    const poly_float* inc_buffer = phase_inc_buffer_->buffer;
    const poly_int* offset_buffer = phase_buffer_->buffer;
    const mono_float sample_inc = 1.0f / num_samples;

    poly_float noise_coefficient = 1.0f;
    poly_float noise_coefficient_delta = 0.0f;
    if constexpr (waveform == analog::kWhiteNoise) {
      noise_coefficient = noiseColour(voiceValue(shape_buffer[0]), getSampleRate());
      poly_float end_coefficient = noiseColour(voiceValue(shape_buffer[num_samples - 1]), getSampleRate());
      noise_coefficient_delta = (end_coefficient - noise_coefficient) * sample_inc;
    }

    // Wear moves the pulse width and triangle slopes.
    const poly_float shape_offset = (waveform == analog::kSquare || waveform == analog::kTriangle) ?
                                    wear_shape_ : poly_float(0.0f);

    // Shape 0 on every lane lets the saw and sine skip their Shape work.
    poly_float loudest_shape = 0.0f;
    for (int i = 0; i < num_samples; ++i)
      loudest_shape = utils::max(loudest_shape, shape_buffer[i] + shape_offset);
    const bool shaped = loudest_shape.sum() > 0.0f;

    // The two Distortion slots. For Analog oscillators their order doesn't
    // matter: FM adds up, Hard Sync ratios multiply, and ring mods multiply.
    struct Slot {
      AnalogModulation modulation;
      const poly_float* source;
      const poly_float* values;
      const poly_float* last_values;
    };
    auto makeSlot = [this](int type_input, const poly_float* values, const poly_float* last_values) {
      const DistortionType type = static_cast<DistortionType>(utils::iclamp(
          static_cast<int>(input(type_input)->at(0)[0]), 0, kNumDistortionTypes - 1));
      Slot slot { analogModulation(type, waveform), nullptr, values, last_values };
      Output* source = modulationSource(type);
      if (slot.modulation == kAnalogFm || slot.modulation == kAnalogRm) {
        if (source == nullptr)
          slot.modulation = kAnalogNoModulation;
        else
          slot.source = source->buffer;
      }
      return slot;
    };
    const Slot slots[2] = {
      makeSlot(kDistortionType, distortion_values_, last_distortion_values_),
      makeSlot(kSecondaryDistortionType, secondary_distortion_values_, last_secondary_distortion_values_)
    };
    bool fm = false, rm = false, sync = false;
    for (const Slot& slot : slots) {
      fm = fm || slot.modulation == kAnalogFm;
      rm = rm || slot.modulation == kAnalogRm;
      sync = sync || slot.modulation == kAnalogSync;
    }

    // Positions restart when a mode comes back, instead of using stale ones.
    for (auto& lane_state : analog_states_) {
      if (!fm)
        lane_state.last_read = analog::kUnsetPosition;
      if (!sync)
        lane_state.sync_master = analog::kUnsetPosition;
    }

    const poly_mask all_lanes = poly_int(analog::kUninitializedStep);
    const int num_phase_updates = (poly_float::kSize - 1 + num_active_voices * active_oscillators_) / poly_float::kSize;
    for (int n = 1; n <= num_phase_updates; ++n) {
      // Detuned blocks accumulate first; the centre block (0) runs last and blends.
      const int p = n % num_phase_updates;
      const bool center = p == 0;

      poly_int phase = single_voice ? compactAndLoadVoice(phases_ + 2 * p, active_voice_mask) : phases_[p];
      poly_float phase_inc_mult = single_voice ? compactAndLoadVoice(phase_inc_mults_ + 2 * p, active_voice_mask)
                                               : phase_inc_mults_[p];
      poly_float current_phase_inc_mult = single_voice ?
                                          compactAndLoadVoice(from_phase_inc_mults_ + 2 * p, active_voice_mask) :
                                          from_phase_inc_mults_[p];
      analog::LaneState state = single_voice ? compactAndLoadState(analog_states_ + 2 * p, active_voice_mask)
                                             : analog_states_[p];

      // Lanes past the last unison oscillator stay silent.
      poly_mask valid = all_lanes;
      if (single_voice && (p + 1) * static_cast<int>(poly_float::kSize) > active_oscillators_)
        valid = active_voice_mask;

      poly_float delta_phase_inc_mult = (phase_inc_mult - current_phase_inc_mult) * sample_inc;
      poly_float center_mult = center ? current_center_amplitude : poly_float(1.0f);
      poly_float detuned_mult = center ? current_detuned_amplitude : poly_float(1.0f);
      poly_float delta_center = center ? (center_amplitude - current_center_amplitude) * sample_inc : poly_float(0.0f);
      poly_float delta_detuned = center ? (detuned_amplitude - current_detuned_amplitude) * sample_inc :
                                          poly_float(0.0f);
      poly_float coefficient = noise_coefficient;

      // Each slot's amount, gliding from the last block, per unison oscillator.
      poly_float amounts[2], amount_deltas[2];
      for (int s = 0; s < 2; ++s) {
        const Slot& slot = slots[s];
        poly_float target = single_voice ? compactAndLoadVoice(slot.values + 2 * p, active_voice_mask) : slot.values[p];
        poly_float start = single_voice ? compactAndLoadVoice(slot.last_values + 2 * p, active_voice_mask)
                                        : slot.last_values[p];
        amounts[s] = start;
        amount_deltas[s] = (target - start) * sample_inc;
      }

      for (int i = 0; i < num_samples; ++i) {
        current_phase_inc_mult += delta_phase_inc_mult;
        poly_float increment = inc_buffer[i] * current_phase_inc_mult;
        phase += utils::toInt(increment);
        poly_float t = analog::phaseToCycles(phase + offset_buffer[i]);
        poly_float cycles_per_sample = utils::max(increment * analog::kPhaseToCycles, analog::kMinCyclesPerSample);
        poly_float shape = voiceValue(utils::clamp(shape_buffer[i] + shape_offset, 0.0f, 1.0f));
        coefficient += noise_coefficient_delta;
        for (int s = 0; s < 2; ++s)
          amounts[s] += amount_deltas[s];

        if (fm) {
          poly_float offset = 0.0f;
          for (int s = 0; s < 2; ++s) {
            if (slots[s].modulation == kAnalogFm)
              offset += voiceValue(slots[s].source[i]) * amounts[s];
          }
          t = utils::mod(t + offset * kAnalogFmCycles);
          // Band-limit with the actual movement of the modulated phase.
          poly_float movement = t - state.last_read;
          movement -= utils::round(movement);
          poly_mask known = poly_float::greaterThanOrEqual(state.last_read, 0.0f);
          cycles_per_sample = utils::max(utils::maskLoad(cycles_per_sample, movement, known),
                                         analog::kMinCyclesPerSample);
          state.last_read = t;
        }

        poly_float value;
        if constexpr (waveform <= analog::kSquare) {
          if (sync) {
            poly_float ratio = 1.0f;
            for (int s = 0; s < 2; ++s) {
              if (slots[s].modulation == kAnalogSync)
                ratio *= amounts[s] * static_cast<mono_float>(kMaxSync);
            }
            value = syncSample<waveform>(state, utils::mod(t + 0.5f), ratio, shape);
          }
          else
            value = analogSample<waveform>(state, t, cycles_per_sample, poly_float(1.0f) / cycles_per_sample, shape,
                                           coefficient, shaped);
        }
        else
          value = analogSample<waveform>(state, t, cycles_per_sample, poly_float(1.0f) / cycles_per_sample, shape,
                                         coefficient, shaped);

        if (rm) {
          for (int s = 0; s < 2; ++s) {
            if (slots[s].modulation == kAnalogRm)
              value *= utils::interpolate(1.0f, voiceValue(slots[s].source[i]), amounts[s]);
          }
        }
        center_mult += delta_center;
        detuned_mult += delta_detuned;
        audio_out[i] = center_mult * (value & valid) + detuned_mult * audio_out[i];
        VITAL_ASSERT(utils::isFinite(audio_out[i]));
      }

      if (single_voice) {
        expandAndWriteVoice(phases_ + 2 * p, phase, active_voice_mask);
        expandAndWriteState(analog_states_ + 2 * p, state, active_voice_mask);
      }
      else {
        phases_[p] = phase;
        analog_states_[p] = state;
      }
    }

    if (single_voice)
      convertVoiceChannels(num_samples, audio_out, active_voice_mask);
  }

  void SynthOscillator::resetAnalog(poly_mask reset_mask) {
    // Pitched noise restarts the same pattern on every note, so its timbre is
    // repeatable; white noise gets fresh randomness per note.
    static constexpr uint32_t kLfsrSeed = 0x1d3b;
    static constexpr uint32_t kLfsrSeedStep = 0x2c1b;
    static constexpr uint32_t kSeedStep = 0x9e3779b9u;
    // The sub square toggles at the main saw's reset, like a flip-flop divider.
    static constexpr uint32_t kSubOffsets[analog::kNumSubOctaves] = { 0x80000000u, 0xa0000000u };

    const int octave = utils::iclamp(static_cast<int>(std::roundf(input(kSubOctave)->at(0)[0])),
                                     0, analog::kNumSubOctaves - 1);
    for (int v = 0; v < kNumVoicesPerProcess; ++v) {
      if (!reset_mask[2 * v])
        continue;

      const mono_float random = random_generator_.next() * 0.5f + 0.5f;
      const uint32_t voice_seed = analog::mixSeed(static_cast<uint32_t>(random * 16777215.0f) ^ kSeedStep);
      for (int i = 0; i < kNumPolyPhase; ++i) {
        analog::LaneState& state = analog_states_[i];
        for (int side = 0; side < 2; ++side) {
          const int lane = 2 * v + side;
          // An odd unison count has one centre oscillator in both lanes.
          const uint32_t unison_index = (i == 0 && unison_ < active_oscillators_) ? 1 : 2 * i + side;
          uint32_t lfsr = ((kLfsrSeed + unison_index * kLfsrSeedStep) & 0x7fffu) | 1u;
          state.random.set(lane, analog::mixSeed(voice_seed + unison_index * kSeedStep));
          state.noise_filter.set(lane, 0.0f);
          state.cycle_seed.set(lane, lfsr);
          state.current.set(lane, analog::lfsrOutput(lfsr));
          lfsr = analog::clockLfsr(lfsr);
          state.next.set(lane, analog::lfsrOutput(lfsr));
          state.lfsr.set(lane, analog::clockLfsr(lfsr));
          state.step.set(lane, analog::kUninitializedStep);
          state.last_jump.set(lane, 0.0f);
          state.last_read.set(lane, analog::kUnsetPosition);
          state.sync_master.set(lane, analog::kUnsetPosition);
        }
      }

      const uint32_t sub_phase = (phases_[0][2 * v + 1] >> (octave + 1)) + kSubOffsets[octave];
      sub_phase_.set(2 * v, sub_phase);
      sub_phase_.set(2 * v + 1, sub_phase);
    }
  }

  void SynthOscillator::addAnalogSub(poly_float* audio_out, int num_samples, poly_mask reset_mask) {
    const int octave_shift = 1 + utils::iclamp(static_cast<int>(std::roundf(input(kSubOctave)->at(0)[0])),
                                               0, analog::kNumSubOctaves - 1);
    const int waveform = utils::iclamp(static_cast<int>(std::roundf(input(kSubWaveform)->at(0)[0])),
                                       0, analog::kNumSubWaveforms - 1);
    const poly_float* level = input(kSubLevel)->source->buffer;
    const poly_float* phase_inc_buffer = phase_inc_buffer_->buffer;
    const poly_int trigger_offset = input(kReset)->source->trigger_offset;

    poly_float loudest = 0.0f;
    for (int i = 0; i < num_samples; ++i)
      loudest = utils::max(loudest, level[i]);

    // A silent sub still keeps time, so it stays locked when it is raised.
    if (loudest.sum() <= 0.0f) {
      for (int i = 0; i < num_samples; ++i) {
        poly_int increment = utils::toInt(phase_inc_buffer[i]);
        sub_phase_ += octave_shift == 1 ? utils::shiftRight<1>(increment) : utils::shiftRight<2>(increment);
      }
      return;
    }

    if (waveform == analog::kSubSine)
      renderSub<analog::kSubSine>(audio_out, num_samples, phase_inc_buffer, level, sub_phase_, octave_shift,
                                  reset_mask, trigger_offset, sub_gain_);
    else if (waveform == analog::kSubTriangle)
      renderSub<analog::kSubTriangle>(audio_out, num_samples, phase_inc_buffer, level, sub_phase_, octave_shift,
                                      reset_mask, trigger_offset, sub_gain_);
    else
      renderSub<analog::kSubSquare>(audio_out, num_samples, phase_inc_buffer, level, sub_phase_, octave_shift,
                                    reset_mask, trigger_offset, sub_gain_);
  }

  void SynthOscillator::setVoiceCardSalt(const std::string& salt) {
    uint32_t hash = 0x811c9dc5u;
    for (char c : salt)
      hash = (hash ^ static_cast<uint8_t>(c)) * 0x01000193u;
    voice_card_.salt = hash;
    voice_card_.ready = false;
  }

  void SynthOscillator::ageAnalogPitch(int num_samples, poly_mask reset_mask, poly_mask active_mask) {
    poly_float drift = utils::clamp(input(kAnalogDrift)->at(0), 0.0f, 1.0f);
    poly_float spread = utils::clamp(input(kAnalogVoiceSpread)->at(0), 0.0f, 1.0f);
    poly_float sag = utils::clamp(input(kAnalogSag)->at(0), 0.0f, 1.0f);
    poly_float wear = utils::clamp(input(kAnalogWear)->at(0), 0.0f, 1.0f);
    poly_float noise = utils::clamp(input(kAnalogNoise)->at(0), 0.0f, 1.0f);
    poly_float hum = utils::clamp(input(kAnalogHum)->at(0), 0.0f, 1.0f);
    poly_float largest = utils::max(utils::max(utils::max(drift, spread), utils::max(sag, wear)),
                                    utils::max(noise, hum));
    poly_mask aged = poly_float::greaterThan(largest, 0.0f);
    // Nothing to do with every amount at 0, once the pitch and level have glided back.
    poly_mask settled = poly_float::equal(drift_ratio_, 1.0f) & poly_float::equal(drift_target_gain_, 1.0f);
    if (!aged.anyMask() && !(~settled).anyMask()) {
      wear_amount_ = 0.0f;
      noise_amount_ = 0.0f;
      hum_amount_ = 0.0f;
      wear_shape_ = 0.0f;
      sub_gain_ = 1.0f;
      return;
    }

    if (!voice_card_.ready) {
      for (int v = 0; v < kNumVoicesPerProcess; ++v) {
        const uint32_t card = analog::mixSeed(voice_card_.salt ^
            analog::mixSeed(static_cast<uint32_t>(voice_card_.index * kNumVoicesPerProcess + v + 1)));
        for (int side = 0; side < 2; ++side) {
          const int lane = 2 * v + side;
          voice_card_.pitch.set(lane, cardBipolar(analog::mixSeed(card + 1)));
          voice_card_.level.set(lane, cardBipolar(analog::mixSeed(card + 2)));
          drift_random_.set(lane, analog::mixSeed(card + 4));
          noise_random_.set(lane, analog::mixSeed(card + 5));
          voice_card_.sub_level.set(lane, cardBipolar(analog::mixSeed(card + 6)));
          voice_card_.shape.set(lane, cardBipolar(analog::mixSeed(card + 7)));
          voice_card_.asymmetry.set(lane, cardBipolar(analog::mixSeed(card + 8)));
          // Every unison oscillator lane has its own wander.
          for (int i = 0; i < kNumPolyPhase; ++i)
            unison_random_[i].set(lane, analog::mixSeed(card + 16 + 2 * i + side));
        }
      }
      voice_card_.ready = true;
    }

    // Exact steps of the drift processes over this block, so block size doesn't matter.
    const mono_float seconds = static_cast<mono_float>(num_samples) / getSampleRate();
    const mono_float wander_keep = std::exp(-seconds / kWanderSeconds);
    const mono_float wander_scale = kUniformToUnitVariance * std::sqrt(1.0f - wander_keep * wander_keep);
    const mono_float jitter_keep = std::exp(-seconds / kJitterSeconds);
    const mono_float smooth = 1.0f - std::exp(-seconds / kWanderSmoothSeconds);
    drift_random_ = analog::nextRandom(drift_random_);
    drift_wander_ = drift_wander_ * wander_keep + analog::randomBipolar(drift_random_) * wander_scale;
    drift_random_ = analog::nextRandom(drift_random_);
    drift_jitter_ = drift_jitter_ * jitter_keep + analog::randomBipolar(drift_random_) *
                    (kUniformToUnitVariance * std::sqrt(1.0f - jitter_keep * jitter_keep));
    drift_wander_smooth_ += (drift_wander_ - drift_wander_smooth_) * smooth;

    wear_amount_ = wear;
    noise_amount_ = noise;
    hum_amount_ = hum;

    // The supply load follows the voice count; a new note joins the supply as it is.
    const mono_float voices = utils::max(input(kSoundingVoices)->at(0)[0] - 1.0f, 0.0f);
    const mono_float target_load = 1.0f - std::exp(-voices / kSagVoices);
    sag_load_ = utils::maskLoad(sag_load_, poly_float(target_load), reset_mask);
    sag_load_ += (poly_float(target_load) - sag_load_) * (1.0f - std::exp(-seconds / kSagSeconds));

    poly_float cents = drift * drift * (drift_wander_smooth_ * kWanderCents + drift_jitter_ * kJitterCents) +
                       spread * spread * voice_card_.pitch * kSpreadCents -
                       sag * sag * sag_load_ * kSagCents;
    poly_float ratio = utils::maskLoad(poly_float(1.0f), futils::midiOffsetToRatio(cents * 0.01f), aged);
    poly_float gain = futils::dbToMagnitude(spread * voice_card_.level * kSpreadDb) *
                      (poly_float(1.0f) - sag * sag_load_ * kSagLevel);
    drift_target_gain_ = utils::maskLoad(poly_float(1.0f), gain, aged);
    sub_gain_ = utils::maskLoad(poly_float(1.0f), futils::dbToMagnitude(spread * voice_card_.sub_level * kSpreadSubDb),
                                aged);
    wear_shape_ = wear_amount_ * (poly_float::abs(voice_card_.shape) * kWearShape +
                                  poly_float::abs(drift_wander_smooth_) * kWearShapeWander);

    // A lone voice also fills the other voice's lanes with its unison, as the
    // phase increments do. A new note starts on its own pitch.
    ratio = utils::maskLoad(utils::swapVoices(ratio), ratio, active_mask);
    poly_float current = utils::maskLoad(drift_ratio_, ratio, reset_mask);
    poly_float delta = (ratio - current) * (1.0f / num_samples);
    poly_float* increments = phase_inc_buffer_->buffer;
    for (int i = 0; i < num_samples; ++i) {
      current += delta;
      increments[i] *= current;
    }
    drift_ratio_ = ratio;

    // Each unison oscillator wanders on its own too. The unison multipliers
    // glide from the last block's, so they only need this block's target.
    poly_float unison_depth = drift * drift * (kUnisonWanderCents * 0.01f);
    for (int i = 0; i < kNumPolyPhase; ++i) {
      unison_random_[i] = analog::nextRandom(unison_random_[i]);
      unison_wander_[i] = unison_wander_[i] * wander_keep + analog::randomBipolar(unison_random_[i]) * wander_scale;
      poly_float unison_ratio = utils::maskLoad(poly_float(1.0f),
                                                futils::midiOffsetToRatio(unison_wander_[i] * unison_depth), aged);
      phase_inc_mults_[i] *= unison_ratio;
      from_phase_inc_mults_[i] = utils::maskLoad(from_phase_inc_mults_[i], phase_inc_mults_[i], reset_mask);
    }
  }

  void SynthOscillator::ageAnalogWave(poly_float* audio_out, int num_samples) {
    if (!poly_float::greaterThan(wear_amount_, 0.0f).anyMask())
      return;

    // The edge softens with Wear squared, from none to about 1 kHz.
    const mono_float sample_rate = getSampleRate();
    poly_float edge_seconds = wear_amount_ * wear_amount_ * kWearEdgeSeconds;
    poly_float edge_coefficient = 1.0f;
    for (int lane = 0; lane < poly_float::kSize; ++lane) {
      if (edge_seconds[lane] > 0.0f)
        edge_coefficient.set(lane, 1.0f - std::exp(-1.0f / (edge_seconds[lane] * sample_rate)));
    }
    const mono_float mean_coefficient = 1.0f - std::exp(-1.0f / (kWearMeanSeconds * sample_rate));
    poly_float asymmetry = wear_amount_ * voice_card_.asymmetry * kWearAsymmetry;

    for (int i = 0; i < num_samples; ++i) {
      poly_float value = audio_out[i];
      poly_float square = value * value;
      wear_square_mean_ += (square - wear_square_mean_) * mean_coefficient;
      value += asymmetry * (square - wear_square_mean_);
      wear_edge_ += (value - wear_edge_) * edge_coefficient;
      audio_out[i] = wear_edge_;
    }
  }

  void SynthOscillator::addAnalogNoise(poly_float* audio_out, int num_samples, poly_mask reset_mask) {
    if (!poly_float::greaterThan(utils::max(noise_amount_, hum_amount_), 0.0f).anyMask())
      return;

    const mono_float sample_rate = getSampleRate();
    poly_float hiss = noise_amount_ * noise_amount_ * kHissLevel;
    poly_float hum = hum_amount_ * hum_amount_ * kHumLevel;
    const bool humming = poly_float::greaterThan(hum, 0.0f).anyMask();
    poly_float crackle_amount = utils::max(noise_amount_ - kCrackleStart, 0.0f) * (1.0f / (1.0f - kCrackleStart));
    poly_float crackle_chance = crackle_amount * crackle_amount * (kCracklesPerSecond / sample_rate);
    const bool crackles = poly_float::greaterThan(crackle_chance, 0.0f).anyMask() ||
                          poly_float::notEqual(crackle_, 0.0f).anyMask();
    const mono_float crackle_decay = std::exp(-1.0f / (kCrackleSeconds * sample_rate));

    // Rectifier ripple: mostly twice the mains frequency, with a little of the
    // mains itself and higher harmonics. One SIMD sine gives all four.
    const mono_float mains = input(kAnalogHumFrequency)->at(0)[0] >= 0.5f ? 60.0f : 50.0f;
    const mono_float hum_increment = mains / sample_rate;
    const poly_float harmonics(1.0f, 2.0f, 3.0f, 4.0f);
    const poly_float harmonic_levels(0.5f, 0.8f, 0.35f, 0.2f);

    const poly_int trigger_offset = input(kReset)->source->trigger_offset;
    for (int i = 0; i < num_samples; ++i) {
      noise_random_ = analog::nextRandom(noise_random_);
      poly_float noise = analog::randomBipolar(noise_random_);
      poly_float value = noise * hiss;

      hum_phase_ += hum_increment;
      hum_phase_ -= std::floor(hum_phase_);
      if (humming)
        value += hum * (analog::sinCycles(utils::mod(harmonics * hum_phase_)) * harmonic_levels).sum();

      if (crackles) {
        noise_random_ = analog::nextRandom(noise_random_);
        poly_mask pop = poly_float::lessThan(analog::randomUnipolar(noise_random_), crackle_chance);
        crackle_ = utils::maskLoad(crackle_ * crackle_decay, noise * kCrackleLevel, pop);
        value += crackle_ * noise;
      }

      poly_mask silent = reset_mask & poly_int::greaterThan(trigger_offset, poly_int(i));
      audio_out[i] += value & ~silent;
    }
  }

  void SynthOscillator::ageAnalogLevel(poly_float* audio_out, int num_samples, poly_mask reset_mask) {
    poly_mask settled = poly_float::equal(drift_gain_, 1.0f) & poly_float::equal(drift_target_gain_, 1.0f);
    if (!(~settled).anyMask())
      return;

    poly_float current = utils::maskLoad(drift_gain_, drift_target_gain_, reset_mask);
    poly_float delta = (drift_target_gain_ - current) * (1.0f / num_samples);
    for (int i = 0; i < num_samples; ++i) {
      current += delta;
      audio_out[i] *= current;
    }
    drift_gain_ = drift_target_gain_;
  }
} // namespace vital
