// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "common.h"

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <new>
#include <type_traits>

namespace vital {
  // Zero-filled storage whose untouched pages stay out of the process
  // footprint. Value-initialised new[] writes every page instead. Voice clones
  // and inactive filter models hold many such buffers.
  //
  // On macOS a large calloc comes from fresh pages the OS has already zeroed.
  // Windows' heap clears every block under about half a megabyte by hand,
  // which makes all of its pages resident, so there blocks of a page or more
  // come straight from the OS instead (zeroed_buffer.cpp).
#if defined(_WIN32)
  void* allocateZeroedPages(size_t bytes);
  void freeZeroedPages(void* memory);
#endif

  struct ZeroedBufferDeleter {
    bool pages = false;
    void operator()(void* memory) const {
#if defined(_WIN32)
      if (pages) {
        freeZeroedPages(memory);
        return;
      }
#endif
      std::free(memory);
    }
  };
  template<class T> using ZeroedBuffer = std::unique_ptr<T[], ZeroedBufferDeleter>;

  template<class T> ZeroedBuffer<T> allocateZeroed(size_t count) {
    // Elements are never constructed or destroyed: only plain sample types
    // (float, poly_float, whose destructor does nothing) belong here.
    static_assert(std::is_standard_layout_v<T>);
    static_assert(alignof(T) <= 16, "calloc guarantees 16-byte alignment on the supported platforms");
#if defined(_WIN32)
    // Smaller blocks would waste most of a 64 KB address range each.
    constexpr size_t kMinPageBytes = 64 * 1024;
    if (count * sizeof(T) >= kMinPageBytes) {
      void* pages = allocateZeroedPages(count * sizeof(T));
      if (pages == nullptr)
        throw std::bad_alloc();
      return ZeroedBuffer<T>(static_cast<T*>(pages), ZeroedBufferDeleter { true });
    }
#endif
    void* memory = std::calloc(count, sizeof(T));
    if (memory == nullptr)
      throw std::bad_alloc();
    VITAL_ASSERT(reinterpret_cast<std::uintptr_t>(memory) % alignof(T) == 0);
    return ZeroedBuffer<T>(static_cast<T*>(memory));
  }
} // namespace vital
