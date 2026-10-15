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

#pragma once

#include "synth_module.h"
#include "atlas_fft.h"

#include "JuceHeader.h"

#include <array>
#include <complex>
#include <vector>

namespace vital {

  class PhaseShiftModule : public SynthModule {
    public:
      PhaseShiftModule(std::string prefix = "");

      void init() override;
      void processWithInput(const poly_float* audio_in, int num_samples) override;
      void hardReset() override;
      void enable(bool enable) override;

      Processor* clone() const override { VITAL_ASSERT(false); return nullptr; }

    private:
      static constexpr int kFftOrder = 10;
      static constexpr int kFftSize = 1 << kFftOrder;
      static constexpr int kHopSize = 128;
      static constexpr int kCurveSize = 512;
      static constexpr int kMaxDelayFrames = 128;
      static constexpr int kHistorySize = kMaxDelayFrames + 1;
      static constexpr int kOutputRingSize = kFftSize * 4;
      static constexpr int kChannels = 2;

      void createWindow();
      void processFrame(mono_float depth_frames, mono_float tone);
      void delaySpectrum(int channel, std::vector<std::complex<float>>& spectrum,
                         mono_float depth_frames, mono_float tone);
      int getDelayFramesForBin(int bin, mono_float depth_frames, mono_float tone) const;
      mono_float curveValueForIndex(mono_float index, mono_float tone) const;
      mono_float logMappedIndex(int bin) const;
      void addFrameToOutput(int channel, const std::vector<std::complex<float>>& spectrum);

      Output* amount_;
      Output* tone_;
      Output* mix_;
      std::string prefix_;

      atlas::ComplexFft fft_;
      std::array<mono_float, kFftSize> window_;
      std::array<mono_float, kFftSize / 2 + 1> mapped_index_;
      std::array<std::vector<float>, kChannels> input_ring_;
      std::array<std::vector<float>, kChannels> output_ring_;
      std::array<std::vector<std::complex<float>>, kChannels> fft_buffers_;
      std::array<std::vector<std::complex<float>>, kChannels> fft_work_buffers_;
      std::array<std::vector<std::complex<float>>, kChannels> spectral_history_;
      int input_write_;
      int output_read_;
      int hop_counter_;
      int history_write_frame_;
      mono_float mix_amount_;
      bool idle_ = true;

      JUCE_LEAK_DETECTOR(PhaseShiftModule)
  };
} // namespace vital
