#pragma once
#include "FastMath.h"
#include <algorithm>

// Ported from Scream/src/libs/adsr.h

enum class ADSRStage
{
    Idle,
    Attack,
    Decay,
    Sustain,
    Release,
    NumStages
};

constexpr float ADSR_MIN_LEVEL = 0.0001f;

struct SimpleADSR
{
    ADSRStage currentStage = ADSRStage::Idle;
    float currentLevel  = 0.0f;
    float multiplier    = 1.0f;
    uint32_t currentIdx = 0;
    uint32_t nextStageIdx = 0;

    uint32_t attackLengthSamples  = 0;
    uint32_t decayLengthSamples   = 0;
    uint32_t releaseLengthSamples = 0;
    float sustain = 1.0f;

    void setParams (float attack, float decay, float sus, float release, float sampleRate)
    {
        attackLengthSamples  = (uint32_t)(attack * sampleRate);
        decayLengthSamples   = (uint32_t)(decay * sampleRate);
        releaseLengthSamples = (uint32_t)(release * sampleRate);
        sustain = sus;
    }

    void calcMultiplier (float startLevel, float endLevel, uint32_t lengthInSamples)
    {
        float v = 0;
        if (lengthInSamples)
            v = 1.0f + (FastMath::fastlog (endLevel) - FastMath::fastlog (startLevel)) / (float)(lengthInSamples);
        multiplier = v;
    }

    void setStage (ADSRStage newStage)
    {
        currentStage = newStage;
        currentIdx = 0;

        switch (newStage)
        {
            case ADSRStage::Idle:
                currentLevel = 0.0f;
                multiplier = 1.0f;
                break;
            case ADSRStage::Attack:
                nextStageIdx = attackLengthSamples;
                if (ADSR_MIN_LEVEL > currentLevel)
                    currentLevel = ADSR_MIN_LEVEL;
                calcMultiplier (currentLevel, 1.0f, nextStageIdx);
                break;
            case ADSRStage::Decay:
                nextStageIdx = decayLengthSamples;
                currentLevel = 1.0f;
                calcMultiplier (currentLevel, std::max (sustain, ADSR_MIN_LEVEL), nextStageIdx);
                break;
            case ADSRStage::Sustain:
                currentLevel = sustain;
                multiplier = 1.0f;
                break;
            case ADSRStage::Release:
                nextStageIdx = releaseLengthSamples;
                calcMultiplier (currentLevel, ADSR_MIN_LEVEL, nextStageIdx);
                break;
            default:
                break;
        }
    }

    float tick()
    {
        if (currentStage != ADSRStage::Idle && currentStage != ADSRStage::Sustain)
        {
            if (currentIdx == nextStageIdx)
            {
                int next = (int)currentStage + 1;
                if (next >= (int)ADSRStage::NumStages)
                    next = (int)ADSRStage::Idle;
                setStage ((ADSRStage)next);
            }
            currentLevel *= multiplier;
            currentIdx++;
        }
        return std::clamp (currentLevel, 0.0f, 1.0f);
    }
};
