/* Copyright 2013-2019 Matt Tytel
 *
 * vital is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * vital is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with vital.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "phase_shift_module.h"

#include "futils.h"
#include "poly_utils.h"

#include <cmath>

namespace vital {

  PhaseShiftModule::PhaseShiftModule(std::string prefix) :
      SynthModule(0, 1), amount_(nullptr), tone_(nullptr), mix_(nullptr), prefix_(prefix),
      fft_(kFftOrder), input_write_(0), output_read_(0), hop_counter_(0), history_write_frame_(0),
      mix_amount_(1.0f) {
    createWindow();
    for (int bin = 0; bin <= kFftSize / 2; ++bin)
      mapped_index_[bin] = logMappedIndex(bin);

    for (int channel = 0; channel < kChannels; ++channel) {
      input_ring_[channel].assign(kFftSize, 0.0f);
      output_ring_[channel].assign(kOutputRingSize, 0.0f);
      fft_buffers_[channel].assign(kFftSize, std::complex<float>());
      fft_work_buffers_[channel].assign(kFftSize, std::complex<float>());
      spectral_history_[channel].assign(kHistorySize * kFftSize, std::complex<float>());
    }
  }

  void PhaseShiftModule::init() {
    amount_ = createMonoModControl(prefix_ + "phase_shift_amount");
    tone_ = createMonoModControl(prefix_ + "phase_shift_tone");
    mix_ = createMonoModControl(prefix_ + "phase_shift_dry_wet");

    SynthModule::init();
  }

  void PhaseShiftModule::createWindow() {
    for (int i = 0; i < kFftSize; ++i)
      window_[i] = 0.5f - 0.5f * std::cos(2.0f * kPi * i / static_cast<mono_float>(kFftSize));
  }

  void PhaseShiftModule::hardReset() {
    for (int channel = 0; channel < kChannels; ++channel)
      std::fill(input_ring_[channel].begin(), input_ring_[channel].end(), 0.0f);
    input_write_ = 0;
    clearSpectralState();
  }

  // Clears the wet path but keeps the input ring, which delays the dry path:
  // leaving the effect must not open a gap in the dry signal.
  void PhaseShiftModule::clearSpectralState() {
    for (int channel = 0; channel < kChannels; ++channel) {
      std::fill(output_ring_[channel].begin(), output_ring_[channel].end(), 0.0f);
      std::fill(fft_buffers_[channel].begin(), fft_buffers_[channel].end(), std::complex<float>());
      std::fill(fft_work_buffers_[channel].begin(), fft_work_buffers_[channel].end(), std::complex<float>());
      std::fill(spectral_history_[channel].begin(), spectral_history_[channel].end(), std::complex<float>());
    }

    output_read_ = 0;
    hop_counter_ = 0;
    history_write_frame_ = 0;
    mix_amount_ = 1.0f;
    // State is clear; the next active block snaps the mix to its target.
    idle_ = true;
  }

  // Writes a sample into the input ring and returns the one it displaces,
  // kFftSize - 1 samples old: the dry signal aligned with the wet path, whose
  // frames start from that oldest sample.
  poly_float PhaseShiftModule::delayDry(poly_float in) {
    for (int channel = 0; channel < kChannels; ++channel)
      input_ring_[channel][input_write_] = in[channel];
    input_write_ = (input_write_ + 1) % kFftSize;

    poly_float delayed = in;
    for (int channel = 0; channel < kChannels; ++channel)
      delayed.set(channel, input_ring_[channel][input_write_]);
    return delayed;
  }

  mono_float PhaseShiftModule::logMappedIndex(int bin) const {
    if (bin <= 2)
      return 0.0f;

    constexpr mono_float kMinBin = 2.0f;
    constexpr mono_float kMaxBin = kFftSize * 0.5f;
    mono_float normalized = std::log(bin / kMinBin) / std::log(kMaxBin / kMinBin);
    return utils::clamp(normalized, 0.0f, 1.0f) * (kCurveSize - 1);
  }

  mono_float PhaseShiftModule::curveValueForIndex(mono_float index, mono_float tone) const {
    mono_float normalized = utils::clamp(index / (kCurveSize - 1), 0.0f, 1.0f);

    mono_float falling = 1.0f - normalized;
    mono_float shaped_low = falling * falling;
    mono_float rising = normalized;
    tone = utils::clamp(tone, -1.0f, 1.0f);

    if (tone > 0.0f)
      return utils::interpolate(falling, shaped_low, tone);

    return utils::interpolate(falling, rising, -tone);
  }

  int PhaseShiftModule::getDelayFramesForBin(int bin, mono_float depth_frames, mono_float tone) const {
    if (depth_frames <= 0.0f)
      return 0;

    mono_float mapped_index = mapped_index_[bin];
    mono_float curve_value = curveValueForIndex(mapped_index, tone);
    return utils::iclamp(static_cast<int>(std::floor(curve_value * depth_frames)), 0, kMaxDelayFrames);
  }

  void PhaseShiftModule::delaySpectrum(int channel, std::vector<std::complex<float>>& spectrum,
                                       mono_float depth_frames, mono_float tone) {
    std::vector<std::complex<float>>& history = spectral_history_[channel];
    int write_base = history_write_frame_ * kFftSize;

    for (int bin = 0; bin < kFftSize; ++bin) {
      int positive_bin = bin <= kFftSize / 2 ? bin : kFftSize - bin;
      int delay_frames = getDelayFramesForBin(positive_bin, depth_frames, tone);
      std::complex<float> current = spectrum[bin];

      if (delay_frames > 0) {
        int read_frame = (history_write_frame_ - delay_frames + kHistorySize) % kHistorySize;
        spectrum[bin] = history[read_frame * kFftSize + bin];
      }

      history[write_base + bin] = current;
    }
  }

  void PhaseShiftModule::addFrameToOutput(int channel, const std::vector<std::complex<float>>& spectrum) {
    std::vector<float>& ring = output_ring_[channel];
    constexpr mono_float kOverlapScale = 1.0f / 3.0f;

    for (int i = 0; i < kFftSize; ++i) {
      int write_index = (output_read_ + i) % kOutputRingSize;
      ring[write_index] += spectrum[i].real() * window_[i] * kOverlapScale;
    }
  }

  void PhaseShiftModule::processFrame(mono_float depth_frames, mono_float tone) {
    for (int channel = 0; channel < kChannels; ++channel) {
      std::vector<std::complex<float>>& spectrum = fft_buffers_[channel];
      std::vector<std::complex<float>>& fft_work = fft_work_buffers_[channel];
      std::vector<float>& input = input_ring_[channel];

      for (int i = 0; i < kFftSize; ++i) {
        int read_index = (input_write_ + i) % kFftSize;
        fft_work[i] = { input[read_index] * window_[i], 0.0f };
      }

      fft_.perform(fft_work.data(), spectrum.data(), false);
      delaySpectrum(channel, spectrum, depth_frames, tone);
      fft_.perform(spectrum.data(), fft_work.data(), true);
      addFrameToOutput(channel, fft_work);
    }

    history_write_frame_ = (history_write_frame_ + 1) % kHistorySize;
  }

  void PhaseShiftModule::processWithInput(const poly_float* audio_in, int num_samples) {
    SynthModule::process(num_samples);

    mono_float target_amount = utils::clamp(amount_->buffer[0][0], 0.0f, 1.0f);
    mono_float target_mix = utils::clamp(mix_->buffer[0][0], 0.0f, 1.0f);
    poly_float* audio_out = output()->buffer;

    // The dry path always runs through the input ring, so the effect's
    // latency is the same whether it is working or idle, and the mix never
    // combines two different moments of the signal.
    if (target_amount <= 0.0001f || target_mix <= 0.0001f) {
      // Clear the ~2 MB of spectral history once on entering the idle state,
      // not on every block while idle.
      if (!idle_)
        clearSpectralState();
      for (int sample = 0; sample < num_samples; ++sample)
        audio_out[sample] = delayDry(audio_in[sample]);
      return;
    }
    if (idle_) {
      // The output ring is empty after a reset; start at the target mix
      // instead of ramping down from the reset value of 1.
      idle_ = false;
      mix_amount_ = target_mix;
    }

    mono_float depth_frames = target_amount * kMaxDelayFrames;
    mono_float tone = utils::clamp(tone_->buffer[0][0], -1.0f, 1.0f);
    mono_float current_mix = mix_amount_;
    mono_float mix_delta = (target_mix - current_mix) * (1.0f / num_samples);

    for (int sample = 0; sample < num_samples; ++sample) {
      current_mix += mix_delta;

      const poly_float dry = delayDry(audio_in[sample]);
      if (++hop_counter_ >= kHopSize) {
        hop_counter_ = 0;
        processFrame(depth_frames, tone);
      }

      poly_float output = dry;
      for (int channel = 0; channel < kChannels; ++channel) {
        float wet = output_ring_[channel][output_read_];
        output_ring_[channel][output_read_] = 0.0f;
        output.set(channel, dry[channel] + (wet - dry[channel]) * current_mix);
      }

      output_read_ = (output_read_ + 1) % kOutputRingSize;
      audio_out[sample] = output;
    }

    mix_amount_ = target_mix;
  }

  void PhaseShiftModule::enable(bool enable) {
    SynthModule::enable(enable);
    if (!enable)
      hardReset();
  }
} // namespace vital
