// SPDX-License-Identifier: GPL-3.0-or-later
#include "multisample_player.h"
#include "multisample_slot.h"
#include "poly_values.h"

#include <algorithm>
#include <cmath>

namespace vital {
  namespace {
    // Frames index - 1 .. index + 2 of both channels, as floats: lanes 0-1 of
    // `before` hold left and right at index - 1, lanes 2-3 at index; `after`
    // likewise at index + 1 and index + 2.
    force_inline void loadStereoFrames(const int16_t* left, const int16_t* right, int64_t index,
                                       poly_float& before, poly_float& after) {
#if VITAL_SSE2
      const __m128i l = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(left + index - 1));
      const __m128i r = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(right + index - 1));
      const __m128i pairs = _mm_unpacklo_epi16(l, r);
      // Each 16-bit value into the top of a 32-bit lane, then shifted down with its sign.
      before = _mm_cvtepi32_ps(_mm_srai_epi32(_mm_unpacklo_epi16(pairs, pairs), 16));
      after = _mm_cvtepi32_ps(_mm_srai_epi32(_mm_unpackhi_epi16(pairs, pairs), 16));
#elif VITAL_NEON
      const int16x4x2_t pairs = vzip_s16(vld1_s16(left + index - 1), vld1_s16(right + index - 1));
      before = vcvtq_f32_s32(vmovl_s16(pairs.val[0]));
      after = vcvtq_f32_s32(vmovl_s16(pairs.val[1]));
#endif
    }

    // Lanes 2-3 moved to lanes 0-1.
    force_inline poly_float upperPair(poly_float value) {
#if VITAL_SSE2
      return _mm_movehl_ps(value.value, value.value);
#elif VITAL_NEON
      return vcombine_f32(vget_high_f32(value.value), vget_high_f32(value.value));
#endif
    }
    // -96 dB: an envelope below this is finished.
    constexpr float kMultiSampleSilence = 1.585e-5f;
    constexpr float kMultiSampleFadeSeconds = 0.006f;
    constexpr float kMultiSampleScale = 1.0f / 32768.0f;

    // A CC's value, 0..1, through the modulation's curve when it has one.
    float shapedCc(const multisample::CcMod& mod, const MultiSampleSlot& slot) {
      const float value = std::max(0.0f, std::min(127.0f, slot.ccValue(mod.cc)));
      const multisample::Instrument* instrument = slot.get();
      if (mod.curve < 0 || instrument == nullptr || mod.curve >= static_cast<int>(instrument->curve_tables.size()))
        return value / 127.0f;
      const auto& table = instrument->curve_tables[static_cast<size_t>(mod.curve)];
      const int index = std::min(126, static_cast<int>(value));
      const float t = value - static_cast<float>(index);
      return table[static_cast<size_t>(index)] + t * (table[static_cast<size_t>(index) + 1] - table[static_cast<size_t>(index)]);
    }

    float applyMultiSampleMods(float value, const std::vector<multisample::CcMod>& mods, const MultiSampleSlot& slot) {
      for (const multisample::CcMod& mod : mods)
        value += mod.amount * shapedCc(mod, slot);
      return value;
    }

  } // namespace

  void MultiSamplePlayer::panGains(float pan, float& left, float& right) {
    pan = std::max(-1.0f, std::min(1.0f, pan));
    left = pan <= 0.0f ? 1.0f : 1.0f - pan;
    right = pan >= 0.0f ? 1.0f : 1.0f + pan;
  }

  float MultiSamplePlayer::ccGain(const multisample::Region& region, const MultiSampleSlot& slot) {
    float gain = 1.0f;
    for (const multisample::CcMod& mod : region.amplitude_cc)
      gain *= std::max(0.0f, mod.amount * shapedCc(mod, slot));
    if (!region.volume_cc.empty())
      gain *= std::pow(10.0f, applyMultiSampleMods(0.0f, region.volume_cc, slot) / 20.0f);
    return gain;
  }

  float MultiSamplePlayer::ccPan(const multisample::Region& region, const MultiSampleSlot& slot) {
    return applyMultiSampleMods(region.pan, region.pan_cc, slot);
  }

  float MultiSamplePlayer::pitchOffset(float tuned_note) const {
    if (region_ == nullptr)
      return 0.0f;
    return (tuned_note - region_->pitch_keycenter) * region_->pitch_keytrack * 0.01f +
           static_cast<float>(region_->transpose) + region_->tune_cents * 0.01f;
  }

  void MultiSamplePlayer::start(const Start& start, const MultiSampleSlot& slot) {
    const multisample::Region& region = *start.region;
    region_ = &region;
    active_ = true;
    serial_ = start.serial;
    generation_ = start.generation;
    sample_rate_ = std::max(1.0f, start.sample_rate);
    ignore_note_off_ = start.ignore_note_off || region.loop_mode == multisample::LoopMode::kOneShot;

    end_ = std::max<int64_t>(0, std::min(region.data_frames, region.end));
    position_ = static_cast<double>(region.offset);
    if (region.offset_random > 0.0f)
      position_ += std::floor(start.random * region.offset_random);
    position_ = std::min(position_, static_cast<double>(std::max<int64_t>(0, end_ - 1)));
    loop_start_ = region.loop_start;
    loop_end_ = region.loop_end;
    looping_ = !start.ignore_note_off && loop_end_ > loop_start_ + 1 &&
               (region.loop_mode == multisample::LoopMode::kContinuous ||
                region.loop_mode == multisample::LoopMode::kSustain);
    alternate_ = region.loop_alternate && loop_end_ > loop_start_ + 2;
    turned_ = false;
    direction_ = 1.0;
    start_delay_ = std::max(0, start.offset) + static_cast<int>(region.delay * sample_rate_);
    note_off_at_ = -1;
    last_rate_ = 0.0;

    const int velocity = std::max(1, std::min(127, start.velocity));
    note_gain_ = region.baseGain() * region.velocity_gain[static_cast<size_t>(velocity)] * start.gain;
    last_gain_ = -1.0f;
    fade_ = 1.0f;
    fade_step_ = 0.0f;

    // Envelope times at note-on, with CC modulation and SF2 key scaling.
    multisample::Envelope envelope = region.ampeg;
    if (!region.ampeg_cc.empty()) {
      envelope.delay = applyMultiSampleMods(envelope.delay, region.ampeg_cc.delay, slot);
      envelope.start = applyMultiSampleMods(envelope.start, region.ampeg_cc.start, slot);
      envelope.attack = applyMultiSampleMods(envelope.attack, region.ampeg_cc.attack, slot);
      envelope.hold = applyMultiSampleMods(envelope.hold, region.ampeg_cc.hold, slot);
      envelope.decay = applyMultiSampleMods(envelope.decay, region.ampeg_cc.decay, slot);
      envelope.sustain = applyMultiSampleMods(envelope.sustain, region.ampeg_cc.sustain, slot);
      envelope.release = applyMultiSampleMods(envelope.release, region.ampeg_cc.release, slot);
    }
    if (region.hold_keytrack != 0.0f || region.decay_keytrack != 0.0f) {
      const float keys = 60.0f - start.key;
      envelope.hold *= std::pow(2.0f, keys * region.hold_keytrack / 1200.0f);
      envelope.decay *= std::pow(2.0f, keys * region.decay_keytrack / 1200.0f);
    }

    const float start_level = std::max(0.0f, std::min(1.0f, envelope.start));
    sustain_ = std::max(0.0f, std::min(1.0f, envelope.sustain));
    const float attack_samples = std::max(0.0f, envelope.attack) * sample_rate_;
    attack_step_ = attack_samples >= 1.0f ? (1.0f - start_level) / attack_samples : 2.0f;
    hold_samples_ = std::max(0.0f, envelope.hold) * sample_rate_;
    const float decay_samples = std::max(0.0f, envelope.decay) * sample_rate_;
    const float decay_target = std::max(sustain_, kMultiSampleSilence);
    decay_multiplier_ = decay_samples >= 1.0f ? std::pow(decay_target, 1.0f / decay_samples) : 0.0f;
    release_seconds_ = std::max(0.001f, envelope.release);

    stage_ = kDelay;
    stage_samples_ = static_cast<int>(std::max(0.0f, envelope.delay) * sample_rate_);
    level_ = start_level;
  }

  void MultiSamplePlayer::follow(const MultiSampleSlot& slot) {
    if (!active_ || region_ == nullptr || !region_->ampeg_dynamic)
      return;
    const multisample::Region& region = *region_;
    if (!region.ampeg_cc.release.empty()) {
      const float seconds = std::max(0.001f, applyMultiSampleMods(region.ampeg.release, region.ampeg_cc.release, slot));
      if (seconds != release_seconds_) {
        release_seconds_ = seconds;
        // A release under way goes on from its level at the new rate.
        if (stage_ == kRelease)
          release_multiplier_ = std::pow(kMultiSampleSilence, 1.0f / std::max(1.0f, seconds * sample_rate_));
      }
    }
    if (!region.ampeg_cc.sustain.empty()) {
      sustain_ = std::max(0.0f, std::min(1.0f, applyMultiSampleMods(region.ampeg.sustain, region.ampeg_cc.sustain, slot)));
      if (stage_ == kSustain)
        level_ = std::max(sustain_, kMultiSampleSilence);
    }
  }

  void MultiSamplePlayer::noteOff(int offset) {
    if (!active_ || ignore_note_off_)
      return;
    note_off_at_ = std::max(0, offset);
  }

  void MultiSamplePlayer::enterRelease(float seconds) {
    if (stage_ == kDone)
      return;
    if (stage_ == kDelay) {
      stage_ = kDone;
      level_ = 0.0f;
      return;
    }
    const float samples = std::max(1.0f, seconds * sample_rate_);
    release_multiplier_ = std::pow(kMultiSampleSilence, 1.0f / samples);
    stage_ = kRelease;
    if (region_ != nullptr && region_->loop_mode == multisample::LoopMode::kSustain) {
      // Out of the loop, the sample plays on forward to its end.
      looping_ = false;
      direction_ = 1.0;
    }
  }

  void MultiSamplePlayer::choke(float sample_rate) {
    if (!active_ || region_ == nullptr)
      return;
    sample_rate_ = std::max(1.0f, sample_rate);
    switch (region_->off_mode) {
      case multisample::OffMode::kNormal:
        ignore_note_off_ = false;
        enterRelease(release_seconds_);
        break;
      case multisample::OffMode::kTime:
        ignore_note_off_ = false;
        enterRelease(std::max(0.001f, region_->off_time));
        break;
      case multisample::OffMode::kFast:
      default:
        fadeOut(sample_rate);
        break;
    }
  }

  void MultiSamplePlayer::fadeOut(float sample_rate) {
    if (!active_)
      return;
    const float step = 1.0f / std::max(1.0f, kMultiSampleFadeSeconds * sample_rate);
    fade_step_ = std::max(fade_step_, step);
  }

  void MultiSamplePlayer::checkChoke(const MultiSampleSlot& slot, float sample_rate) {
    if (!active_ || region_ == nullptr || region_->off_by_slot < 0 || fade_step_ > 0.0f)
      return;
    if (slot.groupStamp(region_->off_by_slot) > serial_)
      choke(sample_rate);
  }

  void MultiSamplePlayer::advanceEnvelope() {
    switch (stage_) {
      case kDelay:
        if (stage_samples_ > 0) {
          stage_samples_--;
          return;
        }
        stage_ = kAttack;
        [[fallthrough]];
      case kAttack:
        level_ += attack_step_;
        if (level_ >= 1.0f) {
          level_ = 1.0f;
          stage_ = kHold;
          stage_samples_ = static_cast<int>(hold_samples_);
        }
        return;
      case kHold:
        if (stage_samples_ > 0) {
          stage_samples_--;
          return;
        }
        stage_ = kDecay;
        [[fallthrough]];
      case kDecay:
        level_ *= decay_multiplier_;
        if (level_ <= sustain_) {
          level_ = sustain_;
          stage_ = kSustain;
          if (sustain_ <= kMultiSampleSilence) {
            stage_ = kDone;
            level_ = 0.0f;
          }
        }
        return;
      case kSustain:
        return;
      case kRelease:
        level_ *= release_multiplier_;
        if (level_ < kMultiSampleSilence) {
          level_ = 0.0f;
          stage_ = kDone;
        }
        return;
      case kDone:
        return;
    }
  }

  // A plain stretch: no start delay, note-off or fade in it, the envelope
  // decaying, sustaining or releasing, no ping-pong loop, and every frame it
  // reads (and the position it ends on) clear of the sample's ends and the
  // loop's end. There every check render() makes per sample is known to pass.
  int MultiSamplePlayer::plainStretch(int from, int to, double rate, double rate_step, int64_t frames) const {
    if (start_delay_ > 0 || fade_step_ > 0.0f || (looping_ && alternate_))
      return 0;
    if (stage_ != kDecay && stage_ != kSustain && stage_ != kRelease)
      return 0;
    int limit = to;
    if (note_off_at_ >= from && note_off_at_ < limit)
      limit = note_off_at_;
    if (limit <= from || position_ < 1.0)
      return 0;
    // Frames up to index + 2 are read, so positions stay below frames - 2;
    // a loop wraps (and looks ahead) from loop_end - 2; a one-shot ends at end_.
    const double safe_end = looping_ ? static_cast<double>(std::min(loop_end_, frames)) - 2.0
                                     : static_cast<double>(std::min(end_, frames - 2));
    // The rate moves linearly, so its largest value is at one end; a frame of
    // margin covers the rounding of the running position.
    const double fastest = std::max(rate + rate_step, rate + (limit - from) * rate_step);
    if (fastest <= 0.0)
      return 0;
    const double room = (safe_end - 1.0 - position_) / fastest;
    if (room < 1.0)
      return 0;
    return static_cast<int>(std::min<double>(limit - from, std::floor(room)));
  }

  // The same arithmetic as render()'s loop, in the same order, for a plain
  // stretch, with the player's state in locals. Returns the sample it stopped
  // before; active_ is false if the envelope finished.
  template<bool kStereo, bool kWidth>
  int MultiSamplePlayer::renderStretch(float* left, float* right, int from, int to, const int16_t* data_left,
                                       const int16_t* data_right, float width, float gain_step, float left_step,
                                       float right_step, double rate_step, float& current_gain, float& current_left,
                                       float& current_right, double& rate) {
    double position = position_;
    float level = level_;
    Stage stage = stage_;
    const float fade = fade_;
    const float decay_multiplier = decay_multiplier_, release_multiplier = release_multiplier_, sustain = sustain_;
    auto cubic = [](const int16_t* data, int64_t index, float t) {
      const float xm1 = data[index - 1];
      const float x0 = data[index];
      const float x1 = data[index + 1];
      const float x2 = data[index + 2];
      const float c1 = 0.5f * (x1 - xm1);
      const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
      const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
      return (((c3 * t + c2) * t + c1) * t + x0) * kMultiSampleScale;
    };
    int i = from;
    for (; i < to; ++i) {
      current_gain += gain_step;
      current_left += left_step;
      current_right += right_step;
      rate += rate_step;

      if (stage == kDecay) {
        level *= decay_multiplier;
        if (level <= sustain) {
          level = sustain;
          stage = kSustain;
          if (sustain <= kMultiSampleSilence) {
            stage = kDone;
            level = 0.0f;
          }
        }
      }
      else if (stage == kRelease) {
        level *= release_multiplier;
        if (level < kMultiSampleSilence) {
          level = 0.0f;
          stage = kDone;
        }
      }
      if (stage == kDone) {
        active_ = false;
        break;
      }

      const int64_t index = static_cast<int64_t>(position);
      const float t = static_cast<float>(position - static_cast<double>(index));
      float sample_left, sample_right;
      if (kStereo) {
        // Both channels' cubic at once, lane by lane the same arithmetic.
        poly_float before, after;
        loadStereoFrames(data_left, data_right, index, before, after);
        const poly_float xm1 = before, x0 = upperPair(before), x1 = after, x2 = upperPair(after);
        const poly_float c1 = poly_float(0.5f) * (x1 - xm1);
        const poly_float c2 = xm1 - poly_float(2.5f) * x0 + poly_float(2.0f) * x1 - poly_float(0.5f) * x2;
        const poly_float c3 = poly_float(0.5f) * (x2 - xm1) + poly_float(1.5f) * (x0 - x1);
        const poly_float tt = t;
        const poly_float value = (((c3 * tt + c2) * tt + c1) * tt + x0) * poly_float(kMultiSampleScale);
        sample_left = value[0];
        sample_right = value[1];
      }
      else
        sample_left = sample_right = cubic(data_left, index, t);
      if (kWidth) {
        const float mid = 0.5f * (sample_left + sample_right);
        const float side = 0.5f * (sample_left - sample_right) * width;
        sample_left = mid + side;
        sample_right = mid - side;
      }
      const float amplitude = level * fade * current_gain;
      left[i] += sample_left * amplitude * current_left;
      right[i] += sample_right * amplitude * current_right;
      position += rate;
    }
    position_ = position;
    level_ = level;
    stage_ = stage;
    return i;
  }

  void MultiSamplePlayer::render(float* left, float* right, int num_samples, double rate_start, double rate_end,
                                 float gain, float left_gain, float right_gain) {
    if (!active_ || region_ == nullptr)
      return;
    const multisample::Region& region = *region_;
    if (region.left == nullptr || end_ <= 0) {
      active_ = false;
      return;
    }

    const int16_t* data_left = region.left;
    const int16_t* data_right = region.right != nullptr ? region.right : region.left;
    const bool stereo = data_right != data_left;
    const int64_t frames = region.data_frames;
    const float width = region.width;

    gain *= note_gain_;
    if (last_gain_ < 0.0f) {
      last_gain_ = gain;
      last_left_ = left_gain;
      last_right_ = right_gain;
    }
    if (last_rate_ <= 0.0)
      rate_start = rate_end;
    const float inverse = 1.0f / std::max(1, num_samples);
    const float gain_step = (gain - last_gain_) * inverse;
    const float left_step = (left_gain - last_left_) * inverse;
    const float right_step = (right_gain - last_right_) * inverse;
    const double rate_step = (rate_end - rate_start) / std::max(1, num_samples);
    float current_gain = last_gain_;
    float current_left = last_left_;
    float current_right = last_right_;
    double rate = rate_start;
    const int64_t loop_length = loop_end_ - loop_start_;
    // Ping-pong loops turn on their first and last frames.
    const int64_t loop_last = loop_end_ - 1;

    auto readAt = [&](const int16_t* data, int64_t index) -> float {
      if (looping_ && alternate_) {
        if (index > loop_last)
          index = 2 * loop_last - index;
        else if (turned_ && index < loop_start_)
          index = 2 * loop_start_ - index;
      }
      else if (looping_ && index >= loop_end_)
        index -= loop_length;
      index = std::max<int64_t>(0, std::min(frames - 1, index));
      return data[index];
    };

    for (int i = 0; i < num_samples; ++i) {
      // Most of a note's life is a plain stretch; play it without the checks.
      const int plain = plainStretch(i, num_samples, rate, rate_step, frames);
      if (plain > 0) {
        const bool widen = stereo && width != 1.0f;
        if (stereo && widen)
          i = renderStretch<true, true>(left, right, i, i + plain, data_left, data_right, width, gain_step, left_step,
                                        right_step, rate_step, current_gain, current_left, current_right, rate);
        else if (stereo)
          i = renderStretch<true, false>(left, right, i, i + plain, data_left, data_right, width, gain_step, left_step,
                                         right_step, rate_step, current_gain, current_left, current_right, rate);
        else
          i = renderStretch<false, false>(left, right, i, i + plain, data_left, data_right, width, gain_step, left_step,
                                          right_step, rate_step, current_gain, current_left, current_right, rate);
        if (!active_ || i >= num_samples)
          break;
      }

      current_gain += gain_step;
      current_left += left_step;
      current_right += right_step;
      rate += rate_step;

      if (i == note_off_at_) {
        note_off_at_ = -1;
        if (!ignore_note_off_)
          enterRelease(release_seconds_);
      }
      if (start_delay_ > 0) {
        start_delay_--;
        continue;
      }

      advanceEnvelope();
      if (fade_step_ > 0.0f) {
        fade_ -= fade_step_;
        if (fade_ <= 0.0f) {
          fade_ = 0.0f;
          stage_ = kDone;
        }
      }
      if (stage_ == kDone) {
        active_ = false;
        break;
      }

      const int64_t index = static_cast<int64_t>(position_);
      const float t = static_cast<float>(position_ - static_cast<double>(index));
      float values[2];
      const int num_channels = stereo ? 2 : 1;
      for (int channel = 0; channel < num_channels; ++channel) {
        const int16_t* data = channel == 0 ? data_left : data_right;
        float xm1, x0, x1, x2;
        const bool near_loop = looping_ && (index + 2 >= loop_end_ || (alternate_ && turned_ && index - 1 < loop_start_));
        if (index >= 1 && index + 2 < frames && !near_loop) {
          xm1 = data[index - 1];
          x0 = data[index];
          x1 = data[index + 1];
          x2 = data[index + 2];
        }
        else {
          xm1 = readAt(data, index - 1);
          x0 = readAt(data, index);
          x1 = readAt(data, index + 1);
          x2 = readAt(data, index + 2);
        }
        const float c1 = 0.5f * (x1 - xm1);
        const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
        const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
        values[channel] = (((c3 * t + c2) * t + c1) * t + x0) * kMultiSampleScale;
      }
      float sample_left = values[0];
      float sample_right = stereo ? values[1] : values[0];
      if (stereo && width != 1.0f) {
        const float mid = 0.5f * (sample_left + sample_right);
        const float side = 0.5f * (sample_left - sample_right) * width;
        sample_left = mid + side;
        sample_right = mid - side;
      }

      const float amplitude = level_ * fade_ * current_gain;
      left[i] += sample_left * amplitude * current_left;
      right[i] += sample_right * amplitude * current_right;

      if (looping_ && alternate_) {
        position_ += rate * direction_;
        const double first = static_cast<double>(loop_start_);
        const double last = static_cast<double>(loop_last);
        // Reflect off the ends; a rate wider than the loop bounces more than once.
        for (int bounce = 0; bounce < 8 && (position_ > last || (turned_ && position_ < first)); ++bounce) {
          if (position_ > last) {
            position_ = 2.0 * last - position_;
            direction_ = -1.0;
            turned_ = true;
          }
          else {
            position_ = 2.0 * first - position_;
            direction_ = 1.0;
          }
        }
        position_ = std::min(last, position_);
        if (turned_)
          position_ = std::max(first, position_);
        continue;
      }
      position_ += rate;
      if (looping_ && position_ >= static_cast<double>(loop_end_))
        position_ -= static_cast<double>(loop_length) * std::floor((position_ - loop_start_) / loop_length);
      else if (position_ >= static_cast<double>(end_)) {
        active_ = false;
        stage_ = kDone;
        break;
      }
    }

    note_off_at_ = -1;
    last_gain_ = gain;
    last_left_ = left_gain;
    last_right_ = right_gain;
    last_rate_ = rate_end;
  }
} // namespace vital
