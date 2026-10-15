#include "spectral_gate_module.h"

#include "poly_utils.h"

#include <cmath>

namespace vital {

  SpectralGateModule::SpectralGateModule(std::string prefix) : SynthModule(0, 1), prefix_(prefix) {
    // Prepare every supported transform and periodic Hann window off the audio
    // thread. Four overlapping squared windows sum to exactly 1.5 at every size.
    for (int index = 0; index < kNumFftSizes; ++index) {
      const int order = kMinFftOrder + index;
      const int size = 1 << order;
      ffts_[index] = std::make_unique<juce::dsp::FFT>(order);
      windows_[index].resize(size);
      for (int i = 0; i < size; ++i)
        windows_[index][i] = 0.5f - 0.5f * std::cos(2.0f * juce::MathConstants<float>::pi * i / size);
    }
    hardReset();
  }

  void SpectralGateModule::init() {
    fft_size_control_ = createBaseControl(prefix_ + "spectral_gate_fft_size");
    cutoff_ = createMonoModControl(prefix_ + "spectral_gate_cutoff");
    balance_ = createMonoModControl(prefix_ + "spectral_gate_balance");
    tilt_ = createMonoModControl(prefix_ + "spectral_gate_tilt");
    tilt_enabled_ = createBaseControl(prefix_ + "spectral_gate_tilt_enabled");
    mix_ = createMonoModControl(prefix_ + "spectral_gate_mix");
    SynthModule::init();
  }

  void SpectralGateModule::processFrame() {
    const poly_float cutoff = utils::clamp(cutoff_->buffer[0], 0.0f, 1.0f);
    const poly_float balance = utils::clamp(balance_->buffer[0], 0.0f, 1.0f);
    const poly_float tilt = tilt_enabled_->value() ?
        utils::clamp(tilt_->buffer[0], -1.0f, 1.0f) : poly_float(0.0f);

    // The voice handler sums both voices into both SIMD pairs, so lanes 2-3
    // repeat lanes 0-1: only lanes 0-1 are transformed, and processWithInput
    // copies their output to lanes 2-3.
    for (int lane = 0; lane < 2; ++lane) {
      for (int i = 0; i < fft_size_; ++i)
        fft_buffer_[i] = input_buffer_[(position_ + i) & (fft_size_ - 1)][lane] * windows_[fft_index_][i];
      ffts_[fft_index_]->performRealOnlyForwardTransform(fft_buffer_.data(), true);

      // Preserve SpectralSuite's normalized magnitudes and nonlinear controls.
      // The high test deliberately takes precedence when its two ranges overlap.
      const float gate_high = std::pow(cutoff[lane], 10.0f);
      const float gate_low = cutoff[lane] * 0.6f;
      const float strong = std::pow(balance[lane], 3.0f);
      const float weak = std::pow(1.0f - balance[lane], 4.0f);
      for (int bin = 0; bin <= fft_size_ / 2; ++bin) {
        const float real = fft_buffer_[2 * bin];
        const float imaginary = fft_buffer_[2 * bin + 1];
        const float magnitude = std::sqrt(real * real + imaginary * imaginary) * (2.0f / fft_size_);
        const float fraction = static_cast<float>(bin) / (fft_size_ / 2);
        const float offset = (fraction - 0.5f) * 2.0f * gate_high * tilt[lane];
        const float gain = magnitude >= gate_high + offset ? strong :
                           (magnitude < gate_low + 0.8f * offset ? weak : 0.0f);
        // Multiplying both Cartesian components retains the original phase.
        fft_buffer_[2 * bin] = real * gain;
        fft_buffer_[2 * bin + 1] = imaginary * gain;
      }

      ffts_[fft_index_]->performRealOnlyInverseTransform(fft_buffer_.data());
      for (int i = 0; i < fft_size_; ++i) {
        const int index = (position_ + i) & (fft_size_ - 1);
        output_buffer_[index].set(lane, output_buffer_[index][lane] +
            fft_buffer_[i] * windows_[fft_index_][i] * (2.0f / 3.0f));
      }
    }
  }

  int SpectralGateModule::requestedFftIndex() const {
    const float requested = fft_size_control_ ? fft_size_control_->value() : kFftOrder - kMinFftOrder;
    return requested >= 0.0f && requested <= kNumFftSizes - 1 ?
        static_cast<int>(requested + 0.5f) : kFftOrder - kMinFftOrder;
  }

  int SpectralGateModule::getLatencySamples() const {
    return 1 << (kMinFftOrder + requestedFftIndex());
  }

  void SpectralGateModule::processWithInput(const poly_float* audio_in, int num_samples) {
    if (num_samples <= 0)
      return;
    SynthModule::process(num_samples);
    // Clamp before shifting: neither automation nor malformed state may select
    // an FFT larger than 8192. Changing size never creates a plan or buffer here.
    const int next_index = requestedFftIndex();
    if (next_index != fft_index_) {
      const bool was_running = samples_since_reset_ > 0;
      const poly_float previous_output = last_output_;
      fft_index_ = next_index;
      fft_size_ = 1 << (kMinFftOrder + fft_index_);
      hardReset();
      // Fade the previous output while the new FFT fills, then fade in its
      // correctly aligned dry/wet output. Do not leak old spectral frames.
      size_transition_ = was_running;
      transition_tail_ = previous_output;
    }

    const poly_float target_mix = utils::clamp(mix_->buffer[0], 0.0f, 1.0f);
    if (!mix_initialized_) {
      mix_amount_ = target_mix;
      mix_initialized_ = true;
    }
    const float smoothing = 1.0f - std::exp(-1.0f / (0.005f * getSampleRate()));
    poly_float* audio_out = output()->buffer;
    for (int i = 0; i < num_samples; ++i) {
      // The input ring also supplies the dry signal delayed by exactly N samples.
      const poly_float dry = input_buffer_[position_];
      const poly_float wet = output_buffer_[position_];
      input_buffer_[position_] = audio_in[i];
      output_buffer_[position_] = 0.0f;
      mix_amount_ += (target_mix - mix_amount_) * smoothing;
      poly_float result = utils::interpolate(dry, wet, mix_amount_);
      if (size_transition_) {
        const int fade_samples = std::max(1, static_cast<int>(getSampleRate() * 0.005f));
        const float fade_out = std::max(0.0f, 1.0f - static_cast<float>(samples_since_reset_ + 1) / fade_samples);
        const float fade_in = juce::jlimit(0.0f, 1.0f,
            static_cast<float>(samples_since_reset_ - fft_size_) / fade_samples);
        result = result * fade_in + transition_tail_ * fade_out;
        if (fade_in >= 1.0f)
          size_transition_ = false;
      }
      audio_out[i] = poly_float(result[0], result[1], result[0], result[1]);
      last_output_ = result;
      // Only startup/transition needs a count; keep it bounded during long runs.
      samples_since_reset_ = std::min(samples_since_reset_ + 1, fft_size_ + getSampleRate());
      position_ = (position_ + 1) & (fft_size_ - 1);
      if (++hop_position_ == (fft_size_ / 4)) {
        hop_position_ = 0;
        processFrame();
      }
    }
  }

  void SpectralGateModule::hardReset() {
    input_buffer_.fill(0.0f);
    output_buffer_.fill(0.0f);
    position_ = 0;
    hop_position_ = 0;
    mix_initialized_ = false;
    samples_since_reset_ = 0;
    last_output_ = 0.0f;
    transition_tail_ = 0.0f;
    size_transition_ = false;
  }

  void SpectralGateModule::enable(bool enable) {
    if (enabled() != enable)
      hardReset();
    SynthModule::enable(enable);
  }

  void SpectralGateModule::setSampleRate(int sample_rate) {
    const int previous_rate = getSampleRate();
    SynthModule::setSampleRate(sample_rate);
    if (getSampleRate() != previous_rate)
      hardReset();
  }

  void SpectralGateModule::setOversampleAmount(int oversample) {
    const int previous_oversample = getOversampleAmount();
    SynthModule::setOversampleAmount(oversample);
    if (getOversampleAmount() != previous_oversample)
      hardReset();
  }
}
