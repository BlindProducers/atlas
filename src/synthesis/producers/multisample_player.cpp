// SPDX-License-Identifier: GPL-3.0-or-later
#include "multisample_player.h"
#include "multisample_slot.h"

#include <algorithm>
#include <cmath>

namespace vital {
  namespace {
    // -96 dB: an envelope below this is finished.
    constexpr float kMultiSampleSilence = 1.585e-5f;
    constexpr float kMultiSampleFadeSeconds = 0.006f;
    constexpr float kMultiSampleScale = 1.0f / 32768.0f;

    float applyMultiSampleMods(float value, const std::vector<multisample::CcMod>& mods, const MultiSampleSlot& slot) {
      for (const multisample::CcMod& mod : mods)
        value += mod.amount * slot.ccValue(mod.cc) / 127.0f;
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
      gain *= std::max(0.0f, mod.amount * slot.ccValue(mod.cc) / 127.0f);
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
    if (region_ != nullptr && region_->loop_mode == multisample::LoopMode::kSustain)
      looping_ = false;
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

    auto readAt = [&](const int16_t* data, int64_t index) -> float {
      if (looping_ && index >= loop_end_)
        index -= loop_length;
      index = std::max<int64_t>(0, std::min(frames - 1, index));
      return data[index];
    };

    for (int i = 0; i < num_samples; ++i) {
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
        if (index >= 1 && index + 2 < frames && !(looping_ && index + 2 >= loop_end_)) {
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
