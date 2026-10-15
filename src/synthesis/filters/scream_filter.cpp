// SPDX-License-Identifier: GPL-3.0-or-later
#include "scream_filter.h"
#include "../../../third_party/scream/Source/DSP/ScreamDSP.h"
#include <algorithm>
#include <array>
#include <cmath>

namespace vital {
// Reuse BP Scream's SVFs, nonlinear feedback and double-precision ADAA tanh.
// Voice lifetime and gain come from Atlas's Env 1, so neither of BP's input
// gates nor its additional autogain ADSR belongs in this voice filter.
struct ScreamFilter::State {
  struct Lane {
    TanhADAA2 tanh1, tanh2;
    float lp[2] {}, hp[2] {}, feedback = 0, peak = 0, gain_db = 0;
    std::array<SmoothedParam, 5> parameters;
    int silence = 0;
    bool fresh = true;
  };
  std::array<Lane, poly_float::kSize> lanes;
};

ScreamFilter::ScreamFilter() : Processor(kNumInputs, 1), state_(std::make_unique<State>()) {}
ScreamFilter::ScreamFilter(const ScreamFilter& other) : Processor(other),
    state_(std::make_unique<State>()), fx_mode_(other.fx_mode_) {}
ScreamFilter::~ScreamFilter() = default;

void ScreamFilter::reset(poly_mask mask) {
  for (int lane = 0; lane < poly_float::kSize; ++lane)
    if (mask[lane]) state_->lanes[lane] = State::Lane {};
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
  const int silence_limit = std::max(1, int(getSampleRate() * .002));
  const auto* resets = input(kReset)->source;
  const auto reset_mask = getResetMask(kReset) & resets->trigger_mask;
  const bool autogain = input(kAutogain)->at(0)[0] > .5f;

  for (int lane = 0; lane < poly_float::kSize; ++lane) {
    auto& s = state_->lanes[lane];
    const float targets[] { std::clamp(input(kResonance)->at(0)[lane], 0.f, 1.f),
      std::clamp(input(kScream)->at(0)[lane], 0.f, 1.f),
      std::clamp(input(kInputGain)->at(0)[lane], -72.f, 24.f),
      std::clamp(input(kMix)->at(0)[lane], 0.f, 1.f),
      std::clamp(input(kOutputGain)->at(0)[lane], -24.f, 0.f) };
    auto update = [&] {
      for (int p = 0; p < 5; ++p) {
        if (s.fresh) s.parameters[p].reset(targets[p]);
        else s.parameters[p].setTarget(targets[p], smoothing);
      }
      s.fresh = false;
    };
    update();
    // The SVF's tan() prewarp is singular at Nyquist; keep cutoffs below it
    // at low processing rates (e.g. 32 kHz, where 20 kHz is above Nyquist).
    const float max_cutoff = std::min(20000.f, 0.45f / float(inverse_rate));
    // The filters' coefficients and the gains depend only on the cutoff and
    // the smoothed controls, which hold still unless modulated: work them out
    // again only when one changes (same arithmetic, same result).
    bool have_filters = false, have_input_gain = false, have_output_gain = false;
    float filters_midi = 0, filters_scream = 0, filters_resonance = 0, input_db = 0, output_db = 0;
    float in_gain = 1, out_gain = 1, feedback_gain = 1;
    SvfCoeffs lp {}, hp {};
    for (int i = 0; i < num_samples; ++i) {
      if (reset_mask[lane] && i == resets->trigger_offset[lane]) {
        s = State::Lane {};
        update();
      }
      auto next = [&](int p) {
        auto& value = s.parameters[p];
        return value.remaining > 0 ? value.tick() : value.current;
      };
      const float resonance = next(0), scream = next(1);
      const float input_gain_db = next(2);
      if (!have_input_gain || input_gain_db != input_db) {
        have_input_gain = true;
        input_db = input_gain_db;
        in_gain = FastMath::fast_dB_to_gain(input_gain_db);
      }
      const float mix = next(3);
      const float output_gain_db = next(4);
      if (!have_output_gain || output_gain_db != output_db) {
        have_output_gain = true;
        output_db = output_gain_db;
        out_gain = FastMath::fast_dB_to_gain(output_gain_db);
      }
      const float midi = input(kMidiCutoff)->at(i)[lane];
      if (!have_filters || midi != filters_midi || scream != filters_scream || resonance != filters_resonance) {
        have_filters = true;
        filters_midi = midi;
        filters_scream = scream;
        filters_resonance = resonance;
        const float cutoff = std::clamp(FastMath::midi_to_Hz(midi), 5.f, max_cutoff);
        const float hp_midi = FastMath::lerpf(scream, HP_CUTOFF_MIN, CUTOFF_MAX) - CUTOFF_MAX + midi;
        const float hp_cutoff = std::clamp(FastMath::midi_to_Hz(hp_midi), 5.f, max_cutoff);
        lp = svfLP(cutoff, LP_Q_MIN, inverse_rate);
        hp = svfHP(hp_cutoff, FastMath::lerpf(resonance, HP_Q_MIN, HP_Q_MAX), inverse_rate);
        feedback_gain = FastMath::fast_dB_to_gain(FastMath::lerpf(resonance, FB_GAIN_MIN, FB_GAIN_MAX));
      }
      const float dry = input(kAudio)->at(i)[lane];
      float wet = float(s.tanh1.process(std::clamp(double(dry * in_gain + s.feedback), -32., 32.)));
      wet = svfProcess(wet, lp, s.lp);
      float feedback = wet * feedback_gain;
      feedback = svfProcess(feedback, hp, s.hp);
      s.feedback = float(s.tanh2.process(std::clamp(double(feedback), -32., 32.)));
      if (!std::isfinite(wet) || !std::isfinite(s.feedback)) {
        s = State::Lane {}; update(); wet = 0;
      }

      // An FX slot has no per-note envelope. Exact silence on its input ends
      // the feedback within 2 ms, without an adjustable envelope-follower gate.
      if (fx_mode_) {
        s.silence = std::abs(dry) < 1e-12f ? std::min(s.silence + 1, silence_limit) : 0;
        const float silence_gain = std::min(1.f, 2.f * (1.f - float(s.silence) / silence_limit));
        wet *= silence_gain;
        if (s.silence == silence_limit) {
          s.tanh1.reset(); s.tanh2.reset();
          s.feedback = s.lp[0] = s.lp[1] = s.hp[0] = s.hp[1] = 0;
          // Without this, autogain climbs to +24 dB during silence and the
          // next note's attack bursts through before the gain can settle.
          s.peak = 0; s.gain_db = 0;
        }
      }
      float mixed = dry + mix * (wet - dry);
      s.peak = Dynamics::detectPeak(std::abs(mixed), s.peak, attack, release);
      const float target_db = autogain ? -std::max(-24.f, FastMath::fast_gain_to_dB(s.peak)) : 0.f;
      s.gain_db += (1.f - attack) * (target_db - s.gain_db);
      mixed *= FastMath::fast_dB_to_gain(s.gain_db) * out_gain;
      output()->buffer[i].set(lane, mixed);
    }
  }
}
}
