// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "common.h"
#include "poly_utils.h"

#include <cstdint>

// SIMD building blocks for the Analog oscillator type. Each lane is one unison
// oscillator. Phases are 32 bit accumulators, where 2^32 is one cycle, exactly
// as in the wavetable oscillator, so both types stay phase aligned.
namespace vital {
  namespace analog {
    enum Waveform {
      kSine,
      kTriangle,
      kSaw,
      kSquare,
      kWhiteNoise,
      kPitchedNoise,
      kNumWaveforms
    };

    enum SubOctave {
      kSubOctaveDown1,
      kSubOctaveDown2,
      kNumSubOctaves
    };

    enum SubWaveform {
      kSubSine,
      kSubTriangle,
      kSubSquare,
      kNumSubWaveforms
    };

    // Pitched noise draws this many random steps per oscillator cycle.
    constexpr int kPitchedNoiseSteps = 32;
    constexpr uint32_t kUninitializedStep = 0xffffffffu;
    constexpr mono_float kPhaseToCycles = 1.0f / 4294967296.0f;
    constexpr mono_float kMinCyclesPerSample = 1.0e-7f;

    // Marks lane positions that must be taken from the next sample.
    constexpr mono_float kUnsetPosition = -1.0f;

    // Per unison-lane state. Kept in arrays parallel to the oscillator phases.
    struct LaneState {
      poly_int random = 1;
      poly_float noise_filter = 0.0f;
      poly_int lfsr = 1;
      poly_int cycle_seed = 1;
      poly_int step = kUninitializedStep;
      poly_float current = 0.0f;
      poly_float next = 0.0f;
      poly_float last_jump = 0.0f;
      // FM: the previous read position, for the actual phase movement.
      poly_float last_read = kUnsetPosition;
      // Hard Sync: master position, slave phase, and a two sample look-ahead
      // of naive samples and band-limiting corrections.
      poly_float sync_master = kUnsetPosition;
      poly_float sync_slave = 0.0f;
      poly_float sync_history1 = 0.0f;
      poly_float sync_history2 = 0.0f;
      poly_float sync_correction0 = 0.0f;
      poly_float sync_correction1 = 0.0f;
      poly_float sync_correction2 = 0.0f;
      poly_float sync_correction3 = 0.0f;

      // Calls `function` with the same field of every given state.
      template<class Function, class... States>
      static void visitFields(Function function, States&... states) {
        function(states.random...);
        function(states.noise_filter...);
        function(states.lfsr...);
        function(states.cycle_seed...);
        function(states.step...);
        function(states.current...);
        function(states.next...);
        function(states.last_jump...);
        function(states.last_read...);
        function(states.sync_master...);
        function(states.sync_slave...);
        function(states.sync_history1...);
        function(states.sync_history2...);
        function(states.sync_correction0...);
        function(states.sync_correction1...);
        function(states.sync_correction2...);
        function(states.sync_correction3...);
      }
    };

    // Converts an unsigned 32 bit phase to cycles in [0, 1).
    force_inline poly_float phaseToCycles(poly_int phase) {
      poly_float t = utils::toFloat(phase) * kPhaseToCycles;
      t += poly_float(1.0f) & poly_float::lessThan(t, 0.0f);
      return t - (poly_float(1.0f) & poly_float::greaterThanOrEqual(t, 1.0f));
    }

    // Residual of a unit step band-limited with an integrated cubic B-spline
    // (a four point polyBLEP), as a magnitude at |distance| samples.
    force_inline poly_float blepResidual(poly_float distance) {
      poly_float d2 = distance * distance;
      poly_float near = poly_float(0.5f) - distance * (2.0f / 3.0f) + d2 * distance * (1.0f / 3.0f) - d2 * d2 * 0.125f;
      poly_float remaining = poly_float(2.0f) - distance;
      poly_float r2 = remaining * remaining;
      poly_float far = r2 * r2 * (1.0f / 24.0f);
      poly_float result = utils::maskLoad(far, near, poly_float::lessThan(distance, 1.0f));
      return result & poly_float::lessThan(distance, 2.0f);
    }

    // Residual of a unit slope change band-limited with the same kernel (polyBLAMP).
    force_inline poly_float blampResidual(poly_float distance) {
      poly_float d2 = distance * distance;
      poly_float d4 = d2 * d2;
      poly_float near = poly_float(7.0f / 30.0f) - distance * 0.5f + d2 * (1.0f / 3.0f) -
                        d4 * (1.0f / 12.0f) + d4 * distance * (1.0f / 40.0f);
      poly_float remaining = poly_float(2.0f) - distance;
      poly_float r2 = remaining * remaining;
      poly_float far = r2 * r2 * remaining * (1.0f / 120.0f);
      poly_float result = utils::maskLoad(far, near, poly_float::lessThan(distance, 1.0f));
      return result & poly_float::lessThan(distance, 2.0f);
    }

    // Correction for a jump of `height` at cycle position `edge`, where `t` is
    // the current position and `samples_per_cycle` is 1 / cycles per sample.
    force_inline poly_float stepCorrection(poly_float t, poly_float edge, poly_float samples_per_cycle,
                                           poly_float height) {
      poly_float since = utils::mod(t - edge);
      poly_float until = poly_float(1.0f) - since;
      return height * (blepResidual(until * samples_per_cycle) - blepResidual(since * samples_per_cycle));
    }

    // Correction for a slope change of `change` per sample at cycle position `corner`.
    force_inline poly_float cornerCorrection(poly_float t, poly_float corner, poly_float samples_per_cycle,
                                             poly_float change) {
      poly_float since = utils::mod(t - corner);
      poly_float until = poly_float(1.0f) - since;
      return change * (blampResidual(until * samples_per_cycle) + blampResidual(since * samples_per_cycle));
    }

    // sin(2 pi x) for x in [-0.25, 0.25], accurate to about 1e-7.
    force_inline poly_float sinQuarter(poly_float x) {
      static constexpr mono_float kTwoPi = 6.283185307179586f;
      poly_float z = x * kTwoPi;
      poly_float z2 = z * z;
      poly_float result = poly_float(-1.0f / 39916800.0f);
      result = result * z2 + (1.0f / 362880.0f);
      result = result * z2 - (1.0f / 5040.0f);
      result = result * z2 + (1.0f / 120.0f);
      result = result * z2 - (1.0f / 6.0f);
      result = result * z2 + 1.0f;
      return result * z;
    }

    // cos(2 pi t) for any t, in cycles.
    force_inline poly_float cosCycles(poly_float t) {
      poly_float u = t + 0.25f;
      u = u - utils::round(u);
      u = utils::maskLoad(u, poly_float(0.5f) - u, poly_float::greaterThan(u, 0.25f));
      u = utils::maskLoad(u, poly_float(-0.5f) - u, poly_float::lessThan(u, -0.25f));
      return sinQuarter(u);
    }

    // sin(2 pi t) for any t, in cycles.
    force_inline poly_float sinCycles(poly_float t) {
      return cosCycles(t - 0.25f);
    }

    force_inline poly_int nextRandom(poly_int state) {
      state ^= utils::shiftLeft<13>(state);
      state ^= utils::shiftRight<17>(state);
      state ^= utils::shiftLeft<5>(state);
      return state;
    }

    // Uniform in [-1, 1).
    force_inline poly_float randomBipolar(poly_int state) {
      poly_int bits = utils::shiftRight<9>(state) | poly_int(0x3f800000);
      return utils::reinterpretToFloat(bits) * 2.0f - 3.0f;
    }

    // Uniform in [0, 1).
    force_inline poly_float randomUnipolar(poly_int state) {
      poly_int bits = utils::shiftRight<9>(state) | poly_int(0x3f800000);
      return utils::reinterpretToFloat(bits) - 1.0f;
    }

    // 15 bit linear feedback shift register, as used for classic chip noise.
    force_inline poly_int clockLfsr(poly_int state) {
      poly_int feedback = (state ^ utils::shiftRight<1>(state)) & poly_int(1);
      return utils::shiftRight<1>(state) | utils::shiftLeft<14>(feedback);
    }

    force_inline poly_float lfsrOutput(poly_int state) {
      poly_mask set = poly_int::equal(state & poly_int(1), poly_int(1));
      return utils::maskLoad(poly_float(1.0f), poly_float(-1.0f), set);
    }

    force_inline uint32_t clockLfsr(uint32_t state) {
      uint32_t feedback = (state ^ (state >> 1)) & 1u;
      return (state >> 1) | (feedback << 14);
    }

    force_inline mono_float lfsrOutput(uint32_t state) {
      return (state & 1u) ? -1.0f : 1.0f;
    }

    force_inline uint32_t mixSeed(uint32_t value) {
      value ^= value >> 16;
      value *= 0x7feb352du;
      value ^= value >> 15;
      value *= 0x846ca68bu;
      value ^= value >> 16;
      return value ? value : 1u;
    }
  } // namespace analog
} // namespace vital
