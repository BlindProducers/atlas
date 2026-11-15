#pragma once

#include "Design.h"

#include <complex>
#include <vector>

namespace pm
{

using cplx = std::complex<double>;

/** Modal model of the soundboard: an orthotropic ribbed plate with clamped edges.

    Each mode has a frequency, a decay rate, its shape sampled where every note meets the bridge,
    and a complex radiation weight for each microphone (a Rayleigh integral over the board).
*/
struct Soundboard
{
    static constexpr double lengthX = 1.40, lengthY = 1.90;   // m
    static constexpr int numMics = 2;

    int count = 0;
    double mass = 0.0;                         // kg, modal mass of every mode
    std::vector<double> omega;                 // rad/s, undamped
    std::vector<double> sigma;                 // 1/s
    std::vector<cplx> pole;                    // -sigma + j * damped frequency
    std::vector<double> shapeAtNote;           // [note * count + mode]
    std::vector<double> shapeAtRegion;         // [region * count + mode]
    std::vector<double> shapeAtKey;            // [region * count + mode], under the keys of each region
    std::vector<double> shapeAtKeybed;         // [mode], under the middle of the keyboard
    std::vector<cplx> radiation[numMics];      // [mode]

    /** Per unit force at a note's bridge point: the plate's velocity there, m/(N s), and the sound
        at each microphone. */
    void evaluate (int note, cplx s, cplx& mobilityOut, cplx& transfer0, cplx& transfer1) const;

    /** Residue of a transfer at a mode's pole, without the note's shape factor. */
    cplx modeOutput (int mode, int mic) const;
};

Soundboard makeSoundboard (const PianoDesign&, double maxFrequency);


} // namespace pm
