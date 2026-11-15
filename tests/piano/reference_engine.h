// The original Engine.{h,cpp} of the piano, with its NEON intrinsics read through
// piano_simd.h so that it builds on x64 too. The tests play it against the port.
#pragma once

#include "Model.h"
#include "piano_simd.h"

#include <memory>
#include <vector>

namespace pm
{

/** Plays a PianoModel. Everything in process() and the note and pedal calls is real-time safe;
    setModel() allocates and must be called off the audio thread or while audio is stopped.
*/
class ReferenceEngine
{
public:
    struct Settings
    {
        float volume = 1.0f;            // linear
        float hammerHardness = 1.0f;    // scales the felt stiffness
        float dynamics = 1.0f;          // 1 = full velocity range, 0 = every note at mezzo forte
        float sympathetic = 1.0f;       // scales string to string resonance
        float actionNoise = 1.0f;       // knock of the key and hammer on the body
        float phantoms = 1.0f;          // longitudinal string vibration
    };

    void setModel (std::shared_ptr<const PianoModel>);
    void reset();

    void noteOn (int midi, float velocity);      // velocity 0 to 127
    void noteOff (int midi, float velocity);
    void setSustain (float amount);              // 0 to 1, continuous
    void setSostenuto (bool down);
    void setSoft (float amount);                 // una corda, 0 to 1
    void allNotesOff();

    Settings settings;

    /** Renders into left and right (overwrites). */
    void process (float* left, float* right, int numSamples);

    int activeResonators() const { return liveResonators; }

private:
    static constexpr int kChunk = 32;

    struct LongitudinalFilters
    {
        static constexpr float damping = 1.0f / 40.0f;   // 1 / Q of the longitudinal modes
        float g[2] {}, a1[2] {}, a2[2] {}, a3[2] {}, dcRate = 0.0f, fadeRate = 0.0f;
    };

    struct Voice
    {
        const NoteModel* note = nullptr;
        std::vector<float> zr, zi, pr, pi;          // main bank: state and working poles
        std::vector<float> gate;                     // per resonator: state power above 1 / gate is audible
        std::vector<float> szr, szi, spr, spi;       // sympathetic bank
        std::vector<float> sgate;
        float own[kChunk] {};                        // own bridge motion, delayed one chunk length

        int active = 0;                              // live main resonators, a multiple of 4
        bool contact = false;
        double hammerPosition = 0.0, hammerVelocity = 0.0, lastCompression = 0.0;
        float stringAtHammer = 0.0f;
        float hardness = 1.0f;                       // felt hardness of the blow in progress (soft pedal)
        int contactSamples = 0;

        float knock = 0.0f;                          // peak force of the action knock still to play, N
        int knockPosition = 0;                       // sample within the knock's pulse; negative while it is still to come
        int knockLength = 0;                         // the pulse's length in samples

        float damper = 1.0f, damperTarget = 1.0f, damperRate = 0.0f;   // 1 = resting on the strings
        float appliedDamper = -1.0f;
        bool keyDown = false, heldBySostenuto = false;

        LongitudinalFilters longCoefficients;        // longitudinal modes: filters for this note at this sample rate,
        float longFilter[2][2] {};                   // their states,
        float longDc = 0.0f, longFade = 0.0f;        // the slow part of the tension, and the fade-in of a fresh note

        bool sympatheticLive = false;
        int checkCountdown = 0;
    };

    void processChunk (float* left, float* right, int n);
    void applyDamper (Voice&);
    void updateDamperTargets();
    void runContact (Voice&, int n);
    void runFree (Voice&, int n);
    void runSympathetic (Voice&, const float* input, int n);
    void runLongitudinal (Voice&, int n);
    LongitudinalFilters longitudinalFilters (const NoteModel&) const;
    float longitudinalSample (Voice&, const float* u, int partials);     // one sample, before the Phantoms gain
    void runBoard (int n);
    void prune (Voice&);
    void startKnock (Voice&, double speed, double seconds, float spread, int delaySamples);
    float unit();                                    // the next random value in [-1, 1)

    std::shared_ptr<const PianoModel> model;
    Voice voices[kNumNotes];

    std::vector<float> boardRe, boardIm;             // soundboard state
    std::vector<float> boardGate;
    bool boardAwake = false;                         // the board is skipped while it is silent and nothing drives it
    int boardCheck = 0;
    float regionRing[kNumRegions][kChunk] {};        // bridge motion per region, delayed one chunk length
    int ringPosition = 0;

    // per chunk scratch
    simd::f4 accL[kChunk], accR[kChunk], accOwn[kChunk];
    float regionNew[kNumRegions][kChunk];
    float knockForce[kNumRegions][kChunk];           // action knocks under the keys of each region
    float longL[kChunk], longR[kChunk];              // longitudinal string sound, panned by note
    uint32_t knockRegions = 0;                       // bit per region with a knock in this chunk
    alignas (16) float thumpIn[kChunk][kThumpModes];     // this chunk's push from the hammers on the board's lowest modes
    bool thumpActive = false;

    float sustain = 0.0f, soft = 0.0f;
    bool sostenuto = false;
    bool anySympathetic = false;
    int liveResonators = 0;
    float dampersLift = 0.0f;
    uint32_t random = 1;                             // for the small differences between one knock and the next
};

} // namespace pm
