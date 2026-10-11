// SPDX-License-Identifier: GPL-3.0-or-later
#include "braids_engine.h"

#include "braids/macro_oscillator.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>

namespace vital {
namespace braids_engine {
  static_assert(kNumModels == braids::MACRO_OSC_SHAPE_LAST, "Braids model count");
  static_assert(kNumResolutions == braids::RESOLUTION_LAST, "Braids resolution count");
  static_assert(kNumSampleRates == braids::SAMPLE_RATE_LAST, "Braids sample rate count");

  namespace {
    // braids.cc: the BITS and RATE settings applied to each rendered block.
    constexpr uint16_t kBitReductionMasks[kNumResolutions] = {
      0xc000, 0xe000, 0xf000, 0xf800, 0xff00, 0xfff0, 0xffff
    };
    constexpr int kDecimationFactors[kNumSampleRates] = { 24, 12, 6, 4, 3, 2, 1 };

    // The oscillator's constructors leave its members uninitialised; start
    // from zeroed memory so a voice is deterministic before Init runs.
    braids::MacroOscillator* createOscillator() {
      void* memory = ::operator new(sizeof(braids::MacroOscillator));
      std::memset(memory, 0, sizeof(braids::MacroOscillator));
      auto* oscillator = new (memory) braids::MacroOscillator();
      oscillator->Init();
      return oscillator;
    }

    int16_t toParameter(float value) {
      if (!(value >= 0.0f))
        return 0;
      return static_cast<int16_t>(std::min(32767.0f, value * 32767.0f + 0.5f));
    }
  } // namespace

  Voice::Voice() : oscillator_(createOscillator()) {
    oscillator_->set_shape(braids::MACRO_OSC_SHAPE_CSAW);
  }

  Voice::Voice(const Voice& other) :
      oscillator_(createOscillator()), model_(other.model_), resolution_(other.resolution_),
      sample_rate_(other.sample_rate_), timbre_(other.timbre_), color_(other.color_), pitch_(other.pitch_) {
    oscillator_->set_shape(static_cast<braids::MacroOscillatorShape>(model_));
  }

  Voice::~Voice() = default;

  void Voice::reset() {
    oscillator_->Init();
    oscillator_->set_shape(static_cast<braids::MacroOscillatorShape>(model_));
  }

  void Voice::strike() {
    oscillator_->Strike();
  }

  void Voice::setModel(int model) {
    model_ = std::max(0, std::min(kNumModels - 1, model));
    oscillator_->set_shape(static_cast<braids::MacroOscillatorShape>(model_));
  }

  void Voice::setParameters(float timbre, float color) {
    timbre_ = toParameter(timbre);
    color_ = toParameter(color);
  }

  void Voice::setNote(float note) {
    // Pitch is in 1/128 semitones, 0 to 16383 (braids.cc clamps the same way).
    const float pitch = std::round(note * 128.0f);
    pitch_ = static_cast<int16_t>(std::isfinite(pitch) ? std::max(0.0f, std::min(16383.0f, pitch)) : 60 << 7);
  }

  void Voice::setResolution(int resolution) {
    resolution_ = std::max(0, std::min(kNumResolutions - 1, resolution));
  }

  void Voice::setSampleRate(int sample_rate) {
    sample_rate_ = std::max(0, std::min(kNumSampleRates - 1, sample_rate));
  }

  void Voice::render(float* output) {
    // No external sync: the sync input stays low.
    static constexpr uint8_t kNoSync[kBlockSize] = {};
    int16_t buffer[kBlockSize];
    oscillator_->set_parameters(timbre_, color_);
    oscillator_->set_pitch(pitch_);
    oscillator_->Render(kNoSync, buffer, kBlockSize);

    const int decimation = kDecimationFactors[sample_rate_];
    const uint16_t mask = kBitReductionMasks[resolution_];
    int16_t held = 0;
    for (int i = 0; i < kBlockSize; ++i) {
      if (i % decimation == 0)
        held = static_cast<int16_t>(buffer[i] & mask);
      output[i] = static_cast<float>(held) * (1.0f / 32768.0f);
    }
  }
} // namespace braids_engine
} // namespace vital
