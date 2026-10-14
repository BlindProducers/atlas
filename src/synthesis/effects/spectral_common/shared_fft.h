// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <juce_dsp/juce_dsp.h>

#include <map>
#include <memory>
#include <mutex>

namespace atlas {
  // A juce::dsp::FFT of the given order, shared by every caller in the process
  // on Apple platforms. There each FFT owns a vDSP setup of several hundred KB,
  // and every voice's Filter Table and Spectral source held their own. A vDSP
  // setup is read-only once built and can be used from several threads at
  // once, and the results do not change. JUCE's portable FFT serialises
  // perform() on a spin lock, so on other platforms each caller keeps its own
  // (they are small there).
  inline std::shared_ptr<const juce::dsp::FFT> sharedFft(int order) {
   #if JUCE_MAC || JUCE_IOS
    static std::mutex mutex;
    static std::map<int, std::weak_ptr<const juce::dsp::FFT>> cache;
    const std::lock_guard<std::mutex> lock(mutex);
    auto& entry = cache[order];
    if (auto fft = entry.lock())
      return fft;
    auto fft = std::make_shared<const juce::dsp::FFT>(order);
    entry = fft;
    return fft;
   #else
    return std::make_shared<const juce::dsp::FFT>(order);
   #endif
  }
} // namespace atlas
