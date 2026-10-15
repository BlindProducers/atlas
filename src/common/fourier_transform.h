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

#include "JuceHeader.h"
#include "atlas_fft.h"

#include <mutex>

namespace vital {
  // Vital's real FFT, on PFFFT through atlas::Fft on every platform (Vital
  // chose IPP, vDSP or KISS FFT per platform). `data` holds 2 * size floats.
  // The forward transform leaves bins 0 .. size / 2 interleaved, with the
  // Nyquist bin's real part at data[size]; the inverse reads them, scales by
  // 1 / size and clears the upper half. One object must not run on two
  // threads at once.
  class FourierTransform {
    public:
      FourierTransform(int bits) : fft_(bits) { }

      void transformRealForward(float* data) { fft_.performRealOnlyForwardTransform(data, true); }
      void transformRealInverse(float* data) { fft_.performRealOnlyInverseTransform(data); }

    private:
      atlas::Fft fft_;

      JUCE_LEAK_DETECTOR(FourierTransform)
  };

  // One transform per size for the whole process, for the wavetable code.
  // Wavetables are rendered on more than one thread and a FourierTransform
  // keeps scratch, so each call holds a lock. Never on the audio thread.
  template <size_t bits>
  class FFT {
    public:
      static void transformRealForward(float* data) {
        FFT& fft = instance();
        const std::lock_guard<std::mutex> lock(fft.mutex_);
        fft.fourier_transform_.transformRealForward(data);
      }

      static void transformRealInverse(float* data) {
        FFT& fft = instance();
        const std::lock_guard<std::mutex> lock(fft.mutex_);
        fft.fourier_transform_.transformRealInverse(data);
      }

    private:
      static FFT& instance() {
        static FFT<bits> instance;
        return instance;
      }

      FFT() : fourier_transform_(bits) { }

      std::mutex mutex_;
      FourierTransform fourier_transform_;
  };

} // namespace vital
