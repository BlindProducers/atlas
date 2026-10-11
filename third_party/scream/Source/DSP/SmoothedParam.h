#pragma once
#include <cmath>

// Ported from wip_libs/src/param_smoothing.h

struct SmoothedParam
{
    float current = 0;
    float target  = 0;
    float inc     = 0;
    int   remaining = 0;

    void reset (float newValue)
    {
        target = current = newValue;
        inc = 0;
        remaining = 0;
    }

    float tick()
    {
        float v = current;
        if (remaining-- > 0)
            current += inc;
        return v;
    }

    float tickN (int steps)
    {
        int numSteps = steps < remaining ? steps : remaining;
        current   += inc * numSteps;
        remaining -= numSteps;
        return current;
    }

    void setTarget (float newValue, int steps)
    {
        if (steps == 0) steps = 1;
        if (target == newValue && remaining)
            return;

        float newInc = 0;
        float diff = newValue - current;

        while (newInc == 0 && newValue != target && steps)
        {
            newInc = diff / (float) steps;
            if (std::fabs (newInc) < 1.0e-5f)
            {
                newInc = 0;
                steps >>= 1;
            }
        }

        if (newInc == 0)
        {
            current = newValue;
            steps = 0;
        }

        target    = newValue;
        inc       = newInc;
        remaining = steps;
    }
};
