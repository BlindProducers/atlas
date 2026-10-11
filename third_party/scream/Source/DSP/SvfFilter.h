#pragma once
#include "FastMath.h"

// SvfLinearTrapOptimised2
// https://cytomic.com/files/dsp/SvfLinearTrapOptimised2.pdf
// Ported exactly from Scream/src/dsp.h

struct SvfCoeffs
{
    float a1, a2, a3, m0, m1, m2;
};

inline float svfCalcG (float fc, float fsInv)
{
    return FastMath::fasttan_normalised (FastMath::HALF_PI * 1.27f * fc * fsInv);
}

inline SvfCoeffs svfLP (float fc, float Q, float fsInv)
{
    float g  = svfCalcG (fc, fsInv);
    float k  = 1.0f / Q;
    float a1 = 1.0f / (1.0f + g * (g + k));
    float a2 = g * a1;
    float a3 = g * a2;
    return { a1, a2, a3, 0.0f, 0.0f, 1.0f };
}

inline SvfCoeffs svfHP (float fc, float Q, float fsInv)
{
    float g  = svfCalcG (fc, fsInv);
    float k  = 1.0f / Q;
    float a1 = 1.0f / (1.0f + g * (g + k));
    float a2 = g * a1;
    float a3 = g * a2;
    return { a1, a2, a3, 1.0f, -k, -1.0f };
}

inline float svfProcess (float v0, const SvfCoeffs& c, float* s)
{
    float v3 = v0 - s[1];
    float v1 = c.a1 * s[0] + c.a2 * v3;
    float v2 = s[1] + c.a2 * s[0] + c.a3 * v3;
    s[0] = 2 * v1 - s[0];
    s[1] = 2 * v2 - s[1];
    return c.m0 * v0 + c.m1 * v1 + c.m2 * v2;
}
