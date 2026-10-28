// SPDX-License-Identifier: GPL-3.0-or-later
// The Particles effect's engine: Mutable Instruments' granular processor
// (MIT, clouds/ beside this file, unmodified) run at its own 32 kHz in
// 32-sample blocks behind a pair of resamplers. Atlas's own code; see
// README.atlas.md. This is the only Atlas file that includes the processor's
// headers, so its stmlib macros stay out of the unity builds.
#pragma once
#include "maim/queue_buffer.h"
#include "maim/resampler.h"
#include <cstdint>
#include <memory>
#include <vector>

namespace atlas::particles {

enum Mode { kGranular, kStretch, kLoopingDelay, kSpectral, kNumModes };
// The processor's own quality numbering: bit 0 mono, bit 1 8-bit at 16 kHz.
enum Quality { kStereo16, kMono16, kStereo8, kMono8, kNumQualities };

constexpr int kNativeRate = 32000;
constexpr int kBlockSize = 32;

// The processor's controls, already in its own units.
struct Parameters {
  int mode = kGranular;
  int quality = kStereo16;
  float input_gain = 1.0f;   // linear, applied before the 16-bit input
  float position = 0.0f;     // 0..1
  float size = 0.5f;         // 0..1
  float pitch = 0.0f;        // semitones, -48..48
  float density = 0.65f;     // 0..1; 0.5 sows no grains
  float texture = 0.5f;      // 0..1
  float dry_wet = 0.5f;      // 0..1
  float stereo_spread = 0.0f;
  float feedback = 0.0f;
  float reverb = 0.0f;
  bool freeze = false;
};

class Engine {
 public:
  Engine();
  ~Engine();

  // Allocates the processor's buffers and the resampling kernels for this
  // processing rate: not for the audio thread.
  void prepare(int sample_rate);
  bool prepared() const { return sample_rate_ > 0; }
  int sampleRate() const { return sample_rate_; }
  // Empties the recording buffer and every signal path, as switching the
  // module on does.
  void reset();
  // Once a block, before process; the processor reads them every 32 samples.
  void setParameters(const Parameters& parameters);
  // Fires one grain (the module's TRIGGER input) in the next 32-sample block.
  void trigger() { trigger_pending_ = true; }
  void process(const float* in_left, const float* in_right, float* out_left, float* out_right, int samples);

  // Host samples from input to output at a processing rate: the down-sampler's
  // half width, a whole block at 32 kHz and the up-sampler's half width, as
  // the output waits for them.
  static int latencyFor(int sample_rate);
  int latencySamples() const { return latencyFor(sample_rate_); }

 private:
  struct Processor;

  void runBlock();

  int sample_rate_ = 0;
  bool identity_ = true;
  Parameters parameters_;
  bool trigger_pending_ = false;

  std::unique_ptr<Processor> processor_;
  std::unique_ptr<maim::ResamplerKernel> down_kernel_, up_kernel_;
  maim::Resampler down_[2], up_[2];
  maim::QueueBuffer delay_[2];
  int block_fill_ = 0;
  int startup_remaining_ = 0;
  std::vector<int16_t> block_in_, block_out_;
};

// Seeds the grains' random generator (stmlib's, one per thread, which Braids
// shares), so a test can start two renders alike.
void seedRandom(uint32_t seed);

// The processor driven directly at 32 kHz, block by block, the way the
// original firmware's desktop test does: the reference the engine test
// compares Engine against. `input` and the result are interleaved 16-bit
// stereo; `parameters` holds one entry per 32-sample block.
std::vector<int16_t> renderReference(const std::vector<int16_t>& input, const std::vector<Parameters>& parameters,
                                     const std::vector<bool>& triggers);
}
