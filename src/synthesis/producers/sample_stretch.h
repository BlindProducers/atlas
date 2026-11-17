// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <memory>

namespace atlas {

// Signalsmith Stretch (third_party/signalsmith-stretch) behind a small
// interface: the Sample type's engine, built with fast math in the synthesis
// unity file, never sees the library's headers, which this class compiles with
// strict floating point and the project's PFFFT. One stretcher plays one
// stereo voice. Only configure() allocates.
class SampleStretcher {
public:
  static constexpr int kChannels = 2;
  // Input samples per output sample, at most: four times faster, or a quarter
  // of the speed.
  static constexpr float kMaxSpeed = 4.0f;
  static constexpr float kMinSpeed = 0.25f;

  SampleStretcher();
  ~SampleStretcher();
  SampleStretcher(const SampleStretcher&) = delete;
  SampleStretcher& operator=(const SampleStretcher&) = delete;

  void configure(int sample_rate);
  void setTranspose(float semitones);
  void setPreserveFormants(bool preserve);
  // How many input samples per channel start() reads at this speed.
  int startLength(float speed) const;
  // Forgets the past and lines the output up with the first sample of
  // `input`, which holds startLength(speed) samples per channel.
  void start(const float* const* input, float speed);
  // Reads input_samples per channel and writes output_samples per channel;
  // their ratio is the time stretch.
  void process(const float* const* input, int input_samples, float* const* output, int output_samples);
  // The output's delay behind the input, in samples, once started.
  int latency() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace atlas
