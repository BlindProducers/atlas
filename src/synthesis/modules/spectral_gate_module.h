#pragma once

#include "synth_module.h"
#include "atlas_fft.h"
#include "JuceHeader.h"

#include <array>
#include <memory>
#include <vector>

namespace vital {

  // SpectralSuite's strong/weak magnitude gate, using a streaming STFT.
  // All FFT storage is prepared at construction, including inactive FX slots.
  class SpectralGateModule : public SynthModule {
    public:
      static constexpr int kFftOrder = 11;
      static constexpr int kFftSize = 1 << kFftOrder;
      static constexpr int kHopSize = kFftSize / 4;
      static constexpr int kMinFftOrder = 8;
      static constexpr int kMaxFftOrder = 13;
      static constexpr int kMaxFftSize = 1 << kMaxFftOrder;
      static constexpr int kNumFftSizes = kMaxFftOrder - kMinFftOrder + 1;

      explicit SpectralGateModule(std::string prefix = "");
      void init() override;
      void processWithInput(const poly_float* audio_in, int num_samples) override;
      void hardReset() override;
      void enable(bool enable) override;
      void setSampleRate(int sample_rate) override;
      void setOversampleAmount(int oversample) override;
      Processor* clone() const override { VITAL_ASSERT(false); return nullptr; }
      int getFftSize() const { return fft_size_; }
      int getLatencySamples() const override;

    private:
      void processFrame();
      int requestedFftIndex() const;

      std::string prefix_;
      Output* cutoff_ = nullptr;
      Output* balance_ = nullptr;
      Output* tilt_ = nullptr;
      Output* mix_ = nullptr;
      Value* tilt_enabled_ = nullptr;
      Value* fft_size_control_ = nullptr;
      std::array<std::unique_ptr<atlas::Fft>, kNumFftSizes> ffts_;
      std::array<std::vector<float>, kNumFftSizes> windows_;
      alignas(16) std::array<float, 2 * kMaxFftSize> fft_buffer_ {};
      std::array<poly_float, kMaxFftSize> input_buffer_;
      std::array<poly_float, kMaxFftSize> output_buffer_;
      int fft_index_ = kFftOrder - kMinFftOrder;
      int fft_size_ = kFftSize;
      int samples_since_reset_ = 0;
      poly_float last_output_ = 0.0f;
      poly_float transition_tail_ = 0.0f;
      bool size_transition_ = false;
      int position_ = 0;
      int hop_position_ = 0;
      poly_float mix_amount_ = 1.0f;
      bool mix_initialized_ = false;

      JUCE_LEAK_DETECTOR(SpectralGateModule)
  };
}
