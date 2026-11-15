// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "Model.h"
#include "piano_simd.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace pm
{

/** The piano's engine, split for a polyphonic synth.

    The original Engine plays all 88 notes in one object and sums them. Atlas gives every note to a
    synth voice, so the parts of the model that belong to one note render inside that voice (a Lane):
    the hammer, the strings, their longitudinal vibration, the knock of the action, and the
    soundboard's answer to that note's thud and knock. The board is linear, so a board per note sums
    to the one board of the original. What couples the notes stays in the instrument and renders once
    per block after the voices: the sympathetic strings, driven by every note's motion at the bridge
    through an eight-region delay of one chunk, as in the original.

    The audio thread calls renderNote, renderBody and the note and pedal calls; everything else runs
    off it. A new model (Prepared) is handed over through offer() and taken at the end of a block.
*/
class Instrument
{
public:
    static constexpr int kChunk = 32;
    static constexpr int kMaxBlock = 1024;
    // The largest note has 2,244 main resonators at 48 kHz; a note's sympathetic bank has one per
    // partial, at most Model.cpp's kSympatheticMaxPartials; the board has about 1,250 modes to
    // 10 kHz plus the case modes. A model that outgrows these is cut to them (Prepared::make).
    static constexpr int kBankCapacity = 2560;
    static constexpr int kSympatheticCapacity = 40;
    static constexpr int kBoardCapacity = 1280;
    // Notes fading out after their voice was stolen, rendered by the voice that took them over.
    static constexpr int kFadeSlots = 8;

    struct LongitudinalFilters
    {
        static constexpr float damping = 1.0f / 40.0f;   // 1 / Q of the longitudinal modes
        float g[2] {}, a1[2] {}, a2[2] {}, a3[2] {}, dcRate = 0.0f, fadeRate = 0.0f;
    };

    /** A model with the tables an instrument reads beside it. Built off the audio thread. */
    struct Prepared
    {
        std::shared_ptr<const PianoModel> model;
        // Per resonator: state power above 1 / gate is audible (Engine.cpp's makeGate).
        std::vector<float> mainGate[kNumNotes], sympatheticGate[kNumNotes], boardGate;
        LongitudinalFilters longitudinal[kNumNotes];
        int mainCount[kNumNotes] {}, sympatheticCount[kNumNotes] {};
        int boardCount = 0;
        int plateCount = 0;      // board modes before the group that holds the case modes, a multiple of four

        static std::shared_ptr<const Prepared> make (std::shared_ptr<const PianoModel>);
    };

    /** The controls read when a key is struck. */
    struct Blow
    {
        float hardness = 1.0f;      // scales the felt stiffness
        float dynamics = 1.0f;      // 1 = full velocity range, 0 = every note at mezzo forte
        float actionNoise = 1.0f;   // knock of the key and hammer on the body
    };

    /** One struck note, the state a synth voice carries. Its buffers are allocated once. */
    struct Lane
    {
        Lane();

        const Prepared* prepared = nullptr;
        int note = -1;                               // index into the model's notes, -1 when silent
        std::vector<float> zr, zi, pr, pi;           // main bank: state and working poles
        int active = 0;                              // live main resonators, a multiple of 4

        bool contact = false;
        double hammerPosition = 0.0, hammerVelocity = 0.0, lastCompression = 0.0;
        float stringAtHammer = 0.0f;
        float hardness = 1.0f;                       // felt hardness of the blow in progress
        int contactSamples = 0;
        float phantoms = 1.0f;                       // longitudinal string vibration, read every block

        float knock = 0.0f;                          // peak force of the action knock still to play, N
        int knockPosition = 0;                       // sample within the knock's pulse; negative while it is still to come
        int knockLength = 0;

        float damper = 1.0f, damperTarget = 1.0f, damperRate = 0.0f;   // 1 = resting on the strings
        float appliedDamper = -1.0f;
        bool keyDown = false, heldBySostenuto = false;
        float lastLift = 0.0f;                       // the pedals as this lane last saw them
        bool lastSostenuto = false;

        float longFilter[2][2] {};
        float longDc = 0.0f, longFade = 0.0f;

        std::vector<float> boardRe, boardIm;         // this note's share of the soundboard
        int boardActive = 0;                         // plate modes running, a multiple of 4, pruned from the top
        bool caseActive = false;                     // the case modes, after the plate's

        int checkCountdown = 0;
        bool audible = false;                        // above the hold floor at the last check
        int fade = -1;                               // a fade slot this voice still renders, or -1
        uint32_t renderedBlock = ~0u;                // the block this lane last rendered in, and how far into it
        int renderedTo = 0;

        bool busy() const { return note >= 0 && (contact || active > 0 || knock > 0.0f || boardActive > 0 || caseActive); }
        /** Whether the note still asks the voice to stay open. */
        bool sounding() const { return busy() && audible; }
    };

    Instrument();
    ~Instrument();

    // ---- Builder thread ----
    /** Hands a prepared model to the audio thread, which takes it at the end of a block. A model
        offered before this one and not taken yet is dropped. */
    void offer (std::shared_ptr<const Prepared>);
    /** Deletes what the audio thread handed back. */
    void collect();
    bool hasOffer() const { return incoming.load() != nullptr; }
    /** Audio thread: adopts an offered model now instead of at the end of the block. */
    void takeOffer();

    // ---- Audio thread ----
    const Prepared* current() const { return prepared.get(); }
    double sampleRate() const { return prepared != nullptr ? prepared->model->sampleRate : 0.0; }

    /** Before the voices render a block: a voice will strike `note` (0 to 87) `sample` samples into
        it. The lane sounding that note then renders up to that sample and stops, so the strings can
        change hands there. */
    void announceStrike (int note, int sample);
    /** Before striking `note` in the lane: if another lane still sounds it, its strings change
        hands, so the hammer meets them as they move (the original has one set of strings per note)
        and that lane falls silent. Returns how far into this block the strings were rendered by their
        old lane (the announced sample, 0 when it had not rendered yet, or the whole block when the
        strike was not announced), or -1 when nothing changed hands. A lane sounding another note
        hands that to a fade slot first. */
    int takeOver (Lane&, int note, int fadeSamples);
    /** Strikes `note` in the lane, `sample` samples into the block. A lane still sounding the same
        note is struck as it moves; one sounding another note hands that to a fade slot first. */
    void strike (Lane&, int note, float velocity, const Blow&, int fadeSamples, int sample);
    /** The key comes up `sample` samples into the block: the damper falls unless a pedal holds it,
        and the key knocks on its return. */
    void release (Lane&, float lift, float actionNoise, int sample);
    /** Drops the lane's note and its fade slot at once. */
    void clear (Lane&);
    /** Moves the lane's note into a fade slot that fades out over `samples` as the lane renders it. */
    void fadeOut (Lane&, int samples);

    /** What the model should be built for, written by the audio thread and read by the builder. */
    struct Wanted
    {
        double sampleRate = 0.0, a4 = 440.0, stretch = 1.0, unison = 1.0, sustain = 1.0;
        bool operator== (const Wanted& o) const
        {
            return sampleRate == o.sampleRate && a4 == o.a4 && stretch == o.stretch && unison == o.unison && sustain == o.sustain;
        }
        bool operator!= (const Wanted& o) const { return ! (*this == o); }
    };
    void want (const Wanted&);
    /** Builder thread: the last wanted settings, or false while the audio thread is writing them. */
    bool readWanted (Wanted&) const;

    enum Pedal { kSustainPedal, kSostenutoPedal, kSoftPedal };
    static constexpr int kMaxPedalEvents = 64;
    /** A pedal moves `sample` samples into this block: sustain 0 to 1 (continuous), sostenuto 0 or
        1, soft 0 to 1 (una corda). Events arrive in order, before the voices render the block, and
        take effect at their sample in every lane and in the body. */
    void pedalEvent (Pedal, float value, int sample);
    void setSympathetic (float amount) { sympathetic = amount; }
    void setWidth (float amount) { width = amount; }

    /** Renders `n` samples of the lane's note into left and right (overwrites), starting `offset`
        samples into a block of `blockSize` samples. Chunks follow the block's grid of kChunk. */
    void renderNote (Lane&, float* left, float* right, int offset, int n, int blockSize);
    /** Renders the sympathetic strings for the block the voices just rendered (overwrites), then
        closes the block: the histories shift and a model offered meanwhile takes over. */
    void renderBody (float* left, float* right, int blockSize);

    bool anyOpen() const { return anySympathetic; }

private:
    struct BodyNote
    {
        std::vector<float> szr, szi, spr, spi;       // sympathetic bank
        float damper = 1.0f, damperTarget = 1.0f, damperRate = 0.0f, appliedDamper = -1.0f;
        int heldLanes = 0;                           // lanes whose key is down on this note
        bool heldBySostenuto = false;
        bool sympatheticLive = false;
        int checkCountdown = 0;
        uint32_t ownBlock = ~0u;                     // the last block a lane wrote this note's own motion in
    };

    struct FadeSlot
    {
        Lane lane;
        int remaining = 0, length = 1;
    };

    struct Pedals
    {
        float lift = 0.0f;                           // how far the sustain pedal has lifted the dampers, 0 to 1
        bool sostenuto = false;
        float soft = 0.0f;
    };
    struct PedalEvent
    {
        int sample = 0;
        Pedal pedal = kSustainPedal;
        float value = 0.0f;
    };

    void openBlock (int blockSize);
    /** The pedals `sample` samples into the block, and the sample of the next pedal event after it. */
    Pedals pedalsAt (int sample) const;
    int nextPedalEvent (int sample) const;
    /** Gives the lane the pedals at `sample`: a sostenuto pressed while its key is down holds it;
        its release, and the sustain pedal, set a free damper's target anew. */
    void syncPedals (Lane&, int sample);
    void keyDown (int note);
    void keyUp (int note, float lift, float dampersLift);
    void startKnock (Lane&, double speed, double seconds, float spread, int delaySamples, float actionNoise);
    float unit();                                    // the next random value in [-1, 1)

    void noteChunk (Lane&, float* left, float* right, int offset, int n);
    void applyDamper (Lane&);
    void applyDamper (BodyNote&, int index);
    void runContact (Lane&, int n, simd::f4* accL, simd::f4* accR, simd::f4* accOwn, float* longL, float* longR,
                     float (*thumpIn)[kThumpModes], bool& thumpActive);
    void runFree (Lane&, int n, simd::f4* accL, simd::f4* accR, simd::f4* accOwn);
    void runLongitudinal (Lane&, int n, float* longL, float* longR);
    float longitudinalSample (Lane&, const float* u, int partials);
    void runBoard (Lane&, int n, const float* knockForce, const float (*thumpIn)[kThumpModes], bool thumpActive,
                   bool bus, simd::f4* accL, simd::f4* accR, simd::f4 (*regionAcc)[kChunk]);
    void runSympathetic (BodyNote&, int index, const float* input, int n, simd::f4* accL, simd::f4* accR);
    void prune (Lane&);
    void pruneBoard (Lane&);
    void checkAudible (Lane&);
    void renderFade (Lane&, float* left, float* right, int offset, int n, int blockSize);
    void dropFade (Lane&);

    std::shared_ptr<const Prepared> prepared, retired;
    std::atomic<double> wantedValues[5] {};
    std::atomic<uint32_t> wantedSequence { 0 };
    std::atomic<std::shared_ptr<const Prepared>*> incoming { nullptr };
    std::atomic<std::shared_ptr<const Prepared>*> outgoing { nullptr };

    std::array<BodyNote, kNumNotes> notes;
    std::array<FadeSlot, kFadeSlots> fades;
    std::array<Lane*, kNumNotes> owners {};           // the lane sounding each note, or null
    std::array<int, kNumNotes> handOver;              // the sample a strike of each note is announced for this block, or -1

    // Bridge motion of each region, and of each note's own strings, over the block: the last kChunk
    // samples of the previous block, then this block's. The sympathetic strings read one chunk back.
    float regionHistory[kNumRegions][kChunk + kMaxBlock] {};
    std::vector<float> ownHistory;                   // [note][kChunk + kMaxBlock]
    uint32_t block = 0;
    int blockSamples = 0;
    bool blockOpen = false;

    float sympathetic = 1.0f;
    float width = 1.0f;
    // The pedals where the block starts, the events within it, and how the body last saw them.
    float sustainStart = 0.0f, softStart = 0.0f;
    bool sostenutoStart = false;
    std::array<PedalEvent, kMaxPedalEvents> pedalEvents;
    int numPedalEvents = 0;
    float bodyLift = 0.0f;
    bool bodySostenuto = false;
    bool anySympathetic = false;
    uint32_t random;                                 // for the small differences between one knock and the next
};

} // namespace pm
