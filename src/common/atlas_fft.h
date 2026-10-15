// SPDX-License-Identifier: GPL-3.0-or-later
// Atlas's FFTs, on PFFFT (third_party/pffft): SSE on x64 and NEON on arm64, the
// same code on macOS and Windows. They take juce::dsp::FFT's data layout and
// scaling, which the spectral code was written against. Only Convolution
// Reverb keeps an FFT of its own, inside juce::dsp::Convolution.
#pragma once

#include <complex>
#include <memory>

struct PFFFT_Setup;

namespace atlas {
  namespace fft_detail {
    struct AlignedFree { void operator()(float* data) const noexcept; };
    using AlignedFloats = std::unique_ptr<float[], AlignedFree>;
  }

  // A real FFT of 2^order points (order 5 and up), a stand-in for
  // juce::dsp::FFT's real-only transforms:
  // - `data` holds 2 * size floats, as juce::dsp::FFT requires;
  // - the forward transform leaves the interleaved complex bins 0 .. size / 2
  //   in the first size + 2 floats (DC and Nyquist with zero imaginary parts)
  //   and, unless only the non-negative frequencies are asked for, the
  //   conjugates of bins size / 2 - 1 .. 1 after them;
  // - the inverse reads bins 0 .. size / 2, returns size samples scaled by
  //   1 / size and clears the upper size floats.
  // PFFFT needs 16-byte aligned data; a buffer that is not is shifted into
  // alignment within its own spare upper half and back, which costs a copy
  // each way. Every Fft of one size shares PFFFT's read-only tables. Each
  // keeps its own scratch, so one Fft must not run on two threads at once.
  // The transforms never allocate.
  class Fft {
    public:
      explicit Fft(int order);
      Fft(Fft&&) noexcept;
      Fft& operator=(Fft&&) noexcept;
      ~Fft();

      int getSize() const noexcept { return size_; }

      void performRealOnlyForwardTransform(float* data, bool only_non_negative_frequencies = false) noexcept;
      void performRealOnlyInverseTransform(float* data) noexcept;

    private:
      int size_ = 0;
      std::shared_ptr<PFFFT_Setup> setup_;
      fft_detail::AlignedFloats work_;
  };

  // A complex FFT of 2^order points (order 4 and up), a stand-in for
  // juce::dsp::FFT::perform: `size` interleaved complex values in and out,
  // the forward transform unscaled and the inverse scaled by 1 / size. Input
  // and output may be the same buffer. Same sharing rules as Fft. Buffers
  // that are not 16-byte aligned (std::vector's are on macOS and Windows)
  // are copied through an aligned buffer of its own.
  class ComplexFft {
    public:
      explicit ComplexFft(int order);
      ComplexFft(ComplexFft&&) noexcept;
      ComplexFft& operator=(ComplexFft&&) noexcept;
      ~ComplexFft();

      int getSize() const noexcept { return size_; }

      void perform(const std::complex<float>* input, std::complex<float>* output, bool inverse) noexcept;

    private:
      int size_ = 0;
      std::shared_ptr<PFFFT_Setup> setup_;
      fft_detail::AlignedFloats work_, staging_;
  };
} // namespace atlas
