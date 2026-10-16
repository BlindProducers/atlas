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

#include "envelope.h"
#include "futils.h"
#include "envelope_time_utils.h"
#include "synth_constants.h"

#include <algorithm>
#include <cmath>

namespace vital {

  namespace {
    constexpr mono_float kMinimumClicklessRelease = 0.001f;

    // Analog mode. Attack charges a capacitor toward a target above full
    // level and turns to decay when it reaches the peak, so it rises fast and
    // bends over at the top. Decay and release discharge toward sustain and
    // zero; their times reach 99% of the way, about 4.6 time constants.
    // Release ends at -80 dB, so the voice can stop.
    constexpr mono_float kAnalogAttackTarget = 1.3f;
    constexpr mono_float kAnalogStageTimeConstants = 4.6051702f;
    constexpr mono_float kAnalogSilence = 0.0001f;

    // Age, squared, scales each worn part at full amount: each voice card's
    // stage times (up to an octave either way), a slow wander of all times,
    // a sluggish floor under the shortest times, a varying overshoot, a lower
    // attack peak and a sustain that leaks away while the note is held.
    constexpr mono_float kAgeTimeOctaves = 1.0f;
    constexpr mono_float kAgeWanderOctaves = 0.35f;
    constexpr mono_float kAgeWanderSeconds = 4.0f;
    constexpr mono_float kAgeAttackFloor = 0.03f;
    constexpr mono_float kAgeStageFloor = 0.05f;
    constexpr mono_float kAgeOvershoot = 0.2f;
    constexpr mono_float kAgePeakDrop = 0.35f;
    constexpr mono_float kAgeLeak = 0.6f;
    constexpr mono_float kAgeLeakSeconds = 3.0f;
    // Uniform noise in [-1, 1] times this has unit variance.
    constexpr mono_float kEnvelopeUniformToUnitVariance = 1.7320508f;

    force_inline uint32_t envelopeMixSeed(uint32_t value) {
      value ^= value >> 16;
      value *= 0x7feb352du;
      value ^= value >> 15;
      value *= 0x846ca68bu;
      value ^= value >> 16;
      return value ? value : 1u;
    }

    force_inline mono_float envelopeSeedBipolar(uint32_t value) {
      return static_cast<mono_float>(value >> 8) * (2.0f / 16777216.0f) - 1.0f;
    }

    force_inline poly_int envelopeNextRandom(poly_int state) {
      state ^= utils::shiftLeft<13>(state);
      state ^= utils::shiftRight<17>(state);
      state ^= utils::shiftLeft<5>(state);
      return state;
    }

    force_inline poly_float envelopeRandomBipolar(poly_int state) {
      poly_int bits = utils::shiftRight<9>(state) | poly_int(0x3f800000);
      return utils::reinterpretToFloat(bits) * 2.0f - 3.0f;
    }

    // 1 - e^(-seconds / time_constant) per lane, exact for long times too.
    force_inline poly_float envelopeRcCoefficient(mono_float seconds, poly_float time_constant) {
      poly_float result;
      for (int lane = 0; lane < poly_float::kSize; ++lane)
        result.set(lane, -std::expm1(-seconds / std::max(time_constant[lane], 1.0e-9f)));
      return result;
    }
  }

  Envelope::Envelope() :
      Processor(kNumInputs, kNumOutputs), current_value_(0.0f),
      position_(0.0f), value_(0.0f), poly_state_(0.0f), start_value_(0.0f),
      attack_power_(0.0f), decay_power_(0.0f), release_power_(0.0f), sustain_(0.0f),
      retrigger_from_zero_(false) { }

  void Envelope::process(int num_samples) {
    if (isControlRate())
      processControlRate(num_samples);
    else
      processAudioRate(num_samples);
  }

  poly_float Envelope::tempoSyncedTime(poly_float time) {
    poly_mask sync_mask = poly_float::greaterThan(input(kTempoSync)->at(0), 0.5f);
    if (!sync_mask.anyMask())
      return time;

    poly_float beats_per_second = utils::max(input(kBeatsPerSecond)->at(0), 0.0001f);
    poly_float target_ratio = poly_float(1.0f) / utils::max(time * beats_per_second, 0.000001f);
    poly_float closest_ratio = 1.0f / envelope_time::divisions[0].beats;
    poly_float closest_distance = 1000000.0f;

    for (const auto& division : envelope_time::divisions) {
      poly_float ratio = 1.0f / division.beats;
      poly_float distance = poly_float::abs(ratio - target_ratio);
      poly_mask closer = poly_float::lessThanOrEqual(distance, closest_distance);
      closest_ratio = utils::maskLoad(closest_ratio, ratio, closer);
      closest_distance = utils::maskLoad(closest_distance, distance, closer);
    }

    poly_float synced = poly_float(1.0f) / utils::max(closest_ratio * beats_per_second, 0.000001f);
    // Below half the shortest division a stage is None: it keeps its free
    // time (0 for Delay and Hold) instead of snapping up to 1/64t.
    poly_mask none_mask = poly_float::lessThan(time * beats_per_second, envelope_time::kNoneThresholdBeats);
    synced = utils::maskLoad(synced, time, none_mask);
    return utils::maskLoad(time, synced, sync_mask);
  }

  void Envelope::processControlRate(int num_samples) {
    if (num_samples <= 0)
      return;

    const poly_mask trigger_mask = input(kTrigger)->source->trigger_mask;
    const poly_float original_trigger_value = input(kTrigger)->source->trigger_value;
    const poly_int trigger_offset = input(kTrigger)->source->trigger_offset;
    const Output* reset = input(kReset)->source;
    const poly_mask fresh_voice = reset->trigger_mask & poly_float::equal(reset->trigger_value, kVoiceOn);
    const poly_float delay_time = tempoSyncedTime(utils::max(input(kDelay)->at(0), 0.0f));
    const poly_float attack_time = tempoSyncedTime(utils::max(input(kAttack)->at(0), 0.000000001f));
    const poly_float hold_time = tempoSyncedTime(utils::max(input(kHold)->at(0), 0.0f));
    const poly_float decay_time = tempoSyncedTime(utils::max(input(kDecay)->at(0), 0.000000001f));
    const poly_float release_time = tempoSyncedTime(utils::max(input(kRelease)->at(0), 0.000000001f));
    const poly_mask has_delay_mask = poly_float::notEqual(delay_time, 0.0f);
    const poly_mask has_hold_mask = poly_float::notEqual(hold_time, 0.0f);
    const poly_float sustain = utils::clamp(input(kSustain)->at(0), 0.0f, 1.0f);
    const poly_float tick_time = 1.0f / getSampleRate();

    for (int sample = 0; sample < num_samples; ++sample) {
      const poly_mask triggering = trigger_mask & poly_int::equal(sample, trigger_offset);
      const poly_mask note_on_mask = poly_float::equal(original_trigger_value, kVoiceOn);
      const poly_float trigger_value = utils::maskLoad(original_trigger_value, kVoiceIdle,
                                                        has_delay_mask & note_on_mask);
      const poly_float previous_state = poly_state_;
      poly_state_ = utils::maskLoad(poly_state_, trigger_value, triggering);
      position_ = utils::maskLoad(position_, 0.0f, triggering);
      start_value_ = utils::maskLoad(start_value_, value_, triggering);
      if (retrigger_from_zero_)
        start_value_ = utils::maskLoad(start_value_, 0.0f, triggering & note_on_mask);
      const poly_mask dead_retrigger = triggering & note_on_mask &
                                       poly_float::equal(previous_state, kVoiceKill);
      start_value_ = utils::maskLoad(start_value_, 0.0f, dead_retrigger);
      start_value_ = utils::maskLoad(start_value_, 0.0f,
          triggering & note_on_mask & fresh_voice & poly_int::equal(sample, reset->trigger_offset));

      const poly_mask delay_mask = poly_float::equal(poly_state_, kVoiceIdle);
      const poly_mask attack_mask = poly_float::equal(poly_state_, kVoiceOn);
      const poly_mask hold_mask = poly_float::equal(poly_state_, kVoiceHold);
      const poly_mask decay_mask = poly_float::equal(poly_state_, kVoiceDecay);
      const poly_mask release_mask = poly_float::equal(poly_state_, kVoiceOff);
      const poly_mask kill_mask = poly_float::equal(poly_state_, kVoiceKill);

      const poly_float power = (-input(kAttackPower)->at(0) & attack_mask) +
                               (input(kDecayPower)->at(0) & decay_mask) +
                               (input(kReleasePower)->at(0) & release_mask);
      const poly_float shaped = futils::powerScale(position_, power);
      const poly_float attack_value = utils::interpolate(start_value_, poly_float(1.0f), shaped);
      const poly_float decay_value = poly_float(1.0f) - (poly_float(1.0f) - sustain) * shaped;
      const poly_float release_value = start_value_ * (poly_float(1.0f) - shaped);
      const poly_float kill_value = start_value_ * (poly_float(1.0f) - position_);
      value_ = (attack_value & attack_mask) +
               (poly_float(1.0f) & hold_mask) +
               (decay_value & decay_mask) +
               (release_value & release_mask) +
               (kill_value & kill_mask);
      value_ = utils::clamp(value_, 0.0f, 1.0f);

      const poly_float delta_position =
          (tick_time / utils::max(delay_time, 0.0000001f) & delay_mask) +
          (tick_time / attack_time & attack_mask) +
          (tick_time / utils::max(hold_time, 0.0000001f) & hold_mask) +
          (tick_time / decay_time & decay_mask) +
          (tick_time / release_time & release_mask) +
          (tick_time * (1.0f / kVoiceKillTime) & kill_mask);
      position_ = utils::clamp(position_ + delta_position, 0.0f, 1.0f);

      const poly_mask at_end = poly_float::greaterThanOrEqual(position_, 1.0f);
      const poly_mask delay_transition = delay_mask & at_end;
      const poly_mask hold_transition = attack_mask & at_end & has_hold_mask;
      const poly_mask decay_transition = ((attack_mask & ~has_hold_mask) | hold_mask) & at_end;
      const poly_mask release_transition = release_mask & at_end;
      poly_state_ = utils::maskLoad(poly_state_, kVoiceOn, delay_transition);
      poly_state_ = utils::maskLoad(poly_state_, kVoiceHold, hold_transition);
      poly_state_ = utils::maskLoad(poly_state_, kVoiceDecay, decay_transition);
      poly_state_ = utils::maskLoad(poly_state_, kVoiceKill, release_transition);
      position_ = position_ & ~(delay_transition | hold_transition | decay_transition);
    }

    output(kValue)->trigger_value = value_;
    output(kValue)->buffer[0] = value_;
    output(kPhase)->buffer[0] = poly_state_ + position_;
  }

  poly_float Envelope::processSection(poly_float* audio_out, int from, int to,
                                      poly_float power, poly_float delta_power,
                                      poly_float position, poly_float delta_position,
                                      poly_float start, poly_float end, poly_float delta_end) {
    int num_samples = to - from;
    poly_float power_change = delta_power * num_samples;
    poly_float position_change = delta_position * num_samples;

    poly_float current_power = power;
    poly_float current_position = position;
    poly_float current_end = end;
    for (int i = from; i < to; ++i) {
      poly_float t = futils::powerScale(current_position, current_power);
      audio_out[i] = utils::interpolate(start, current_end, t);

      current_power += delta_power;
      current_position = utils::clamp(current_position + delta_position, 0.0f, 1.0f);
      current_end += delta_end;
    }

    return utils::clamp(position + delta_position * num_samples, 0.0f, 1.0f);
  }

  void Envelope::setVoiceCardSalt(const std::string& salt) {
    uint32_t hash = 0x811c9dc5u;
    for (char c : salt)
      hash = (hash ^ static_cast<uint8_t>(c)) * 0x01000193u;
    voice_card_.salt = hash;
    voice_card_.ready = false;
  }

  void Envelope::prepareVoiceCards() {
    if (voice_card_.ready)
      return;

    static constexpr int kVoicesPerProcess = poly_float::kSize / 2;
    for (int v = 0; v < kVoicesPerProcess; ++v) {
      const uint32_t card = envelopeMixSeed(voice_card_.salt ^
                                    envelopeMixSeed(static_cast<uint32_t>(voice_card_.index * kVoicesPerProcess + v + 1)));
      for (int side = 0; side < 2; ++side) {
        const int lane = 2 * v + side;
        voice_card_.attack.set(lane, envelopeSeedBipolar(envelopeMixSeed(card + 1)));
        voice_card_.decay.set(lane, envelopeSeedBipolar(envelopeMixSeed(card + 2)));
        voice_card_.release.set(lane, envelopeSeedBipolar(envelopeMixSeed(card + 3)));
        voice_card_.overshoot.set(lane, envelopeSeedBipolar(envelopeMixSeed(card + 4)));
        voice_card_.peak.set(lane, envelopeSeedBipolar(envelopeMixSeed(card + 5)));
        voice_card_.leak.set(lane, envelopeSeedBipolar(envelopeMixSeed(card + 6)));
        wander_random_.set(lane, envelopeMixSeed(card + 7));
      }
    }
    voice_card_.ready = true;
  }

  void Envelope::processAnalog(int num_samples) {
    const mono_float tick = 1.0f / getSampleRate();
    const mono_float seconds = num_samples * tick;

    poly_float age = utils::clamp(input(kAnalogAge)->at(0), 0.0f, 1.0f);
    age *= age;
    if (poly_float::greaterThan(age, 0.0f).anyMask()) {
      prepareVoiceCards();
      const mono_float keep = std::exp(-seconds / kAgeWanderSeconds);
      wander_random_ = envelopeNextRandom(wander_random_);
      time_wander_ = time_wander_ * keep + envelopeRandomBipolar(wander_random_) *
                     (kEnvelopeUniformToUnitVariance * std::sqrt(1.0f - keep * keep));
    }
    else
      time_wander_ = 0.0f;

    // Worn timing: each stage's time scaled by its card's error and the wander,
    // with a sluggish floor.
    poly_float wander = time_wander_ * kAgeWanderOctaves;
    auto agedTime = [&](poly_float time, poly_float card, mono_float floor) {
      poly_float scale = futils::exp2(age * (card * kAgeTimeOctaves + wander));
      return utils::max(time * scale, age * floor);
    };

    poly_float delay_time = tempoSyncedTime(utils::max(input(kDelay)->at(0), 0.0f));
    poly_float attack_time = agedTime(tempoSyncedTime(utils::max(input(kAttack)->at(0), 0.000000001f)),
                                      voice_card_.attack, kAgeAttackFloor);
    poly_float hold_time = tempoSyncedTime(utils::max(input(kHold)->at(0), 0.0f));
    poly_float decay_time = agedTime(tempoSyncedTime(utils::max(input(kDecay)->at(0), 0.000000001f)),
                                     voice_card_.decay, kAgeStageFloor);
    poly_float release_time = agedTime(tempoSyncedTime(utils::max(input(kRelease)->at(0), kMinimumClicklessRelease)),
                                       voice_card_.release, kAgeStageFloor);
    poly_mask has_delay_mask = poly_float::notEqual(delay_time, 0.0f);
    poly_mask has_hold_mask = poly_float::notEqual(hold_time, 0.0f);
    poly_float delta_delay = poly_float(tick) / utils::max(delay_time, 0.0000001f);
    poly_float delta_hold = poly_float(tick) / utils::max(hold_time, 0.0000001f);
    poly_float delta_kill = tick * (1.0f / kVoiceKillTime);

    // Attack charges toward the overshoot and stops at the peak; its time
    // constant makes that take the Attack time.
    poly_float overshoot = poly_float(kAnalogAttackTarget) + age * voice_card_.overshoot * kAgeOvershoot;
    poly_float peak = poly_float(1.0f) - age * poly_float::abs(voice_card_.peak) * kAgePeakDrop;
    poly_float attack_constant = attack_time / futils::log(overshoot / (overshoot - peak));
    poly_float attack_coefficient = envelopeRcCoefficient(tick, attack_constant);
    poly_float decay_coefficient = envelopeRcCoefficient(tick, decay_time * (1.0f / kAnalogStageTimeConstants));
    poly_float release_coefficient = envelopeRcCoefficient(tick, release_time * (1.0f / kAnalogStageTimeConstants));
    poly_float leak_coefficient = envelopeRcCoefficient(tick, poly_float(kAgeLeakSeconds));
    poly_float leak_depth = age * (poly_float(0.5f) + poly_float::abs(voice_card_.leak) * 0.5f) * kAgeLeak;
    poly_float sustain_end = utils::clamp(input(kSustain)->at(0), 0.0f, 1.0f);
    poly_float sustain = sustain_;
    poly_float delta_sustain = (sustain_end - sustain_) * (1.0f / num_samples);

    poly_mask trigger_mask = input(kTrigger)->source->trigger_mask;
    const Output* reset = input(kReset)->source;
    const poly_mask fresh_voice = reset->trigger_mask & poly_float::equal(reset->trigger_value, kVoiceOn);
    poly_float trigger_value = input(kTrigger)->source->trigger_value;
    poly_mask note_on_mask = poly_float::equal(trigger_value, kVoiceOn);
    trigger_value = utils::maskLoad(trigger_value, kVoiceIdle, has_delay_mask & note_on_mask);
    poly_int trigger_offset = input(kTrigger)->source->trigger_offset;

    poly_float* audio_out = output(kValue)->buffer;
    for (int i = 0; i < num_samples; ++i) {
      poly_mask triggering = trigger_mask & poly_int::equal(i, trigger_offset);
      if (triggering.anyMask()) {
        poly_float previous_state = poly_state_;
        poly_state_ = utils::maskLoad(poly_state_, trigger_value, triggering);
        position_ = utils::maskLoad(position_, 0.0f, triggering);
        // An analog envelope carries on from where it is, unless the voice is new.
        poly_mask from_zero = triggering & note_on_mask &
                              (poly_float::equal(previous_state, kVoiceKill) |
                               (fresh_voice & poly_int::equal(i, reset->trigger_offset)));
        if (retrigger_from_zero_)
          from_zero = from_zero | (triggering & note_on_mask);
        value_ = utils::maskLoad(value_, 0.0f, from_zero);
        start_value_ = utils::maskLoad(start_value_, value_, triggering);
        droop_ = utils::maskLoad(droop_, 0.0f, triggering & note_on_mask);
      }

      poly_mask delay_mask = poly_float::equal(poly_state_, kVoiceIdle);
      poly_mask attack_mask = poly_float::equal(poly_state_, kVoiceOn);
      poly_mask hold_mask = poly_float::equal(poly_state_, kVoiceHold);
      poly_mask decay_mask = poly_float::equal(poly_state_, kVoiceDecay);
      poly_mask release_mask = poly_float::equal(poly_state_, kVoiceOff);
      poly_mask kill_mask = poly_float::equal(poly_state_, kVoiceKill);

      sustain += delta_sustain;
      droop_ += (poly_float(1.0f) - droop_) * (leak_coefficient & decay_mask);
      poly_float sustain_target = sustain * (poly_float(1.0f) - leak_depth * droop_);

      poly_float target = (overshoot & attack_mask) + (peak & hold_mask) + (sustain_target & decay_mask);
      poly_float coefficient = (attack_coefficient & attack_mask) + (poly_float(1.0f) & hold_mask) +
                               (decay_coefficient & decay_mask) + (release_coefficient & release_mask);
      poly_float charged = value_ + (target - value_) * coefficient;
      poly_float kill_value = start_value_ * (poly_float(1.0f) - position_);
      value_ = utils::maskLoad(charged, value_, delay_mask);
      value_ = utils::maskLoad(value_, kill_value, kill_mask);

      // Timed stages: delay, hold and the kill fade.
      position_ += (delta_delay & delay_mask) + (delta_hold & hold_mask) + (delta_kill & kill_mask);
      position_ = utils::min(position_, 1.0f);
      poly_mask timed_end = poly_float::greaterThanOrEqual(position_, 1.0f);

      poly_mask peaked = attack_mask & poly_float::greaterThanOrEqual(value_, peak);
      value_ = utils::maskLoad(value_, peak, peaked);
      poly_mask silent = release_mask & poly_float::lessThan(value_, kAnalogSilence);
      value_ = utils::maskLoad(value_, 0.0f, silent);
      start_value_ = utils::maskLoad(start_value_, 0.0f, silent);

      poly_mask to_attack = delay_mask & timed_end;
      poly_mask to_hold = peaked & has_hold_mask;
      poly_mask to_decay = (peaked & ~has_hold_mask) | (hold_mask & timed_end);
      poly_state_ = utils::maskLoad(poly_state_, kVoiceOn, to_attack);
      poly_state_ = utils::maskLoad(poly_state_, kVoiceHold, to_hold);
      poly_state_ = utils::maskLoad(poly_state_, kVoiceDecay, to_decay);
      poly_state_ = utils::maskLoad(poly_state_, kVoiceKill, silent);
      position_ = utils::maskLoad(position_, 0.0f, to_attack | to_hold | to_decay);
      position_ = utils::maskLoad(position_, 1.0f, silent);

      value_ = utils::clamp(value_, 0.0f, 1.0f);
      audio_out[i] = value_;
    }

    output(kValue)->trigger_value = audio_out[0];
    attack_power_ = -input(kAttackPower)->at(0);
    decay_power_ = input(kDecayPower)->at(0);
    release_power_ = input(kReleasePower)->at(0);
    sustain_ = sustain_end;
    output(kPhase)->buffer[0] = poly_state_ + position_;
  }

  void Envelope::processAudioRate(int num_samples) {
    if (input(kAnalogMode)->at(0)[0] >= 0.5f) {
      processAnalog(num_samples);
      return;
    }

    poly_float delta_time = 1.0f / getSampleRate();
    mono_float delta_sample = 1.0f / num_samples;

    poly_float sustain_end = utils::clamp(input(kSustain)->at(0), 0.0f, 1.0f);
    
    poly_float delay_time = tempoSyncedTime(utils::max(input(kDelay)->at(0), 0.0f));
    poly_float delta_delay = delta_time / utils::max(delay_time, 0.0000001f);
    
    poly_float attack_time = tempoSyncedTime(utils::max(input(kAttack)->at(0), 0.000000001f));
    poly_float delta_attack = delta_time / attack_time;
    poly_float attack_power_end = -input(kAttackPower)->at(0);

    poly_float hold_time = tempoSyncedTime(utils::max(input(kHold)->at(0), 0.0f));
    poly_mask has_hold_mask = poly_float::notEqual(hold_time, 0.0f);
    poly_float delta_hold = delta_time / utils::max(hold_time, 0.0000001f);

    poly_float decay_time = tempoSyncedTime(utils::max(input(kDecay)->at(0), 0.000000001f));
    poly_float delta_decay = delta_time / decay_time;
    poly_float decay_power_end = input(kDecayPower)->at(0);

    poly_float release_time = tempoSyncedTime(utils::max(input(kRelease)->at(0), kMinimumClicklessRelease));
    poly_float delta_release = delta_time / release_time;
    poly_float release_power_end = input(kReleasePower)->at(0);

    poly_float delta_kill = delta_time * (1.0f / kVoiceKillTime);

    poly_mask trigger_mask = input(kTrigger)->source->trigger_mask;
    const Output* reset = input(kReset)->source;
    const poly_mask fresh_voice = reset->trigger_mask & poly_float::equal(reset->trigger_value, kVoiceOn);
    poly_float trigger_value = input(kTrigger)->source->trigger_value;
    poly_mask has_delay_mask = poly_float::notEqual(delay_time, 0.0f);
    poly_mask note_on_mask = poly_float::equal(trigger_value, kVoiceOn);
    trigger_value = utils::maskLoad(trigger_value, kVoiceIdle, has_delay_mask & note_on_mask);
    
    poly_int triggered_position = input(kTrigger)->source->trigger_offset;
    triggered_position = utils::maskLoad(num_samples, triggered_position, trigger_mask);

    poly_float current_position = position_;
    poly_float* audio_out = output(kValue)->buffer;

    for (int i = 0; i < num_samples;) {
      poly_mask triggering = trigger_mask & poly_int::equal(i, triggered_position);
      triggered_position = utils::maskLoad(triggered_position, num_samples, triggering);
      poly_float previous_state = poly_state_;
      poly_state_ = utils::maskLoad(poly_state_, trigger_value, triggering);
      current_position = utils::maskLoad(current_position, 0.0f, triggering);

      start_value_ = utils::maskLoad(start_value_, value_, triggering);
      if (retrigger_from_zero_)
        start_value_ = utils::maskLoad(start_value_, 0.0f, triggering & note_on_mask);
      poly_mask released_retrigger_mask = triggering & note_on_mask &
                                          poly_float::equal(previous_state, kVoiceKill);
      start_value_ = utils::maskLoad(start_value_, 0.0f, released_retrigger_mask);
      // Env 1 can retire a voice while another envelope is still releasing.
      // Only a fresh/dead voice resets to zero; audible retriggers keep value_.
      start_value_ = utils::maskLoad(start_value_, 0.0f,
          triggering & note_on_mask & fresh_voice & poly_int::equal(i, reset->trigger_offset));
      attack_power_ = utils::maskLoad(attack_power_, attack_power_end, triggering);
      decay_power_ = utils::maskLoad(decay_power_, decay_power_end, triggering);
      release_power_ = utils::maskLoad(release_power_, release_power_end, triggering);
      sustain_ = utils::maskLoad(sustain_, sustain_end, triggering);

      poly_mask delay_mask = poly_float::equal(poly_state_, kVoiceIdle);
      poly_mask attack_mask = poly_float::equal(poly_state_, kVoiceOn);
      poly_mask hold_mask = poly_float::equal(poly_state_, kVoiceHold);
      poly_mask decay_mask = poly_float::equal(poly_state_, kVoiceDecay);
      poly_mask release_mask = poly_float::equal(poly_state_, kVoiceOff);
      poly_mask kill_mask = poly_float::equal(poly_state_, kVoiceKill);

      poly_float delta_position = (delta_delay & delay_mask) + (delta_attack & attack_mask) +
                                  (delta_hold & hold_mask) + (delta_decay & decay_mask) +
                                  (delta_release & release_mask) + (delta_kill & kill_mask);

      poly_float from_power = (attack_power_ & attack_mask) +
                              (decay_power_ & decay_mask) +
                              (release_power_ & release_mask);
      poly_float to_power = (attack_power_end & attack_mask) +
                            (decay_power_end & decay_mask) +
                            (release_power_end & release_mask);

      poly_float power = utils::interpolate(from_power, to_power, i / (1.0f * num_samples));
      poly_float delta_power = (to_power - from_power) * delta_sample;

      poly_float cycles_remaining = (utils::ceil(current_position) - current_position) / delta_position;
      poly_float end_cycle = utils::maskLoad(num_samples, cycles_remaining + i, attack_mask);
      end_cycle = utils::min(end_cycle, utils::toFloat(triggered_position));
      int last_cycle = std::max((int)utils::minFloat(end_cycle), i + 1);

      poly_float current_sustain = utils::interpolate(sustain_, sustain_end, i / (1.0f * num_samples));
      poly_float start = utils::maskLoad(start_value_, 1.0f, decay_mask | hold_mask);
      poly_float end = (poly_float(1.0f) & (attack_mask | hold_mask)) + (current_sustain & decay_mask);
      poly_float delta_end = ((sustain_end - sustain_) * delta_sample) & decay_mask;

      current_position = processSection(audio_out, i, last_cycle, power, delta_power,
                                        current_position, delta_position, start, end, delta_end);
      i = last_cycle;

      value_ = audio_out[i - 1];
      
      poly_mask attack_transition_mask = delay_mask & poly_float::equal(current_position, 1.0f);      
      poly_mask hold_transition_mask = attack_mask & poly_float::equal(current_position, 1.0f) & has_hold_mask;
      poly_mask decay_turn_mask = (attack_mask & ~has_hold_mask) | hold_mask;
      poly_mask decay_transition_mask = decay_turn_mask & poly_float::equal(current_position, 1.0f);

      poly_state_ = utils::maskLoad(poly_state_, kVoiceOn, attack_transition_mask);
      poly_state_ = utils::maskLoad(poly_state_, kVoiceHold, hold_transition_mask);
      poly_state_ = utils::maskLoad(poly_state_, kVoiceDecay, decay_transition_mask);

      poly_mask transition_mask = attack_transition_mask | hold_transition_mask | decay_transition_mask;
      current_position = current_position & ~transition_mask;

      poly_mask dead_transition_mask = release_mask & poly_float::equal(current_position, 1.0f);
      poly_state_ = utils::maskLoad(poly_state_, kVoiceKill, dead_transition_mask);
    }

    output(kValue)->trigger_value = audio_out[0];

    position_ = current_position;
    attack_power_ = attack_power_end;
    decay_power_ = decay_power_end;
    release_power_ = release_power_end;
    sustain_ = sustain_end;
    output(kPhase)->buffer[0] = poly_state_ + position_;
  }
} // namespace vital
