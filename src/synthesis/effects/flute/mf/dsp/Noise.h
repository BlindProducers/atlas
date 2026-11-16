#pragma once
// Fast deterministic white-noise source (xorshift32), seedable.
// Output is uniform in [-1, 1). Real-time safe, no state beyond one word.

#include <cstdint>

namespace mf::dsp {

class WhiteNoise {
public:
    explicit WhiteNoise(std::uint32_t seed = 0x2545F491u) { setSeed(seed); }
    void setSeed(std::uint32_t seed) { state_ = seed ? seed : 0x2545F491u; }

    float next() {
        std::uint32_t x = state_;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        state_ = x;
        // Top 24 bits -> [-1, 1)
        return static_cast<float>(static_cast<std::int32_t>(x >> 8) - 8388608) * (1.0f / 8388608.0f);
    }

private:
    std::uint32_t state_;
};

} // namespace mf::dsp
