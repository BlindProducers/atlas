#pragma once
// Tone macros (M3): three 0..1 knobs a player understands, mapped onto the
// engine's physical controls. 0.5 everywhere gives the engine defaults, which
// were fitted to the UNSW reference spectra (see docs/milestones/M3-report.md).
//
//  * Air: breath turbulence. noiseGain = 0.1 * 2.5^(2 (air - 0.5)), i.e.
//    0.04 (air 0, "classical", 8 dB less noise) .. 0.25 (air 1, "breathy",
//    8 dB more). The noise is jet-linked: it scales with the blowing pressure,
//    its bandwidth with the jet velocity, and it pulses with the jet. Above the
//    default the extra air is heard directly at the mouth-hole (air missing the
//    edge) and does not drive the tube, which keeps the pitch stable.
//  * Brightness: the tone-hole / end reflection corner, 1.41 .. 2.83 kHz
//    (half an octave either side of the 2 kHz tone-hole cutoff). It acts mostly on the
//    low and middle registers; the top octave's harmonics lie above the cutoff.
//  * Attack: onset chiff, chiff = 0.8 * attack (0 = no burst, a soft "hoo"
//    start; 1 = a strong turbulent "chiff"). From M4 on the performer layer
//    also uses Attack to shape the breath envelope at note starts, and (after
//    the M4 listening check) sets a softer chiff of 0.5 * attack^1.5.
#include "mf/Engine.h"

#include <algorithm>
#include <cmath>

namespace mf {

struct ToneMacros {
    float air = 0.5f;
    float brightness = 0.5f;
    float attack = 0.5f;
};

inline float airToNoiseGain(float air) {
    return 0.1f * std::pow(2.5f, 2.0f * (std::clamp(air, 0.0f, 1.0f) - 0.5f));
}
inline float attackToChiff(float attack) { return 0.8f * std::clamp(attack, 0.0f, 1.0f); }

// Returns c with noiseGain, brightness and chiff set from the macros.
inline EngineControls applyToneMacros(EngineControls c, const ToneMacros& m) {
    c.noiseGain = airToNoiseGain(m.air);
    c.brightness = std::clamp(m.brightness, 0.0f, 1.0f);
    c.chiff = attackToChiff(m.attack);
    return c;
}

} // namespace mf
