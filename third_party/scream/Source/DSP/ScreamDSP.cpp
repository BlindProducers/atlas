#include "ScreamDSP.h"
#include <cstring>
#include <cmath>

void ScreamDSP::prepare (double sr)
{
    sampleRate_ = sr;
    fsInv_ = 1.0f / (float) sr;
    reset();

    for (int ch = 0; ch < 2; ch++)
    {
        state[ch].autogainADSR.setParams (0.020f, 0.001f, 1.0f, 0.1f, (float) sr);
    }
}

void ScreamDSP::reset()
{
    for (auto& s : state)
        s.reset();
    outputGain_.reset (0);
}

void ScreamDSP::processBlock (float** output, int numFrames,
                               const float* audioParams,
                               float keytrackingOffset,
                               const float* modBuffer1, const float* modBuffer2,
                               const float lfoModAmounts[][2],
                               uint8_t lfo1ModFlags, uint8_t lfo2ModFlags,
                               float gateReleaseMs)
{
    const float expanderAttack  = (float) Dynamics::convertCompressorTime (1);
    const float expanderRelease = (float) Dynamics::convertCompressorTime (sampleRate_ * 0.001);

    // Output gate: follows input envelope to tame resonance tail when input stops
    // Uses a slightly longer release than the feedback gate so it sounds natural
    const float gateAttack  = (float) Dynamics::convertCompressorTime (1);
    const float gateRelease = (float) Dynamics::convertCompressorTime (sampleRate_ * (double) gateReleaseMs * 0.001);

    const int paramSmoothingTime = FastMath::droundi (sampleRate_ * 0.030);

    for (int ch = 0; ch < 2; ch++)
    {
        float* audio = output[ch];
        ChannelState s = state[ch];

        for (int i = 0; i < NUM_AUTOMATABLE_PARAMS; i++)
            s.values[i].setTarget (audioParams[i], paramSmoothingTime);

        float lpCutoff = s.values[PARAM_CUTOFF].current;
        float hpCutoff = s.values[PARAM_SCREAM].current;
        float resonance = s.values[PARAM_RESONANCE].current;
        float inGain   = s.values[PARAM_INPUT_GAIN].current;
        float wet      = s.values[PARAM_WET].current;

        float lpQ = FastMath::lerpf (resonance, LP_Q_MIN, LP_Q_MAX);
        float hpQ = FastMath::lerpf (resonance, HP_Q_MIN, HP_Q_MAX);

        lpCutoff  = FastMath::lerpf (lpCutoff, FastMath::MIDI_NOTE_NUM_20Hz, CUTOFF_MAX) + keytrackingOffset;
        hpCutoff  = FastMath::lerpf (hpCutoff, HP_CUTOFF_MIN, CUTOFF_MAX);
        hpCutoff -= CUTOFF_MAX - lpCutoff;

        lpCutoff = FastMath::midi_to_Hz (lpCutoff);
        hpCutoff = FastMath::midi_to_Hz (hpCutoff);
        lpCutoff = FastMath::clampf (lpCutoff, 5.0f, 20000.0f);
        hpCutoff = FastMath::clampf (hpCutoff, 5.0f, 20000.0f);

        float feedbackGain = FastMath::lerpf (resonance, FB_GAIN_MIN, FB_GAIN_MAX);
        feedbackGain = FastMath::fast_dB_to_gain (feedbackGain);

        inGain = FastMath::lerpf (inGain, RANGE_INPUT_GAIN_MIN, RANGE_INPUT_GAIN_MAX);
        inGain = FastMath::fast_dB_to_gain (inGain);

        SvfCoeffs lpC = svfLP (lpCutoff, lpQ, fsInv_);
        SvfCoeffs hpC = svfHP (hpCutoff, hpQ, fsInv_);

        const bool smoothParams = s.values[PARAM_CUTOFF].remaining > 0
                                | s.values[PARAM_SCREAM].remaining > 0
                                | s.values[PARAM_RESONANCE].remaining > 0
                                | s.values[PARAM_INPUT_GAIN].remaining > 0
                                | s.values[PARAM_WET].remaining > 0;

        const bool hasModOrSmoothing = (lfo1ModFlags != 0) || (lfo2ModFlags != 0) || smoothParams;

        for (int i = 0; i < numFrames; i++)
        {
            if (hasModOrSmoothing)
            {
                float modvals[NUM_AUTOMATABLE_PARAMS];
                for (int j = 0; j < NUM_AUTOMATABLE_PARAMS; j++)
                {
                    s.values[j].tick();
                    float v = s.values[j].current;

                    if (lfo1ModFlags & (1 << j))
                        v += lfoModAmounts[j][0] * modBuffer1[i];
                    if (lfo2ModFlags & (1 << j))
                        v += lfoModAmounts[j][1] * modBuffer2[i];

                    modvals[j] = FastMath::clampf (v, 0.0f, 1.0f);
                }

                lpCutoff  = modvals[PARAM_CUTOFF];
                hpCutoff  = modvals[PARAM_SCREAM];
                resonance = modvals[PARAM_RESONANCE];
                inGain    = modvals[PARAM_INPUT_GAIN];
                wet       = modvals[PARAM_WET];

                lpQ = FastMath::lerpf (resonance, LP_Q_MIN, LP_Q_MAX);
                hpQ = FastMath::lerpf (resonance, HP_Q_MIN, HP_Q_MAX);

                lpCutoff  = FastMath::lerpf (lpCutoff, FastMath::MIDI_NOTE_NUM_20Hz, CUTOFF_MAX) + keytrackingOffset;
                hpCutoff  = FastMath::lerpf (hpCutoff, HP_CUTOFF_MIN, CUTOFF_MAX);
                hpCutoff -= CUTOFF_MAX - lpCutoff;

                lpCutoff = FastMath::midi_to_Hz (lpCutoff);
                hpCutoff = FastMath::midi_to_Hz (hpCutoff);
                lpCutoff = FastMath::clampf (lpCutoff, 20.0f, 20000.0f);
                hpCutoff = FastMath::clampf (hpCutoff, 20.0f, 20000.0f);

                feedbackGain = FastMath::lerpf (resonance, FB_GAIN_MIN, FB_GAIN_MAX);
                feedbackGain = FastMath::fast_dB_to_gain (feedbackGain);

                inGain = FastMath::lerpf (inGain, RANGE_INPUT_GAIN_MIN, RANGE_INPUT_GAIN_MAX);
                inGain = FastMath::fast_dB_to_gain (inGain);

                lpC = svfLP (lpCutoff, lpQ, fsInv_);
                hpC = svfHP (hpCutoff, hpQ, fsInv_);
            }

            float x = audio[i];
            x *= inGain;

            // Feedforward
            float y = x + s.fbYn1;

            // Pre-LP tanh ADAA2
            y = (float) s.tanh1.process ((double) y);

            // Lowpass filter
            y = svfProcess (y, lpC, s.lp);

            // NaN protection
            if (y != y) y = 0;

            // Output gate: track input level to fade wet signal when input stops
            // This tames the resonance tail that the filter state produces after input ends
            s.gatePeak = Dynamics::detectPeak (std::fabs (x), s.gatePeak, gateAttack, gateRelease);
            float gatePeakDB   = FastMath::fast_gain_to_dB (s.gatePeak);
            float gateDB       = Dynamics::hardKneeExpander (gatePeakDB, -54.0f, 3.0f);
            gateDB            -= gatePeakDB;
            float gateGain     = FastMath::fast_dB_to_gain (gateDB);
            if (gateDB < -140.0f) gateGain = 0;

            // Wet/dry mix with output gate applied to the wet (filtered) signal
            audio[i] = wet * y * gateGain + (1.0f - wet) * audio[i];

            // Feedback path
            float feed = y * feedbackGain;
            feed = svfProcess (feed, hpC, s.hp);
            feed = (float) s.tanh2.process ((double) feed);

            // Feedback gate (expander)
            s.peakXn1 = Dynamics::detectPeak (std::fabs (x), s.peakXn1, expanderAttack, expanderRelease);
            float peakDB      = FastMath::fast_gain_to_dB (s.peakXn1);
            float reductionDB = Dynamics::hardKneeExpander (peakDB, -120.0f, 2.0f);
            reductionDB -= peakDB;
            float reductionGain = FastMath::fast_dB_to_gain (reductionDB);
            if (reductionDB < -140.0f)
                reductionGain = 0;
            feed *= reductionGain;

            s.fbYn1 = feed;
        }

        // Denormal protection
        auto roundToZero = [](float& n) { if (std::fabs (n) < 1.0e-8f) n = 0; };
        roundToZero (s.lp[0]);
        roundToZero (s.lp[1]);
        roundToZero (s.hp[0]);
        roundToZero (s.hp[1]);
        roundToZero (s.fbYn1);
        roundToZero (s.gatePeak);

        state[ch] = s;
    }
}

void ScreamDSP::applyAutogain (float** output, int numFrames, bool autogainOn)
{
    const float autogainAttack  = (float) Dynamics::convertCompressorTime (sampleRate_ * 0.005);
    const float autogainRelease = (float) Dynamics::convertCompressorTime (sampleRate_ * 0.5);

    for (int ch = 0; ch < 2; ch++)
    {
        float* audio = output[ch];
        ChannelState& s = state[ch];

        for (int i = 0; i < numFrames; i++)
        {
            float x = audio[i];
            s.autogainLastPeak.current = Dynamics::detectPeak (
                std::fabs (x), s.autogainLastPeak.current, autogainAttack, autogainRelease);
        }

        float inPeakDB = FastMath::fast_gain_to_dB (s.autogainLastPeak.current);

        if (s.autogainADSR.currentStage == ADSRStage::Idle && inPeakDB > -96.0f)
            s.autogainADSR.setStage (ADSRStage::Attack);
        if ((s.autogainADSR.currentStage == ADSRStage::Attack ||
             s.autogainADSR.currentStage == ADSRStage::Sustain) && inPeakDB < -64.0f)
            s.autogainADSR.setStage (ADSRStage::Release);

        if (inPeakDB < -24.0f)
            inPeakDB = -24.0f;

        float autogainTargetDB = autogainOn ? -inPeakDB : 0.0f;
        int smoothingTimeSamples = FastMath::droundi (sampleRate_ * 0.005);
        s.autogainDB.setTarget (autogainTargetDB, smoothingTimeSamples);

        for (int i = 0; i < numFrames; i++)
        {
            float gainDB = s.autogainDB.tick();
            float gain   = FastMath::fast_dB_to_gain (gainDB);
            gain *= s.autogainADSR.tick();
            audio[i] *= gain;
        }
    }
}

void ScreamDSP::applyOutputGain (float** output, int numFrames, float outputGainNorm)
{
    int smoothTime = FastMath::droundi (sampleRate_ * 0.05);
    outputGain_.setTarget (outputGainNorm, smoothTime);

    for (int ch = 0; ch < 2; ch++)
    {
        float* buffer = output[ch];
        SmoothedParam sv = outputGain_;
        const bool isUpdating = sv.remaining > 0;
        float dB   = FastMath::lerpf (sv.current, RANGE_OUTPUT_GAIN_MIN, RANGE_OUTPUT_GAIN_MAX);
        float gain = FastMath::fast_dB_to_gain (dB);

        for (int i = 0; i < numFrames; i++)
        {
            if (isUpdating)
            {
                sv.tick();
                dB   = FastMath::lerpf (sv.current, RANGE_OUTPUT_GAIN_MIN, RANGE_OUTPUT_GAIN_MAX);
                gain = FastMath::fast_dB_to_gain (dB);
            }
            buffer[i] *= gain;
        }
    }
    outputGain_.tickN (numFrames);
}
