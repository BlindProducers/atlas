// SPDX-License-Identifier: GPL-3.0-or-later
// Built outside the unity files (see CMakeLists.txt) so that <windows.h> and
// its macros never reach the DSP code.
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "zeroed_buffer.h"

namespace vital {
  void* allocateZeroedPages(size_t bytes) {
    // Committed pages read as zero and join the working set only when written.
    return VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  }

  void freeZeroedPages(void* memory) {
    if (memory != nullptr)
      VirtualFree(memory, 0, MEM_RELEASE);
  }
} // namespace vital
#endif
