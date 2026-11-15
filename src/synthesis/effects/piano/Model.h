#pragma once

#include "Design.h"

#include <memory>
#include <vector>

namespace pm
{

/** A bank of complex one-pole resonators, stored as separate arrays so they vectorise.

    Every resonator holds a complex state z and steps as z <- pole * (z + input). All resonators of
    a bank share the same real input, so the weights that differ between them sit on the read side.
    Sizes are padded to a multiple of four; the padding has zero poles and zero weights.
*/
struct Bank
{
    int count = 0;                          // padded
    int used = 0;                           // real resonators
    std::vector<float> poleRe, poleIm;      // one sample, dampers up
    std::vector<float> subRe, subIm;        // one hammer substep
    std::vector<float> damping;             // fraction of the radius lost per sample with the damper down
    std::vector<float> hammerRe, hammerIm;  // string displacement under the hammer, m per unit state
    std::vector<float> outRe[2], outIm[2];  // sound at each microphone
    std::vector<float> busRe, busIm;        // bridge displacement, for sympathetic strings

    void resize (int realCount);
};

constexpr int kThumpModes = 40;      // the board's modes below about 400 Hz
constexpr int kCaseModes = 3;        // modes for the case and frame, after the board's own in PianoModel::board
constexpr int kLongPartials = 48;    // most partials of a note that the longitudinal model reads

struct NoteModel
{
    Bank main;          // driven by the hammer
    Bank sympathetic;   // driven by the bridge
    int region = 0;
    bool hasDamper = true;

    float hammerMass = 0.009f, hammerK = 1.0e9f, hammerP = 2.5f, hammerHysteresis = 0.0f;
    float stringsHit = 1.0f;                 // sum of the strings' strike gains
    int substeps = 4;                        // hammer steps per sample, enough for the felt to stay stable
    float knockGain = 1.0f;                  // strength of the action knock for this note

    // The blow's push on the bridge, per unit of hammer impulse on one string, into the board's lowest modes.
    float thump[kThumpModes] {};
    int thumpCount = 0;                      // modes driven, a multiple of four

    // Longitudinal string vibration (phantom partials): the string's tension follows the square of
    // its motion, and the first two longitudinal modes colour the result.
    std::vector<int> longIndex;              // main-bank index of each partial's strongest resonator
    std::vector<float> longRe, longIm;       // partial number times modal displacement, per unit state
    float longGain = 0.0f;                   // output level per unit of that squared, 0 = off
    float longHz[2] {};                      // longitudinal mode frequencies, 0 = above the usable range
    float longOut[2] { 1.0f, 1.0f };         // its share at each output: the note's place between left and right
};

struct PianoModel
{
    double sampleRate = 48000.0;
    NoteModel notes[kNumNotes];

    Bank board;                              // soundboard modes: uses poleRe/Im, outRe/Im
    std::vector<float> boardBus[kNumRegions];   // bridge displacement of each region per unit Im(state)
    int boardBusCount = 0;                      // the modes that move the bridge: the first so many, a multiple of 4
    std::vector<float> knock[kNumRegions];      // action knock force to each soundboard mode, from under the keys of each region
    float knockNewtons = 1.0f, knockPower = 0.85f;
    float regionGain[kNumRegions][kNumRegions] {};
    float outputGain = 1.0f;
};

std::shared_ptr<const PianoModel> buildModel (const Voicing&);

} // namespace pm
