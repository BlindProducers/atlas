// SPDX-License-Identifier: MIT
// Adapted from Astralay (Copyright (c) 2026 Iron Labs, MIT licence); see LICENSE.astralay
// and README.atlas.md.
#include "real_fft.h"

namespace atlas::glitch_delay
{

RealFft::RealFft (int order)
    : size (1 << order), fft (order), scratch ((size_t) (2 << order), 0.0f)
{
}

void RealFft::forward (const float* time, float* spectrum) noexcept
{
    std::copy (time, time + size, scratch.begin());
    fft.performRealOnlyForwardTransform (scratch.data(), true);

    // The DC and Nyquist bins are purely real.
    scratch[1] = 0.0f;
    scratch[(size_t) size + 1] = 0.0f;
    std::copy (scratch.begin(), scratch.begin() + size + 2, spectrum);
}

void RealFft::inverse (const float* spectrum, float* time) noexcept
{
    std::copy (spectrum, spectrum + size + 2, scratch.begin());
    std::fill (scratch.begin() + size + 2, scratch.end(), 0.0f);
    fft.performRealOnlyInverseTransform (scratch.data());
    std::copy (scratch.begin(), scratch.begin() + size, time);
}

} // namespace atlas::glitch_delay
