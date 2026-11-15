#include "reference_engine.h"

#include "Denormals.h"
#include "Hammer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace pm
{

namespace
{
    constexpr float kAudible = 3.16e-6f;         // -110 dB re full scale: quieter resonators are switched off
    constexpr int kCheckChunks = 16;             // how often a voice looks for resonators to switch off
    constexpr double kKnockSeconds = 0.0008;     // length of the action knock's force pulse at the speed it was measured at, 1.5 m/s
    constexpr double kKnockSpeed = 1.5;
    constexpr uint32_t kRandomSeed = 0x9E3779B9u;

    void makeGate (const Bank& bank, float gain, std::vector<float>& gate)
    {
        gate.assign ((size_t) bank.count, 0.0f);
        const float scale = gain * gain / (kAudible * kAudible);
        for (size_t r = 0; r < (size_t) bank.count; ++r)
        {
            const float l = bank.outRe[0][r] * bank.outRe[0][r] + bank.outIm[0][r] * bank.outIm[0][r];
            const float rr = bank.outRe[1][r] * bank.outRe[1][r] + bank.outIm[1][r] * bank.outIm[1][r];
            gate[r] = std::max (l, rr) * scale;
        }
    }

    bool anyAudible (const float* zr, const float* zi, const float* gate, int first, int last)
    {
        for (int q = first; q < last; q += 4)
        {
            const simd::f4 re = simd::load (zr + q), im = simd::load (zi + q);
            const simd::f4 power = simd::mul (simd::fma (simd::mul (re, re), im, im), simd::load (gate + q));
            if (simd::maxLane (power) > 1.0f)
                return true;
        }
        return false;
    }
}

void ReferenceEngine::setModel (std::shared_ptr<const PianoModel> newModel)
{
    model = std::move (newModel);
    for (int i = 0; i < kNumNotes; ++i)
    {
        auto& v = voices[i];
        v = Voice();
        v.note = &model->notes[i];
        const auto mainCount = (size_t) v.note->main.count, symCount = (size_t) v.note->sympathetic.count;
        v.zr.assign (mainCount, 0.0f); v.zi.assign (mainCount, 0.0f);
        v.pr = v.note->main.poleRe; v.pi = v.note->main.poleIm;
        v.szr.assign (symCount, 0.0f); v.szi.assign (symCount, 0.0f);
        v.spr = v.note->sympathetic.poleRe; v.spi = v.note->sympathetic.poleIm;
        makeGate (v.note->main, model->outputGain, v.gate);
        makeGate (v.note->sympathetic, model->outputGain, v.sgate);
        v.longCoefficients = longitudinalFilters (*v.note);
        v.checkCountdown = i % kCheckChunks;
    }
    boardRe.assign ((size_t) model->board.count, 0.0f);
    boardIm.assign ((size_t) model->board.count, 0.0f);
    makeGate (model->board, model->outputGain, boardGate);
    reset();
}

void ReferenceEngine::reset()
{
    if (model == nullptr)
        return;
    for (auto& v : voices)
    {
        std::fill (v.zr.begin(), v.zr.end(), 0.0f); std::fill (v.zi.begin(), v.zi.end(), 0.0f);
        std::fill (v.szr.begin(), v.szr.end(), 0.0f); std::fill (v.szi.begin(), v.szi.end(), 0.0f);
        std::fill (std::begin (v.own), std::end (v.own), 0.0f);
        v.active = 0; v.contact = false; v.knock = 0.0f;
        std::memset (v.longFilter, 0, sizeof (v.longFilter)); v.longDc = 0.0f; v.longFade = 0.0f;
        v.keyDown = false; v.heldBySostenuto = false; v.sympatheticLive = false;
        v.damper = v.damperTarget = v.note->hasDamper ? 1.0f : 0.0f;
        v.appliedDamper = -1.0f;
    }
    std::fill (boardRe.begin(), boardRe.end(), 0.0f);
    std::fill (boardIm.begin(), boardIm.end(), 0.0f);
    boardAwake = false;
    std::memset (regionRing, 0, sizeof (regionRing));
    sustain = 0.0f; soft = 0.0f; dampersLift = 0.0f; sostenuto = false;
    random = kRandomSeed;
}

float ReferenceEngine::unit()
{
    random ^= random << 13; random ^= random >> 17; random ^= random << 5;      // xorshift32
    return (float) (int32_t) random * (1.0f / 2147483648.0f);
}

/** A knock on the body: a half-sine-squared force pulse whose impulse grows with the speed of the blow as
    measured, spread over `seconds` (a slower key lands with a longer, duller pulse), and never quite the same
    twice. */
void ReferenceEngine::startKnock (Voice& v, double speed, double seconds, float spread, int delaySamples)
{
    seconds *= 1.0 + 0.15 * unit();
    const double impulse = std::pow (speed, (double) model->knockPower) * kKnockSeconds / seconds;
    v.knockLength = std::max (4, (int) (seconds * model->sampleRate));
    v.knock = settings.actionNoise * model->knockNewtons * v.note->knockGain * (float) impulse * (1.0f + spread * unit());
    v.knockPosition = -delaySamples;
}

void ReferenceEngine::noteOn (int midi, float velocity)
{
    const int index = midi - kLowestMidi;
    if (model == nullptr || index < 0 || index >= kNumNotes)
        return;
    auto& v = voices[index];
    const auto& bank = v.note->main;

    v.keyDown = true;
    v.damper = v.damperTarget = 0.0f;       // the key has lifted the damper well before the hammer arrives

    const float shaped = 64.0f + (std::clamp (velocity, 1.0f, 127.0f) - 64.0f) * settings.dynamics;
    // The soft pedal shifts the hammers onto less compacted felt: a gentler, slightly slower blow.
    const double speed = hammerVelocity (shaped) * (1.0 - 0.2 * soft);
    v.hardness = 1.0f - 0.45f * soft;

    // The hammer starts touching the string where the string is now, so a note that is still
    // sounding is struck as it moves.
    float string = 0.0f;
    for (int r = 0; r < bank.count; ++r)
        string += bank.hammerRe[(size_t) r] * v.zr[(size_t) r] - bank.hammerIm[(size_t) r] * v.zi[(size_t) r];
    v.stringAtHammer = string;
    v.hammerPosition = string;
    v.hammerVelocity = speed;
    v.lastCompression = 0.0;
    v.contact = true;
    v.contactSamples = 0;
    if (v.active == 0)
        v.longFade = 0.0f;       // a string at rest: its longitudinal sound comes in with the note
    v.active = bank.count;

    // The key reaches the keybed as the hammer arrives; a slow key lands with a longer, duller pulse.
    startKnock (v, speed, std::clamp (kKnockSeconds * std::pow (kKnockSpeed / speed, 0.2), 0.0006, 0.0015), 0.2f, 0);
}

void ReferenceEngine::noteOff (int midi, float velocity)
{
    const int index = midi - kLowestMidi;
    if (model == nullptr || index < 0 || index >= kNumNotes)
        return;
    auto& v = voices[index];
    if (v.keyDown && ! v.contact)
    {
        // The key coming back to rest: sooner and harder after a quick release, as a soft, longer pulse.
        const double quick = std::clamp (velocity, 0.0f, 127.0f) / 127.0;
        const double speed = 0.034 + 0.105 * quick * quick;
        const double delay = (0.11 - 0.05 * quick) * (1.0 + 0.15 * unit());
        startKnock (v, speed, 0.002, 0.3f, (int) (delay * model->sampleRate));
    }
    v.keyDown = false;
    if (! v.heldBySostenuto && v.note->hasDamper)
    {
        v.damperTarget = 1.0f - dampersLift;
        const float seconds = 0.06f - 0.045f * std::clamp (velocity, 0.0f, 127.0f) / 127.0f;
        v.damperRate = 1.0f / (seconds * (float) model->sampleRate);
    }
}

void ReferenceEngine::updateDamperTargets()
{
    for (auto& v : voices)
        if (! v.keyDown && ! v.heldBySostenuto && v.note->hasDamper)
        {
            v.damperTarget = 1.0f - dampersLift;
            v.damperRate = 1.0f / (0.03f * (float) model->sampleRate);
        }
}

void ReferenceEngine::setSustain (float amount)
{
    if (model == nullptr)
        return;
    sustain = std::clamp (amount, 0.0f, 1.0f);
    const float t = std::clamp ((sustain - 0.3f) / 0.4f, 0.0f, 1.0f);   // dampers leave the strings over the middle of the travel
    dampersLift = t * t * (3.0f - 2.0f * t);
    updateDamperTargets();
}

void ReferenceEngine::setSostenuto (bool down)
{
    if (model == nullptr || down == sostenuto)
        return;
    sostenuto = down;
    for (auto& v : voices)
        v.heldBySostenuto = down && v.keyDown;
    if (! down)
        updateDamperTargets();
}

void ReferenceEngine::setSoft (float amount)
{
    soft = std::clamp (amount, 0.0f, 1.0f);
}

void ReferenceEngine::allNotesOff()
{
    if (model == nullptr)
        return;
    sustain = 0.0f; dampersLift = 0.0f; sostenuto = false;
    for (auto& v : voices)
    {
        v.keyDown = false; v.heldBySostenuto = false;
    }
    updateDamperTargets();
}

void ReferenceEngine::applyDamper (Voice& v)
{
    const float d = v.damper;
    auto apply = [d] (const Bank& bank, std::vector<float>& pr, std::vector<float>& pi)
    {
        const simd::f4 amount = simd::set (d), one = simd::set (1.0f);
        for (int q = 0; q < bank.count; q += 4)
        {
            const simd::f4 f = simd::fms (one, amount, simd::load (&bank.damping[(size_t) q]));
            simd::store (&pr[(size_t) q], simd::mul (simd::load (&bank.poleRe[(size_t) q]), f));
            simd::store (&pi[(size_t) q], simd::mul (simd::load (&bank.poleIm[(size_t) q]), f));
        }
    };
    apply (v.note->main, v.pr, v.pi);
    apply (v.note->sympathetic, v.spr, v.spi);
    v.appliedDamper = d;
}

void ReferenceEngine::runContact (Voice& v, int n)
{
    const auto& note = *v.note;
    const auto& b = note.main;
    const int count = b.count, steps = note.substeps;
    const double dt = 1.0 / (model->sampleRate * steps);
    const double stiffness = (double) note.hammerK * settings.hammerHardness * v.hardness;
    const double exponent = note.hammerP, hysteresis = note.hammerHysteresis;
    const double pull = (double) note.stringsHit / note.hammerMass;

    const float longGain = settings.phantoms * note.longGain;
    const float gainL = longGain * note.longOut[0], gainR = longGain * note.longOut[1];
    const bool longitudinal = longGain > 0.0f;
    const int partials = (int) note.longIndex.size();
    float u[kLongPartials + 2] {};

    float* zr = v.zr.data(); float* zi = v.zi.data();
    const float *psr = b.subRe.data(), *psi = b.subIm.data(), *hr = b.hammerRe.data(), *hi = b.hammerIm.data();
    const float *lr = b.outRe[0].data(), *li = b.outIm[0].data(), *rr = b.outRe[1].data(), *ri = b.outIm[1].data();
    const float *ur = b.busRe.data(), *ui = b.busIm.data();

    for (int i = 0; i < n; ++i)
    {
        double impulse = 0.0;
        for (int s = 0; s < steps; ++s)
        {
            double f = 0.0;
            if (v.contact)
            {
                const double compression = v.hammerPosition - (double) v.stringAtHammer;
                f = feltForce (compression, (compression - v.lastCompression) / dt, stiffness, exponent, hysteresis);
                v.lastCompression = compression;
                v.hammerVelocity -= pull * f * dt;
                v.hammerPosition += v.hammerVelocity * dt;
                if (compression < -kHammerClear)
                    v.contact = false;      // the rest of this chunk runs without the hammer
            }

            impulse += dt * f;
            const simd::f4 in = simd::set ((float) (dt * f));
            simd::f4 string = simd::set (0.0f);

            if (s + 1 < steps)
            {
                for (int q = 0; q < count; q += 4)
                {
                    const simd::f4 re = simd::add (simd::load (zr + q), in), im = simd::load (zi + q);
                    const simd::f4 pr = simd::load (psr + q), pi = simd::load (psi + q);
                    const simd::f4 nr = simd::fms (simd::mul (re, pr), im, pi);
                    const simd::f4 ni = simd::fma (simd::mul (re, pi), im, pr);
                    simd::store (zr + q, nr); simd::store (zi + q, ni);
                    string = simd::fms (simd::fma (string, simd::load (hr + q), nr), simd::load (hi + q), ni);
                }
            }
            else
            {
                simd::f4 l = simd::set (0.0f), r = l, own = l;
                for (int q = 0; q < count; q += 4)
                {
                    const simd::f4 re = simd::add (simd::load (zr + q), in), im = simd::load (zi + q);
                    const simd::f4 pr = simd::load (psr + q), pi = simd::load (psi + q);
                    const simd::f4 nr = simd::fms (simd::mul (re, pr), im, pi);
                    const simd::f4 ni = simd::fma (simd::mul (re, pi), im, pr);
                    simd::store (zr + q, nr); simd::store (zi + q, ni);
                    string = simd::fms (simd::fma (string, simd::load (hr + q), nr), simd::load (hi + q), ni);
                    l = simd::fms (simd::fma (l, simd::load (lr + q), nr), simd::load (li + q), ni);
                    r = simd::fms (simd::fma (r, simd::load (rr + q), nr), simd::load (ri + q), ni);
                    own = simd::fms (simd::fma (own, simd::load (ur + q), nr), simd::load (ui + q), ni);
                }
                accL[i] = simd::add (accL[i], l);
                accR[i] = simd::add (accR[i], r);
                accOwn[i] = simd::add (accOwn[i], own);
            }
            v.stringAtHammer = simd::sum (string);
        }

        if (impulse > 0.0 && note.thumpCount > 0)
        {
            if (! thumpActive) { std::memset (thumpIn, 0, sizeof (thumpIn)); thumpActive = true; }
            const float push = (float) impulse;
            for (int m = 0; m < note.thumpCount; ++m)
                thumpIn[i][m] += note.thump[m] * push;
        }

        if (longitudinal)
        {
            for (int k = 0; k < partials; ++k)
            {
                const auto r = (size_t) note.longIndex[(size_t) k];
                u[k] = note.longRe[(size_t) k] * zr[r] - note.longIm[(size_t) k] * zi[r];
            }
            const float x = longitudinalSample (v, u, partials);
            longL[i] += gainL * x;
            longR[i] += gainR * x;
        }
    }

    v.contactSamples += n;
    if (v.contactSamples > (int) (0.05 * model->sampleRate))
        v.contact = false;
}

void ReferenceEngine::runFree (Voice& v, int n)
{
    const auto& b = v.note->main;
    float* zr = v.zr.data(); float* zi = v.zi.data();
    const float *pr = v.pr.data(), *pi = v.pi.data();
    const float *lr = b.outRe[0].data(), *li = b.outIm[0].data(), *rr = b.outRe[1].data(), *ri = b.outIm[1].data();
    const float *ur = b.busRe.data(), *ui = b.busIm.data();

    for (int q = 0; q < v.active; q += 4)
    {
        simd::f4 re = simd::load (zr + q), im = simd::load (zi + q);
        const simd::f4 ar = simd::load (pr + q), ai = simd::load (pi + q);
        const simd::f4 clr = simd::load (lr + q), cli = simd::load (li + q), crr = simd::load (rr + q), cri = simd::load (ri + q);
        const simd::f4 cur = simd::load (ur + q), cui = simd::load (ui + q);
        for (int i = 0; i < n; ++i)
        {
            const simd::f4 nr = simd::fms (simd::mul (re, ar), im, ai);
            im = simd::fma (simd::mul (re, ai), im, ar);
            re = nr;
            accL[i] = simd::fms (simd::fma (accL[i], clr, re), cli, im);
            accR[i] = simd::fms (simd::fma (accR[i], crr, re), cri, im);
            accOwn[i] = simd::fms (simd::fma (accOwn[i], cur, re), cui, im);
        }
        simd::store (zr + q, re); simd::store (zi + q, im);
    }
}

void ReferenceEngine::runSympathetic (Voice& v, const float* input, int n)
{
    const auto& b = v.note->sympathetic;
    float* zr = v.szr.data(); float* zi = v.szi.data();
    const float *pr = v.spr.data(), *pi = v.spi.data();
    const float *lr = b.outRe[0].data(), *li = b.outIm[0].data(), *rr = b.outRe[1].data(), *ri = b.outIm[1].data();

    for (int q = 0; q < b.count; q += 4)
    {
        simd::f4 re = simd::load (zr + q), im = simd::load (zi + q);
        const simd::f4 ar = simd::load (pr + q), ai = simd::load (pi + q);
        const simd::f4 clr = simd::load (lr + q), cli = simd::load (li + q), crr = simd::load (rr + q), cri = simd::load (ri + q);
        for (int i = 0; i < n; ++i)
        {
            re = simd::add (re, simd::set (input[i]));
            const simd::f4 nr = simd::fms (simd::mul (re, ar), im, ai);
            im = simd::fma (simd::mul (re, ai), im, ar);
            re = nr;
            accL[i] = simd::fms (simd::fma (accL[i], clr, re), cli, im);
            accR[i] = simd::fms (simd::fma (accR[i], crr, re), cri, im);
        }
        simd::store (zr + q, re); simd::store (zi + q, im);
    }
}

ReferenceEngine::LongitudinalFilters ReferenceEngine::longitudinalFilters (const NoteModel& note) const
{
    LongitudinalFilters c;
    const float sampleRate = (float) model->sampleRate;
    for (int m = 0; m < 2; ++m)
    {
        c.g[m] = note.longHz[m] > 0.0f ? std::tan ((float) kPi * note.longHz[m] / sampleRate) : 0.0f;
        c.a1[m] = 1.0f / (1.0f + c.g[m] * (c.g[m] + LongitudinalFilters::damping));
        c.a2[m] = c.g[m] * c.a1[m];
        c.a3[m] = c.g[m] * c.a2[m];
    }
    c.dcRate = 1.0f - std::exp (-2.0f * (float) kPi * 40.0f / sampleRate);
    c.fadeRate = 1.0f - std::exp (-1.0f / (0.006f * sampleRate));       // the board answers it within a few milliseconds
    return c;
}

float ReferenceEngine::longitudinalSample (Voice& v, const float* u, int partials)
{
    const LongitudinalFilters& c = v.longCoefficients;
    float squares = 0.0f, next = 0.0f, second = 0.0f;
    for (int k = 0; k < partials; ++k)
    {
        squares += u[k] * u[k];
        next += u[k] * u[k + 1];
        second += u[k] * u[k + 2];
    }

    // Tension common to the whole string, without its slow drift, plus what the longitudinal
    // modes add above their own frequencies.
    v.longDc += c.dcRate * (squares - v.longDc);
    float force = squares - v.longDc;
    const float drive[2] = { next, second };
    for (int m = 0; m < 2; ++m)
    {
        if (c.g[m] <= 0.0f)
            continue;
        float* state = v.longFilter[m];
        const float v3 = drive[m] - state[1];
        const float v1 = c.a1[m] * state[0] + c.a2[m] * v3;
        const float v2 = state[1] + c.a2[m] * state[0] + c.a3[m] * v3;
        state[0] = 2.0f * v1 - state[0];
        state[1] = 2.0f * v2 - state[1];
        force += 2.0f * (drive[m] - LongitudinalFilters::damping * v1 - v2);      // high-pass output
    }
    v.longFade += c.fadeRate * (1.0f - v.longFade);
    return v.longFade * force;
}

void ReferenceEngine::runLongitudinal (Voice& v, int n)
{
    const auto& note = *v.note;
    const int partials = (int) note.longIndex.size();
    float zr[kLongPartials + 2], zi[kLongPartials + 2], pr[kLongPartials + 2], pi[kLongPartials + 2], u[kLongPartials + 2];

    // The strongest resonator of each partial, followed from its state at the start of the chunk.
    for (int k = 0; k < partials; ++k)
    {
        const auto r = (size_t) note.longIndex[(size_t) k];
        zr[k] = v.zr[r]; zi[k] = v.zi[r]; pr[k] = v.pr[r]; pi[k] = v.pi[r];
    }
    u[partials] = u[partials + 1] = 0.0f;

    const float gain = settings.phantoms * note.longGain;
    const float gainL = gain * note.longOut[0], gainR = gain * note.longOut[1];
    const float* wr = note.longRe.data();
    const float* wi = note.longIm.data();

    for (int i = 0; i < n; ++i)
    {
        for (int k = 0; k < partials; ++k)
        {
            const float re = zr[k] * pr[k] - zi[k] * pi[k];
            zi[k] = zr[k] * pi[k] + zi[k] * pr[k];
            zr[k] = re;
            u[k] = wr[k] * re - wi[k] * zi[k];
        }
        const float x = longitudinalSample (v, u, partials);
        longL[i] += gainL * x;
        longR[i] += gainR * x;
    }
}

void ReferenceEngine::runBoard (int n)
{
    const auto& b = model->board;
    float* zr = boardRe.data(); float* zi = boardIm.data();
    const float *pr = b.poleRe.data(), *pi = b.poleIm.data();
    const float *lr = b.outRe[0].data(), *li = b.outIm[0].data(), *rr = b.outRe[1].data(), *ri = b.outIm[1].data();
    simd::f4 regionAcc[kNumRegions][kChunk];
    if (anySympathetic)
        for (auto& region : regionAcc)
            for (int i = 0; i < n; ++i)
                region[i] = simd::set (0.0f);

    int knockList[kNumRegions], knocks = 0;
    for (int r = 0; r < kNumRegions; ++r)
        if (knockRegions & (1u << r))
            knockList[knocks++] = r;

    for (int q = 0; q < b.count; q += 4)
    {
        simd::f4 re = simd::load (zr + q), im = simd::load (zi + q);
        const simd::f4 ar = simd::load (pr + q), ai = simd::load (pi + q);
        const simd::f4 clr = simd::load (lr + q), cli = simd::load (li + q), crr = simd::load (rr + q), cri = simd::load (ri + q);

        simd::f4 knock[kNumRegions];
        for (int j = 0; j < knocks; ++j)
            knock[j] = simd::load (&model->knock[knockList[j]][(size_t) q]);

        const bool thump = thumpActive && q < kThumpModes;
        const bool bus = anySympathetic && q < model->boardBusCount;
        simd::f4 weights[kNumRegions];
        if (bus)
            for (int r = 0; r < kNumRegions; ++r)
                weights[r] = simd::load (&model->boardBus[r][(size_t) q]);

        for (int i = 0; i < n; ++i)
        {
            for (int j = 0; j < knocks; ++j)
                re = simd::fmaScalar (re, knock[j], knockForce[knockList[j]][i]);
            if (thump)
                re = simd::add (re, simd::load (&thumpIn[i][q]));
            const simd::f4 nr = simd::fms (simd::mul (re, ar), im, ai);
            im = simd::fma (simd::mul (re, ai), im, ar);
            re = nr;
            accL[i] = simd::fms (simd::fma (accL[i], clr, re), cli, im);
            accR[i] = simd::fms (simd::fma (accR[i], crr, re), cri, im);
            if (bus)
                for (int r = 0; r < kNumRegions; ++r)
                    regionAcc[r][i] = simd::fma (regionAcc[r][i], weights[r], im);
        }
        simd::store (zr + q, re); simd::store (zi + q, im);
    }

    if (anySympathetic)
        for (int r = 0; r < kNumRegions; ++r)
            for (int i = 0; i < n; ++i)
                regionNew[r][i] += simd::sum (regionAcc[r][i]);
}

void ReferenceEngine::prune (Voice& v)
{
    int a = v.active;
    while (a > 0 && ! anyAudible (v.zr.data(), v.zi.data(), v.gate.data(), a - 4, a))
    {
        a -= 4;
        std::memset (&v.zr[(size_t) a], 0, 4 * sizeof (float));
        std::memset (&v.zi[(size_t) a], 0, 4 * sizeof (float));
    }
    v.active = a;
}

void ReferenceEngine::processChunk (float* left, float* right, int n)
{
    const float Ts = (float) (1.0 / model->sampleRate);
    const simd::f4 zero = simd::set (0.0f);
    for (int i = 0; i < n; ++i) { accL[i] = zero; accR[i] = zero; longL[i] = 0.0f; longR[i] = 0.0f; }
    std::memset (regionNew, 0, sizeof (regionNew));

    // Bridge motion each region's strings feel: every region's motion one chunk length ago.
    float regionIn[kNumRegions][kChunk];
    float regionPeak[kNumRegions] {};
    const float amount = settings.sympathetic * Ts;
    for (int a = 0; a < kNumRegions; ++a)
        for (int i = 0; i < n; ++i)
        {
            const int p = (ringPosition + i) & (kChunk - 1);
            float sum = 0.0f;
            for (int b = 0; b < kNumRegions; ++b)
                sum += model->regionGain[a][b] * regionRing[b][p];
            regionIn[a][i] = amount * sum;
            regionPeak[a] = std::max (regionPeak[a], std::fabs (regionIn[a][i]));
        }

    knockRegions = 0;
    thumpActive = false;
    bool sympatheticNow = false;
    int live = boardAwake ? model->board.count : 0;

    for (int index = 0; index < kNumNotes; ++index)
    {
        auto& v = voices[index];
        const auto& note = *v.note;

        const float step = v.damperRate * (float) n;
        if (v.damper < v.damperTarget)
            v.damper = std::min (v.damperTarget, v.damper + step);
        else if (v.damper > v.damperTarget)
            v.damper = std::max (v.damperTarget, v.damper - step);
        if (std::fabs (v.damper - v.appliedDamper) > 1.0e-4f)
            applyDamper (v);

        const bool open = v.damper < 0.999f;
        const bool busy = v.contact || v.active > 0 || v.sympatheticLive || open || v.knock > 0.0f;
        if (! busy)
        {
            for (int i = 0; i < n; ++i)
                v.own[(ringPosition + i) & (kChunk - 1)] = 0.0f;
            continue;
        }

        for (int i = 0; i < n; ++i) accOwn[i] = zero;

        if (v.contact)
        {
            runContact (v, n);
        }
        else if (v.active > 0)
        {
            if (note.longGain > 0.0f && settings.phantoms > 0.0f)
                runLongitudinal (v, n);
            runFree (v, n);
        }

        if (v.knock > 0.0f)
        {
            const uint32_t bit = 1u << note.region;
            if (! (knockRegions & bit)) { std::memset (knockForce[note.region], 0, sizeof (knockForce[0])); knockRegions |= bit; }
            float* force = knockForce[note.region];
            const int length = v.knockLength;
            for (int i = 0; i < n && v.knockPosition < length; ++i, ++v.knockPosition)
                if (v.knockPosition >= 0)
                {
                    const float s = std::sin ((float) kPi * (float) v.knockPosition / (float) length);
                    force[i] += v.knock * s * s;
                }
            if (v.knockPosition >= length)
                v.knock = 0.0f;
        }

        // Sympathetic strings hear every region except this note's own struck strings.
        const float self = model->regionGain[note.region][note.region] * amount;
        if (note.sympathetic.count > 0 && (v.sympatheticLive || (open && regionPeak[note.region] > 0.0f)))
        {
            float input[kChunk];
            for (int i = 0; i < n; ++i)
                input[i] = regionIn[note.region][i] - self * v.own[(ringPosition + i) & (kChunk - 1)];
            runSympathetic (v, input, n);
            v.sympatheticLive = true;
            live += note.sympathetic.count;
        }
        sympatheticNow |= open;

        for (int i = 0; i < n; ++i)
        {
            const float own = simd::sum (accOwn[i]);
            v.own[(ringPosition + i) & (kChunk - 1)] = own;
            regionNew[note.region][i] += own;
        }

        if (--v.checkCountdown <= 0)
        {
            v.checkCountdown = kCheckChunks;
            if (! v.contact)
                prune (v);
            if (v.sympatheticLive && ! anyAudible (v.szr.data(), v.szi.data(), v.sgate.data(), 0, note.sympathetic.count)
                && ! (open && regionPeak[note.region] > 1.0e-18f))
            {
                v.sympatheticLive = false;
                std::fill (v.szr.begin(), v.szr.end(), 0.0f);
                std::fill (v.szi.begin(), v.szi.end(), 0.0f);
            }
        }
        live += v.active;
    }

    anySympathetic = sympatheticNow;

    boardAwake |= knockRegions != 0 || thumpActive;
    if (boardAwake)
    {
        runBoard (n);
        if (knockRegions == 0 && ! thumpActive && ++boardCheck >= kCheckChunks)
        {
            boardCheck = 0;
            if (! anyAudible (boardRe.data(), boardIm.data(), boardGate.data(), 0, model->board.count))
            {
                boardAwake = false;
                std::fill (boardRe.begin(), boardRe.end(), 0.0f);
                std::fill (boardIm.begin(), boardIm.end(), 0.0f);
            }
        }
    }

    for (int r = 0; r < kNumRegions; ++r)
        for (int i = 0; i < n; ++i)
            regionRing[r][(ringPosition + i) & (kChunk - 1)] = regionNew[r][i];
    ringPosition = (ringPosition + n) & (kChunk - 1);

    const float gain = settings.volume * model->outputGain;
    for (int i = 0; i < n; ++i)
    {
        left[i] = gain * (simd::sum (accL[i]) + longL[i]);
        right[i] = gain * (simd::sum (accR[i]) + longR[i]);
    }
    liveResonators = live;
}

void ReferenceEngine::process (float* left, float* right, int numSamples)
{
    if (model == nullptr)
    {
        std::memset (left, 0, sizeof (float) * (size_t) numSamples);
        std::memset (right, 0, sizeof (float) * (size_t) numSamples);
        return;
    }

    const FlushDenormals flush;
    for (int done = 0; done < numSamples;)
    {
        const int n = std::min (kChunk, numSamples - done);
        processChunk (left + done, right + done, n);
        done += n;
    }
}

} // namespace pm
