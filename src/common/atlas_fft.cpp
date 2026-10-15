// SPDX-License-Identifier: GPL-3.0-or-later
#include "atlas_fft.h"

#include "JuceHeader.h"
#include "pffft/pffft.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <new>
#include <mutex>
#include <utility>

namespace atlas {
  namespace fft_detail {
    void AlignedFree::operator()(float* data) const noexcept { pffft_aligned_free(data); }
  }

  namespace {
    // SSE loads need 16 bytes; NEON needs less, but the same rule on every
    // platform keeps one code path.
    constexpr std::uintptr_t kAlignment = 16;

    bool isAligned(const void* data) noexcept {
      return (reinterpret_cast<std::uintptr_t>(data) & (kAlignment - 1)) == 0;
    }

    // The first aligned float at or after `data`. Floats are 4-byte aligned,
    // so it is at most 3 floats on.
    float* alignedStart(float* data) noexcept {
      const auto address = reinterpret_cast<std::uintptr_t>(data);
      return reinterpret_cast<float*>((address + kAlignment - 1) & ~(kAlignment - 1));
    }

    fft_detail::AlignedFloats allocateFloats(int count) {
      auto* data = static_cast<float*>(pffft_aligned_malloc(static_cast<size_t>(count) * sizeof(float)));
      if (data == nullptr)
        throw std::bad_alloc();
      std::fill(data, data + count, 0.0f);
      return fft_detail::AlignedFloats(data);
    }

    // PFFFT takes sizes of 2^a 3^b 5^c that are a multiple of 2 * 4 * 4 (real)
    // or 4 * 4 (complex) with its four-lane SIMD.
    bool isValidSize(int size, pffft_transform_t transform) {
      const int lanes = pffft_simd_size();
      const int multiple = (transform == PFFFT_REAL ? 2 : 1) * lanes * lanes;
      if (size <= 0 || size % multiple != 0)
        return false;
      for (int factor : { 2, 3, 5 })
        while (size % factor == 0)
          size /= factor;
      return size == 1;
    }

    // PFFFT's twiddles and factors are read-only once built, so every FFT of
    // one size and kind in the process shares them.
    std::shared_ptr<PFFFT_Setup> sharedSetup(int size, pffft_transform_t transform) {
      jassert(isValidSize(size, transform));
      static std::mutex mutex;
      static std::map<std::pair<int, int>, std::weak_ptr<PFFFT_Setup>> setups;
      const std::lock_guard<std::mutex> lock(mutex);
      auto& entry = setups[{ size, static_cast<int>(transform) }];
      if (auto setup = entry.lock())
        return setup;
      std::shared_ptr<PFFFT_Setup> setup(pffft_new_setup(size, transform), pffft_destroy_setup);
      if (setup == nullptr)
        throw std::bad_alloc();
      entry = setup;
      return setup;
    }
  } // namespace

  Fft::Fft(int order) :
      size_(1 << order), setup_(sharedSetup(size_, PFFFT_REAL)), work_(allocateFloats(size_)) { }

  Fft::Fft(Fft&&) noexcept = default;
  Fft& Fft::operator=(Fft&&) noexcept = default;
  Fft::~Fft() = default;

  void Fft::performRealOnlyForwardTransform(float* data, bool only_non_negative_frequencies) noexcept {
    const int n = size_;
    // An unaligned buffer moves up into its spare upper half and back.
    float* aligned = alignedStart(data);
    if (aligned != data)
      std::memmove(aligned, data, static_cast<size_t>(n) * sizeof(float));
    pffft_transform_ordered(setup_.get(), aligned, aligned, work_.get(), PFFFT_FORWARD);
    if (aligned != data)
      std::memmove(data, aligned, static_cast<size_t>(n) * sizeof(float));

    // PFFFT stores the (real) Nyquist bin in DC's imaginary part.
    data[n] = data[1];
    data[n + 1] = 0.0f;
    data[1] = 0.0f;

    if (!only_non_negative_frequencies) {
      for (int k = 1; k < n / 2; ++k) {
        data[2 * (n - k)] = data[2 * k];
        data[2 * (n - k) + 1] = -data[2 * k + 1];
      }
    }
  }

  void Fft::performRealOnlyInverseTransform(float* data) noexcept {
    const int n = size_;
    const float nyquist = data[n];
    float* aligned = alignedStart(data);
    if (aligned != data)
      std::memmove(aligned, data, static_cast<size_t>(n) * sizeof(float));
    aligned[1] = nyquist;
    pffft_transform_ordered(setup_.get(), aligned, aligned, work_.get(), PFFFT_BACKWARD);

    const float scale = 1.0f / static_cast<float>(n);
    if (aligned == data) {
      for (int i = 0; i < n; ++i)
        data[i] *= scale;
    }
    else {
      // Ascending, so each sample is read before the shift back overwrites it.
      for (int i = 0; i < n; ++i)
        data[i] = aligned[i] * scale;
    }
    std::fill(data + n, data + 2 * n, 0.0f);
  }

  ComplexFft::ComplexFft(int order) :
      size_(1 << order), setup_(sharedSetup(size_, PFFFT_COMPLEX)), work_(allocateFloats(2 * size_)),
      staging_(allocateFloats(2 * size_)) { }

  ComplexFft::ComplexFft(ComplexFft&&) noexcept = default;
  ComplexFft& ComplexFft::operator=(ComplexFft&&) noexcept = default;
  ComplexFft::~ComplexFft() = default;

  void ComplexFft::perform(const std::complex<float>* input, std::complex<float>* output, bool inverse) noexcept {
    const int floats = 2 * size_;
    const auto* in = reinterpret_cast<const float*>(input);
    auto* out = reinterpret_cast<float*>(output);
    const pffft_direction_t direction = inverse ? PFFFT_BACKWARD : PFFFT_FORWARD;
    if (isAligned(in) && isAligned(out)) {
      pffft_transform_ordered(setup_.get(), in, out, work_.get(), direction);
    }
    else {
      float* staging = staging_.get();
      std::memcpy(staging, in, static_cast<size_t>(floats) * sizeof(float));
      pffft_transform_ordered(setup_.get(), staging, staging, work_.get(), direction);
      std::memcpy(out, staging, static_cast<size_t>(floats) * sizeof(float));
    }

    if (inverse) {
      const float scale = 1.0f / static_cast<float>(size_);
      for (int i = 0; i < floats; ++i)
        out[i] *= scale;
    }
  }
} // namespace atlas
