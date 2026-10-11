#pragma once
#include "LFOData.h"
#include <vector>

// Sync rate values in beats (matching original)
constexpr int LFO_RATE_COUNT = 18;

inline const char* lfoSyncRateName (int idx)
{
    static const char* names[] = {
        "4 Bars", "2 Bars", "1 Bar", "3 / 4", "2 / 3", "1 / 2",
        "3 / 8", "1 / 3", "1 / 4", "3 / 16", "1 / 6", "1 / 8",
        "1 / 12", "1 / 16", "1 / 24", "1 / 32", "1 / 48", "1 / 64"
    };
    if (idx >= 0 && idx < LFO_RATE_COUNT) return names[idx];
    return "";
}

inline double lfoSyncValue (int idx)
{
    static const double values[] = {
        4.0, 2.0, 1.0, 3.0/4.0, 2.0/3.0, 1.0/2.0,
        3.0/8.0, 1.0/3.0, 1.0/4.0, 3.0/16.0, 1.0/6.0, 1.0/8.0,
        1.0/12.0, 1.0/16.0, 1.0/24.0, 1.0/32.0, 1.0/48.0, 1.0/64.0
    };
    if (idx >= 0 && idx < LFO_RATE_COUNT) return values[idx];
    return 1.0;
}

// Sec rate normalization matching original
constexpr double SEC_EXPONENT_MIN = -6.643856189774724;
constexpr double SEC_EXPONENT_MAX = 3.0;

inline double normaliseSec (double sec)
{
    double v2 = std::log2 (sec);
    double v1 = (v2 - SEC_EXPONENT_MIN) / (SEC_EXPONENT_MAX - SEC_EXPONENT_MIN);
    return std::clamp (v1, 0.0, 1.0);
}

inline double denormaliseSec (double v)
{
    double v2 = SEC_EXPONENT_MIN + v * (SEC_EXPONENT_MAX - SEC_EXPONENT_MIN);
    return std::exp2 (v2);
}

class LFOProcessor
{
public:
    // Render LFO output into buffer
    // patternIdx: which pattern (0-7)
    // isSyncMode: true=beat sync, false=free rate
    // syncRateIdx: index into sync rate table (0-17)
    // freeRateNorm: normalized free rate (0-1)
    // bpm: current tempo
    // sampleRate: current sample rate
    void renderLFO (float* buffer, int numSamples,
                    LFOData& lfoData, int patternIdx,
                    bool isSyncMode, int syncRateIdx, double freeRateNorm,
                    double bpm, double sampleRate);
};
