#include "Soundboard.h"

#include <algorithm>
#include <cmath>
#include <thread>

namespace pm
{

namespace
{
    // Dynamic rigidities (bending stiffness / mass per area, m^4/s^2) of the ribbed board treated as a
    // homogeneous plate, after Ege and Boutillon's measurements (about 100 to 150).
    constexpr double kRigidityX = 95.0, kRigidityY = 130.0, kRigidityXY = 67.0;
    constexpr double kMassPerArea = 4.8;        // kg/m^2: 8 mm spruce plus ribs
    constexpr double kCrownHz = 35.0;           // stiffening by the crown and the strings' down-bearing
    constexpr double kSoundSpeed = 343.0;

    struct Vec3 { double x, y, z; };
    constexpr Vec3 kMics[Soundboard::numMics] = { { 0.25, 0.90, 0.90 }, { 1.15, 0.90, 0.90 } };
    constexpr double kKeybedX = 0.70, kKeybedY = 0.12;
    // Where the keys' actions meet the keybed, across the board: spread about the middle, but kept away
    // from the clamped edges of the plate, where every mode shape is small.
    constexpr double kKeyboardLeft = 0.30, kKeyboardWidth = 0.80;

    /** Where a key's action meets the keybed, across the board. */
    double keyX (int midi)
    {
        return kKeyboardLeft + kKeyboardWidth * (midi - kLowestMidi) / (double) (kNumNotes - 1);
    }

    /** Mode shape of a clamped-clamped beam (asymptotic form), mean square 1 over the length. */
    double beamShape (int m, double x, double length)
    {
        const double beta = (m + 0.5) * kPi / length;
        const double edge = (m & 1) ? 1.0 : -1.0;
        return std::sqrt (2.0) * std::sin (beta * x - kPi / 4.0) + std::exp (-beta * x) + edge * std::exp (-beta * (length - x));
    }

    double lossFactor (double hz)
    {
        return 0.012 + 0.03 / (1.0 + (hz / 90.0) * (hz / 90.0));
    }
}

void Soundboard::evaluate (int note, cplx s, cplx& mobilityOut, cplx& transfer0, cplx& transfer1) const
{
    const double* shape = &shapeAtNote[(size_t) note * (size_t) count];
    const cplx* rad0 = radiation[0].data();
    const cplx* rad1 = radiation[1].data();
    cplx y = 0.0, h0 = 0.0, h1 = 0.0;
    for (int m = 0; m < count; ++m)
    {
        const auto i = (size_t) m;
        const cplx term = shape[m] / (s * s + 2.0 * sigma[i] * s + omega[i] * omega[i]);
        y += shape[m] * term;
        h0 += rad0[m] * term;
        h1 += rad1[m] * term;
    }
    mobilityOut = y * s / mass;
    transfer0 = h0 * s * s / mass;
    transfer1 = h1 * s * s / mass;
}

cplx Soundboard::modeOutput (int mode, int mic) const
{
    const auto i = (size_t) mode;
    const cplx p = pole[i];
    return radiation[mic][i] * p * p / (mass * (p - std::conj (p)));
}

Soundboard makeSoundboard (const PianoDesign& design, double maxFrequency)
{
    Soundboard sb;
    sb.mass = kMassPerArea * Soundboard::lengthX * Soundboard::lengthY;

    struct Mode { int m, n; double omega; };
    std::vector<Mode> modes;
    const double maxOmega2 = std::pow (2.0 * kPi * maxFrequency, 2.0);
    const double crown2 = std::pow (2.0 * kPi * kCrownHz, 2.0);

    for (int m = 1;; ++m)
    {
        const double bx = (m + 0.5) * kPi / Soundboard::lengthX, bx2 = bx * bx;
        if (kRigidityX * bx2 * bx2 + crown2 > maxOmega2)
            break;
        for (int n = 1;; ++n)
        {
            const double by = (n + 0.5) * kPi / Soundboard::lengthY, by2 = by * by;
            const double w2 = kRigidityX * bx2 * bx2 + 2.0 * kRigidityXY * bx2 * by2 + kRigidityY * by2 * by2 + crown2;
            if (w2 > maxOmega2)
                break;
            modes.push_back ({ m, n, std::sqrt (w2) });
        }
    }
    std::sort (modes.begin(), modes.end(), [] (const Mode& a, const Mode& b) { return a.omega < b.omega; });

    const int count = sb.count = (int) modes.size();
    const auto N = (size_t) count;
    sb.omega.resize (N); sb.sigma.resize (N); sb.pole.resize (N);
    sb.shapeAtNote.resize (N * kNumNotes);
    sb.shapeAtRegion.resize (N * kNumRegions);
    sb.shapeAtKey.resize (N * kNumRegions);
    sb.shapeAtKeybed.resize (N);
    for (auto& r : sb.radiation) r.resize (N);

    // Region points: the average bridge position of the notes in each region.
    double regionX[kNumRegions] {}, regionY[kNumRegions] {}, regionKeyX[kNumRegions] {};
    int regionCount[kNumRegions] {};
    for (const auto& n : design.notes)
    {
        regionX[n.region] += n.bridgeX; regionY[n.region] += n.bridgeY; regionKeyX[n.region] += keyX (n.midi); ++regionCount[n.region];
    }

    for (int i = 0; i < count; ++i)
    {
        const auto& mode = modes[(size_t) i];
        const double hz = mode.omega / (2.0 * kPi);
        const double eta = lossFactor (hz) * (1.0 + 0.3 * hashUnit (design.voicing.seed, 7000u + (uint32_t) i, 1u));
        const double sigma = 0.5 * eta * mode.omega;
        sb.omega[(size_t) i] = mode.omega;
        sb.sigma[(size_t) i] = sigma;
        sb.pole[(size_t) i] = cplx (-sigma, std::sqrt (mode.omega * mode.omega - sigma * sigma));

        for (int note = 0; note < kNumNotes; ++note)
        {
            const auto& nd = design.notes[(size_t) note];
            sb.shapeAtNote[(size_t) note * N + (size_t) i] = beamShape (mode.m, nd.bridgeX, Soundboard::lengthX)
                                                             * beamShape (mode.n, nd.bridgeY, Soundboard::lengthY);
        }
        for (int r = 0; r < kNumRegions; ++r)
        {
            sb.shapeAtRegion[(size_t) r * N + (size_t) i] = beamShape (mode.m, regionX[r] / regionCount[r], Soundboard::lengthX)
                                                            * beamShape (mode.n, regionY[r] / regionCount[r], Soundboard::lengthY);
            sb.shapeAtKey[(size_t) r * N + (size_t) i] = beamShape (mode.m, regionKeyX[r] / regionCount[r], Soundboard::lengthX)
                                                         * beamShape (mode.n, kKeybedY, Soundboard::lengthY);
        }
        sb.shapeAtKeybed[(size_t) i] = beamShape (mode.m, kKeybedX, Soundboard::lengthX) * beamShape (mode.n, kKeybedY, Soundboard::lengthY);
    }

    // Radiation: Rayleigh integral of each mode shape to each microphone, at the mode's own frequency.
    constexpr int nx = 96, ny = 128;
    const double dx = Soundboard::lengthX / nx, dy = Soundboard::lengthY / ny;
    int maxM = 0, maxN = 0;
    for (const auto& mode : modes) { maxM = std::max (maxM, mode.m); maxN = std::max (maxN, mode.n); }

    std::vector<float> shapeX ((size_t) (maxM + 1) * nx), shapeY ((size_t) (maxN + 1) * ny);
    for (int m = 1; m <= maxM; ++m)
        for (int i = 0; i < nx; ++i)
            shapeX[(size_t) m * nx + (size_t) i] = (float) beamShape (m, (i + 0.5) * dx, Soundboard::lengthX);
    for (int n = 1; n <= maxN; ++n)
        for (int j = 0; j < ny; ++j)
            shapeY[(size_t) n * ny + (size_t) j] = (float) beamShape (n, (j + 0.5) * dy, Soundboard::lengthY);

    std::vector<float> distance[Soundboard::numMics];
    for (int mic = 0; mic < Soundboard::numMics; ++mic)
    {
        distance[mic].resize ((size_t) nx * ny);
        for (int i = 0; i < nx; ++i)
            for (int j = 0; j < ny; ++j)
            {
                const double ex = (i + 0.5) * dx - kMics[mic].x, ey = (j + 0.5) * dy - kMics[mic].y;
                distance[mic][(size_t) i * ny + (size_t) j] = (float) std::sqrt (ex * ex + ey * ey + kMics[mic].z * kMics[mic].z);
            }
    }

    auto integrate = [&] (int first, int last)
    {
        std::vector<float> rowRe ((size_t) ny), rowIm ((size_t) ny);
        for (int i = first; i < last; ++i)
        {
            const auto& mode = modes[(size_t) i];
            const float k = (float) (mode.omega / kSoundSpeed);
            const float* sx = &shapeX[(size_t) mode.m * nx];
            const float* sy = &shapeY[(size_t) mode.n * ny];

            for (int mic = 0; mic < Soundboard::numMics; ++mic)
            {
                double re = 0.0, im = 0.0;
                const float* r = distance[mic].data();
                for (int a = 0; a < nx; ++a)
                {
                    float rowR = 0.0f, rowI = 0.0f;
                    for (int b = 0; b < ny; ++b)
                    {
                        const float dist = r[(size_t) a * ny + (size_t) b];
                        const float w = sy[b] / dist;
                        rowR += w * std::cos (k * dist);
                        rowI -= w * std::sin (k * dist);
                    }
                    re += sx[a] * rowR; im += sx[a] * rowI;
                }
                sb.radiation[mic][(size_t) i] = cplx (re, im) * (dx * dy);
            }
        }
    };

    const int threads = (int) std::max (1u, std::min (8u, std::thread::hardware_concurrency()));
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t)
        pool.emplace_back (integrate, count * t / threads, count * (t + 1) / threads);
    for (auto& t : pool) t.join();

    return sb;
}

} // namespace pm
