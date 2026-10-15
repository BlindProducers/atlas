// SPDX-License-Identifier: MIT
// Adapted from Astralay (Copyright (c) 2026 Iron Labs, MIT licence); see LICENSE.astralay
// and README.atlas.md.
#pragma once

#include "atlas_fft.h"

#include <juce_core/juce_core.h>
#include <vector>

namespace atlas::glitch_delay
{

/** A real-input FFT. Astralay ran this on PFFFT, and Atlas does too, through atlas::Fft like every
    other Atlas FFT.

    Spectra are interleaved complex bins 0 to size / 2 inclusive, so a spectrum buffer needs
    size + 2 floats. The inverse is scaled, so forward then inverse returns the original signal.
    Input and output may be the same buffer.
*/
class RealFft
{
public:
    explicit RealFft (int order);

    int getSize() const noexcept { return size; }

    void forward (const float* time, float* spectrum) noexcept;
    void inverse (const float* spectrum, float* time) noexcept;

private:
    int size;
    atlas::Fft fft;
    std::vector<float> scratch;   // 2 * size floats, as atlas::Fft's real transforms need.

    JUCE_DECLARE_NON_COPYABLE (RealFft)
};

} // namespace atlas::glitch_delay
