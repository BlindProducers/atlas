// SPDX-License-Identifier: GPL-3.0-or-later
#include "scream_filter.h"
#include "poly_utils.h"
#include "scream_tanh.h"
#include "../../../third_party/scream/Source/DSP/ScreamDSP.h"
#include <algorithm>
#include <cmath>

namespace vital {
namespace {
  // BP Scream's scalar arithmetic on four lanes, operation for operation, so
  // each lane rounds as the scalar code did. Selects instead of poly_float's
  // min, max and maskLoad, which treat NaN and -0 differently on SSE2 and NEON.
  force_inline poly_float select(poly_mask mask, poly_float yes, poly_float no) {
    return utils::reinterpretToFloat((utils::reinterpretToInt(no) & ~mask) | (utils::reinterpretToInt(yes) & mask));
  }

  // std::clamp(value, low, high).
  force_inline poly_float clamp(poly_float value, poly_float low, poly_float high) {
    return select(poly_float::lessThan(value, low), low, select(poly_float::lessThan(high, value), high, value));
  }

  // FastMath::fastexp2: Paul Mineiro's approximation.
  force_inline poly_float fastExp2(poly_float p) {
    const poly_float offset = select(poly_float::lessThan(p, 0.0f), 1.0f, 0.0f);
    const poly_float z = (p - utils::toFloat(utils::toInt(p))) + offset;
    const poly_float v = ((p + 121.2740575f) + poly_float(27.7280233f) / (poly_float(4.84252568f) - z)) - z * 1.49012907f;
    return utils::reinterpretToFloat(utils::toInt(poly_float(float(1 << 23)) * v));
  }

  // FastMath::fast_gain_to_dB (20 fastlog10) for gains >= 0.
  force_inline poly_float fastGainToDb(poly_float gain) {
    const poly_int bits = utils::reinterpretToInt(gain);
    const poly_float mantissa = utils::reinterpretToFloat((bits & 0x007FFFFF) | 0x3f000000);
    const poly_float y = utils::toFloat(bits) * 3.588557191657796e-8f;
    const poly_float log = ((y - 37.39560623879553f) - mantissa * 0.4509520553155725f) -
                           poly_float(0.5195416459062518f) / (mantissa + 0.3520887068f);
    return log * 20.0f;
  }

  force_inline poly_float fastDbToGain(poly_float db) { return fastExp2(db * 0.166666667f); }

  force_inline poly_float midiToHz(poly_float midi) {
    const poly_float hz = fastExp2((midi - 69.0f) * 0.0833333f) * 440.0f;
    return select(poly_float::greaterThan(hz, 20000.0f), 20000.0f, hz);
  }

  // SvfCoeffs for every lane (svfLP and svfHP).
  struct Svf {
    poly_float a1, a2, a3, m0, m1, m2;

    void design(poly_float cutoff, poly_float q, float inverse_rate) {
      const poly_float x = poly_float(FastMath::HALF_PI * 1.27f) * cutoff * inverse_rate;
      const poly_float y = poly_float(1.0f) - x * x;
      const poly_float g = x * (((y * -0.0187108f) + 0.31583526f) + poly_float(1.27365776f) / y);
      const poly_float k = poly_float(1.0f) / q;
      a1 = poly_float(1.0f) / (g * (g + k) + 1.0f);
      a2 = g * a1;
      a3 = g * a2;
      m1 = -k;
    }

    // svfProcess with the state s0, s1.
    force_inline poly_float process(poly_float v0, poly_float& s0, poly_float& s1) const {
      const poly_float v3 = v0 - s1;
      const poly_float v1 = a1 * s0 + a2 * v3;
      const poly_float v2 = (s1 + a2 * s0) + a3 * v3;
      s0 = v1 * 2.0f - s0;
      s1 = v2 * 2.0f - s1;
      return (m0 * v0 + m1 * v1) + m2 * v2;
    }
  };

  force_inline poly_mask finite(poly_float value) { return poly_float::equal(value - value, 0.0f); }

  force_inline poly_float mirror(poly_float value) { return poly_float(value[0], value[1], value[0], value[1]); }
}

// Reuse BP Scream's SVFs and nonlinear feedback; its double-precision ADAA tanh
// runs on every lane at once (scream_tanh.h). Voice lifetime and gain come from
// Atlas's Env 1, so neither of BP's input gates nor its additional autogain
// ADSR belongs in this voice filter.
struct ScreamFilter::State {
  static constexpr int kNumParameters = 5;   // resonance, scream, input gain, mix, output gain
  poly_float lp0 = 0.0f, lp1 = 0.0f, hp0 = 0.0f, hp1 = 0.0f, feedback = 0.0f, peak = 0.0f, gain_db = 0.0f;
  poly_float silence = 0.0f;
  // BP Scream's SmoothedParam, a lane per voice channel.
  poly_float current[kNumParameters], target[kNumParameters], increment[kNumParameters];
  poly_int remaining[kNumParameters];
  bool fresh[poly_float::kSize] { true, true, true, true };
  scream::TanhAdaa2 tanh1, tanh2;

  State() {
    for (int p = 0; p < kNumParameters; ++p) {
      current[p] = target[p] = increment[p] = 0.0f;
      remaining[p] = 0;
    }
  }

  void clearLane(int lane) {
    for (poly_float* value : { &lp0, &lp1, &hp0, &hp1, &feedback, &peak, &gain_db, &silence })
      value->set(lane, 0.0f);
    tanh1.resetLane(lane);
    tanh2.resetLane(lane);
  }

  void resetLane(int lane) {
    clearLane(lane);
    for (int p = 0; p < kNumParameters; ++p) {
      current[p].set(lane, 0.0f); target[p].set(lane, 0.0f); increment[p].set(lane, 0.0f);
      remaining[p].set(lane, 0);
    }
    fresh[lane] = true;
  }

  // Starts each control of a lane towards its target, as BP Scream's
  // SmoothedParam::setTarget does (or jumps there on a fresh lane).
  void retarget(int lane, const float* targets, int smoothing) {
    for (int p = 0; p < kNumParameters; ++p) {
      SmoothedParam value;
      value.current = current[p][lane];
      value.target = target[p][lane];
      value.inc = increment[p][lane];
      value.remaining = int(remaining[p][lane]);
      if (fresh[lane]) value.reset(targets[p]);
      else value.setTarget(targets[p], smoothing);
      current[p].set(lane, value.current);
      target[p].set(lane, value.target);
      increment[p].set(lane, value.inc);
      remaining[p].set(lane, uint32_t(value.remaining));
    }
    fresh[lane] = false;
  }

  bool smoothing() const {
    poly_int any = 0;
    for (const poly_int& count : remaining) any = any | count;
    return any.sum() != 0;
  }
};

ScreamFilter::ScreamFilter() : Processor(kNumInputs, 1), state_(std::make_unique<State>()) {}
ScreamFilter::ScreamFilter(const ScreamFilter& other) : Processor(other),
    state_(std::make_unique<State>()), fx_mode_(other.fx_mode_) {}
ScreamFilter::~ScreamFilter() = default;

void ScreamFilter::reset(poly_mask mask) {
  for (int lane = 0; lane < poly_float::kSize; ++lane)
    if (mask[lane]) state_->resetLane(lane);
}

void ScreamFilter::process(int num_samples) {
  if (num_samples <= 0) return;
  juce::ScopedNoDenormals no_denormals;
  if (prepared_rate_ != getSampleRate()) {
    prepared_rate_ = getSampleRate();
    reset(poly_mask(-1));
  }
  const float inverse_rate = 1.f / getSampleRate();
  const int smoothing = std::max(1, int(getSampleRate() * .030));
  const float attack = float(Dynamics::convertCompressorTime(getSampleRate() * .005));
  const float release = float(Dynamics::convertCompressorTime(getSampleRate() * .5));
  const float silence_limit = float(std::max(1, int(getSampleRate() * .002)));
  const auto* resets = input(kReset)->source;
  const auto reset_mask = getResetMask(kReset) & resets->trigger_mask;
  const bool autogain = input(kAutogain)->at(0)[0] > .5f;
  // The SVF's tan() prewarp is singular at Nyquist; keep cutoffs below it
  // at low processing rates (e.g. 32 kHz, where 20 kHz is above Nyquist).
  const float max_cutoff = std::min(20000.f, 0.45f / float(inverse_rate));
  // A Filter FX slot's lanes 2-3 repeat lanes 0-1: process one stereo pair
  // and keep lanes 2-3 a copy of it.
  const int pairs = fx_mode_ ? 1 : poly_float::kSize / 2;
  const int lanes = 2 * pairs;
  State& s = *state_;

  float targets[poly_float::kSize][State::kNumParameters];
  int reset_offsets[poly_float::kSize];
  for (int lane = 0; lane < poly_float::kSize; ++lane) {
    const int source = lane % lanes;
    targets[lane][0] = std::clamp(input(kResonance)->at(0)[source], 0.f, 1.f);
    targets[lane][1] = std::clamp(input(kScream)->at(0)[source], 0.f, 1.f);
    targets[lane][2] = std::clamp(input(kInputGain)->at(0)[source], -72.f, 24.f);
    targets[lane][3] = std::clamp(input(kMix)->at(0)[source], 0.f, 1.f);
    targets[lane][4] = std::clamp(input(kOutputGain)->at(0)[source], -24.f, 0.f);
    reset_offsets[lane] = reset_mask[source] ? resets->trigger_offset[source] : -1;
    s.retarget(lane, targets[lane], smoothing);
  }
  bool smoothing_active = s.smoothing();

  // The filters' coefficients and the gains depend only on the cutoff and the
  // smoothed controls, which hold still unless modulated: work them out again
  // only when one changes (every lane gets the same result it had).
  bool have_filters = false, have_input_gain = false, have_output_gain = false;
  poly_float filters_midi, filters_scream, filters_resonance, input_db, output_db;
  poly_float in_gain, out_gain, feedback_gain;
  Svf lp, hp;
  lp.m0 = 0.0f; lp.m1 = 0.0f; lp.m2 = 1.0f;
  hp.m0 = 1.0f; hp.m2 = -1.0f;
  const poly_float limit_low = -32.0f, limit_high = 32.0f;
  const poly_float lp_q = LP_Q_MIN, cutoff_low = 5.0f, cutoff_high = max_cutoff;
  const float follow = 1.0f - attack;

  const poly_float* audio = input(kAudio)->source->buffer;
  const poly_float* midi_cutoff = input(kMidiCutoff)->source->buffer;
  poly_float* out = output()->buffer;
  for (int i = 0; i < num_samples; ++i) {
    for (int lane = 0; lane < poly_float::kSize; ++lane) {
      if (i == reset_offsets[lane]) {
        s.resetLane(lane);
        s.retarget(lane, targets[lane], smoothing);
        smoothing_active = true;
      }
    }
    // The smoothed controls' values for this sample, then their next step.
    const poly_float resonance = s.current[0], scream = s.current[1], input_gain_db = s.current[2];
    const poly_float mix = s.current[3], output_gain_db = s.current[4];
    if (smoothing_active) {
      for (int p = 0; p < State::kNumParameters; ++p) {
        const poly_mask moving = poly_int::greaterThan(s.remaining[p], 0);
        s.current[p] = select(moving, s.current[p] + s.increment[p], s.current[p]);
        s.remaining[p] = s.remaining[p] - (poly_int(1) & moving);
      }
      smoothing_active = s.smoothing();
    }

    if (!have_input_gain || poly_float::notEqual(input_gain_db, input_db).anyMask()) {
      have_input_gain = true;
      input_db = input_gain_db;
      in_gain = fastDbToGain(input_gain_db);
    }
    if (!have_output_gain || poly_float::notEqual(output_gain_db, output_db).anyMask()) {
      have_output_gain = true;
      output_db = output_gain_db;
      out_gain = fastDbToGain(output_gain_db);
    }
    const poly_float midi = fx_mode_ ? mirror(midi_cutoff[i]) : midi_cutoff[i];
    if (!have_filters || (poly_float::notEqual(midi, filters_midi) | poly_float::notEqual(scream, filters_scream) |
                          poly_float::notEqual(resonance, filters_resonance)).anyMask()) {
      have_filters = true;
      filters_midi = midi;
      filters_scream = scream;
      filters_resonance = resonance;
      const poly_float cutoff = clamp(midiToHz(midi), cutoff_low, cutoff_high);
      const poly_float hp_midi = ((scream * (CUTOFF_MAX - HP_CUTOFF_MIN) + HP_CUTOFF_MIN) - CUTOFF_MAX) + midi;
      const poly_float hp_cutoff = clamp(midiToHz(hp_midi), cutoff_low, cutoff_high);
      lp.design(cutoff, lp_q, inverse_rate);
      lp.m1 = 0.0f;
      hp.design(hp_cutoff, resonance * (HP_Q_MAX - HP_Q_MIN) + HP_Q_MIN, inverse_rate);
      feedback_gain = fastDbToGain(resonance * (FB_GAIN_MAX - FB_GAIN_MIN) + FB_GAIN_MIN);
    }

    const poly_float dry = fx_mode_ ? mirror(audio[i]) : audio[i];
    const poly_float shaped = s.tanh1.process(clamp(dry * in_gain + s.feedback, limit_low, limit_high), pairs);
    poly_float wet = lp.process(shaped, s.lp0, s.lp1);
    const poly_float feedback = hp.process(wet * feedback_gain, s.hp0, s.hp1);
    s.feedback = s.tanh2.process(clamp(feedback, limit_low, limit_high), pairs);
    const poly_mask broken = ~(finite(wet) & finite(s.feedback));
    if (broken.anyMask()) {
      for (int lane = 0; lane < poly_float::kSize; ++lane) {
        if (broken[lane]) {
          s.resetLane(lane);
          s.retarget(lane, targets[lane], smoothing);
          wet.set(lane, 0.0f);
        }
      }
      smoothing_active = true;
    }

    // An FX slot has no per-note envelope. Exact silence on its input ends
    // the feedback within 2 ms, without an adjustable envelope-follower gate.
    if (fx_mode_) {
      const poly_float counted = select(poly_float::lessThan(silence_limit, s.silence + 1.0f), silence_limit, s.silence + 1.0f);
      s.silence = select(poly_float::lessThan(poly_float::abs(dry), 1e-12f), counted, 0.0f);
      const poly_float fade = (poly_float(1.0f) - s.silence / silence_limit) * 2.0f;
      wet = wet * select(poly_float::lessThan(fade, 1.0f), fade, 1.0f);
      const poly_mask ended = poly_float::equal(s.silence, silence_limit);
      if (ended.anyMask()) {
        for (int lane = 0; lane < poly_float::kSize; ++lane) {
          if (ended[lane]) {
            s.tanh1.resetLane(lane);
            s.tanh2.resetLane(lane);
          }
        }
        // Without clearing peak and gain, autogain climbs to +24 dB during
        // silence and the next note's attack bursts through before the gain
        // can settle.
        for (poly_float* value : { &s.feedback, &s.lp0, &s.lp1, &s.hp0, &s.hp1, &s.peak, &s.gain_db })
          *value = select(ended, 0.0f, *value);
      }
    }
    poly_float mixed = dry + mix * (wet - dry);
    // Dynamics::detectPeak.
    const poly_float level = poly_float::abs(mixed);
    const poly_float peak = select(poly_float::greaterThan(level, s.peak),
                                   s.peak * attack + level * follow, s.peak * release);
    s.peak = select(poly_float::lessThan(peak, 1.0e-6f), 0.0f, peak);
    poly_float target_db = 0.0f;
    if (autogain) {
      const poly_float level_db = fastGainToDb(s.peak);
      target_db = -select(poly_float::lessThan(-24.0f, level_db), level_db, -24.0f);
    }
    s.gain_db = s.gain_db + (target_db - s.gain_db) * follow;
    out[i] = mixed * (fastDbToGain(s.gain_db) * out_gain);
  }
}
}
