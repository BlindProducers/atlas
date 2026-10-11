#pragma once
#include <cmath>

// Ported from Scream/src/dsp.h

namespace Dynamics
{

// Source: Will Pirkle - Designing Audio Effect Plugins in C++
inline double convertCompressorTime (double numSamples)
{
    constexpr double TLD_AUDIO_ENVELOPE_ANALOG_TC = -0.99967234081320612357829304641019;
    return std::exp (TLD_AUDIO_ENVELOPE_ANALOG_TC / numSamples);
}

// Giannoulis, Massberg, Reiss compressor
inline float hardKneeExpander (float x_dB, float threshold_dB, float ratio)
{
    if (x_dB >= threshold_dB)
        return x_dB;
    return threshold_dB + ratio * (x_dB - threshold_dB);
}

inline float detectPeak (float x, float yn_1, float attack, float release)
{
    float y;
    if (x > yn_1)
        y = attack * yn_1 + (1.0f - attack) * x;
    else
        y = release * yn_1;

    if (y < 1.0e-6f)
        y = 0;

    return y;
}

} // namespace Dynamics
