// SPDX-License-Identifier: GPL-3.0-or-later
#include "maim_engine.h"
#include "gsm_codec.h"
#include "lpc_codec.h"
#include "mp3_manager.h"
#include "opus_codec.h"
#include <algorithm>
#include <cmath>

namespace atlas::maim {
namespace {
  constexpr float kPi = 3.14159265358979323846f;
  constexpr float kSmoothingSeconds = 0.01f;

  int clampInt(int value, int low, int high) { return value < low ? low : value > high ? high : value; }
  float clampFloat(float value, float low, float high) {
    return !std::isfinite(value) ? low : value < low ? low : value > high ? high : value;
  }
}

void Biquad::lowPass(float rate, float cutoff) {
  const float w0 = 2.0f * kPi * cutoff / rate;
  const float alpha = std::sin(w0) / (2.0f * 0.70710678f);
  const float cos_w0 = std::cos(w0);
  const float a0 = 1.0f + alpha;
  b0 = (1.0f - cos_w0) / 2.0f / a0;
  b1 = (1.0f - cos_w0) / a0;
  b2 = b0;
  a1 = -2.0f * cos_w0 / a0;
  a2 = (1.0f - alpha) / a0;
}

void Biquad::highPass(float rate, float cutoff) {
  const float w0 = 2.0f * kPi * cutoff / rate;
  const float alpha = std::sin(w0) / (2.0f * 0.70710678f);
  const float cos_w0 = std::cos(w0);
  const float a0 = 1.0f + alpha;
  b0 = (1.0f + cos_w0) / 2.0f / a0;
  b1 = -(1.0f + cos_w0) / a0;
  b2 = b0;
  a1 = -2.0f * cos_w0 / a0;
  a2 = (1.0f - alpha) / a0;
}

// ------------------------------------------------------------- static maths

Engine::Family Engine::familyOf(int encoder) {
  switch (encoder) {
    case kOpus: return kFamilyOpus;
    case kGsm: return kFamilyGsm;
    case kLpc: return kFamilyLpc;
    default: return kFamilyMp3;
  }
}

int Engine::mp3RateFor(int sample_rate) {
  for (int base : { 48000, 44100 })
    for (int doubling = 0; doubling < 5; ++doubling)
      if ((base << doubling) == sample_rate) return base;
  if (sample_rate >= 48000) return 48000;
  if (sample_rate >= 44100) return 44100;
  return 32000;
}

int Engine::codecRateFor(int encoder, int lpc_rate_index, int sample_rate) {
  switch (familyOf(encoder)) {
    case kFamilyOpus: return kOpusRate;
    case kFamilyGsm: return kGsmRate;
    case kFamilyLpc: return LpcCodec::rateFor(lpc_rate_index, sample_rate);
    default: return mp3RateFor(sample_rate);
  }
}

int Engine::codecLatencyFor(int encoder, int lpc_rate_index, int sample_rate) {
  switch (familyOf(encoder)) {
    case kFamilyOpus: return OpusCodec::kFrame + OpusCodec::kLookahead;
    case kFamilyGsm: return GsmCodec::kFrame;
    case kFamilyLpc: return LpcCodec::frameSizeFor(LpcCodec::rateFor(lpc_rate_index, sample_rate));
    default: return Mp3Manager::latencyFor(encoder == kBlade ? kBlade : kLame);
  }
}

int Engine::resamplerHalfFor(int in_rate, int out_rate) {
  if (in_rate == out_rate) return 0;
  const double stretch = std::max(1.0, static_cast<double>(in_rate) / out_rate);
  return static_cast<int>(std::ceil(ResamplerKernel::kHalfWidth * stretch));
}

int Engine::latencyFor(int encoder, int lpc_rate_index, int sample_rate) {
  if (sample_rate <= 0) return 0;
  const int codec_rate = codecRateFor(encoder, lpc_rate_index, sample_rate);
  const int codec_latency = codecLatencyFor(encoder, lpc_rate_index, sample_rate);
  if (codec_rate == sample_rate) return codec_latency;
  const double ratio = static_cast<double>(sample_rate) / codec_rate;
  double total = resamplerHalfFor(sample_rate, codec_rate) + codec_latency * ratio;
  if (familyOf(encoder) == kFamilyLpc) {
    // Held samples change when the down-sampler yields one: half a codec
    // sample later on average.
    total += 0.5 * ratio;
  }
  else {
    total += resamplerHalfFor(codec_rate, sample_rate) * ratio + slackFor(sample_rate, codec_rate);
  }
  return static_cast<int>(std::lround(total));
}

int Engine::maxLatencyFor(int sample_rate) {
  int longest = 0;
  for (int encoder = 0; encoder < kNumEncoders; ++encoder) {
    if (encoder == kLpc) {
      for (int index = 0; index < kNumLpcRates; ++index)
        longest = std::max(longest, latencyFor(encoder, index, sample_rate));
    }
    else {
      longest = std::max(longest, latencyFor(encoder, 0, sample_rate));
    }
  }
  return longest;
}

// -------------------------------------------------------------------- Path

void Engine::Path::reserve(int max_half, int max_step) {
  for (int ch = 0; ch < 2; ++ch) {
    down[ch].reserve(max_half, max_step);
    up[ch].reserve(max_half, max_step);
    slack[ch].reserve(4 * max_step + 64);
  }
}

void Engine::Path::bind(FrameCodec* new_codec, Family new_family, int new_lpc_rate,
                        const ResamplerKernel* down_kernel, const ResamplerKernel* up_kernel, bool hold_output) {
  codec = new_codec;
  family = new_family;
  lpc_rate = new_lpc_rate;
  hold = hold_output;
  identity = down_kernel == nullptr;
  for (int ch = 0; ch < 2; ++ch) {
    down[ch].bind(down_kernel);
    up[ch].bind(up_kernel);
    slack[ch].clear();
    held[ch] = 0.0f;
  }
  slack_needed = down_kernel != nullptr ? slackFor(down_kernel->inRate(), down_kernel->outRate()) : 0;
  slack_remaining = -1;
  if (codec != nullptr) codec->resetFraming();
}

void Engine::Path::tick(float& left, float& right) {
  if (codec == nullptr) {
    left = right = 0.0f;
    return;
  }
  if (identity) {
    codec->processSample(left, right);
    return;
  }
  down[0].push(left);
  down[1].push(right);
  float codec_left = 0.0f, codec_right = 0.0f;
  while (down[0].pull(codec_left)) {
    down[1].pull(codec_right);
    codec->processSample(codec_left, codec_right);
    if (hold) {
      held[0] = codec_left;
      held[1] = codec_right;
    }
    else {
      up[0].push(codec_left);
      up[1].push(codec_right);
    }
  }
  if (hold) {
    left = held[0];
    right = held[1];
    return;
  }
  float out_left = 0.0f, out_right = 0.0f;
  while (up[0].pull(out_left)) {
    up[1].pull(out_right);
    slack[0].push(out_left);
    slack[1].push(out_right);
  }
  // The output starts slack_needed host samples after the first sample
  // arrives, which covers a whole codec sample's worth of them arriving
  // late; from then on one leaves every host sample.
  if (slack_remaining != 0) {
    if (slack_remaining < 0) {
      if (slack[0].size() == 0) {
        left = right = 0.0f;
        return;
      }
      slack_remaining = slack_needed + 1;
    }
    if (--slack_remaining > 0) {
      left = right = 0.0f;
      return;
    }
  }
  left = slack[0].pop();
  right = slack[1].pop();
}

// ------------------------------------------------------------------ Engine

Engine::Engine() : mp3_(std::make_unique<Mp3Manager>()), opus_(std::make_unique<OpusCodec>()),
                   gsm_(std::make_unique<GsmCodec>()) {
  for (auto& lpc : lpc_) lpc = std::make_unique<LpcCodec>();
}

Engine::~Engine() = default;

const ResamplerKernel* Engine::kernelFor(int in_rate, int out_rate) const {
  if (in_rate == out_rate) return nullptr;
  for (const auto& kernel : kernels_)
    if (kernel->inRate() == in_rate && kernel->outRate() == out_rate) return kernel.get();
  return nullptr;
}

void Engine::prepare(int sample_rate) {
  sample_rate_ = std::max(1, sample_rate);
  const int mp3_rate = mp3RateFor(sample_rate_);
  mp3_->prepare(mp3_rate);
  opus_->prepare();
  gsm_->prepare();
  for (auto& lpc : lpc_) lpc->prepare();

  // One kernel per rate pair the codecs can need at this processing rate.
  kernels_.clear();
  std::vector<int> codec_rates { mp3_rate, kOpusRate, kGsmRate };
  for (int index = 0; index < kNumLpcRates; ++index) codec_rates.push_back(LpcCodec::rateFor(index, sample_rate_));
  int max_half = 0, max_step = 1;
  for (int codec_rate : codec_rates) {
    if (codec_rate == sample_rate_) continue;
    for (int direction = 0; direction < 2; ++direction) {
      const int in = direction == 0 ? sample_rate_ : codec_rate;
      const int out = direction == 0 ? codec_rate : sample_rate_;
      if (kernelFor(in, out) != nullptr) continue;
      // The LPC vocoder's output is held, not interpolated back up.
      const bool lpc_rate = std::find(kLpcRates.begin(), kLpcRates.end(), codec_rate) != kLpcRates.end() &&
                            codec_rate != mp3_rate && codec_rate != kOpusRate && codec_rate != kGsmRate;
      if (direction == 1 && lpc_rate) continue;
      kernels_.push_back(std::make_unique<ResamplerKernel>(in, out));
      max_half = std::max(max_half, kernels_.back()->half());
      max_step = std::max(max_step, (in + out - 1) / out);
    }
  }
  for (auto& path : paths_) {
    path.reserve(max_half, max_step);
    path.unbind();
  }

  dry_capacity_ = maxLatencyFor(sample_rate_) + kFadeLength + 16;
  for (auto& line : dry_) line.assign(static_cast<size_t>(dry_capacity_), 0.0f);
  dry_write_ = 0;
  smoothing_ = 1.0f - std::exp(-1.0f / (kSmoothingSeconds * static_cast<float>(sample_rate_)));
  low_cut_hz_ = high_cut_hz_ = -1.0f;
  active_ = 0;
  switching_ = false;
  bindPath(paths_[0], 0, familyOf(parameters_.encoder), LpcCodec::rateFor(parameters_.lpc_rate_index, sample_rate_));
  dry_latency_ = dry_previous_latency_ = actualLatency();
  dry_fade_remaining_ = 0;
  reset();
}

void Engine::bindPath(Path& path, int index, Family family, int lpc_rate) {
  FrameCodec* codec = nullptr;
  int codec_rate = sample_rate_;
  switch (family) {
    case kFamilyOpus: codec = opus_.get(); codec_rate = kOpusRate; opus_->reset(); break;
    case kFamilyGsm: codec = gsm_.get(); codec_rate = kGsmRate; gsm_->reset(); break;
    case kFamilyLpc:
      codec = lpc_[index].get();
      codec_rate = lpc_rate;
      lpc_[index]->setRate(lpc_rate);
      break;
    default: codec = mp3_.get(); codec_rate = mp3RateFor(sample_rate_); mp3_->resetFraming(); break;
  }
  path.bind(codec, family, lpc_rate, kernelFor(sample_rate_, codec_rate), kernelFor(codec_rate, sample_rate_),
            family == kFamilyLpc);
}

void Engine::reset() {
  if (!prepared()) return;
  if (switching_) {
    paths_[1 - active_].unbind();
    switching_ = false;
  }
  Path& active = paths_[active_];
  bindPath(active, active_, active.family, active.lpc_rate);
  for (auto& line : dry_) std::fill(line.begin(), line.end(), 0.0f);
  dry_write_ = 0;
  for (int ch = 0; ch < 2; ++ch) {
    low_cut_[ch].reset();
    high_cut_[ch].reset();
  }
  drive_gain_ = drive_target_;
  makeup_gain_ = makeup_target_;
  mix_ = mix_target_;
}

int Engine::currentCodecRate() const {
  const Path& path = paths_[active_];
  return path.codec == nullptr ? sample_rate_ : path.codec->codecRate();
}

bool Engine::mp3Settled() const { return mp3_->settled(); }
int Engine::mp3CurrentBitrate() const { return mp3_->currentBitrate(); }
int Engine::opusReportedLookahead() const { return opus_->reportedLookahead(); }

int Engine::actualLatency() const {
  const Path& path = paths_[active_];
  int encoder = parameters_.encoder;
  int lpc_index = parameters_.lpc_rate_index;
  switch (path.family) {
    case kFamilyMp3: encoder = mp3_->currentEncoder(); break;
    case kFamilyOpus: encoder = kOpus; break;
    case kFamilyGsm: encoder = kGsm; break;
    case kFamilyLpc:
      encoder = kLpc;
      for (int index = 0; index < kNumLpcRates; ++index)
        if (LpcCodec::rateFor(index, sample_rate_) == path.lpc_rate) { lpc_index = index; break; }
      break;
  }
  return latencyFor(encoder, lpc_index, sample_rate_);
}

void Engine::updateFilters() {
  const float nyquist = static_cast<float>(sample_rate_) * 0.5f - 1.0f;
  const float low = clampFloat(parameters_.low_cut_hz, 1.0f, nyquist);
  const float high = clampFloat(parameters_.high_cut_hz, 1.0f, nyquist);
  if (low != low_cut_hz_) {
    low_cut_hz_ = low;
    for (auto& filter : low_cut_) filter.highPass(static_cast<float>(sample_rate_), low);
  }
  if (high != high_cut_hz_) {
    high_cut_hz_ = high;
    for (auto& filter : high_cut_) filter.lowPass(static_cast<float>(sample_rate_), high);
  }
}

void Engine::setParameters(const Parameters& parameters, bool realtime) {
  parameters_ = parameters;
  parameters_.encoder = clampInt(parameters_.encoder, 0, kNumEncoders - 1);
  parameters_.bitrate_index = clampInt(parameters_.bitrate_index, 0, kNumBitrates - 1);
  parameters_.lpc_rate_index = clampInt(parameters_.lpc_rate_index, 0, kNumLpcRates - 1);
  if (!prepared()) return;

  // Which codec, and for LPC at which rate, the parameters ask for.
  const Family wanted = familyOf(parameters_.encoder);
  const int wanted_lpc_rate = LpcCodec::rateFor(parameters_.lpc_rate_index, sample_rate_);
  Path& active = paths_[active_];
  const bool same = active.codec != nullptr && active.family == wanted &&
                    (wanted != kFamilyLpc || active.lpc_rate == wanted_lpc_rate);
  if (!same) {
    if (switching_) {
      // A second change while the first is still fading: finish that one now.
      paths_[1 - active_].unbind();
      switching_ = false;
    }
    const int next = 1 - active_;
    bindPath(paths_[next], next, wanted, wanted_lpc_rate);
    // The old path keeps playing until the new one has filled its pipeline.
    switching_ = true;
    warmup_remaining_ = latencyFor(parameters_.encoder, parameters_.lpc_rate_index, sample_rate_);
    fade_remaining_ = kFadeLength;
    active_ = next;
  }

  // The MP3 encoders: which one and at what bitrate, and every bend.
  if (wanted == kFamilyMp3)
    mp3_->setTarget(parameters_.encoder == kBlade ? kBlade : kLame, kBitrates[static_cast<size_t>(parameters_.bitrate_index)], realtime);
  Mp3Bends bends;
  bends.error = clampFloat(parameters_.error, 0.0f, 1.0f);
  bends.butterfly_straight = clampFloat(parameters_.butterfly_straight, 0.0f, 1.0f);
  bends.butterfly_crossed = clampFloat(parameters_.butterfly_crossed, 0.0f, 1.0f);
  bends.band_step = clampInt(parameters_.band_step, 8, 18);
  bends.band_invert = parameters_.band_invert;
  bends.window_step = clampInt(parameters_.window_step, -64, 64);
  bends.shift = clampInt(parameters_.shift, -100, 100);
  bends.level_shift = clampFloat(parameters_.level_shift, -1.0f, 1.0f);
  bends.feedback = clampFloat(parameters_.feedback, 0.0f, 1.0f);
  bends.turbo = clampFloat(parameters_.turbo, 0.0f, 1.0f);
  bends.tilt = clampFloat(parameters_.tilt, -1.0f, 1.0f);
  const int band_shift = clampInt(parameters_.band_shift, -31, 31);
  for (int band = 0; band < 32; ++band) {
    const int source = parameters_.band_reverse ? 31 - band : band;
    bends.band_order[static_cast<size_t>(band)] = ((source + band_shift) % 32 + 32) % 32;
  }
  mp3_->setBends(bends);

  // The packet codecs.
  const float loss_period = clampFloat(parameters_.loss_period, 0.05f, 2.05f);
  const float loss_width = clampFloat(parameters_.loss_width, 0.0f, 1.0f);
  const float loss_jitter = clampFloat(parameters_.loss_jitter, 0.0f, 1.0f);
  const float error = bends.error;
  const int loss_mode = parameters_.loss_mode == kStick ? kStick : kDrop;
  opus_->setParameters(kBitrates[static_cast<size_t>(parameters_.bitrate_index)], bends.turbo, error, loss_mode);
  opus_->packetLoss().setParameters(loss_period, loss_width, loss_jitter);
  gsm_->setParameters(error, loss_mode);
  gsm_->packetLoss().setParameters(loss_period, loss_width, loss_jitter);
  for (auto& lpc : lpc_) {
    lpc->setParameters(error, loss_mode);
    lpc->packetLoss().setParameters(loss_period, loss_width, loss_jitter);
  }

  drive_target_ = dbToGain(clampFloat(parameters_.drive_db, -36.0f, 36.0f));
  makeup_target_ = dbToGain(clampFloat(parameters_.makeup_db, -36.0f, 36.0f));
  mix_target_ = clampFloat(parameters_.mix, 0.0f, 1.0f);
  updateFilters();
}

void Engine::process(const float* in_left, const float* in_right, float* out_left, float* out_right, int samples) {
  if (!prepared()) {
    if (out_left != in_left) std::copy(in_left, in_left + samples, out_left);
    if (out_right != in_right) std::copy(in_right, in_right + samples, out_right);
    return;
  }
  Path& active = paths_[active_];
  Path& old = paths_[1 - active_];
  // The latency the wet path has now: the MP3 manager may have swapped
  // encoders since the last block.
  const int wet_latency = actualLatency();
  for (int i = 0; i < samples; ++i) {
    drive_gain_ += (drive_target_ - drive_gain_) * smoothing_;
    makeup_gain_ += (makeup_target_ - makeup_gain_) * smoothing_;
    mix_ += (mix_target_ - mix_) * smoothing_;

    const float dry_left = in_left[i], dry_right = in_right[i];
    dry_[0][static_cast<size_t>(dry_write_)] = dry_left;
    dry_[1][static_cast<size_t>(dry_write_)] = dry_right;

    float wet_left = dry_left * drive_gain_, wet_right = dry_right * drive_gain_;
    float old_left = wet_left, old_right = wet_right;
    active.tick(wet_left, wet_right);
    if (switching_) {
      old.tick(old_left, old_right);
      if (warmup_remaining_ > 0) {
        --warmup_remaining_;
        wet_left = old_left;
        wet_right = old_right;
      }
      else {
        const float mix = 1.0f - static_cast<float>(fade_remaining_) / static_cast<float>(kFadeLength);
        wet_left = old_left + (wet_left - old_left) * mix;
        wet_right = old_right + (wet_right - old_right) * mix;
        if (--fade_remaining_ <= 0) {
          switching_ = false;
          old.unbind();
        }
      }
    }
    for (int ch = 0; ch < 2; ++ch) {
      float& wet = ch == 0 ? wet_left : wet_right;
      wet = high_cut_[ch].process(low_cut_[ch].process(wet));
      if (!std::isfinite(wet)) {
        wet = 0.0f;
        low_cut_[ch].reset();
        high_cut_[ch].reset();
      }
    }
    wet_left *= makeup_gain_;
    wet_right *= makeup_gain_;

    // The dry signal, delayed to line up with the wet one. A change of
    // latency crossfades between the two taps once the wet path has moved.
    const int target = switching_ && warmup_remaining_ > 0 ? dry_latency_ : wet_latency;
    if (target != dry_latency_ && dry_fade_remaining_ == 0) {
      dry_previous_latency_ = dry_latency_;
      dry_latency_ = std::min(target, dry_capacity_ - 1);
      dry_fade_remaining_ = kFadeLength;
    }
    const auto delayed = [&](int ch, int latency) {
      int index = dry_write_ - latency;
      if (index < 0) index += dry_capacity_;
      return dry_[ch][static_cast<size_t>(index)];
    };
    float delayed_left = delayed(0, dry_latency_), delayed_right = delayed(1, dry_latency_);
    if (dry_fade_remaining_ > 0) {
      const float mix = 1.0f - static_cast<float>(dry_fade_remaining_) / static_cast<float>(kFadeLength);
      delayed_left = delayed(0, dry_previous_latency_) + (delayed_left - delayed(0, dry_previous_latency_)) * mix;
      delayed_right = delayed(1, dry_previous_latency_) + (delayed_right - delayed(1, dry_previous_latency_)) * mix;
      --dry_fade_remaining_;
    }
    if (++dry_write_ >= dry_capacity_) dry_write_ = 0;

    out_left[i] = delayed_left + (wet_left - delayed_left) * mix_;
    out_right[i] = delayed_right + (wet_right - delayed_right) * mix_;
  }
}
}
