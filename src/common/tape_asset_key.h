// The key that decrypted the Tape effect's former encrypted noise clips was
// removed from this repository's history, together with the clips, before
// Atlas went public. Factory/Samples/Tape now holds plain, documented WAV
// recordings (see the README.atlas.md there); this zeroed array only keeps
// older commits compiling.
#pragma once
#include <cstdint>
namespace vital::tape {
inline constexpr uint8_t asset_keys[64] = {};
}
