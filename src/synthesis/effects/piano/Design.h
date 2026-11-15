#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace pm
{

constexpr int kNumNotes = 88;
constexpr int kLowestMidi = 21;
constexpr int kMaxStrings = 3;
constexpr int kNumRegions = 8;
constexpr double kPi = 3.14159265358979323846;

/** Settings the model tables are built from. Changing any of them needs a rebuild. */
struct Voicing
{
    double sampleRate = 48000.0;
    double a4 = 440.0;
    double stretch = 1.0;        // 0 = equal temperament on the first partial, 1 = full octave stretch
    double unison = 1.0;         // spread of the strings of one note, 1 = as voiced
    double sustain = 1.0;        // > 1 lengthens the decay (less energy leaves through the bridge)
    double stringLoss = 1.0;     // scales the losses inside the strings (air, internal friction)
    double sympathetic = 1.0;    // strength of string to string coupling through the bridge
    uint32_t seed = 1;
};

struct NoteDesign
{
    int midi = 0;
    int strings = 1;
    bool wound = false;
    bool hasDamper = true;
    int region = 0;

    // string
    double f0 = 0.0;             // Hz, fundamental of the ideal (flexible) string after tuning
    double inharmonicity = 0.0;  // B
    double length = 0.0;         // m
    double tension = 0.0;        // N, per string
    double mu = 0.0;             // kg/m, per string
    double coreDiameter = 0.0;   // m
    double detuneCents[kMaxStrings] {};
    double inharmonicityScale[kMaxStrings] { 1.0, 1.0, 1.0 };
    double horizontalCents[kMaxStrings] {};

    // hammer
    double strikeRatio = 0.125;  // distance from the agraffe / speaking length
    double hammerWidth = 0.008;  // m, contact length along the string
    double hammerMass = 0.009;   // kg
    double hammerK = 1.0e9;      // N / m^p, per string
    double hammerP = 2.5;
    double hammerHysteresis = 0.0; // s/m, Hunt-Crossley damping factor
    double strikeGain[kMaxStrings] { 1.0, 1.0, 1.0 };        // how evenly the hammer meets each string
    double strikeOffset[kMaxStrings] {};                     // each string's strike point relative to strikeRatio
    double horizontalGain[kMaxStrings] {};                   // sideways part of the blow

    // bridge
    double bridgeX = 0.0, bridgeY = 0.0; // m, on the soundboard
    double bridgeTilt = 0.15;            // couples the two polarisations

    // damper
    double damperTime = 0.08;    // s, time constant of the fundamental once the damper rests on the string
};

/** Constants fitted against measurements (tools/fit_decay.py, tools/fit_tone.py). */
struct Calibration
{
    struct Point { double x, y; };

    std::vector<Point> mobilityByNote;        // average bridge mobility, m/(N s), by MIDI note
    std::vector<Point> mobilityByFrequency;   // factor on the mobility, by Hz
    std::vector<Point> radiationByFrequency;  // average radiated level, dB, by Hz
    struct PartialLevels { double note; double db[8]; };
    std::vector<PartialLevels> voicing;       // level trim of partials 1 to 8, dB, by MIDI note
    static constexpr int kFinePartials = 12;
    struct FineLevels { int note; double db[kFinePartials]; };
    std::vector<FineLevels> fineVoicing;      // level trim of partials 1 to 12 of one note, dB, on top of the voicing by register
    std::vector<Point> tiltByNote;            // for partials above the 8th: dB per octave from the 8th's trim
    std::vector<Point> evennessByPartial;     // how far each partial is pulled to the board's local average response, by partial number
    double frictionPlain = 2.4e-3;            // internal friction of plain steel strings
    double frictionWound = 3.75e-3;           // and of wound strings
    double frictionFloor = 0.0;               // a part of the friction in proportion to frequency alone, as a stiffness term; 0.15 matched a recorded piano's aftersound but was heard as worse
    double airScale = 1.0;
    std::vector<Point> lossByNote;            // 1/s, loss at the string's ends, by MIDI note: the same for every partial
    std::vector<Point> unisonByNote;          // spread of a note's strings in cents, by MIDI note
    double strikeUnevenness = 0.06;           // how unevenly the hammer meets the strings of a note
    double strikeSpread = 0.04;               // how far apart along the string the strings of a note are met
    double horizontalBlow = 0.20;             // sideways part of the blow
    double bridgeTilt = 0.25;                 // how far the bridge's motion leans from the vertical
    double horizontalCents = 0.35;            // pitch difference between a string's two polarisations
    double inharmonicityMismatch = 0.01;      // stiffness difference between the strings of a note
    double longitudinalCoupling = 0.1;        // share of the string's tension changes that moves the soundboard
    double sympatheticGain = 0.21;           // how strongly the bridge's motion drives the other strings
    std::vector<Point> knockByFrequency;      // action knock: how strongly it drives the body's modes, dB, by Hz
    std::vector<Point> knockByNote;           // and how strong it is for each note, dB, by MIDI note
    double knockNewtons = 0.87;               // and its peak force at 1 m/s hammer speed, as a 0.8 ms pulse
    double knockPower = 1.0;                  // how the knock grows with hammer speed (a recorded piano's body under its top notes grows 13 dB from pp to ff)
    std::vector<Point> thumpByFrequency;      // the blow's push on the bridge: how strongly it drives the board's low modes, dB, by Hz
    std::vector<Point> thumpByNote;           // and a trim for each note, dB, by MIDI note
    double thumpToStrings = 0.5;              // share (in dB terms) of that fitted gain the sympathetic strings get from the lowest modes
    double outputGain = 0.944;                // overall level, set so whole passages come out as loud as the reference

    Calibration();
    double mobility (int midi, double hz) const;
    double endLoss (int midi) const;
    double radiationLevel (double hz) const;        // linear gain, the same for every note
    double knockLevel (double hz) const;            // linear gain
    double knockGain (int midi) const;              // linear gain
    double thumpLevel (double hz) const;            // linear gain
    double thumpGain (int midi) const;              // linear gain
    double radiation (int midi, double hz, int partial) const;   // with the note's voicing trims
    double evenness (int partial) const;            // 0 = the board's response at one point as it is, 1 = its local average
};

struct PianoDesign
{
    Voicing voicing;
    Calibration calibration;
    std::array<NoteDesign, kNumNotes> notes;
};

PianoDesign makeDesign (const Voicing&);

/** Deterministic value in [-1, 1] from three integers. */
double hashUnit (uint32_t a, uint32_t b, uint32_t c);

/** Frequency of partial k of a stiff string. */
inline double partialRatio (int k, double B)
{
    return k * std::sqrt (1.0 + B * k * k);
}

} // namespace pm
