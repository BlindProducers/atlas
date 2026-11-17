// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "common.h"

#include <vector>

namespace vital::sample_slices {
  // Where the Slicer mode's slices begin, in samples of the file, for
  // Sample::setSlices. Message thread: both read the whole file.

  // An even grid of `divisions` slices.
  std::vector<int> grid(int length, int divisions);

  // A slice at every transient: a rise in level of at least 3 dB at full
  // sensitivity, 18 dB at none, at least 50 ms after the last. `left` and
  // `right` (null for a mono file) hold `length` samples at `sample_rate`.
  std::vector<int> transients(const mono_float* left, const mono_float* right, int length, int sample_rate,
                              float sensitivity);
} // namespace vital::sample_slices
