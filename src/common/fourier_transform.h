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

// PFFFT (third_party/pffft) is Atlas's FFT on every platform, so Windows and
// macOS transform alike; on Windows JUCE's own FFT would be a scalar fallback
// that took about a third of a wavetable voice's CPU. The CMake option
// ATLAS_USE_PFFFT (on by default) builds pffft.c and defines this for every
// target, which must all agree.
#ifndef ATLAS_USE_PFFFT
  #define ATLAS_USE_PFFFT 1
#endif

#if ATLAS_USE_PFFFT
  #include "pffft/pffft.h"
  #include <cstdint>
  #include <cstring>
  #if JUCE_WINDOWS
    #include <malloc.h>
    #define ATLAS_FFT_ALLOCA _alloca
  #else
    #include <alloca.h>
    #define ATLAS_FFT_ALLOCA alloca
  #endif
#endif

namespace vital {
  #if ATLAS_USE_PFFFT

  // The same layout and scaling as juce::dsp::FFT's real transforms: bins 0
  // to N/2 as interleaved real and imaginary parts (DC and Nyquist with no
  // imaginary part), the forward transform unscaled, the inverse scaled by
  // 1/N with the buffer's second half cleared, as JUCE's vDSP engine leaves
  // it. Scratch memory is taken on the stack for each call, so one object can
  // serve several threads, as FFT<bits>::transform() does.
  class FourierTransform {
    public:
      FourierTransform(int bits) : size_(1 << bits), setup_(pffft_new_setup(1 << bits, PFFFT_REAL)) {
        jassert(setup_ != nullptr);
      }

      ~FourierTransform() {
        if (setup_ != nullptr)
          pffft_destroy_setup(setup_);
      }

      void transformRealForward(float* data) {
        float* buffer = alignScratch(ATLAS_FFT_ALLOCA(scratchBytes()));
        float* work = buffer + size_;
        std::memcpy(buffer, data, size_ * sizeof(float));
        pffft_transform_ordered(setup_, buffer, buffer, work, PFFFT_FORWARD);
        // PFFFT keeps the Nyquist bin's real part where DC's imaginary part goes.
        data[0] = buffer[0];
        data[1] = 0.0f;
        std::memcpy(data + 2, buffer + 2, (size_ - 2) * sizeof(float));
        data[size_] = buffer[1];
        data[size_ + 1] = 0.0f;
      }

      void transformRealInverse(float* data) {
        float* buffer = alignScratch(ATLAS_FFT_ALLOCA(scratchBytes()));
        float* work = buffer + size_;
        buffer[0] = data[0];
        buffer[1] = data[size_];
        std::memcpy(buffer + 2, data + 2, (size_ - 2) * sizeof(float));
        pffft_transform_ordered(setup_, buffer, buffer, work, PFFFT_BACKWARD);
        const float scale = 1.0f / size_;
        for (int i = 0; i < size_; ++i)
          data[i] = buffer[i] * scale;
        std::memset(data + size_, 0, size_ * sizeof(float));
      }

    private:
      static constexpr size_t kAlignment = 16;

      size_t scratchBytes() const { return 2 * size_ * sizeof(float) + kAlignment; }

      static float* alignScratch(void* memory) {
        const auto address = reinterpret_cast<std::uintptr_t>(memory);
        return reinterpret_cast<float*>((address + kAlignment - 1) & ~std::uintptr_t(kAlignment - 1));
      }

      int size_;
      PFFFT_Setup* setup_;

      JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FourierTransform)
  };

  #elif INTEL_IPP

  #include "ipps.h"

  class FourierTransform {
    public:
      FourierTransform(int bits) : size_(1 << bits) {
        int spec_size = 0;
        int spec_buffer_size = 0;
        int buffer_size = 0;
        ippsFFTGetSize_R_32f(bits, IPP_FFT_DIV_INV_BY_N, ippAlgHintNone, &spec_size, &spec_buffer_size, &buffer_size);

        spec_ = std::make_unique<Ipp8u[]>(spec_size);
        spec_buffer_ = std::make_unique<Ipp8u[]>(spec_buffer_size);
        buffer_ = std::make_unique<Ipp8u[]>(buffer_size);

        ippsFFTInit_R_32f(&ipp_specs_, bits, IPP_FFT_DIV_INV_BY_N, ippAlgHintNone, spec_.get(), spec_buffer_.get());
      }

      void transformRealForward(float* data) {
        data[size_] = 0.0f;
        ippsFFTFwd_RToPerm_32f_I((Ipp32f*)data, ipp_specs_, buffer_.get());
        data[size_] = data[1];
        data[size_ + 1] = 0.0f;
        data[1] = 0.0f;
      }

      void transformRealInverse(float* data) {
        data[1] = data[size_];
        ippsFFTInv_PermToR_32f_I((Ipp32f*)data, ipp_specs_, buffer_.get());
        memset(data + size_, 0, size_ * sizeof(float));
      }

    private:
      int size_;
      IppsFFTSpec_R_32f *ipp_specs_;
      std::unique_ptr<Ipp8u[]> spec_;
      std::unique_ptr<Ipp8u[]> spec_buffer_;
      std::unique_ptr<Ipp8u[]> buffer_;

      JUCE_LEAK_DETECTOR(FourierTransform)
  };

  #elif JUCE_MODULE_AVAILABLE_juce_dsp

  class FourierTransform {
    public:
      FourierTransform(int bits) : fft_(bits) { }

      void transformRealForward(float* data) { fft_.performRealOnlyForwardTransform(data, true); }
      void transformRealInverse(float* data) { fft_.performRealOnlyInverseTransform(data); }

    private:
      dsp::FFT fft_;

      JUCE_LEAK_DETECTOR(FourierTransform)
  };

  #elif __APPLE__
  #define VIMAGE_H
  #include <Accelerate/Accelerate.h>

  class FourierTransform {
    public:
      FourierTransform(vDSP_Length bits) : setup_(vDSP_create_fftsetup(bits, 2)), bits_(bits), size_(1 << bits) { }
      ~FourierTransform() {
        vDSP_destroy_fftsetup(setup_);
      }

      void transformRealForward(float* data) {
        static const float kMult = 0.5f;
        data[size_] = 0.0f;
        DSPSplitComplex split = { data, data + 1 };
        vDSP_fft_zrip(setup_, &split, 2, bits_, kFFTDirection_Forward);
        vDSP_vsmul(data, 1, &kMult, data, 1, size_);

        data[size_] = data[1];
        data[size_ + 1] = 0.0f;
        data[1] = 0.0f;
      }

      void transformRealInverse(float* data) {
        float multiplier = 1.0f / size_;
        DSPSplitComplex split = { data, data + 1 };
        data[1] = data[size_];

        vDSP_fft_zrip(setup_, &split, 2, bits_, kFFTDirection_Inverse);
        vDSP_vsmul(data, 1, &multiplier, data, 1, size_ * 2);
        memset(data + size_, 0, size_ * sizeof(float));
      }

    private:
      FFTSetup setup_;
      vDSP_Length bits_;
      vDSP_Length size_;

      JUCE_LEAK_DETECTOR(FourierTransform)
  };

  #else

  #include "kissfft/kissfft.hh"

  class FourierTransform {
    public:
      FourierTransform(size_t bits) : bits_(bits), size_(1 << bits), forward_(size_, false), inverse_(size_, true) {
        buffer_ = std::make_unique<std::complex<float>[]>(size_);
      }

      ~FourierTransform() { }

      void transformRealForward(float* data) {
        for (int i = size_ - 1; i >= 0; --i) {
          data[2 * i] = data[i];
          data[2 * i + 1] = 0.0f;
        }

        forward_.transform((std::complex<float>*)data, buffer_.get());

        int num_floats = size_ * 2;
        memcpy(data, buffer_.get(), num_floats * sizeof(float));
        data[size_] = data[1];
        data[size_ + 1] = 0.0f;
        data[1] = 0.0f;
      }

      void transformRealInverse(float* data) {
        data[0] *= 0.5f;
        data[1] = data[size_];
        inverse_.transform((std::complex<float>*)data, buffer_.get());
        int num_floats = size_ * 2;

        float multiplier = 2.0f / size_;
        for (int i = 0; i < size_; ++i)
          data[i] = buffer_[i].real() * multiplier;

        memset(data + size_, 0, size_ * sizeof(float));
      }

    private:
      size_t bits_;
      size_t size_;
      std::unique_ptr<std::complex<float>[]> buffer_;
      kissfft<float> forward_;
      kissfft<float> inverse_;

      JUCE_LEAK_DETECTOR(FourierTransform)
  };

  #endif

  template <size_t bits>
  class FFT {
    public:
      static FourierTransform* transform() {
        static FFT<bits> instance;
        return &instance.fourier_transform_;
      }

    private:
      FFT() : fourier_transform_(bits) { }

      FourierTransform fourier_transform_;
  };

} // namespace vital
