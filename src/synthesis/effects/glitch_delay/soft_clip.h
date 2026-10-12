// SPDX-License-Identifier: MIT
// Adapted from Astralay (Copyright (c) 2026 Iron Labs, MIT licence); see LICENSE.astralay
// and README.atlas.md.
#pragma once

#include <cmath>

namespace atlas::glitch_delay
{

/** Gentle feedback-path limiter: exactly linear up to the knee, then eases into a ceiling of 1.0.
    Keeps unity feedback plus energy-adding glitches from running away without colouring normal
    levels.
*/
inline float softClip (float x) noexcept
{
    constexpr float knee = 0.9f;
    constexpr float headroom = 1.0f - knee;

    const auto magnitude = std::abs (x);

    if (magnitude <= knee)
        return x;

    const auto shaped = knee + headroom * std::tanh ((magnitude - knee) / headroom);
    return x < 0.0f ? -shaped : shaped;
}

} // namespace atlas::glitch_delay
