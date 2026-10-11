// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <JuceHeader.h>
#include <array>
#include <atomic>
#include <memory>

namespace vital {
// Shared across all slots and plug-in instances. Immutable decoded clips stay
// owned by the worker until the last module is destroyed off the audio thread.
class TapeNoiseLibrary final : private juce::Thread {
 public:
  struct Clip {
    juce::AudioBuffer<float> audio;
    double sample_rate = 48000.0;
  };
  static constexpr int kNumClips = 27;
  static std::shared_ptr<TapeNoiseLibrary> shared();
  explicit TapeNoiseLibrary(juce::File directory = {});
  ~TapeNoiseLibrary() override;
  const Clip* request(int index) noexcept;
  int status(int index) const noexcept;
  static std::unique_ptr<Clip> decode(const juce::File& file);
  static juce::File factoryDirectory();
 private:
  void run() override;
  juce::File directory_;
  bool explicit_directory_;
  std::atomic<uint32_t> requested_ { 0 };
  std::array<std::atomic<const Clip*>, kNumClips> published_ {};
  std::array<std::atomic<int>, kNumClips> status_ {};
  std::array<std::unique_ptr<Clip>, kNumClips> clips_;
};
}
