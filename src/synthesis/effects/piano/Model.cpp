#include "Model.h"

#include "Hammer.h"
#include "Soundboard.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>

namespace pm
{

void Bank::resize (int realCount)
{
    used = realCount;
    count = (realCount + 3) & ~3;
    const auto n = (size_t) count;
    for (auto* v : { &poleRe, &poleIm, &subRe, &subIm, &damping, &hammerRe, &hammerIm,
                     &outRe[0], &outIm[0], &outRe[1], &outIm[1], &busRe, &busIm })
        v->assign (n, 0.0f);
}

namespace
{
    constexpr int kMaxOsc = 2 * kMaxStrings;
    constexpr double kMaxPartialHz = 18000.0;
    constexpr double kThumpBusTrimHz = 75.0;     // the board's modes below this reach the sympathetic strings without the thud's fitted gain
    constexpr double kBoardMaxHz = 10000.0;
    constexpr double kBoardBusMaxHz = 4000.0;
    constexpr double kSympatheticMaxHz = 6000.0;
    constexpr int kSympatheticMaxPartials = 40;
    constexpr double kImageDepth = 1.0;      // how much of the two microphones' level difference each partial keeps
    // The player's view: bass to the left, treble to the right. The level difference between left and right
    // at the ends of the keyboard, and how much later the far ear hears a note struck there.
    constexpr double kPlaceDb = 8.0;
    constexpr double kPlaceDelay = 0.00025;  // s
    // Above these frequencies a phase difference between left and right is no longer a cue to a place, and a partial
    // near inversion between the two is heard as a hollow, phasy tone: the delay's phase stops growing, and the
    // whole difference is held within an angle that keeps the width but not the inversion. (Holding it at every
    // frequency removed an anti-phase band at 150 to 600 Hz but was heard as worse.)
    constexpr double kPlaceDelayMaxHz = 500.0;
    constexpr double kMicPhaseMaxHz = 800.0;
    constexpr double kMicPhaseLimit = 70.0 * kPi / 180.0;
    constexpr double kPruneDb = 100.0;       // resonators this far below the note's loudest at full force are left out

    //==============================================================================
    struct Matrix { cplx a[kMaxOsc][kMaxOsc] {}; };

    /** Solves A x = b in place by elimination with partial pivoting; tiny pivots are replaced. */
    void solve (int n, Matrix A, cplx* b)
    {
        for (int col = 0; col < n; ++col)
        {
            int pivot = col;
            for (int r = col + 1; r < n; ++r)
                if (std::abs (A.a[r][col]) > std::abs (A.a[pivot][col]))
                    pivot = r;
            if (pivot != col)
            {
                for (int c = 0; c < n; ++c) std::swap (A.a[col][c], A.a[pivot][c]);
                std::swap (b[col], b[pivot]);
            }
            if (std::abs (A.a[col][col]) < 1.0e-13)
                A.a[col][col] = 1.0e-13;
            for (int r = col + 1; r < n; ++r)
            {
                const cplx f = A.a[r][col] / A.a[col][col];
                for (int c = col; c < n; ++c) A.a[r][c] -= f * A.a[col][c];
                b[r] -= f * b[col];
            }
        }
        for (int r = n - 1; r >= 0; --r)
        {
            for (int c = r + 1; c < n; ++c) b[r] -= A.a[r][c] * b[c];
            b[r] /= A.a[r][r];
        }
    }

    /** Eigenvalues and right eigenvectors (columns of V) of a small complex matrix. */
    void eigen (int n, const Matrix& A, cplx* values, Matrix& V)
    {
        // Characteristic polynomial by Faddeev-LeVerrier, roots by Durand-Kerner.
        cplx coeff[kMaxOsc + 1];
        coeff[n] = 1.0;
        Matrix M;
        for (int i = 0; i < n; ++i) M.a[i][i] = 1.0;
        for (int k = 1; k <= n; ++k)
        {
            Matrix AM;
            cplx trace = 0.0;
            for (int i = 0; i < n; ++i)
                for (int j = 0; j < n; ++j)
                {
                    cplx s = 0.0;
                    for (int l = 0; l < n; ++l) s += A.a[i][l] * M.a[l][j];
                    AM.a[i][j] = s;
                    if (i == j) trace += s;
                }
            coeff[n - k] = -trace / (double) k;
            M = AM;
            for (int i = 0; i < n; ++i) M.a[i][i] += coeff[n - k];
        }

        // Root scale: the largest |coefficient|^(1/degree), which bounds the roots within a factor of two.
        double radius = 1.0e-9;
        for (int i = 0; i < n; ++i) radius = std::max (radius, std::pow (std::abs (coeff[i]), 1.0 / (n - i)));
        for (int i = 0; i < n; ++i)
            values[i] = std::polar (radius, 0.4 + 2.0 * kPi * i / n);

        for (int iter = 0; iter < 500; ++iter)
        {
            double change = 0.0;
            for (int i = 0; i < n; ++i)
            {
                cplx p = coeff[n];
                for (int k = n - 1; k >= 0; --k) p = p * values[i] + coeff[k];
                cplx q = 1.0;
                for (int j = 0; j < n; ++j)
                    if (j != i) q *= values[i] - values[j];
                if (std::abs (q) < 1.0e-300) q = 1.0e-300;
                const cplx step = p / q;
                values[i] -= step;
                change = std::max (change, std::abs (step));
            }
            if (change < 1.0e-15 * radius)
                break;
        }

        // Eigenvectors by inverse iteration.
        for (int e = 0; e < n; ++e)
        {
            Matrix shifted = A;
            for (int i = 0; i < n; ++i) shifted.a[i][i] -= values[e];
            cplx v[kMaxOsc];
            for (int i = 0; i < n; ++i) v[i] = cplx (1.0 + 0.37 * i, 0.21 * (i + 1) * ((e & 1) ? -1.0 : 1.0));
            for (int iter = 0; iter < 3; ++iter)
            {
                solve (n, shifted, v);
                double norm = 0.0;
                for (int i = 0; i < n; ++i) norm += std::norm (v[i]);
                norm = std::sqrt (norm);
                for (int i = 0; i < n; ++i) v[i] /= norm;
            }
            for (int i = 0; i < n; ++i) V.a[i][e] = v[i];
        }
    }

    //==============================================================================
    struct Resonator
    {
        cplx pole;              // rad/s
        cplx hammer;            // string displacement under the hammer per unit state
        cplx bridge;            // force on the bridge per unit state
        cplx out[2];
        cplx bus;
        double damperRate = 0.0;
        int partial = 0;
        double life = 0.0;      // seconds above the pruning threshold after the hardest blow
        cplx modal;             // partial number times the strings' mean modal displacement per unit state
        bool strongest = false; // carries most of its partial's energy
        bool onset = false;     // the short-lived partner that shapes a partial's start
    };

    /** How long the soundboard takes to answer a partial, as a time constant in seconds.

        The board cannot follow a string at once: its own modes ring first and die away, and the
        partial reaches a listener only as they do. Low partials take tens of milliseconds, high
        ones a few. Without this every partial would start abruptly, which is heard as a click.
    */
    double boardResponseTime (double hz)
    {
        const double t = hz < 400.0 ? 0.0045 * 400.0 / hz : 0.0045 * std::pow (400.0 / hz, 0.25);
        return std::clamp (t, 0.0025, 0.012);
    }

    /** Loss inside the string for one partial, 1/s: air drag plus internal friction. The friction has a part that
        grows with the partial's stiffness term and a part in proportion to frequency alone; without the latter the
        slow unison modes of the upper partials had almost no loss and rang on at 7 dB/s where a recorded piano's
        fall at 40 to 70 dB/s. */
    double stringLoss (const Calibration& c, const NoteDesign& n, double hz, double stiffnessTerm)
    {
        const double airViscosity = 1.8e-5, airDensity = 1.2;
        const double coreMu = 7850.0 * kPi * n.coreDiameter * n.coreDiameter / 4.0;
        const double outer = n.coreDiameter * std::sqrt (n.mu / coreMu);
        const double air = (2.0 * kPi * airViscosity + 2.0 * kPi * outer * std::sqrt (kPi * airViscosity * airDensity * hz)) / (2.0 * n.mu);
        const double friction = kPi * hz * (n.wound ? c.frictionWound : c.frictionPlain) * (stiffnessTerm + c.frictionFloor);
        return c.endLoss (n.midi) + c.airScale * air + friction;
    }

    //==============================================================================
    /** The board's response seen from one note, averaged over half an octave either side.

        The response from one bridge point to one microphone swings by 10 dB and more between
        neighbouring partials. A listener in a room hears an average over many paths, so the model
        keeps the pattern of those swings, at a reduced depth, around a calibrated average level.
    */
    struct LocalAverage
    {
        static constexpr int perOctave = 24, size = 10 * perOctave + 1;
        static constexpr double lowest = 20.0;
        std::vector<double> mobility, transfer[2];

        static double frequency (int g) { return lowest * std::pow (2.0, (double) g / perOctave); }

        static double at (const std::vector<double>& table, double hz)
        {
            const double pos = std::clamp (std::log2 (hz / lowest) * perOctave, 0.0, (double) size - 1.001);
            const auto i = (size_t) pos;
            return table[i] + (table[i + 1] - table[i]) * (pos - (double) i);
        }

        /** The value's phase, and its swing around the average at the reduced depth: about 1 in size. */
        static cplx pattern (cplx value, const std::vector<double>& table, double hz, double even)
        {
            const double magnitude = std::abs (value), average = at (table, hz);
            if (magnitude <= 0.0 || average <= 0.0)
                return 1.0;
            return value / magnitude * std::pow (magnitude / average, 1.0 - even);
        }

        /** The same for a mobility, whose phase lies between -90 and 90 degrees. Half of the phase is
            kept whatever the evenness: the bridge then always takes energy from the string, and is
            springy or heavy enough for the second stage of the decay to join the first smoothly in
            most notes instead of cancelling against it. */
        static cplx mobilityPattern (cplx value, const std::vector<double>& table, double hz, double even)
        {
            const double magnitude = std::abs (value), average = at (table, hz);
            if (magnitude <= 0.0 || average <= 0.0)
                return 1.0;
            return std::polar (std::pow (magnitude / average, 1.0 - even), 0.5 * std::arg (value));
        }
    };

    LocalAverage makeLocalAverage (const Soundboard& sb, int note)
    {
        LocalAverage a;
        const auto n = (size_t) LocalAverage::size;
        std::vector<double> y (n), h0 (n), h1 (n);
        for (int g = 0; g < LocalAverage::size; ++g)
        {
            cplx mobility, t0, t1;
            sb.evaluate (note, cplx (0.0, 2.0 * kPi * LocalAverage::frequency (g)), mobility, t0, t1);
            y[(size_t) g] = std::norm (mobility); h0[(size_t) g] = std::norm (t0); h1[(size_t) g] = std::norm (t1);
        }
        auto smooth = [] (const std::vector<double>& power)
        {
            std::vector<double> out (power.size());
            const int half = LocalAverage::perOctave / 2;
            for (int g = 0; g < LocalAverage::size; ++g)
            {
                double sum = 0.0;
                int count = 0;
                for (int j = std::max (0, g - half); j <= std::min (LocalAverage::size - 1, g + half); ++j, ++count)
                    sum += power[(size_t) j];
                out[(size_t) g] = std::sqrt (sum / count);
            }
            return out;
        };
        a.mobility = smooth (y);
        a.transfer[0] = smooth (h0);
        a.transfer[1] = smooth (h1);
        return a;
    }

    //==============================================================================
    struct Strike
    {
        std::vector<cplx> z;        // resonator states when the hammer has left
        double contact = 0.0;       // seconds
        double peakForce = 0.0;     // N per string
        double stiffness = 0.0;     // largest felt stiffness times the give per step: below about 0.2 is stable
    };

    /** One hammer blow on a silent note. */
    Strike strike (const std::vector<Resonator>& main, const NoteDesign& n, double velocity, double hardness, int steps, double Ts)
    {
        const auto count = main.size();
        const double dt = Ts / steps;
        std::vector<cplx> step (count);
        Strike s;
        s.z.assign (count, 0.0);

        double stringsHit = 0.0;
        for (int i = 0; i < n.strings; ++i) stringsHit += n.strikeGain[i];
        double give = dt * dt * stringsHit / n.hammerMass;      // how far hammer and string part per unit force in one step
        for (size_t r = 0; r < count; ++r)
        {
            step[r] = std::exp (main[r].pole * dt);
            give += dt * (main[r].hammer * step[r]).real();
        }

        double position = 0.0, speed = hammerVelocity (velocity), lastCompression = 0.0;
        const int maxSteps = (int) (0.05 / dt);
        for (int i = 0; i < maxSteps; ++i)
        {
            double string = 0.0;
            for (size_t r = 0; r < count; ++r) string += (main[r].hammer * s.z[r]).real();
            const double compression = position - string;
            if (compression < -kHammerClear)
                break;
            const double force = feltForce (compression, (compression - lastCompression) / dt, hardness * n.hammerK, n.hammerP, n.hammerHysteresis);
            lastCompression = compression;
            speed -= stringsHit * force / n.hammerMass * dt;
            position += speed * dt;
            for (size_t r = 0; r < count; ++r) s.z[r] = step[r] * (s.z[r] + dt * force);
            if (force > 0.0)
            {
                s.contact = dt * i;
                s.stiffness = std::max (s.stiffness, n.hammerP * force / compression * give);
            }
            s.peakForce = std::max (s.peakForce, force);
        }
        return s;
    }

    void appendToFile (const char* path, const std::string& text)
    {
        static std::mutex lock;
        const std::lock_guard<std::mutex> guard (lock);
        if (FILE* f = std::fopen (path, "a")) { std::fputs (text.c_str(), f); std::fclose (f); }
    }

    //==============================================================================
    void buildNote (const PianoDesign& design, const Soundboard& sb, const LocalAverage& average, int index, PianoModel& model)
    {
        const auto& n = design.notes[(size_t) index];
        const auto& cal = design.calibration;
        auto& out = model.notes[index];
        const double fs = design.voicing.sampleRate, Ts = 1.0 / fs;
        const int S = n.strings, size = 2 * S;
        const double modalMass = 0.5 * n.mu * n.length;
        const double kappa = n.tension / n.length;
        const double theta = n.bridgeTilt;
        const double maxHz = std::min (0.45 * fs, kMaxPartialHz);

        // Damper: felt resting on the string between 13% and 21% of its length from the agraffe.
        const double damperA = 0.13, damperB = 0.21;
        auto damperCover = [&] (int k)
        {
            const double a = 2.0 * k * kPi;
            return 0.5 - (std::sin (a * damperB) - std::sin (a * damperA)) / (2.0 * a * (damperB - damperA));
        };

        std::vector<Resonator> main, sympathetic;
        const char* decayDump = std::getenv ("PIANO_DUMP");     // calibration: every partial's decay
        std::string decayText;

        const double place = (n.midi - 64.5) / 43.5;                          // -1 at A0, +1 at C8
        const double placeRatio = std::pow (10.0, -kPlaceDb * place / 20.0);   // left over right
        const double placeSide = std::sqrt (2.0 / (1.0 + placeRatio * placeRatio));
        out.longOut[0] = (float) (placeSide * placeRatio);
        out.longOut[1] = (float) placeSide;

        for (int k = 1;; ++k)
        {
            const double hz = n.f0 * partialRatio (k, n.inharmonicity);
            if (hz > maxHz)
                break;
            const double omega = 2.0 * kPi * hz;

            // Uncoupled oscillators: each string's vertical motion, then each string's horizontal motion.
            Matrix M;
            for (int s = 0; s < S; ++s)
            {
                const double f = n.f0 * std::pow (2.0, n.detuneCents[s] / 1200.0) * partialRatio (k, n.inharmonicity * n.inharmonicityScale[s]);
                const double loss = design.voicing.stringLoss * stringLoss (cal, n, hz, n.inharmonicity * k * k);
                M.a[s][s] = cplx (-loss, 2.0 * kPi * f - omega);
                M.a[S + s][S + s] = cplx (-loss, 2.0 * kPi * f * std::pow (2.0, n.horizontalCents[s] / 1200.0) - omega);
            }

            // The bridge: a calibrated average mobility and radiation, with the board's own pattern on top.
            cplx plateMobility, H[2];
            sb.evaluate (index, cplx (0.0, omega), plateMobility, H[0], H[1]);
            const double meanMobility = cal.mobility (n.midi, hz) / design.voicing.sustain;
            const cplx Ys = meanMobility * LocalAverage::mobilityPattern (plateMobility, average.mobility, hz, cal.evenness (k));
            const double radiation = cal.radiation (n.midi, hz, k);
            // Stereo image. The partial's loudness follows the evened response common to both
            // microphones; its place between left and right is the note's place on the keyboard, moved
            // by the level difference the two microphones really have at this partial, applied as a
            // constant-power pan so that the image moves without the loudness changing. Each side keeps
            // its own microphone's phase, and the far side is delayed as it is at a player's ears.
            {
                const double magL = std::abs (H[0]), magR = std::abs (H[1]);
                const double avgL = LocalAverage::at (average.transfer[0], hz), avgR = LocalAverage::at (average.transfer[1], hz);
                double common = 1.0, ratio = placeRatio, mean = 0.0, difference = 0.0;     // phase of left over right
                if (magL > 0.0 && magR > 0.0 && avgL > 0.0 && avgR > 0.0)
                {
                    common = std::pow (std::sqrt (magL * magR / (avgL * avgR)), 1.0 - cal.evenness (k));
                    ratio *= std::pow ((magL / avgL) / (magR / avgR), kImageDepth);     // left over right
                    const cplx unitL = H[0] / magL, unitR = H[1] / magR;
                    mean = std::arg (unitL + unitR);
                    difference = std::arg (unitL * std::conj (unitR));
                }
                // the far side lags by the delay; above 800 Hz the whole difference is held within the limit
                difference += (place < 0.0 ? 1.0 : -1.0) * 2.0 * kPi * std::min (hz, kPlaceDelayMaxHz) * kPlaceDelay * std::abs (place);
                if (hz > kMicPhaseMaxHz)
                    difference = std::clamp (difference, -kMicPhaseLimit, kMicPhaseLimit);
                const double side = std::sqrt (2.0 / (1.0 + ratio * ratio));
                const cplx phase[2] = { std::polar (1.0, mean + 0.5 * difference), std::polar (1.0, mean - 0.5 * difference) };
                H[0] = phase[0] * radiation * common * side * ratio;
                H[1] = phase[1] * radiation * common * side;
            }

            // Coupling through the bridge, which moves along a direction tilted from the vertical.
            const double horizontalMobility = 0.2 * meanMobility;
            for (int i = 0; i < size; ++i)
                for (int j = 0; j < size; ++j)
                {
                    const bool hi = i >= S, hj = j >= S;
                    cplx y = Ys * (hi ? theta : 1.0) * (hj ? theta : 1.0);
                    if (hi && hj) y += horizontalMobility;
                    M.a[i][j] -= kappa * y;
                }

            cplx values[kMaxOsc];
            Matrix V;
            eigen (size, M, values, V);

            // Each string is met at a slightly different point, and the felt spreads the blow over its width.
            const double widthArg = 0.5 * k * kPi * n.hammerWidth / n.length;
            const double spread = widthArg > 1.0e-9 ? std::sin (widthArg) / widthArg : 1.0;
            double strikeWeight[kMaxStrings];
            cplx blow[kMaxOsc], drive[kMaxOsc];
            for (int s = 0; s < S; ++s)
            {
                strikeWeight[s] = spread * std::sin (k * kPi * n.strikeRatio * (1.0 + n.strikeOffset[s]));
                blow[s] = n.strikeGain[s] * strikeWeight[s]; blow[S + s] = n.horizontalGain[s] * strikeWeight[s];
                drive[s] = 1.0; drive[S + s] = theta;
            }
            solve (size, V, blow);      // the hammer's blow, and the bridge's motion, in terms of the coupled modes
            solve (size, V, drive);

            const double bridgeForce = ((k & 1) ? 1.0 : -1.0) * k * kPi * n.tension / n.length;   // per unit modal displacement
            const cplx minusJ (0.0, -1.0);
            const double damperRate = damperCover (k) / (damperCover (1) * n.damperTime);

            int best = 0;
            double bestWeight = -1.0, strongestWeight = -1.0;
            size_t strongest = 0;
            cplx bridgeRead[kMaxOsc];
            const size_t first = main.size();

            for (int r = 0; r < size; ++r)
            {
                cplx hammerRead = 0.0, meanRead = 0.0;
                bridgeRead[r] = 0.0;
                for (int s = 0; s < S; ++s)
                {
                    hammerRead += strikeWeight[s] * V.a[s][r] / (double) S;
                    meanRead += V.a[s][r] / (double) S;
                    bridgeRead[r] += V.a[s][r] + theta * V.a[S + s][r];
                }

                Resonator res;
                // The coupled system only loses energy; keep that true whatever the rounding.
                res.pole = cplx (std::min (values[r].real(), -0.02), omega + values[r].imag());
                res.partial = k;
                res.hammer = minusJ * hammerRead * blow[r] / (modalMass * omega);
                res.bridge = minusJ * bridgeForce * bridgeRead[r] * blow[r] / (modalMass * omega);
                res.out[0] = H[0] * res.bridge;
                res.out[1] = H[1] * res.bridge;
                res.bus = Ys * res.bridge / res.pole;
                res.damperRate = damperRate;
                res.modal = minusJ * (double) k * meanRead * blow[r] / (modalMass * omega);
                main.push_back (res);

                const double weight = std::abs (bridgeRead[r] * drive[r]);
                if (weight > bestWeight) { bestWeight = weight; best = r; }
                if (std::abs (res.bridge) > strongestWeight) { strongestWeight = std::abs (res.bridge); strongest = main.size() - 1; }
            }
            main[strongest].strongest = true;

            // Each resonator gets a partner with the same frequency and the opposite output that dies
            // within the board's response time: together they start from nothing and swell to the tone.
            const double settle = 1.0 / boardResponseTime (hz);
            for (size_t r = first, last = main.size(); r < last; ++r)
            {
                Resonator partner;
                partner.pole = main[r].pole - settle;
                partner.partial = k;
                partner.out[0] = -main[r].out[0];
                partner.out[1] = -main[r].out[1];
                partner.damperRate = damperRate;
                partner.onset = true;
                main.push_back (partner);
            }

            if (const char* watch = std::getenv ("PIANO_DEBUG_NOTE"); watch != nullptr && std::atoi (watch) == n.midi && k <= 2)
                for (size_t r = first; r < main.size(); ++r)
                    std::printf ("note %d partial %d: decay %7.2f dB/s, offset %+7.3f Hz, bridge weight %6.1f dB, hammer weight %6.1f dB\n", n.midi, k,
                                 -8.686 * main[r].pole.real(), (main[r].pole.imag() - omega) / (2.0 * kPi),
                                 20.0 * std::log10 (std::abs (main[r].bridge) / std::abs (main[first].bridge) + 1.0e-12),
                                 20.0 * std::log10 (std::abs (main[r].hammer) / std::abs (main[first].hammer) + 1.0e-12));

            if (decayDump != nullptr)
            {
                // power-weighted decay of the partial's resonators: the slope its level starts with
                double power = 0.0, weighted = 0.0, slowest = 1.0e9;
                for (size_t r = first; r < main.size(); ++r)
                {
                    const double w = std::norm (main[r].bridge);      // zero for the onset partners
                    power += w; weighted += -w * main[r].pole.real();
                }
                for (size_t r = first; r < main.size(); ++r)
                    if (std::norm (main[r].bridge) > 1.0e-4 * power)
                        slowest = std::min (slowest, -main[r].pole.real());
                char text[160];
                std::snprintf (text, sizeof (text), "%d %d %.3f %.5f %.5f\n", n.midi, k, hz, 8.686 * weighted / power, 8.686 * slowest);
                decayText += text;
            }

            if (hz < kSympatheticMaxHz && k <= kSympatheticMaxPartials)
            {
                // A string set ringing by the bridge decays as one string does, more slowly than the
                // three struck together.
                Resonator res;
                const double loss = design.voicing.stringLoss * stringLoss (cal, n, hz, n.inharmonicity * k * k);
                res.pole = cplx (-(loss + kappa * Ys.real()), omega + values[best].imag());
                res.partial = k;
                const cplx force = minusJ * bridgeForce * bridgeForce * bridgeRead[best] * drive[best] / (modalMass * omega);
                res.out[0] = H[0] * force;
                res.out[1] = H[1] * force;
                res.damperRate = damperRate;
                sympathetic.push_back (res);
            }
        }

        if (decayDump != nullptr)
            appendToFile (decayDump, decayText);

        // Strike the note as hard as it can be struck, with felt twice as hard as designed: this finds how
        // finely the hammer has to be stepped for the stiff felt to stay stable, and which resonators ever matter.
        int steps = 4;
        Strike hardest = strike (main, n, 127.0, 2.0, steps, Ts);
        while (steps < 64 && ! (std::isfinite (hardest.peakForce) && hardest.peakForce < 1.0e5 && hardest.stiffness < 0.2))
        {
            steps *= 2;
            hardest = strike (main, n, 127.0, 2.0, steps, Ts);
        }
        out.substeps = steps;

        if (std::getenv ("PIANO_DEBUG") != nullptr)
            std::printf ("note %d: %zu resonators, %d steps, contact %.2f ms, peak force %.1f N per string, stiffness number %.3f\n",
                         n.midi, main.size(), steps, 1000.0 * hardest.contact, hardest.peakForce, hardest.stiffness);

        if (const char* modeDump = std::getenv ("PIANO_DUMP_MODES"))
        {
            // calibration: every resonator of the first partials after a mezzo forte blow, to trace how each partial decays
            const Strike s = strike (main, n, 80.0, 1.0, steps, Ts);
            std::string text;
            for (size_t r = 0; r < main.size() && main[r].partial <= 14; ++r)
            {
                if (main[r].onset)
                    continue;
                const cplx a = 0.5 * (main[r].out[0] + main[r].out[1]) * s.z[r];
                char line[160];
                std::snprintf (line, sizeof (line), "%d %d %.6f %.6f %.6e %.6e\n", n.midi, main[r].partial, -main[r].pole.real(),
                               main[r].pole.imag() / (2.0 * kPi), a.real(), a.imag());
                text += line;
            }
            appendToFile (modeDump, text);
        }

        if (const char* levelDump = std::getenv ("PIANO_DUMP_LEVELS"))
        {
            // calibration: the level each partial starts with at one velocity, as the mono sum of both outputs
            const char* v = std::getenv ("PIANO_DUMP_VELOCITY");
            const Strike s = strike (main, n, v != nullptr ? std::atof (v) : 80.0, 1.0, steps, Ts);
            std::string text;
            for (size_t r = 0; r < main.size();)
            {
                const int k = main[r].partial;
                cplx sum = 0.0;
                for (; r < main.size() && main[r].partial == k; ++r)
                    if (! main[r].onset)
                        sum += 0.5 * (main[r].out[0] + main[r].out[1]) * s.z[r];
                char line[120];
                std::snprintf (line, sizeof (line), "%d %d %.3f %.3f\n", n.midi, k, main[r - 1].pole.imag() / (2.0 * kPi),
                               20.0 * std::log10 (std::max (std::abs (sum) * cal.outputGain, 1.0e-30)));
                text += line;
            }
            appendToFile (levelDump, text);
        }

        {
            const auto count = main.size();
            double loudest = 0.0;
            std::vector<double> level (count);
            for (size_t r = 0; r < count; ++r)
            {
                level[r] = std::abs (hardest.z[r]) * std::max (std::abs (main[r].out[0]), std::abs (main[r].out[1]));
                loudest = std::max (loudest, level[r]);
            }
            const double threshold = loudest * std::pow (10.0, -kPruneDb / 20.0);
            std::vector<Resonator> kept;
            for (size_t r = 0; r < count; ++r)
                if (level[r] > threshold)
                {
                    main[r].life = std::log (level[r] / threshold) / std::max (1.0e-3, -main[r].pole.real());
                    kept.push_back (main[r]);
                }
            std::sort (kept.begin(), kept.end(), [] (const Resonator& a, const Resonator& b) { return a.life > b.life; });
            main.swap (kept);
        }

        auto fill = [&] (Bank& bank, const std::vector<Resonator>& list)
        {
            bank.resize ((int) list.size());
            for (size_t r = 0; r < list.size(); ++r)
            {
                const auto& res = list[r];
                const cplx p = std::exp (res.pole * Ts), sub = std::exp (res.pole * (Ts / out.substeps));
                bank.poleRe[r] = (float) p.real(); bank.poleIm[r] = (float) p.imag();
                bank.subRe[r] = (float) sub.real(); bank.subIm[r] = (float) sub.imag();
                bank.damping[r] = (float) (1.0 - std::exp (-res.damperRate * Ts));
                bank.hammerRe[r] = (float) res.hammer.real(); bank.hammerIm[r] = (float) res.hammer.imag();
                for (int mic = 0; mic < 2; ++mic)
                {
                    bank.outRe[mic][r] = (float) res.out[mic].real(); bank.outIm[mic][r] = (float) res.out[mic].imag();
                }
                bank.busRe[r] = (float) res.bus.real(); bank.busIm[r] = (float) res.bus.imag();
            }
        };
        fill (out.main, main);
        fill (out.sympathetic, sympathetic);

        // Longitudinal vibration, for the strings long enough for it to be heard.
        constexpr int longMaxMidi = 72;
        out.longIndex.clear(); out.longRe.clear(); out.longIm.clear();
        out.longGain = 0.0f;
        if (n.midi <= longMaxMidi && cal.longitudinalCoupling > 0.0)
        {
            const double young = 2.02e11, area = kPi * n.coreDiameter * n.coreDiameter / 4.0;
            const double tensionPerSquare = kPi * kPi * young * area / (4.0 * n.length * n.length);
            const double longHz = std::sqrt (young * area / n.mu) / (2.0 * n.length);
            out.longGain = (float) (S * tensionPerSquare * cal.longitudinalCoupling * cal.radiation (n.midi, 1000.0, 2));
            for (int mode = 0; mode < 2; ++mode)
                out.longHz[mode] = (mode + 1) * longHz < 0.4 * fs ? (float) ((mode + 1) * longHz) : 0.0f;

            // one entry per partial in order, pointing at that partial's strongest resonator in the sorted bank
            for (int k = 1; k <= kLongPartials; ++k)
            {
                int found = -1;
                for (size_t r = 0; r < main.size(); ++r)
                    if (main[r].partial == k && main[r].strongest) { found = (int) r; break; }
                if (found < 0 || main[(size_t) found].pole.imag() > 2.0 * kPi * 0.25 * fs)
                    break;
                out.longIndex.push_back (found);
                out.longRe.push_back ((float) main[(size_t) found].modal.real());
                out.longIm.push_back ((float) main[(size_t) found].modal.imag());
            }
            if (out.longIndex.size() < 3)
                out.longGain = 0.0f;
        }

        out.region = n.region;
        out.hasDamper = n.hasDamper;
        out.knockGain = (float) cal.knockGain (n.midi);
        out.hammerMass = (float) n.hammerMass;
        out.hammerK = (float) n.hammerK;
        out.hammerP = (float) n.hammerP;
        out.hammerHysteresis = (float) n.hammerHysteresis;
        double stringsHit = 0.0;
        for (int s = 0; s < S; ++s) stringsHit += n.strikeGain[s];
        out.stringsHit = (float) stringsHit;

        // Below the fundamental the string is a lever: it passes the hammer's push on to the bridge in proportion
        // to the striking point, and the board's lowest modes answer with a thud. From the fundamental up the
        // partials carry the blow, so the modes near it are left out.
        for (int m = 0; m < std::min (kThumpModes, sb.count); ++m)
        {
            const double hz = sb.omega[(size_t) m] / (2.0 * kPi);
            const double clear = std::clamp ((0.9 * n.f0 - hz) / (0.4 * n.f0), 0.0, 1.0);
            if (clear <= 0.0)
                break;
            out.thump[m] = (float) (n.strikeRatio * stringsHit * clear * cal.thumpLevel (hz) * cal.thumpGain (n.midi)
                                    * sb.shapeAtNote[(size_t) index * (size_t) sb.count + (size_t) m]);
            out.thumpCount = ((m + 1) + 3) & ~3;
        }
    }

    template <typename Function>
    void forEachNoteInParallel (Function&& work)
    {
        const int threads = (int) std::max (1u, std::min (8u, std::thread::hardware_concurrency()));
        std::vector<std::thread> pool;
        for (int t = 0; t < threads; ++t)
            pool.emplace_back ([&work, t, threads]
            {
                for (int i = t; i < kNumNotes; i += threads)
                    work (i);
            });
        for (auto& t : pool) t.join();
    }
}

std::shared_ptr<const PianoModel> buildModel (const Voicing& voicing)
{
    const PianoDesign design = makeDesign (voicing);
    const auto& cal = design.calibration;
    const Soundboard sb = makeSoundboard (design, std::min (kBoardMaxHz, 0.45 * voicing.sampleRate));

    auto model = std::make_shared<PianoModel>();
    model->sampleRate = voicing.sampleRate;
    model->outputGain = (float) cal.outputGain;
    model->knockNewtons = (float) cal.knockNewtons;
    model->knockPower = (float) cal.knockPower;
    const double Ts = 1.0 / voicing.sampleRate;

    std::vector<LocalAverage> averages ((size_t) kNumNotes);
    forEachNoteInParallel ([&] (int i) { averages[(size_t) i] = makeLocalAverage (sb, i); });
    forEachNoteInParallel ([&] (int i) { buildNote (design, sb, averages[(size_t) i], i, *model); });

    // The board's average response over all notes: its own modes are read at the calibrated level too,
    // so the body's ring sits where it belongs against the strings.
    std::vector<double> boardTransfer ((size_t) LocalAverage::size), boardMobility ((size_t) LocalAverage::size);
    for (int g = 0; g < LocalAverage::size; ++g)
    {
        double h = 0.0, y = 0.0;
        for (const auto& a : averages)
        {
            h += 0.5 * (a.transfer[0][(size_t) g] * a.transfer[0][(size_t) g] + a.transfer[1][(size_t) g] * a.transfer[1][(size_t) g]);
            y += a.mobility[(size_t) g] * a.mobility[(size_t) g];
        }
        boardTransfer[(size_t) g] = std::sqrt (h / kNumNotes);
        boardMobility[(size_t) g] = std::sqrt (y / kNumNotes);
    }

    // Three more modes below the board's lowest stand for the case and frame, which only the action knock excites.
    static constexpr double caseHz[] = { 27.0, 33.0, 40.0 };
    constexpr double caseLossFactor = 0.08;

    auto& board = model->board;
    board.resize (sb.count + kCaseModes);
    double regionScale[kNumRegions] {};
    int regionNotes[kNumRegions] {};
    for (const auto& n : design.notes)
    {
        regionScale[n.region] += cal.mobility (n.midi, 500.0) / LocalAverage::at (boardMobility, 500.0);
        ++regionNotes[n.region];
    }
    for (int r = 0; r < kNumRegions; ++r)
        model->boardBus[r].assign ((size_t) board.count, 0.0f);
    for (auto& k : model->knock)
        k.assign ((size_t) board.count, 0.0f);

    // The knock's level was fitted entering under the middle of the keyboard. Each region's entry point drives
    // the modes in its own pattern, which is what tells one region's knock from another's; its overall drive is
    // scaled to the middle's, so that the fitted level holds everywhere.
    double knockScale[kNumRegions];
    {
        double centre = 0.0, region[kNumRegions] {};
        for (int m = 0; m < sb.count; ++m)
        {
            const double level = cal.knockLevel (sb.omega[(size_t) m] / (2.0 * kPi));
            centre += std::pow (sb.shapeAtKeybed[(size_t) m] * level, 2.0);
            for (int r = 0; r < kNumRegions; ++r)
                region[r] += std::pow (sb.shapeAtKey[(size_t) r * (size_t) sb.count + (size_t) m] * level, 2.0);
        }
        for (int r = 0; r < kNumRegions; ++r)
            knockScale[r] = std::sqrt (centre / std::max (region[r], 1.0e-30));
    }

    for (int m = 0; m < sb.count; ++m)
    {
        const auto i = (size_t) m;
        const double hz = sb.omega[i] / (2.0 * kPi);
        const cplx p = std::exp (sb.pole[i] * Ts);
        board.poleRe[i] = (float) p.real(); board.poleIm[i] = (float) p.imag();
        const double level = cal.radiationLevel (hz) / LocalAverage::at (boardTransfer, hz);
        for (int mic = 0; mic < 2; ++mic)
        {
            const cplx o = 2.0 * level * sb.modeOutput (m, mic);
            board.outRe[mic][i] = (float) o.real(); board.outIm[mic][i] = (float) o.imag();
        }
        if (hz < kBoardBusMaxHz)
        {
            // The thud's level was fitted at the microphones. For the board's lowest modes that makes them move
            // far more than the bridge does, and the open bass strings would boom with them: the strings get
            // these modes without the fitted gain.
            const double busTrim = hz < kThumpBusTrimHz ? std::pow (std::max (1.0, cal.thumpLevel (hz)), 1.0 - cal.thumpToStrings) : 1.0;
            model->boardBusCount = ((m + 1) + 3) & ~3;
            for (int r = 0; r < kNumRegions; ++r)
                model->boardBus[r][i] = (float) (regionScale[r] / regionNotes[r] * sb.shapeAtRegion[(size_t) r * (size_t) sb.count + i]
                                                 / (sb.mass * sb.pole[i].imag() * busTrim));
        }
        for (int r = 0; r < kNumRegions; ++r)
            model->knock[r][i] = (float) (knockScale[r] * sb.shapeAtKey[(size_t) r * (size_t) sb.count + i] * Ts * cal.knockLevel (hz));
    }

    {
        // The case modes radiate like a typical low board mode, equally left and right.
        double out = 0.0, drive = 0.0;
        int counted = 0;
        for (int m = 0; m < sb.count; ++m)
            if (const double hz = sb.omega[(size_t) m] / (2.0 * kPi); hz > 80.0 && hz < 160.0)
            {
                out += std::hypot (board.outRe[0][(size_t) m], board.outIm[0][(size_t) m]);
                drive += std::abs (sb.shapeAtKeybed[(size_t) m]) * Ts;
                ++counted;
            }
        for (int c = 0; c < kCaseModes; ++c)
        {
            const auto i = (size_t) (sb.count + c);
            const double omega = 2.0 * kPi * caseHz[c];
            const cplx p = std::exp (cplx (-0.5 * caseLossFactor * omega, omega) * Ts);
            board.poleRe[i] = (float) p.real(); board.poleIm[i] = (float) p.imag();
            board.outIm[0][i] = board.outIm[1][i] = (float) (out / std::max (1, counted));
            for (int r = 0; r < kNumRegions; ++r)
                model->knock[r][i] = (float) ((c & 1 ? -1.0 : 1.0) * drive / std::max (1, counted) * cal.knockLevel (caseHz[c]));
        }
    }


    for (int a = 0; a < kNumRegions; ++a)
        for (int b = 0; b < kNumRegions; ++b)
            model->regionGain[a][b] = (float) (voicing.sympathetic * cal.sympatheticGain * std::exp (-std::abs (a - b) / 2.0));

    return model;
}

} // namespace pm
