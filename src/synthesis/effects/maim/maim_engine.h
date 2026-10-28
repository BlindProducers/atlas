// SPDX-License-Identifier: GPL-3.0-or-later
// The Maim effect: audio through a real MP3, Opus, GSM or LPC codec and
// back, with Maim's circuit bends on the MP3 encoders, packet loss on the
// rest, and Drive, filters, Makeup and Mix around them. Adapted from Maim
// by Arden Butterfield (GPL v3); see README.atlas.md.
//
// Atlas runs its effects at an oversampled rate, so each codec runs at its
// own rate behind a pair of resamplers. Changing codec, or the LPC rate,
// starts the new one on a second path and crossfades once it has warmed up;
// the MP3 manager handles encoder and bitrate changes of its own.
#pragma once
#include "maim_common.h"
#include "queue_buffer.h"
#include "resampler.h"
#include <memory>
#include <vector>

namespace atlas::maim {

class FrameCodec;
class Mp3Manager;
class OpusCodec;
class GsmCodec;
class LpcCodec;

class Engine {
 public:
  static constexpr int kFadeLength = 512;
  // Host samples the up-sampler's output waits after it starts, beyond the
  // codec samples it arrives in bursts of, so a late burst never leaves a
  // sample short: ceil(processing rate / codec rate) plus this.
  static constexpr int kSlackMargin = 2;
  static int slackFor(int sample_rate, int codec_rate) { return (sample_rate + codec_rate - 1) / codec_rate + kSlackMargin; }

  Engine();
  ~Engine();

  // Builds every codec and resampler for this rate. Allocates and starts the
  // MP3 build thread: not for the audio thread.
  void prepare(int sample_rate);
  bool prepared() const { return sample_rate_ > 0; }
  int sampleRate() const { return sample_rate_; }
  // Clears the signal paths. The MP3 encoders keep their history: LAME has
  // no reset short of a rebuild.
  void reset();
  // Once a block, before process. `realtime` builds MP3 encoders off thread.
  void setParameters(const Parameters& parameters, bool realtime);
  void process(const float* in_left, const float* in_right, float* out_left, float* out_right, int samples);

  // The delay the last parameters ask for, which the host is told.
  int latencySamples() const { return latencyFor(parameters_.encoder, parameters_.lpc_rate_index, sample_rate_); }
  static int latencyFor(int encoder, int lpc_rate_index, int sample_rate);
  // The longest latency any setting has at this rate.
  static int maxLatencyFor(int sample_rate);
  // The MP3 encoders' rate for a processing rate: the rate itself at 32,
  // 44.1 or 48 kHz and their doublings, else the highest they take below it.
  static int mp3RateFor(int sample_rate);
  static int codecRateFor(int encoder, int lpc_rate_index, int sample_rate);
  // The codec's own delay at its rate, the frame included.
  static int codecLatencyFor(int encoder, int lpc_rate_index, int sample_rate);
  static int resamplerHalfFor(int in_rate, int out_rate);

  // For the tests.
  const Parameters& parameters() const { return parameters_; }
  int currentCodecRate() const;
  bool mp3Settled() const;
  int mp3CurrentBitrate() const;
  int opusReportedLookahead() const;
  bool switching() const { return switching_; }

 private:
  enum Family { kFamilyMp3, kFamilyOpus, kFamilyGsm, kFamilyLpc };
  static Family familyOf(int encoder);

  // One codec's path from the processing rate and back.
  struct Path {
    FrameCodec* codec = nullptr;
    Family family = kFamilyMp3;
    int lpc_rate = 0;
    bool hold = false;      // LPC: hold each decoded sample instead of interpolating
    bool identity = true;   // the codec runs at the processing rate
    Resampler down[2], up[2];
    QueueBuffer slack[2];
    int slack_needed = 0;   // host samples the first output waits
    int slack_remaining = -1;  // counting down from the first output; -1 before it
    float held[2] { 0.0f, 0.0f };
    void reserve(int max_half, int max_step);
    void bind(FrameCodec* new_codec, Family new_family, int new_lpc_rate, const ResamplerKernel* down_kernel,
              const ResamplerKernel* up_kernel, bool hold_output);
    void unbind() { codec = nullptr; }
    void tick(float& left, float& right);
  };

  const ResamplerKernel* kernelFor(int in_rate, int out_rate) const;
  void bindPath(Path& path, int index, Family family, int lpc_rate);
  int actualLatency() const;
  void updateFilters();

  int sample_rate_ = 0;
  Parameters parameters_;
  std::unique_ptr<Mp3Manager> mp3_;
  std::unique_ptr<OpusCodec> opus_;
  std::unique_ptr<GsmCodec> gsm_;
  std::unique_ptr<LpcCodec> lpc_[2];
  std::vector<std::unique_ptr<ResamplerKernel>> kernels_;

  Path paths_[2];
  int active_ = 0;
  bool switching_ = false;
  int warmup_remaining_ = 0;
  int fade_remaining_ = 0;

  // The dry signal, delayed by the wet path's latency for the mix.
  std::vector<float> dry_[2];
  int dry_capacity_ = 0;
  int dry_write_ = 0;
  int dry_latency_ = 0;
  int dry_previous_latency_ = 0;
  int dry_fade_remaining_ = 0;

  float drive_gain_ = 1.0f, drive_target_ = 1.0f;
  float makeup_gain_ = 1.0f, makeup_target_ = 1.0f;
  float mix_ = 1.0f, mix_target_ = 1.0f;
  float smoothing_ = 0.0f;
  float low_cut_hz_ = -1.0f, high_cut_hz_ = -1.0f;
  Biquad low_cut_[2], high_cut_[2];
};
}
