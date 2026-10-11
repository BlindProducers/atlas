#include "LFOProcessor.h"
#include "DSP/FastMath.h"
#include <cmath>
#include <algorithm>

// Skew function matching original dsp.h
static float skewf (float v, float a)
{
    if (a <= 0) return 0;
    if (a >= 1.0f) return 1.0f;
    float va = v * a;
    return va / (1.0f - a - v + 2 * va);
}

static float interpPoints (float normPos, float skewAmt, float pt1y, float pt2y)
{
    if (pt1y == pt2y) return pt2y;
    if (pt1y > pt2y) normPos = 1.0f - normPos;
    float skewedPos = skewf (normPos, skewAmt);
    if (pt1y > pt2y) skewedPos = 1.0f - skewedPos;
    return FastMath::lerpf (skewedPos, pt1y, pt2y);
}

void LFOProcessor::renderLFO (float* buffer, int numSamples,
                               LFOData& lfoData, int patternIdx,
                               bool isSyncMode, int syncRateIdx, double freeRateNorm,
                               double bpm, double sampleRate)
{
    auto points = lfoData.getPoints (patternIdx);
    if (points.empty())
    {
        std::fill_n (buffer, numSamples, 0.0f);
        return;
    }

    const int numPoints = (int) points.size();
    const double patternLength = 1.0;
    const LFOLoopType loopType = lfoData.loopType;
    const bool loopOn = (loopType == LFOLoopType::Loop || loopType == LFOLoopType::Retrig);

    double rateHz;
    if (isSyncMode)
        rateHz = bpm / (lfoSyncValue (syncRateIdx) * 240.0);
    else
        rateHz = 1.0 / denormaliseSec (freeRateNorm);

    const double beatInc = rateHz / sampleRate;

    // Find current segment
    int it = numPoints - 1;
    while (it > 0 && lfoData.phase < points[it].x)
        it--;

    LFOPoint pt1 = points[it];
    float pt2x, pt2y;
    if (it + 1 >= numPoints)
    {
        pt2x = (float) patternLength;
        pt2y = points[0].y;
    }
    else
    {
        pt2x = points[it + 1].x;
        pt2y = points[it + 1].y;
    }

    for (int i = 0; i < numSamples; i++)
    {
        float relPos = (float) lfoData.phase - pt1.x;
        float segLen = pt2x - pt1.x;
        if (segLen > 0) relPos /= segLen;
        else relPos = 0;

        buffer[i] = interpPoints (relPos, pt1.skew, pt1.y, pt2y);

        lfoData.phase += beatInc;

        if (lfoData.phase >= pt2x)
        {
            if (loopOn)
                lfoData.phase -= (int) lfoData.phase;
            else
                lfoData.phase = std::min (1.0, lfoData.phase);

            // Find new segment
            it = numPoints - 1;
            while (it > 0 && lfoData.phase < points[it].x)
                it--;

            bool wrap = loopOn && (it >= numPoints);
            if (wrap) it = 0;

            pt1 = points[it];
            if (it + 1 >= numPoints)
            {
                pt2x = (float) patternLength;
                pt2y = points[0].y;
            }
            else
            {
                pt2x = points[it + 1].x;
                pt2y = points[it + 1].y;
            }
        }
    }
}
