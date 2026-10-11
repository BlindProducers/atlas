// SPDX-License-Identifier: AGPL-3.0-or-later
#include "spectral_lock_engine.h"
#include <algorithm>
#include <cmath>
#include <utility>
namespace atlas::spectral_lock {
const char* const Engine::noteNames[12] =
{ "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

const char* const Engine::scaleNames[Engine::numScales] =
{
    "Chromatic", "Major", "Minor", "Harm Minor", "Dorian", "Phrygian",
    "Lydian", "Mixolydian", "Penta Major", "Penta Minor", "Whole Tone", "Octaves"
};

// Each row maps input pitch class (index) -> output pitch class (value).
// Non-scale tones are pulled to the nearest scale degree.
const int Engine::scaleTables[Engine::numScales][12] =
{
    { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11 },   // Chromatic
    { 0, 0, 2, 2, 4, 5, 5, 7, 7, 9, 9,11 },   // Major
    { 0, 0, 2, 3, 3, 5, 5, 7, 8, 8,10,10 },   // Natural minor
    { 0, 0, 2, 3, 3, 5, 5, 7, 8, 8,11,11 },   // Harmonic minor
    { 0, 0, 2, 3, 3, 5, 5, 7, 7, 9, 9,10 },   // Dorian
    { 0, 1, 1, 3, 3, 5, 5, 7, 8, 8,10,10 },   // Phrygian
    { 0, 0, 2, 2, 4, 4, 6, 7, 7, 9, 9,11 },   // Lydian
    { 0, 0, 2, 2, 4, 5, 5, 7, 7, 9,10,10 },   // Mixolydian
    { 0, 0, 2, 2, 4, 4, 4, 7, 7, 9, 9, 9 },   // Pentatonic major
    { 0, 0, 3, 3, 3, 5, 5, 7, 7,10,10,10 },   // Pentatonic minor
    { 0, 0, 2, 2, 4, 4, 6, 6, 8, 8,10,10 },   // Whole tone
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }    // Everything to the root
};

static inline float coefFromTau (float tauSeconds, double sampleRate) noexcept
{
    return 1.0f - std::exp (-1.0f / juce::jmax (1.0f, (float) (tauSeconds * sampleRate)));
}

// deterministic per-band scatter, so panning/detune are stable across sessions
static inline float hashToBipolar (int k) noexcept
{
    juce::uint32 x = (juce::uint32) (k * 2654435761u);
    x ^= x >> 15; x *= 2246822519u; x ^= x >> 13;
    return ((float) (x & 0xffffff) / 8388608.0f) - 1.0f;   // -1 .. 1
}

Engine::Engine() {
    for (int i = 0; i <= tableSize; ++i)
        sineTable[(size_t) i] = std::sin(juce::MathConstants<float>::twoPi * (float) i / (float) tableSize);
}

void Engine::buildFilterbank()
{
    numBands = 0;
    const float ratio = std::pow (2.0f, 1.0f / (float) bandsPerOctave);
    float f = bankLowHz;

    while (f < bankHighHz && numBands < maxBands)
    {
        auto& b = bands[(size_t) numBands];
        b = Band();
        b.fc     = f;
        b.noteIn = ftom (f);
        b.detune = hashToBipolar (numBands * 7 + 3);
        b.freq   = f;
        b.targetFreq = f;
        b.phase  = 0.5f * (hashToBipolar (numBands) + 1.0f);   // scatter start phases

        // RBJ constant-skirt bandpass, unity peak gain
        const float w0    = juce::MathConstants<float>::twoPi * f / (float) sr;
        const float cw    = std::cos (w0);
        const float alpha = std::sin (w0) / (2.0f * bandQ);
        const float a0    = 1.0f + alpha;

        b.b0 =  alpha / a0;
        b.b1 =  0.0f;
        b.b2 = -alpha / a0;
        b.a1 = (-2.0f * cw) / a0;
        b.a2 = (1.0f - alpha) / a0;

        b.envCoef = coefFromTau (juce::jmax (0.004f, 5.0f / b.fc), sr);

        ++numBands;
        f *= ratio;
    }

    firstBand = 0;
    lastBand  = numBands - 1;
    appliedWidth = -1.0f;
    updatePanning();
}

void Engine::updatePanning()
{
    const float width = smoothedWidth;
    if (std::abs (width - appliedWidth) < 1.0e-4f) return;
    appliedWidth = width;
    for (int k = 0; k < numBands; ++k)
    {
        auto& b = bands[(size_t) k];
        // stereo placement: the source's per-band scatter, scaled by Width
        const float pan = juce::jlimit (-1.0f, 1.0f, hashToBipolar (k * 13 + 1) * width);
        const float a   = (pan + 1.0f) * 0.25f * juce::MathConstants<float>::pi;
        b.panL = std::cos (a);
        b.panR = std::sin (a);
    }
}

void Engine::prepare (double sampleRate, int samplesPerBlock)
{
    sr = sampleRate;
    juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) samplesPerBlock, 2 };
    loSplit.prepare (spec);
    hiSplit.prepare (spec);
    loSplit.setType (juce::dsp::LinkwitzRileyFilterType::lowpass);
    hiSplit.setType (juce::dsp::LinkwitzRileyFilterType::lowpass);
    reset (params);
}

void Engine::reset (const Parameters& parameters)
{
    params = parameters;
    smoothedWidth = juce::jlimit (0.0f, 1.0f, params.width);
    widthPrimed = false;
    buildFilterbank();
    loSplit.reset();
    hiSplit.reset();

    gateAttack   = coefFromTau (0.001f, sr);
    globalPeak   = 0.0f;
    transFast = transSlow = 0.0f;
    audioGate = 1.0f;

    smMix.reset     (sr, 0.02); smMix.setCurrentAndTargetValue (params.mix);
    smAmount.reset  (sr, 0.02); smAmount.setCurrentAndTargetValue (params.amount);
    smLevel.reset   (sr, 0.02); smLevel.setCurrentAndTargetValue (level);
    smShimmer.reset (sr, 0.02); smShimmer.setCurrentAndTargetValue (params.shimmer);
    smFollow.reset  (sr, 0.02); smFollow.setCurrentAndTargetValue (params.follow);
    followDryEnv = followTonalEnv = 0.0f;
    followAttack  = coefFromTau (0.001f, sr);
    followRelease = coefFromTau (0.020f, sr);
    followTonalRelease = coefFromTau (0.005f, sr);

    // latency is essentially zero - the filterbank runs in real time; the
    // only "delay" is the envelope follower's own inertia.
    mappingValid = false;
}

void Engine::updateRangeWindow (float loHz, float hiHz)
{
    if (hiHz < loHz) std::swap (loHz, hiHz);

    int lo = 0, hi = numBands - 1;
    while (lo < numBands - 1 && bands[(size_t) lo].fc < loHz) ++lo;
    while (hi > 0          && bands[(size_t) hi].fc > hiHz) --hi;
    if (hi < lo) hi = lo;

    // silence anything that just fell outside the window so it can't leak
    // stale energy back in when the range is widened again
    if (lo != firstBand || hi != lastBand)
    {
        for (int k = 0; k < numBands; ++k)
        {
            if (k >= lo && k <= hi) continue;
            auto& b = bands[(size_t) k];
            b.s1a = b.s2a = b.s1b = b.s2b = 0.0f;
            b.envSq = b.amp = 0.0f;
            b.gate = 0.0f;
        }
    }

    firstBand = lo;
    lastBand  = hi;
}

void Engine::updateBandMapping()
{
    const int   root      = (int) params.root;
    const float pitch     = params.pitch;
    const float bend      = params.bend * params.bendRange;
    const float spray     = params.spray;
    const float tilt      = params.tilt;
    const auto& matrix = scaleTables[juce::jlimit(0, numScales - 1, params.scale)];

    // Apply the source pitch-class mapping to each active band.
    for (int k = firstBand; k <= lastBand; ++k)
    {
        auto& b = bands[(size_t) k];

        // which note is this band listening to?
        const int n = juce::roundToInt (b.noteIn);
        float outNote;

        b.muted = false;
        const int rel = n - root;
        const int oct = (int) std::floor ((float) rel / 12.0f);
        const int pc  = rel - oct * 12;
        outNote = (float) (root + oct * 12 + matrix[pc]);

        // transpose, bend, spray
        outNote += pitch + bend + spray * b.detune * 0.5f;
        b.targetFreq = juce::jlimit (10.0f, (float) sr * 0.45f, mtof (outNote));

        // spectral tilt around 700 Hz
        const float t = std::pow (b.fc / 700.0f, tilt * 0.9f);
        b.ampGain = juce::jlimit (0.05f, 8.0f, t);


    }
}

void Engine::process (juce::AudioBuffer<float>& buffer, const Parameters& parameters)
{
    juce::ScopedNoDenormals noDenormals;

    const int numCh      = buffer.getNumChannels();
    const int numSamples = buffer.getNumSamples();
    const bool stereo    = numCh > 1;

    if (numSamples <= 0) return;
    params = parameters;
    {
        // Width glides with a 20 ms time constant, one step per block.
        const float target = juce::jlimit (0.0f, 1.0f, params.width);
        const float coefficient = 1.0f - std::exp (-(float) numSamples / (0.02f * (float) sr));
        smoothedWidth += (target - smoothedWidth) * coefficient;
        if (! widthPrimed || std::abs (target - smoothedWidth) < 1.0e-4f) smoothedWidth = target;
        widthPrimed = true;
    }
    updatePanning();

    const float loHz    = params.rangeLo;
    const float hiHz    = juce::jmax (params.rangeHi, loHz + 20.0f);
    updateRangeWindow (loHz, hiHz);
    const std::array<float, 9> mapping { loHz, hiHz, (float) params.root,
        (float) params.scale, params.pitch, params.bend, params.bendRange, params.spray, params.tilt };
    if (!mappingValid || mapping != lastMapping) {
        updateBandMapping();
        lastMapping = mapping;
        mappingValid = true;
    }

    loSplit.setCutoffFrequency (juce::jlimit (20.0f, (float) sr * 0.45f, loHz));
    hiSplit.setCutoffFrequency (juce::jlimit (20.0f, (float) sr * 0.45f, hiHz));

    const float isolate  = params.isolate;
    const bool  muteLo   = params.muteLo > 0.5f;
    const bool  muteHi   = params.muteHi > 0.5f;

    smMix    .setTargetValue (params.mix);
    smAmount .setTargetValue (params.amount);
    smLevel  .setTargetValue (level);
    smShimmer.setTargetValue (params.shimmer);
    smFollow .setTargetValue (juce::jlimit (0.0f, 1.0f, params.follow));

    // GLIDE: 0.4 ms .. 500 ms
    const float glideTau  = 0.0004f + glide * glide * 0.5f;
    const float glideCoef = coefFromTau (glideTau, sr);

    // ISOLATE: threshold rises with the dial, release gets brutally short
    gateRelease = coefFromTau (juce::jmap (isolate, 0.09f, 0.0015f), sr);
    const float gateThresh = isolate * 0.30f * juce::jmax (globalPeak, 1.0e-4f)
                             + isolate * 2.0e-4f;

    const float transFastC = coefFromTau (0.0015f, sr);
    const float transSlowC = coefFromTau (0.120f,  sr);
    const float agC        = coefFromTau (0.010f,  sr);

    const float invSR = 1.0f / (float) sr;

    float* left  = buffer.getWritePointer (0);
    float* right = stereo ? buffer.getWritePointer (1) : nullptr;

    float blockPeak = 0.0f;

    for (int n = 0; n < numSamples; ++n)
    {
        const float inL = left[n];
        const float inR = stereo ? right[n] : inL;
        const float mono = 0.5f * (inL + inR);

        // dry three-way split
        float lowL, restL, midL, highL;
        loSplit.processSample (0, inL, lowL, restL);
        hiSplit.processSample (0, restL, midL, highL);

        float lowR = lowL, midR = midL, highR = highL;
        if (stereo)
        {
            float restR;
            loSplit.processSample (1, inR, lowR, restR);
            hiSplit.processSample (1, restR, midR, highR);
        }

        // filterbank / oscillator bank
        float tonalL = 0.0f, tonalR = 0.0f;
        const float shimmer = smShimmer.getNextValue();

        for (int k = firstBand; k <= lastBand; ++k)
        {
            auto& b = bands[(size_t) k];

            // two cascaded biquads, transposed direct form II
            float y = b.b0 * mono + b.s1a;
            b.s1a = b.b1 * mono - b.a1 * y + b.s2a;
            b.s2a = b.b2 * mono - b.a2 * y;

            const float x2 = y;
            float y2 = b.b0 * x2 + b.s1b;
            b.s1b = b.b1 * x2 - b.a1 * y2 + b.s2b;
            b.s2b = b.b2 * x2 - b.a2 * y2;

            // RMS envelope; time constant scales with the band's own period
            if (! freeze)
            {
                b.envSq += b.envCoef * (y2 * y2 - b.envSq);
                b.amp = std::sqrt (b.envSq) * 1.41421356f;
            }

            // ISOLATE - per-oscillator noise gate
            const float gTarget = (! b.muted && b.amp > gateThresh) ? 1.0f : 0.0f;
            b.gate += (gTarget > b.gate ? gateAttack : gateRelease) * (gTarget - b.gate);

            // GLIDE on the oscillator frequency
            b.freq += glideCoef * (b.targetFreq - b.freq);

            b.phase += b.freq * invSR;
            if (b.phase >= 1.0f) b.phase -= 1.0f;

            float s = sine (b.phase);
            if (shimmer > 0.001f)
            {
                float p2 = b.phase * 2.0f;
                p2 -= (float) (int) p2;
                s += shimmer * 0.7f * sine (p2);          // phase-locked octave up
            }

            s *= b.amp * b.gate * b.ampGain;
            tonalL += s * b.panL;
            tonalR += s * b.panR;

            blockPeak = juce::jmax (blockPeak, b.amp);
        }

        // transient detector on the dry mid band
        const float rect = std::abs (midL) + std::abs (midR);
        transFast += transFastC * (rect - transFast);
        transSlow += transSlowC * (rect - transSlow);
        const float trans = juce::jlimit (0.0f, 1.0f,
                              (transFast / (transSlow + 1.0e-5f) - 1.0f) * 0.7f);

        // ISOLATE's hidden audio gate
        const float agTarget = (transSlow > 6.0e-4f) ? 1.0f : 0.0f;
        audioGate += agC * (agTarget - audioGate);
        const float ag = 1.0f - isolate * (1.0f - audioGate);

        // AMOUNT: tonal vs noise
        const float amount    = smAmount.getNextValue();
        const float noiseGain = juce::jlimit (0.0f, 1.0f, (1.0f - amount)
                                                  + trans * (1.0f - amount * 0.6f));   // Atlas: amount may exceed 1
        const float level = smLevel.getNextValue() * 0.55f;   // filterbank sum trim

        // FOLLOW (Atlas): the tonal output may not outlast or exceed the dry
        // mid band. RMS envelopes: the dry one releases over 20 ms, the tonal
        // one tracks its own ringing (5 ms), so the ratio collapses once the
        // input stops and the low bands' long filter tails are gated off.
        const float follow = smFollow.getNextValue();
        float followGain = 1.0f;
        if (follow > 0.0f)
        {
            const float drySq   = 0.5f * (midL * midL + midR * midR);
            const float tonalRms = (tonalL * tonalL + tonalR * tonalR) * 0.5f * amount * amount * level * level;
            followDryEnv   += (drySq > followDryEnv ? followAttack : followRelease) * (drySq - followDryEnv);
            followTonalEnv += (tonalRms > followTonalEnv ? followAttack : followTonalRelease) * (tonalRms - followTonalEnv);
            const float ratio = std::sqrt (followDryEnv / (followTonalEnv + 1.0e-12f));
            followGain = 1.0f - follow * (1.0f - juce::jmin (1.0f, ratio));
        }

        float wetL = tonalL * amount * level * ag * followGain + midL * noiseGain;
        float wetR = tonalR * amount * level * ag * followGain + midR * noiseGain;

        if (! muteLo)  { wetL += lowL;  wetR += lowR;  }
        if (! muteHi)  { wetL += highL; wetR += highR; }

        const float mix = smMix.getNextValue();
        const float outL = inL * (1.0f - mix) + wetL * mix;
        const float outR = inR * (1.0f - mix) + wetR * mix;

        left[n] = outL;
        if (stereo) right[n] = outR;
        else        left[n]  = 0.5f * (outL + outR);

    }

    // Peak tracking is used by the next block's ISOLATE threshold.
    globalPeak = juce::jmax (blockPeak, globalPeak * 0.92f);
}
}
