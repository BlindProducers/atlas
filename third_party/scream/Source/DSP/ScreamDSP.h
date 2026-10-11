#pragma once
#include "FastMath.h"
#include "SvfFilter.h"
#include "TanhADAA2.h"
#include "Dynamics.h"
#include "SmoothedParam.h"
#include "ADSR.h"

// Constants matching original plugin
constexpr float CUTOFF_MAX    = FastMath::MIDI_NOTE_NUM_20kHz;
constexpr float HP_CUTOFF_MIN = FastMath::MIDI_NOTE_NUM_20Hz - 12.0f;
constexpr float LP_Q_MIN     = FastMath::SQRT1_2;
constexpr float LP_Q_MAX     = FastMath::SQRT1_2;
constexpr float HP_Q_MIN     = FastMath::SQRT1_2;
constexpr float HP_Q_MAX     = FastMath::SQRT2;
constexpr float FB_GAIN_MIN  = -12.0f;
constexpr float FB_GAIN_MAX  = 12.0f;

constexpr float RANGE_INPUT_GAIN_MIN  = -72.0f;
constexpr float RANGE_INPUT_GAIN_MAX  = 24.0f;
constexpr float RANGE_OUTPUT_GAIN_MIN = -24.0f;
constexpr float RANGE_OUTPUT_GAIN_MAX = 0.0f;

constexpr int NUM_AUTOMATABLE_PARAMS = 5;

enum AutoParam
{
    PARAM_CUTOFF = 0,
    PARAM_SCREAM,
    PARAM_RESONANCE,
    PARAM_INPUT_GAIN,
    PARAM_WET
};

// Per-channel filter state
struct ChannelState
{
    SmoothedParam autogainLastPeak;
    SmoothedParam autogainDB;
    SimpleADSR    autogainADSR;

    SmoothedParam values[NUM_AUTOMATABLE_PARAMS];

    TanhADAA2 tanh1;
    TanhADAA2 tanh2;

    float fbYn1    = 0;
    float peakXn1  = 0;   // peak detector for feedback gate
    float gatePeak = 0;   // peak detector for output gate (tames resonance tail)
    float lp[2]    = {};
    float hp[2]    = {};

    void reset()
    {
        tanh1.reset();
        tanh2.reset();
        fbYn1 = 0;
        peakXn1 = 0;
        gatePeak = 0;
        lp[0] = lp[1] = 0;
        hp[0] = hp[1] = 0;
        autogainLastPeak.reset (0);
        autogainDB.reset (0);
        for (auto& v : values) v.reset (0);
    }
};

class ScreamDSP
{
public:
    void prepare (double sampleRate);
    void reset();

    // Process one block for both channels
    // audioParams: [cutoff, scream, resonance, inputGain, wet] normalized 0-1
    // modBuffer1/2: LFO modulation buffers (can be nullptr if no modulation)
    // lfoModAmounts: [5][2] - per-param per-LFO mod depth (-1..1)
    // lfo1ModFlags/lfo2ModFlags: bitmask of which params are modulated
    void processBlock (float** output, int numFrames,
                       const float* audioParams,
                       float keytrackingOffset,
                       const float* modBuffer1, const float* modBuffer2,
                       const float lfoModAmounts[][2],
                       uint8_t lfo1ModFlags, uint8_t lfo2ModFlags,
                       float gateReleaseMs = 30.0f);

    // Apply autogain to the output
    void applyAutogain (float** output, int numFrames, bool autogainOn);

    // Apply output gain with smoothing
    void applyOutputGain (float** output, int numFrames, float outputGainNorm);

    ChannelState state[2];

private:
    double sampleRate_ = 44100;
    float  fsInv_      = 1.0f / 44100.0f;
    SmoothedParam outputGain_;
};
