// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "common.h"

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <new>
#include <type_traits>

namespace vital {
  // Zero-filled storage from calloc. A large block comes from fresh pages that
  // the OS has already zeroed, so the parts that are never written stay out of
  // the process footprint. Value-initialised new[] writes every page instead.
  // Voice clones and inactive filter models hold many such buffers.
  struct ZeroedBufferDeleter {
    void operator()(void* memory) const { std::free(memory); }
  };
  template<class T> using ZeroedBuffer = std::unique_ptr<T[], ZeroedBufferDeleter>;

  template<class T> ZeroedBuffer<T> allocateZeroed(size_t count) {
    // Elements are never constructed or destroyed: only plain sample types
    // (float, poly_float, whose destructor does nothing) belong here.
    static_assert(std::is_standard_layout_v<T>);
    static_assert(alignof(T) <= 16, "calloc guarantees 16-byte alignment on the supported platforms");
    void* memory = std::calloc(count, sizeof(T));
    if (memory == nullptr)
      throw std::bad_alloc();
    VITAL_ASSERT(reinterpret_cast<std::uintptr_t>(memory) % alignof(T) == 0);
    return ZeroedBuffer<T>(static_cast<T*>(memory));
  }
} // namespace vital
