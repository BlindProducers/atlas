// SPDX-License-Identifier: AGPL-3.0-or-later
// Spectral Lock by aquanodemusic (https://github.com/aquanodemusic/aquanode-plugins,
// AGPL-3.0), audio algorithm extracted from the SpectralLock 1.0.0 processor.
// Atlas additions: Width is a control (the source held it at 1) and Follow
// ties the tonal output to the dry signal's envelope to tame tail ringing.
// See README.atlas.md in this folder for the adapter-only changes.
#pragma once
#include <juce_dsp/juce_dsp.h>
#include <array>
namespace atlas::spectral_lock {
struct Parameters {
    float mix = 1, amount = 1, isolate = 0, shimmer = 0;
    float bend = 0, bendRange = 2, rangeLo = 65, rangeHi = 2200;
    float muteLo = 0, muteHi = 0, spray = 0, tilt = 0, pitch = 0;
    int root = 0, scale = 0;
    // Atlas allows amount up to 2: above 1 the tones are boosted and the mid band fully removed.
    float width = 1;   // 0 centres every band, 1 is the source's stereo placement
    float follow = 0;  // 0 leaves the source's behaviour, 1 fully follows the dry envelope
};
class Engine {
public:
    Engine();
    void prepare(double sampleRate, int samplesPerBlock);
    // Atlas: clears all state for a fresh start with these parameters; never allocates.
    void reset(const Parameters& parameters);
    void process(juce::AudioBuffer<float>& buffer, const Parameters& parameters);
    static constexpr int latencySamples = 0;
    static constexpr int numScales = 12;
    static const char* const scaleNames[numScales];
    static const int scaleTables[numScales][12];
    static const char* const noteNames[12];
private:
    Parameters params;
    // Controls intentionally held at the supplied processor's defaults.
    static constexpr float glide = 0.0f, level = 1.0f;
    static constexpr bool freeze = false;
    // 36 bands / octave  ==  33 cent resolution.  ~6.2 octaves of coverage.
    static constexpr int   bandsPerOctave = 36;
    static constexpr float bankLowHz      = 60.0f;
    static constexpr float bankHighHz     = 4800.0f;
    static constexpr int   maxBands       = 240;
    static constexpr float bandQ          = 21.0f;   // per stage; 2 stages cascade

    struct Band
    {
        float fc          = 0.0f;   // analysis centre frequency
        float noteIn      = 0.0f;   // MIDI note number of fc
        float detune      = 0.0f;   // fixed pseudo-random offset, scaled by SPRAY
        float panL        = 0.707f;
        float panR        = 0.707f;

        // biquad coefficients (identical for both cascaded stages)
        float b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        // transposed direct form II state, two stages
        float s1a = 0, s2a = 0, s1b = 0, s2b = 0;

        float envCoef     = 0.0f;   // envelope follower one-pole coefficient
        float envSq       = 0.0f;   // mean square
        float amp         = 0.0f;   // sqrt(envSq) * sqrt2  ->  amplitude estimate
        float gate        = 1.0f;   // ISOLATE per-band gate, smoothed

        float phase       = 0.0f;   // 0..1
        float freq        = 0.0f;   // smoothed (GLIDE)
        float targetFreq  = 0.0f;
        float ampGain     = 0.0f;   // tilt * trim, applied at output
        bool  muted       = false;  // source MIDI capture flag (MIDI is disabled in Atlas)
    };

    std::array<Band, maxBands> bands;
    int numBands = 0;
    int firstBand = 0, lastBand = 0;   // active window from the RANGE slider

    void buildFilterbank();
    void updatePanning();              // Width -> per-band stereo placement
    void updateBandMapping();          // scale / root / pitch -> targetFreq
    void updateRangeWindow (float loHz, float hiHz);

    static constexpr int tableSize = 2048;
    std::array<float, tableSize + 1> sineTable;
    inline float sine (float phase01) const noexcept
    {
        const float p = phase01 * (float) tableSize;
        const int   i = (int) p;
        const float f = p - (float) i;
        return sineTable[i] + f * (sineTable[i + 1] - sineTable[i]);
    }
    static inline float mtof (float note) noexcept
    {
        return 440.0f * std::pow (2.0f, (note - 69.0f) * (1.0f / 12.0f));
    }
    static inline float ftom (float f) noexcept
    {
        return 69.0f + 12.0f * std::log2 (juce::jmax (1.0f, f) / 440.0f);
    }

    juce::dsp::LinkwitzRileyFilter<float> loSplit, hiSplit;
    double sr = 44100.0;
    float gateAttack = 0.0f, gateRelease = 0.0f;
    float globalPeak = 0.0f, transFast = 0.0f, transSlow = 0.0f, audioGate = 1.0f;
    juce::SmoothedValue<float> smMix, smAmount, smLevel, smShimmer, smFollow;
    float appliedWidth = -1.0f;
    float smoothedWidth = 1.0f;       // Atlas: Width glides per block, so automation does not step
    bool widthPrimed = false;         // the first block after a reset takes Width as it is
    // Follow: envelopes of the dry mid band and of the tonal output; the tonal
    // output is held at or below the dry level, so it dies with the input.
    float followDryEnv = 0.0f, followTonalEnv = 0.0f, followAttack = 0.0f, followRelease = 0.0f;
    float followTonalRelease = 0.0f;
    std::array<float, 9> lastMapping {};
    bool mappingValid = false;
};
}
