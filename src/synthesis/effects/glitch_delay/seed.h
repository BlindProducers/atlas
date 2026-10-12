// SPDX-License-Identifier: MIT
// Adapted from Astralay (Copyright (c) 2026 Iron Labs, MIT licence); see LICENSE.astralay
// and README.atlas.md.
#pragma once

#include <cstdint>

namespace atlas::glitch_delay
{

/** Derives an independent seed for a tap, wrapping explicitly modulo 2^64. User seeds (0 to
    9999) keep their existing sequences; full-width random seeds may wrap both the multiplication
    and the addition. Reinterpret the resulting bits for JUCE's signed seed API.

    Atlas builds as C++17, which has no std::bit_cast. Converting an unsigned value that is out of
    range to a signed type keeps the two's complement bits on every compiler Atlas supports (and
    C++20 requires it).
*/
constexpr std::int64_t seedForTap (std::int64_t baseSeed, std::uint64_t tapIndex) noexcept
{
    const auto bits = static_cast<std::uint64_t> (baseSeed) * std::uint64_t { 1000003 } + tapIndex;
    return static_cast<std::int64_t> (bits);
}

} // namespace atlas::glitch_delay
