#include "Design.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace pm
{

namespace
{
    struct Point { double x, y; };

    template <size_t N>
    double interpolate (const Point (&pts)[N], double x)
    {
        if (x <= pts[0].x) return pts[0].y;
        for (size_t i = 1; i < N; ++i)
            if (x <= pts[i].x)
                return pts[i - 1].y + (pts[i].y - pts[i - 1].y) * (x - pts[i - 1].x) / (pts[i].x - pts[i - 1].x);
        return pts[N - 1].y;
    }

    constexpr int kFirstPlainMidi = 41;   // F2: plain steel trichords from here up
    constexpr double kSteelDensity = 7850.0;
    constexpr double kSteelYoung = 2.02e11;

    // Speaking lengths follow a concert grand: published values for D#1, C2, F3, C#5 and G6
    // (Chabassier, Chaigne and Joly, JASA 2013) joined by smooth curves.
    double speakingLength (int midi)
    {
        if (midi < kFirstPlainMidi)
        {
            const double x = midi - 21;
            return 2.01 + 0.00017 * x - 0.001833 * x * x;
        }
        if (midi < 53)
        {
            const double x = 53 - midi;
            return 0.961 * std::exp (0.05403 * x - 8.76e-4 * x * x);
        }
        if (midi <= 73)
            return 0.961 * std::pow (0.9474, midi - 53);
        return 0.326 * std::pow (0.9477, midi - 73);
    }

    double coreDiameter (int midi)
    {
        static const Point wound[] = { { 21, 1.60 }, { 27, 1.40 }, { 29, 1.30 }, { 33, 1.05 }, { 34, 1.00 }, { 36, 0.9488 }, { 40, 0.90 } };
        static const Point plain[] = { { 41, 1.13 }, { 53, 1.0525 }, { 73, 0.921 }, { 91, 0.794 }, { 108, 0.775 } };
        return 1.0e-3 * (midi < kFirstPlainMidi ? interpolate (wound, midi) : interpolate (plain, midi));
    }

    double woundTension (int midi)
    {
        static const Point pts[] = { { 21, 1500 }, { 27, 1781 }, { 28, 1750 }, { 29, 1450 }, { 33, 1350 }, { 34, 900 }, { 36, 865 }, { 40, 830 } };
        return interpolate (pts, midi);
    }

    int stringCount (int midi)
    {
        return midi <= 28 ? 1 : (midi <= 33 ? 2 : 3);
    }

    /** Octave stretch: each note is tuned so its partials meet those of the octave below or above. */
    void tune (PianoDesign& d)
    {
        auto& n = d.notes;
        auto partial = [&] (int i, int k)   // partial k relative to partial 1
        {
            const double B = n[(size_t) i].inharmonicity;
            return partialRatio (k, B) / partialRatio (1, B);
        };

        std::array<double, kNumNotes> cents {};   // deviation of partial 1 from equal temperament
        const int a4 = 69 - kLowestMidi, a3 = a4 - 12;

        // A3 from A4 by a 4:2 octave, the notes between spread evenly.
        const double a3Cents = 1200.0 * std::log2 (2.0 * partial (a4, 2) / partial (a3, 4));
        for (int i = a3; i <= a4; ++i)
            cents[(size_t) i] = a3Cents * (double) (a4 - i) / 12.0;

        for (int i = a3 - 1; i >= 0; --i)
        {
            const int up = i + 12;
            const double midi = i + kLowestMidi;
            const double w63 = std::fmin (1.0, std::fmax (0.0, (52.0 - midi) / 12.0));   // 6:3 octaves in the bass
            const double c42 = cents[(size_t) up] + 1200.0 * std::log2 (2.0 * partial (up, 2) / partial (i, 4));
            const double c63 = cents[(size_t) up] + 1200.0 * std::log2 (2.0 * partial (up, 3) / partial (i, 6));
            cents[(size_t) i] = (1.0 - w63) * c42 + w63 * c63;
        }

        for (int i = a4 + 1; i < kNumNotes; ++i)
        {
            const int down = i - 12;
            const double midi = i + kLowestMidi;
            const double w21 = std::fmin (1.0, std::fmax (0.0, (midi - 76.0) / 16.0));   // 2:1 octaves in the top
            const double c42 = cents[(size_t) down] + 1200.0 * std::log2 (0.5 * partial (down, 4) / partial (i, 2));
            const double c21 = cents[(size_t) down] + 1200.0 * std::log2 (0.5 * partial (down, 2));
            cents[(size_t) i] = (1.0 - w21) * c42 + w21 * c21;
        }

        for (int i = 0; i < kNumNotes; ++i)
        {
            auto& note = n[(size_t) i];
            const double et = d.voicing.a4 * std::pow (2.0, (note.midi - 69) / 12.0);
            const double f1 = et * std::pow (2.0, d.voicing.stretch * cents[(size_t) i] / 1200.0);
            note.f0 = f1 / partialRatio (1, note.inharmonicity);
        }
    }
}

namespace
{
    double interpolate (const std::vector<Calibration::Point>& pts, double x)
    {
        if (pts.empty()) return 1.0;
        if (x <= pts.front().x) return pts.front().y;
        for (size_t i = 1; i < pts.size(); ++i)
            if (x <= pts[i].x)
                return pts[i - 1].y + (pts[i].y - pts[i - 1].y) * (x - pts[i - 1].x) / (pts[i].x - pts[i - 1].x);
        return pts.back().y;
    }

    /** Development only: PIANO_CAL names a text file whose lines replace calibration values,
        "name value" or "name x:y x:y ...", so the fitting scripts can try values without a rebuild. */
    void applyOverrides (Calibration& c)
    {
        const char* path = std::getenv ("PIANO_CAL");
        FILE* f = path != nullptr ? std::fopen (path, "r") : nullptr;
        if (f == nullptr)
            return;
        char line[4096];
        while (std::fgets (line, sizeof (line), f) != nullptr)
        {
            char name[64];
            int used = 0;
            if (std::sscanf (line, "%63s%n", name, &used) != 1)
                continue;
            const std::string key (name);
            std::vector<Calibration::Point> points;
            double x = 0.0, y = 0.0;
            int step = 0;
            const char* rest = line + used;
            while (std::sscanf (rest, " %lf:%lf%n", &x, &y, &step) == 2) { points.push_back ({ x, y }); rest += step; }
            const double value = std::atof (line + used);
            if (key == "mobilityByNote") c.mobilityByNote = points;
            else if (key == "mobilityByFrequency") c.mobilityByFrequency = points;
            else if (key == "radiationByFrequency") c.radiationByFrequency = points;
            else if (key == "voicingFlat")      // no voicing at all: every trim at 0 dB
            {
                c.voicing.assign (2, {});
                c.voicing[0].note = 21.0; c.voicing[1].note = 108.0;
                c.tiltByNote.assign (2, { 21.0, 0.0 });
                c.tiltByNote[1].x = 108.0;
                c.fineVoicing.clear();
            }
            else if (key == "fineVoicingFlat") c.fineVoicing.clear();
            else if (key == "strikeSpread") c.strikeSpread = value;
            else if (key == "evennessByPartial") c.evennessByPartial = points;
            else if (key == "unisonByNote") c.unisonByNote = points;
            else if (key == "strikeUnevenness") c.strikeUnevenness = value;
            else if (key == "horizontalBlow") c.horizontalBlow = value;
            else if (key == "bridgeTilt") c.bridgeTilt = value;
            else if (key == "horizontalCents") c.horizontalCents = value;
            else if (key == "inharmonicityMismatch") c.inharmonicityMismatch = value;
            else if (key == "knockByFrequency") c.knockByFrequency = points;
            else if (key == "knockByNote") c.knockByNote = points;
            else if (key == "thumpByFrequency") c.thumpByFrequency = points;
            else if (key == "thumpByNote") c.thumpByNote = points;
            else if (key == "thumpToStrings") c.thumpToStrings = value;
            else if (key == "sympatheticGain") c.sympatheticGain = value;
            else if (key == "longitudinalCoupling") c.longitudinalCoupling = value;
            else if (key == "knockNewtons") c.knockNewtons = value;
            else if (key == "knockPower") c.knockPower = value;
            else if (key == "outputGain") c.outputGain = value;
            else if (key == "frictionPlain") c.frictionPlain = value;
            else if (key == "frictionWound") c.frictionWound = value;
            else if (key == "frictionFloor") c.frictionFloor = value;
            else if (key == "airScale") c.airScale = value;
            else if (key == "lossByNote") c.lossByNote = points;
        }
        std::fclose (f);
    }
}

Calibration::Calibration()
{
    // tools/fit_sustain.py, smoothed across the keyboard: a bridge impedance of 2,000 to 2,500 kg/s for
    // nearly every note, more mobile at the lowest and highest frequencies
    mobilityByNote = { { 21, 3.9e-4 }, { 27, 3.7e-4 }, { 33, 4.4e-4 }, { 39, 4.9e-4 }, { 45, 5.1e-4 }, { 51, 4.9e-4 }, { 57, 4.1e-4 }, { 63, 4.25e-4 },
                       { 69, 4.3e-4 }, { 75, 4.0e-4 }, { 81, 4.1e-4 }, { 87, 4.2e-4 }, { 93, 5.2e-4 }, { 99, 3.9e-4 }, { 108, 1.3e-4 } };
    mobilityByFrequency = { { 30, 1.78 }, { 60, 1.06 }, { 120, 0.77 }, { 250, 0.95 }, { 500, 1.06 }, { 1000, 1.0 }, { 2000, 1.3 }, { 4000, 2.3 },
                            { 8000, 3.0 }, { 16000, 2.4 } };
    // tools/fit_tone.py, lightly smoothed: the board radiates little below 60 Hz and evenly from 700 Hz up
    radiationByFrequency = { { 22, -60.0 }, { 31, -58.0 }, { 44, -51.0 }, { 62, -45.5 }, { 88, -35.0 }, { 125, -34.5 }, { 177, -39.0 }, { 250, -44.0 },
                             { 354, -42.0 }, { 500, -43.0 }, { 707, -39.0 }, { 1000, -37.0 }, { 1414, -37.0 }, { 2000, -37.0 }, { 2828, -39.0 },
                             { 4000, -42.5 }, { 5657, -45.5 }, { 8000, -47.0 }, { 11314, -51.0 }, { 16000, -56.0 } };
    // tools/fit_sustain.py: how each partial falls over its first seconds. The strings of a note sit about
    // a cent apart in the middle of the keyboard, which is what gives the tone its slow second stage.
    unisonByNote = { { 21, 0.42 }, { 45, 0.68 }, { 69, 0.8 }, { 93, 1.0 }, { 108, 0.53 } };     // cents
    lossByNote = { { 21, 0.038 }, { 33, 0.048 }, { 45, 0.065 }, { 57, 0.082 }, { 69, 0.12 }, { 81, 0.45 }, { 93, 1.7 }, { 108, 3.2 } };
    // Voicing (tools/fit_voicing.py): the level of each of the first eight partials by register, as a
    // voicer sets a piano's tone, measured against the reference. Above the 8th partial the trim
    // continues along a tilt.
    voicing = {
        { 21, { 4.3, 0.4, 2.7, 2.6, -16.2, 12.0, 12.0, -6.7 } },
        { 27, { 0.9, -0.5, 2.0, -1.2, -0.9, 0.8, 1.0, 10.7 } },
        { 33, { -5.3, 1.4, 2.5, -3.7, 10.0, 10.5, 9.4, 4.2 } },
        { 39, { 6.4, 6.1, 4.8, 1.7, 4.1, 3.8, 7.4, -7.6 } },
        { 45, { 1.9, 3.8, 8.3, 12.0, 6.0, -10.6, 4.6, 0.1 } },
        { 51, { 0.7, 0.1, 4.4, 12.0, 9.4, 3.3, 9.3, -2.0 } },
        { 57, { 10.0, 5.2, -1.6, 0.3, 6.9, 6.8, 8.8, -3.9 } },
        { 63, { 8.2, -0.3, -0.9, 0.6, 3.0, 1.4, 6.3, 12.0 } },
        { 69, { 8.4, -5.3, 1.8, 9.8, 0.0, 5.8, 1.1, 11.0 } },
        { 75, { 2.8, -1.8, -2.2, -1.4, 5.6, 3.3, 1.8, 0.6 } },
        { 81, { -0.2, 1.8, -1.3, 0.4, 3.7, -0.5, 6.0, 2.8 } },
        { 87, { 2.8, 1.6, -9.5, -5.4, -15.0, -8.3, -8.9, -7.9 } },
        { 90, { 2.3, -0.1, -8.7, -7.5, -13.0, -6.7, -1.1, -5.5 } },
        { 93, { 3.1, -0.3, -10.6, -10.9, -11.5, -5.8, -5.1, -3.9 } },
        { 96, { 3.2, 1.5, -12.4, -8.0, -6.4, -7.7, -13.7, -13.7 } },
        { 99, { 1.6, 1.0, -17.7, -5.2, 1.1, -6.4, -6.4, -6.4 } },
        { 102, { -0.9, -8.2, -11.7, 4.7, 0.9, 0.9, 0.9, 0.9 } },
        { 105, { -1.5, -14.4, 12.0, 12.0, 12.0, 12.0, 12.0, 12.0 } },
        { 108, { -7.8, -17.5, 12.0, 12.0, 12.0, 12.0, 12.0, 12.0 } },
    };
    // The top two octaves' first two partials carry 3 dB more than the fit on the first 50 ms asked for: the
    // sustained part of those notes measured 3 to 6 dB under the reference and was heard as thin.
    // The board's response from one bridge point to one microphone swings by 10 dB and more between neighbouring
    // partials. The lowest partials carry a note's loudness and sustain, so they are held close to the local
    // average and the keyboard stays even; higher partials keep more of the pattern.
    evennessByPartial = { { 1, 0.9 }, { 2, 0.75 }, { 4, 0.75 }, { 5, 0.5 }, { 8, 0.5 } };
    // Fine voicing (tools/fit_fine_voicing.py): what is left for each note once the register voicing is applied.
    // Single partials of single notes stood out by 10 to 25 dB against the reference and were heard as nasal
    // in the tenor; this table takes them down. Boosts stop at +6 dB, cuts at -20 dB.
    fineVoicing = {
        { 21, { -1.4, 0.9, 2.5, 3.0, 6.0, -20.0, -0.9, -0.4, 6.0, 5.1, 5.3, 6.0 } },
        { 22, { 1.2, 6.0, 1.2, 1.8, 6.0, -10.3, 6.0, 4.3, 6.0, -9.7, 6.0, 6.0 } },
        { 23, { -1.4, 0.1, -2.7, 1.3, 1.1, 6.0, 6.0, 6.0, 1.7, 1.0, 6.0, 6.0 } },
        { 24, { 1.3, -6.5, -0.5, 0.2, -14.0, 6.0, -2.6, -0.1, -1.5, 2.0, 1.1, 6.0 } },
        { 25, { 2.1, 6.0, 5.1, 4.6, -2.2, 6.0, -3.5, -20.0, 6.0, 6.0, 6.0, -18.0 } },
        { 26, { 2.0, 6.0, 0.9, 1.1, 5.8, -3.5, 6.0, 2.6, -4.9, 4.2, 5.6, -15.6 } },
        { 27, { 2.3, 6.0, 1.9, -13.2, 6.0, 1.6, 2.3, 6.0, -2.4, 6.0, -18.0, -2.6 } },
        { 28, { 6.0, 6.0, 2.7, -16.2, 6.0, -2.7, -2.2, -8.7, 5.6, 3.6, -2.1, 4.6 } },
        { 29, { -3.3, 1.2, -0.9, -2.4, 6.0, -0.8, -2.5, -5.1, 6.0, 6.0, -20.0, 1.5 } },
        { 30, { 2.5, -5.4, 1.8, 6.0, -2.5, -8.6, 0.3, -0.9, 6.0, -6.6, 0.5, -5.9 } },
        { 31, { 5.3, 0.3, -1.3, 6.0, -16.5, -16.3, 2.8, 5.0, -3.1, 1.2, -1.7, -7.2 } },
        { 32, { -0.2, -0.2, -11.6, 6.0, 0.8, -3.4, 2.6, 6.0, -3.7, 4.7, 3.3, 6.0 } },
        { 33, { 6.0, 0.2, -12.6, 6.0, -5.9, -2.6, 5.9, -11.2, 6.0, 6.0, 6.0, -10.7 } },
        { 34, { -0.3, 1.0, 0.7, 0.7, -20.0, -4.7, 6.0, -9.7, 6.0, 6.0, 6.0, -0.2 } },
        { 35, { -3.8, 2.3, 3.7, -7.8, -5.7, 3.6, 1.6, -3.0, 6.0, 6.0, 5.2, -6.4 } },
        { 36, { -7.4, 0.8, 2.9, -7.9, 6.0, -1.2, -16.8, -0.1, 6.0, 6.0, 4.6, -1.8 } },
        { 37, { 6.0, 0.5, 5.9, -13.7, 1.9, 6.0, -10.2, 6.0, 6.0, 6.0, 0.8, 6.0 } },
        { 38, { 5.7, -2.7, -10.1, -8.0, 0.6, -14.4, -3.6, 3.6, 6.0, 6.0, -10.5, -4.8 } },
        { 39, { 2.2, -10.8, 0.8, 3.3, 6.0, -20.0, -10.0, 6.0, 6.0, 6.0, 6.0, -3.5 } },
        { 40, { 2.2, -15.2, -8.9, 0.6, 1.8, -0.7, -0.4, -2.8, 6.0, 6.0, 6.0, 4.4 } },
        { 41, { 2.3, -0.6, -9.6, 2.8, -3.7, -0.2, 6.0, 0.6, 6.0, 6.0, -11.9, 6.0 } },
        { 42, { -1.2, 0.6, 6.0, 0.8, 1.9, 6.0, 0.0, 0.6, 6.0, -1.9, -6.8, 6.0 } },
        { 43, { 1.7, -3.9, -12.7, 0.8, -9.1, -5.6, 4.8, -20.0, 6.0, -17.0, -1.3, 6.0 } },
        { 44, { 0.1, 5.1, -1.8, -13.0, -5.6, 6.0, -9.1, 6.0, 6.0, 6.0, -2.3, 5.8 } },
        { 45, { -3.5, 4.4, 0.3, -5.2, -4.5, -6.2, -15.4, -0.9, 6.0, 5.9, 2.8, 6.0 } },
        { 46, { -0.6, -1.8, 6.0, 4.3, 2.1, -0.8, -2.6, -1.4, 6.0, 5.7, 6.0, 6.0 } },
        { 47, { 0.7, -3.7, 4.4, 1.1, 3.1, -3.2, -2.9, 5.1, 6.0, 6.0, 6.0, 6.0 } },
        { 48, { 0.2, -3.0, -4.9, 5.2, 3.0, 0.3, -10.0, -10.7, 6.0, -3.1, 4.7, 1.4 } },
        { 49, { 2.9, 6.0, 0.7, 6.0, 1.4, 1.8, 2.9, 0.7, 6.0, -6.7, 6.0, -3.1 } },
        { 50, { 0.2, -7.2, -1.3, 6.0, 2.2, 5.0, 0.4, 2.4, 6.0, 6.0, 1.0, -3.0 } },
        { 51, { -3.5, 4.7, -1.4, -2.6, -6.9, -5.5, -4.0, -3.8, 6.0, 6.0, -0.3, 3.8 } },
        { 52, { -11.2, 0.5, -15.0, -4.9, -14.2, 4.9, 0.6, -17.6, 6.0, -6.2, -3.7, 0.7 } },
        { 53, { 3.1, 5.4, 4.1, -0.4, -7.6, -6.1, -4.9, 0.6, 6.0, 6.0, -1.0, 2.9 } },
        { 54, { 2.2, 4.2, 1.7, 2.4, 4.8, 3.6, -5.5, -0.9, 6.0, 5.9, 1.5, 5.3 } },
        { 55, { -2.7, -2.4, -7.5, -3.1, -5.4, 1.2, -2.8, 0.9, 6.0, 5.8, -1.3, 1.3 } },
        { 56, { 2.3, 1.5, 6.0, 5.8, -0.6, 0.3, -3.3, -9.1, 6.0, 6.0, -1.1, 6.0 } },
        { 57, { -2.0, 6.0, 3.1, -14.4, -6.6, -1.3, -9.0, 2.0, 6.0, 6.0, -2.6, -0.1 } },
        { 58, { -1.8, -2.1, -2.3, 6.0, -0.4, -2.7, 0.2, 6.0, 6.0, 4.9, -0.2, 2.0 } },
        { 59, { 3.0, 1.3, -5.3, 2.0, 4.2, -0.1, -0.8, 6.0, 6.0, -3.5, 4.8, 6.0 } },
        { 60, { -0.8, -7.7, -3.8, 0.8, 4.3, -2.5, 2.1, -1.2, -4.1, -0.8, -7.8, 2.9 } },
        { 61, { 3.2, 3.7, 0.9, -3.3, -9.1, -0.5, -0.5, -1.4, 6.0, -4.9, -1.9, 1.2 } },
        { 62, { -4.7, 2.7, 2.2, 2.6, 2.3, 1.9, 1.2, 2.0, 6.0, -1.7, -13.6, -3.0 } },
        { 63, { -1.1, 5.2, 5.0, 0.2, 1.5, 1.5, -11.3, -1.6, 6.0, -9.3, -16.2, -2.9 } },
        { 64, { -1.8, -1.3, 5.9, -7.7, 6.0, -0.1, -2.1, 0.2, 3.7, -10.9, -6.6, 5.3 } },
        { 65, { -1.3, 0.1, 5.6, -8.4, 0.1, -3.3, -8.6, -15.0, -2.5, -10.7, -6.4, -9.4 } },
        { 66, { 3.6, -4.2, -4.5, -1.8, -7.3, 6.0, 1.0, -7.3, -3.9, -4.8, -8.7, -2.3 } },
        { 67, { 6.0, -15.0, -13.2, -3.6, -0.9, -3.6, -0.2, -9.2, 3.0, -9.0, -7.4, -7.2 } },
        { 68, { 3.9, 2.3, 1.5, -2.4, 3.7, -0.5, 6.0, -5.8, 0.8, 1.9, -14.9, -10.5 } },
        { 69, { 4.2, 6.0, 6.0, -2.9, 6.0, 6.0, 6.0, 0.9, -2.2, -1.5, 6.0, -4.3 } },
        { 70, { -2.3, 6.0, -3.1, -0.2, -2.2, -8.1, 6.0, 4.8, 6.0, 6.0, 4.2, -1.2 } },
        { 71, { -11.4, 1.5, -5.1, 6.0, 5.2, 6.0, 6.0, -0.8, 2.0, 6.0, -6.1, -2.1 } },
        { 72, { 1.8, -6.5, -2.1, 4.5, -6.7, -3.6, -3.9, 1.3, 6.0, 6.0, -1.3, 0.2 } },
        { 73, { 1.0, 3.1, -2.5, -1.0, -4.0, 2.6, -0.6, -3.7, 6.0, 6.0, -3.0, -11.4 } },
        { 74, { 0.4, -1.3, -3.4, -0.5, 1.6, -4.8, 5.5, -0.6, 6.0, 1.8, 5.2, 4.8 } },
        { 75, { 6.0, -5.3, 1.2, -4.6, -5.5, -2.6, -5.5, -5.3, 5.6, 6.0, 3.9, 5.3 } },
        { 76, { -4.3, 1.6, -5.1, 6.0, -5.8, -4.5, -2.5, -4.6, 5.0, 6.0, 6.0, 6.0 } },
        { 77, { -6.0, 2.1, 1.6, -3.9, -2.8, -2.1, -7.7, -0.6, 6.0, 6.0, 4.5, 6.0 } },
        { 78, { 1.0, -0.8, -5.1, -1.8, -5.2, -1.3, 0.2, -1.2, 6.0, 6.0, 6.0, 0.0 } },
        { 79, { 1.5, -0.8, 3.7, -5.2, 4.3, -4.6, 0.0, 5.9, 6.0, 6.0, 6.0, 1.4 } },
        { 80, { 3.9, -7.5, -4.7, -0.0, -5.7, -0.5, 6.0, 2.5, -7.2, 6.0, 4.6, 0.0 } },
        { 81, { 1.4, 5.7, 4.8, 6.0, -4.5, -0.8, 1.9, 5.7, 6.0, 6.0, 0.0, 0.0 } },
        { 82, { 0.0, 6.0, 2.3, -9.2, -9.6, 3.3, -1.4, 6.0, 5.1, 6.0, 0.0, 0.0 } },
        { 83, { -8.1, -1.6, 6.0, 6.0, 6.0, 5.0, 2.5, -4.9, 6.0, 6.0, 0.0, 0.0 } },
        { 84, { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 } },
        { 85, { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 } },
        { 86, { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 } },
        { 87, { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 } },
        { 88, { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 } },
        { 89, { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 } },
        { 90, { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 } },
        { 91, { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 } },
        { 92, { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 } },
        { 93, { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 } },
        { 94, { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 } },
        { 95, { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 } },
        { 96, { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 } },
        { 97, { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 } },
        { 98, { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 } },
        { 99, { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 } },
        { 100, { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 } },
        { 101, { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 } },
        { 102, { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 } },
        { 103, { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 } },
        { 104, { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 } },
        { 105, { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 } },
        { 106, { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 } },
        { 107, { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 } },
        { 108, { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 } },
    };
    tiltByNote = { { 21, 10.0 }, { 27, -6.4 }, { 33, -3.1 }, { 39, 10.0 }, { 45, 10.0 }, { 51, 10.0 }, { 57, 10.0 }, { 63, 10.0 }, { 69, 10.0 }, { 75, 10.0 }, { 81, 10.0 }, { 87, -0.3 }, { 90, -0.6 }, { 93, -1.0 }, { 96, -0.7 }, { 99, -0.3 }, { 102, -0.1 }, { 105, 0.0 }, { 108, 0.0 } };
    // The knock of key and hammer on the body: broadband, the same for every note, measured in the top
    // octave where no string partial hides it.
    knockByFrequency = { { 30, 30.0 }, { 63, 26.0 }, { 125, 28.0 }, { 250, 16.0 }, { 500, 12.0 }, { 1000, 24.0 }, { 2000, 21.0 }, { 4000, 0.0 }, { 8000, -18.0 } };
    // It is strongest in the treble, where the hard hammers meet short strings close to the bridge.
    knockByNote = { { 21, 0.0 }, { 28, -2.0 }, { 34, -10.0 }, { 40, -12.0 }, { 50, -10.0 }, { 62, -6.0 }, { 72, -4.0 }, { 84, -2.0 }, { 96, 0.0 }, { 108, 0.0 } };

    // The thud of the blow itself: below a note's fundamental the string passes the hammer's push straight on
    // to the bridge, and the board's lowest modes answer. Fitted to the first 0.3 s below each note's fundamental.
    thumpByFrequency = { { 39, 18.4 }, { 61, 32.8 }, { 91, 15.7 }, { 133, 14.4 }, { 192, 7.2 }, { 275, 14.0 }, { 394, 18.5 } };
    thumpByNote = { { 40, -6.3 }, { 52, -10.9 }, { 64, -6.7 }, { 76, 0.0 }, { 88, 5.3 }, { 98, 3.6 }, { 108, 4.0 } };
    applyOverrides (*this);
}

namespace
{
    double byLogFrequency (const std::vector<Calibration::Point>& pts, double hz)
    {
        if (pts.empty()) return 0.0;
        if (hz <= pts.front().x) return pts.front().y;
        for (size_t i = 1; i < pts.size(); ++i)
            if (hz <= pts[i].x)
                return pts[i - 1].y + (pts[i].y - pts[i - 1].y) * std::log (hz / pts[i - 1].x) / std::log (pts[i].x / pts[i - 1].x);
        return pts.back().y;
    }
}

double Calibration::mobility (int midi, double hz) const
{
    return interpolate (mobilityByNote, midi) * byLogFrequency (mobilityByFrequency, hz);
}

double Calibration::evenness (int partial) const
{
    return interpolate (evennessByPartial, partial);
}

double Calibration::endLoss (int midi) const
{
    return interpolate (lossByNote, midi);
}

double Calibration::radiationLevel (double hz) const
{
    return std::pow (10.0, byLogFrequency (radiationByFrequency, hz) / 20.0);
}

double Calibration::knockLevel (double hz) const
{
    return std::pow (10.0, byLogFrequency (knockByFrequency, hz) / 20.0);
}

double Calibration::thumpLevel (double hz) const
{
    return std::pow (10.0, byLogFrequency (thumpByFrequency, hz) / 20.0);
}

double Calibration::thumpGain (int midi) const
{
    return std::pow (10.0, interpolate (thumpByNote, midi) / 20.0);
}

double Calibration::knockGain (int midi) const
{
    return std::pow (10.0, interpolate (knockByNote, midi) / 20.0);
}

double Calibration::radiation (int midi, double hz, int partial) const
{
    const int column = std::min (partial, 8) - 1;
    double trim = voicing.back().db[column];
    if (midi <= voicing.front().note)
        trim = voicing.front().db[column];
    else
        for (size_t i = 1; i < voicing.size(); ++i)
            if (midi <= voicing[i].note)
            {
                const double t = (midi - voicing[i - 1].note) / (voicing[i].note - voicing[i - 1].note);
                trim = voicing[i - 1].db[column] + t * (voicing[i].db[column] - voicing[i - 1].db[column]);
                break;
            }
    if (partial > 8)
        trim += interpolate (tiltByNote, midi) * std::log2 (std::min (partial, 20) / 8.0);     // measured up to about the 20th partial
    if (partial <= kFinePartials)
        for (const auto& f : fineVoicing)
            if (f.note == midi)
            {
                trim += f.db[partial - 1];
                break;
            }
    return radiationLevel (hz) * std::pow (10.0, trim / 20.0);
}

double hashUnit (uint32_t a, uint32_t b, uint32_t c)
{
    uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA77u ^ (c + 0x165667B1u) * 0xC2B2AE3Du;
    h ^= h >> 15; h *= 0x2C1B3C6Du;
    h ^= h >> 12; h *= 0x297A2D39u;
    h ^= h >> 15;
    return (double) h / 2147483647.5 - 1.0;
}

PianoDesign makeDesign (const Voicing& voicing)
{
    PianoDesign d;
    d.voicing = voicing;

    // Hammers: masses near those of a concert grand in the bass and middle, lighter at the top, and a
    // felt law F = K c^p with loss whose stiffness is fitted so that every partial grows with velocity
    // as measured on reference renders (tools/fit_hammer_final.py). Contact lasts about 3 ms soft and
    // 1.8 ms loud at middle C.
    static const Point hammerMass[] = { { 21, 10.0 }, { 28, 9.0 }, { 36, 7.5 }, { 48, 6.5 }, { 60, 6.0 }, { 72, 5.5 }, { 84, 4.5 }, { 93, 3.2 },
                                        { 100, 2.2 }, { 108, 1.2 } };
    static const Point hammerLogK[] = { { 21, 8.06 }, { 28, 8.09 }, { 33, 8.10 }, { 36, 8.02 }, { 40, 7.99 }, { 45, 7.97 }, { 48, 8.05 }, { 52, 8.15 },
                                        { 57, 8.34 }, { 60, 8.34 }, { 64, 8.75 }, { 69, 9.03 }, { 72, 9.29 }, { 76, 9.72 }, { 81, 9.98 }, { 84, 10.42 },
                                        { 88, 10.93 }, { 93, 11.6 }, { 96, 12.4 }, { 100, 13.0 }, { 105, 13.2 }, { 108, 13.4 } };
    static const Point hammerP[] = { { 21, 2.1 }, { 60, 2.3 }, { 108, 3.0 } };
    static const Point strike[] = { { 21, 0.122 }, { 30, 0.122 }, { 36, 0.113 }, { 52, 0.113 }, { 60, 0.118 }, { 69, 0.108 }, { 84, 0.098 }, { 108, 0.088 } };
    static const Point damper[] = { { 21, 0.30 }, { 40, 0.17 }, { 60, 0.09 }, { 88, 0.06 } };

    for (int i = 0; i < kNumNotes; ++i)
    {
        auto& n = d.notes[(size_t) i];
        const int midi = i + kLowestMidi;
        const auto id = (uint32_t) midi;
        n.midi = midi;
        n.strings = stringCount (midi);
        n.wound = midi < kFirstPlainMidi;
        n.hasDamper = midi <= 88;
        n.region = std::min (kNumRegions - 1, i * kNumRegions / kNumNotes);

        const double nominal = voicing.a4 * std::pow (2.0, (midi - 69) / 12.0);
        n.length = speakingLength (midi);
        n.coreDiameter = coreDiameter (midi);
        const double coreMu = kSteelDensity * kPi * n.coreDiameter * n.coreDiameter / 4.0;
        const double waveSpeed = 2.0 * n.length * nominal;

        if (n.wound)
        {
            n.tension = woundTension (midi);
            n.mu = n.tension / (waveSpeed * waveSpeed);
        }
        else
        {
            n.mu = coreMu;
            n.tension = n.mu * waveSpeed * waveSpeed;
        }

        // Stiff string inharmonicity from the core. The windings add a little stiffness, and the
        // clamped ends of the short treble strings raise the measured value.
        const double d4 = std::pow (n.coreDiameter, 4.0);
        double B = kPi * kPi * kPi * kSteelYoung * d4 / (64.0 * n.tension * n.length * n.length);
        B *= n.wound ? 1.3 : 1.0 + 0.45 * std::fmin (1.0, std::fmax (0.0, (midi - 60) / 36.0));
        n.inharmonicity = B;

        // Unison: a small spread around the note's pitch, with no net offset.
        const auto& cal = d.calibration;
        const double spreadCents = voicing.unison * interpolate (cal.unisonByNote, midi);
        // The strings sit either side of the note's pitch: one low, one high, the third anywhere between.
        static const double pattern[kMaxStrings] = { -1.0, 1.0, 0.0 };
        double mean = 0.0;
        for (int s = 0; s < n.strings; ++s)
        {
            n.detuneCents[s] = spreadCents * (pattern[s] * (0.6 + 0.4 * std::abs (hashUnit (voicing.seed, id, 10u + (uint32_t) s)))
                                              + (s == 2 ? 0.8 * hashUnit (voicing.seed, id, 12u) : 0.0));
            mean += n.detuneCents[s] / n.strings;
        }
        for (int s = 0; s < n.strings; ++s)
        {
            n.detuneCents[s] = n.strings > 1 ? n.detuneCents[s] - mean : 0.0;
            n.inharmonicityScale[s] = 1.0 + cal.inharmonicityMismatch * hashUnit (voicing.seed, id, 20u + (uint32_t) s);
            n.horizontalCents[s] = cal.horizontalCents * hashUnit (voicing.seed, id, 30u + (uint32_t) s);
            n.strikeGain[s] = 1.0 + cal.strikeUnevenness * hashUnit (voicing.seed, id, 40u + (uint32_t) s);
            const double side = hashUnit (voicing.seed, id, 50u + (uint32_t) s);
            n.horizontalGain[s] = cal.horizontalBlow * (side < 0.0 ? -1.0 : 1.0) * (0.5 + 0.5 * std::abs (side));
            n.strikeOffset[s] = cal.strikeSpread * hashUnit (voicing.seed, id, 70u + (uint32_t) s);
        }

        n.strikeRatio = interpolate (strike, midi);
        n.hammerWidth = 0.008;
        n.hammerMass = 1.0e-3 * interpolate (hammerMass, midi);
        n.hammerK = std::pow (10.0, interpolate (hammerLogK, midi));
        n.hammerP = interpolate (hammerP, midi);
        n.hammerHysteresis = 0.1;

        // Bridge positions on a 1.40 m by 1.90 m board, x from the bass side, y from the keyboard.
        if (n.wound)
        {
            const double s = (midi - 21) / 19.0;
            n.bridgeX = 0.55 + 0.40 * s;
            n.bridgeY = 1.72 - 0.22 * s;
        }
        else
        {
            const double t = (midi - kFirstPlainMidi) / 67.0;
            n.bridgeX = 1.28 - 1.0 * std::pow (1.0 - t, 1.3);
            n.bridgeY = 0.07 + 0.72 * n.length;
        }

        n.bridgeTilt = d.calibration.bridgeTilt * (1.0 + 0.4 * hashUnit (voicing.seed, id, 60u));
        n.damperTime = interpolate (damper, midi);
    }

    tune (d);
    return d;
}

} // namespace pm
