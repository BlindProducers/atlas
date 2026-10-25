// SPDX-License-Identifier: GPL-3.0-or-later
#include "particles_engine.h"
// Before the processor: stmlib's macros would break JUCE's headers.
#include "zeroed_buffer.h"
#include "clouds/dsp/granular_processor.h"
#include "stmlib/utils/random.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>

namespace atlas::particles {
namespace {
  // The module's two memories: 118784 bytes of main RAM and the 64 KB core-
  // coupled RAM less the 128 bytes the firmware keeps for itself.
  constexpr size_t kLargeBufferBytes = 118784;
  constexpr size_t kSmallBufferBytes = 65536 - 128;

  int resamplerHalf(int in_rate, int out_rate) {
    if (in_rate == out_rate) return 0;
    const double stretch = std::max(1.0, static_cast<double>(in_rate) / out_rate);
    return static_cast<int>(std::ceil(maim::ResamplerKernel::kHalfWidth * stretch));
  }

  int16_t toShort(float sample) {
    // The 16-bit converter clips at full scale, as the module's input does.
    const float scaled = std::round(sample * 32768.0f);
    if (!(scaled > -32768.0f)) return -32768;
    if (scaled > 32767.0f) return 32767;
    return static_cast<int16_t>(scaled);
  }

  // The processor reads its tables as Interpolate(table, x, size), which at
  // x = 1 also reads the entry past the end (weighted by zero): the module's
  // controls stop just short of 1 here so it never does.
  constexpr float kTop = 0.99998f;
  float unit(float x) { return std::clamp(x, 0.0f, kTop); }

  void applyParameters(clouds::GranularProcessor& processor, const Parameters& p, bool trigger) {
    processor.set_playback_mode(static_cast<clouds::PlaybackMode>(std::clamp(p.mode, 0, kNumModes - 1)));
    processor.set_quality(std::clamp(p.quality, 0, kNumQualities - 1));
    processor.set_freeze(p.freeze);
    clouds::Parameters* out = processor.mutable_parameters();
    out->position = unit(p.position);
    out->size = unit(p.size);
    out->pitch = std::clamp(p.pitch, -48.0f, 48.0f);
    out->density = unit(p.density);
    out->texture = unit(p.texture);
    out->dry_wet = unit(p.dry_wet);
    out->stereo_spread = unit(p.stereo_spread);
    out->feedback = unit(p.feedback);
    out->reverb = unit(p.reverb);
    // The module's TRIGGER jack is both: a rising edge for one block and the
    // gate it leaves high meanwhile.
    out->trigger = trigger;
    out->gate = trigger;
  }
}

// The processor and the two memories it records into. The processor's
// constructor initialises nothing, and Init leaves the freeze filter, the
// playback mode, the parameters and the feedback buffer as they were: on the
// module they sit in zeroed RAM, so here they live in zeroed storage too.
struct Engine::Processor {
  vital::ZeroedBuffer<uint8_t> storage = vital::allocateZeroed<uint8_t>(sizeof(clouds::GranularProcessor));
  vital::ZeroedBuffer<uint8_t> large = vital::allocateZeroed<uint8_t>(kLargeBufferBytes);
  vital::ZeroedBuffer<uint8_t> small = vital::allocateZeroed<uint8_t>(kSmallBufferBytes);
  clouds::GranularProcessor* processor = nullptr;

  Processor() { start(); }
  ~Processor() { processor->~GranularProcessor(); }

  void start() {
    static_assert(alignof(clouds::GranularProcessor) <= 16);
    if (processor != nullptr) processor->~GranularProcessor();
    std::memset(storage.get(), 0, sizeof(clouds::GranularProcessor));
    processor = new (storage.get()) clouds::GranularProcessor();
    processor->Init(large.get(), kLargeBufferBytes, small.get(), kSmallBufferBytes);
  }
};

Engine::Engine() = default;
Engine::~Engine() = default;

int Engine::latencyFor(int sample_rate) {
  if (sample_rate <= 0) return 0;
  if (sample_rate == kNativeRate) return kBlockSize;
  // Up-sampled output m needs the block holding 32 kHz sample
  // m * 32000 / rate + the up-sampler's half width, which needs the host
  // samples up to that block's end plus the down-sampler's half width.
  const int down_half = resamplerHalf(sample_rate, kNativeRate);
  const int up_half = resamplerHalf(kNativeRate, sample_rate);
  const double ratio = static_cast<double>(sample_rate) / kNativeRate;
  return down_half + static_cast<int>(std::ceil((up_half + kBlockSize + 1) * ratio)) + 2;
}

void Engine::prepare(int sample_rate) {
  sample_rate_ = std::max(1, sample_rate);
  identity_ = sample_rate_ == kNativeRate;
  if (processor_ == nullptr) processor_ = std::make_unique<Processor>();
  block_in_.assign(2 * kBlockSize, 0);
  block_out_.assign(2 * kBlockSize, 0);

  down_kernel_.reset();
  up_kernel_.reset();
  if (!identity_) {
    down_kernel_ = std::make_unique<maim::ResamplerKernel>(sample_rate_, kNativeRate);
    up_kernel_ = std::make_unique<maim::ResamplerKernel>(kNativeRate, sample_rate_);
    const int down_step = (sample_rate_ + kNativeRate - 1) / kNativeRate + 1;
    const int up_step = kBlockSize + (kNativeRate + sample_rate_ - 1) / sample_rate_ + 1;
    for (int ch = 0; ch < 2; ++ch) {
      down_[ch].reserve(down_kernel_->half(), down_step);
      down_[ch].bind(down_kernel_.get());
      up_[ch].reserve(up_kernel_->half(), up_step);
      up_[ch].bind(up_kernel_.get());
    }
  }
  // The output waits out the latency, then a block's worth of host samples
  // arrives at a time.
  const int burst = static_cast<int>(std::ceil(kBlockSize * static_cast<double>(sample_rate_) / kNativeRate)) + 2;
  for (auto& delay : delay_) delay.reserve(latencyFor(sample_rate_) + 2 * burst + 16);
  reset();
}

void Engine::reset() {
  if (processor_ == nullptr) return;
  processor_->start();
  applyParameters(*processor_->processor, parameters_, false);
  processor_->processor->Prepare();
  for (int ch = 0; ch < 2; ++ch) {
    down_[ch].reset();
    up_[ch].reset();
    delay_[ch].clear();
  }
  std::fill(block_in_.begin(), block_in_.end(), 0);
  block_fill_ = 0;
  startup_remaining_ = latencyFor(sample_rate_);
  trigger_pending_ = false;
}

void Engine::setParameters(const Parameters& parameters) {
  parameters_ = parameters;
}

void Engine::runBlock() {
  clouds::GranularProcessor& processor = *processor_->processor;
  applyParameters(processor, parameters_, trigger_pending_);
  trigger_pending_ = false;
  processor.Process(reinterpret_cast<clouds::ShortFrame*>(block_in_.data()),
                    reinterpret_cast<clouds::ShortFrame*>(block_out_.data()), kBlockSize);
  // The firmware prepares between interrupts: buffer and mode changes, and
  // the stretch mode's correlation search.
  processor.Prepare();
  // The module halves its signal into the 16-bit converter, and its
  // equal-power crossfade leaves either end at 0.707; the analogue stage makes
  // up for both. 2 root 2 does that here, so at Mix 0 the dry signal leaves at
  // the level it came in.
  constexpr float kOutputScale = 2.8284271f / 32768.0f;
  for (int i = 0; i < kBlockSize; ++i) {
    const float left = block_out_[static_cast<size_t>(2 * i)] * kOutputScale;
    const float right = block_out_[static_cast<size_t>(2 * i + 1)] * kOutputScale;
    if (identity_) {
      delay_[0].push(left);
      delay_[1].push(right);
    }
    else {
      up_[0].push(left);
      up_[1].push(right);
    }
  }
  if (!identity_) {
    float left = 0.0f, right = 0.0f;
    while (up_[0].pull(left)) {
      up_[1].pull(right);
      delay_[0].push(left);
      delay_[1].push(right);
    }
  }
}

void Engine::process(const float* in_left, const float* in_right, float* out_left, float* out_right, int samples) {
  if (processor_ == nullptr) {
    std::fill(out_left, out_left + samples, 0.0f);
    std::fill(out_right, out_right + samples, 0.0f);
    return;
  }
  const float gain = parameters_.input_gain;
  for (int s = 0; s < samples; ++s) {
    const float left = in_left[s] * gain;
    const float right = in_right[s] * gain;
    const auto take = [this](float l, float r) {
      block_in_[static_cast<size_t>(2 * block_fill_)] = toShort(l);
      block_in_[static_cast<size_t>(2 * block_fill_ + 1)] = toShort(r);
      if (++block_fill_ == kBlockSize) {
        block_fill_ = 0;
        runBlock();
      }
    };
    if (identity_) {
      take(left, right);
    }
    else {
      down_[0].push(left);
      down_[1].push(right);
      float native_left = 0.0f, native_right = 0.0f;
      while (down_[0].pull(native_left)) {
        down_[1].pull(native_right);
        take(native_left, native_right);
      }
    }
    if (startup_remaining_ > 0) {
      --startup_remaining_;
      out_left[s] = out_right[s] = 0.0f;
    }
    else {
      out_left[s] = delay_[0].pop();
      out_right[s] = delay_[1].pop();
    }
  }
}

void seedRandom(uint32_t seed) {
  stmlib::Random::Seed(seed);
}

std::vector<int16_t> renderReference(const std::vector<int16_t>& input, const std::vector<Parameters>& parameters,
                                     const std::vector<bool>& triggers) {
  auto large = vital::allocateZeroed<uint8_t>(kLargeBufferBytes);
  auto small = vital::allocateZeroed<uint8_t>(kSmallBufferBytes);
  auto storage = vital::allocateZeroed<uint8_t>(sizeof(clouds::GranularProcessor));
  auto* processor = new (storage.get()) clouds::GranularProcessor();
  processor->Init(large.get(), kLargeBufferBytes, small.get(), kSmallBufferBytes);
  const size_t blocks = input.size() / (2 * kBlockSize);
  std::vector<int16_t> input_copy(input.begin(), input.end());
  std::vector<int16_t> output(blocks * 2 * kBlockSize, 0);
  if (!parameters.empty()) applyParameters(*processor, parameters.front(), false);
  processor->Prepare();
  for (size_t index = 0; index < blocks && index < parameters.size(); ++index) {
    applyParameters(*processor, parameters[index], index < triggers.size() && triggers[index]);
    const size_t offset = index * 2 * kBlockSize;
    processor->Process(reinterpret_cast<clouds::ShortFrame*>(input_copy.data() + offset),
                       reinterpret_cast<clouds::ShortFrame*>(output.data() + offset), kBlockSize);
    processor->Prepare();
  }
  processor->~GranularProcessor();
  return output;
}
}
