#pragma once

#include <cmath>

namespace pm
{

/** Hammer speed at the string for a MIDI velocity (0 to 127), in m/s.

    A straight line in decibels: 5 m/s at 127 and 0.23 m/s at 1, a 27 dB range. The first curve reached
    0.16 m/s at 1 and left notes below velocity 60 two to four decibels slower; they were heard as lacking
    attack. Velocities 80 and 120, where the voicing is fitted, are unchanged. (A 20 dB range, the sampled
    reference's, was tried and heard as worse.)
*/
inline double hammerVelocity (double velocity)
{
    return 5.0 * std::pow (10.0, 0.2132 * (velocity - 127.0) / 20.0);
}

/** Felt force for a compression c (m) changing at rate cDot (m/s): a stiffening spring with loss. */
template <typename Float>
inline Float feltForce (Float c, Float cDot, Float stiffness, Float exponent, Float hysteresis)
{
    if (c <= Float (0))
        return Float (0);
    const Float f = stiffness * std::pow (c, exponent) * (Float (1) + hysteresis * cDot);
    return f > Float (0) ? f : Float (0);
}

/** The hammer has left for good once it is this far below the string (m): from here the string
    cannot swing far enough to meet it again. */
constexpr double kHammerClear = 0.002;

} // namespace pm
