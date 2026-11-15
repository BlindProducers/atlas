// SPDX-License-Identifier: GPL-3.0-or-later
#include "piano_instrument.h"

#include "Hammer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace pm
{

namespace
{
    using simd::f4;

    constexpr float kAudible = 3.16e-6f;         // -110 dB re full scale: quieter resonators are switched off
    // A note below -90 dB re full scale no longer holds its voice open; what is left of it is dropped.
    constexpr float kHoldRatio = 100.0f;         // (10^(-90/20) / kAudible)^2
    constexpr int kCheckChunks = 16;             // how often a note looks for resonators to switch off
    constexpr double kKnockSeconds = 0.0008;     // length of the action knock's force pulse at the speed it was measured at, 1.5 m/s
    constexpr double kKnockSpeed = 1.5;
    constexpr uint32_t kRandomSeed = 0x9E3779B9u;
    constexpr float kDamperClosed = 0.999f;

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

    /** Whether any resonator in [first, last) holds more than `threshold` times the audible power. */
    bool anyAbove (const float* zr, const float* zi, const float* gate, int first, int last, float threshold = 1.0f)
    {
        for (int q = first; q < last; q += 4)
        {
            const f4 re = simd::load (zr + q), im = simd::load (zi + q);
            const f4 power = simd::mul (simd::fma (simd::mul (re, re), im, im), simd::load (gate + q));
            if (simd::maxLane (power) > threshold)
                return true;
        }
        return false;
    }

    Instrument::LongitudinalFilters longitudinalFilters (const NoteModel& note, float sampleRate)
    {
        Instrument::LongitudinalFilters c;
        for (int m = 0; m < 2; ++m)
        {
            c.g[m] = note.longHz[m] > 0.0f ? std::tan ((float) kPi * note.longHz[m] / sampleRate) : 0.0f;
            c.a1[m] = 1.0f / (1.0f + c.g[m] * (c.g[m] + Instrument::LongitudinalFilters::damping));
            c.a2[m] = c.g[m] * c.a1[m];
            c.a3[m] = c.g[m] * c.a2[m];
        }
        c.dcRate = 1.0f - std::exp (-2.0f * (float) kPi * 40.0f / sampleRate);
        c.fadeRate = 1.0f - std::exp (-1.0f / (0.006f * sampleRate));       // the board answers it within a few milliseconds
        return c;
    }

    int groups (int count) { return count & ~3; }
}

//==============================================================================
std::shared_ptr<const Instrument::Prepared> Instrument::Prepared::make (std::shared_ptr<const PianoModel> model)
{
    auto p = std::make_shared<Prepared>();
    const auto rate = (float) model->sampleRate;
    for (int i = 0; i < kNumNotes; ++i)
    {
        const NoteModel& note = model->notes[i];
        p->mainCount[i] = groups (std::min (note.main.count, kBankCapacity));
        p->sympatheticCount[i] = groups (std::min (note.sympathetic.count, kSympatheticCapacity));
        makeGate (note.main, model->outputGain, p->mainGate[i]);
        makeGate (note.sympathetic, model->outputGain, p->sympatheticGate[i]);
        p->longitudinal[i] = longitudinalFilters (note, rate);
    }
    p->boardCount = groups (std::min (model->board.count, kBoardCapacity));
    p->plateCount = std::clamp (groups (model->board.used - kCaseModes), 0, p->boardCount);
    makeGate (model->board, model->outputGain, p->boardGate);
    p->model = std::move (model);
    return p;
}

Instrument::Lane::Lane()
{
    zr.assign (kBankCapacity, 0.0f); zi.assign (kBankCapacity, 0.0f);
    pr.assign (kBankCapacity, 0.0f); pi.assign (kBankCapacity, 0.0f);
    boardRe.assign (kBoardCapacity, 0.0f); boardIm.assign (kBoardCapacity, 0.0f);
}

Instrument::Instrument() : random (kRandomSeed)
{
    handOver.fill (-1);
    ownHistory.assign ((size_t) kNumNotes * (kChunk + kMaxBlock), 0.0f);
    for (auto& n : notes)
    {
        n.szr.assign (kSympatheticCapacity, 0.0f); n.szi.assign (kSympatheticCapacity, 0.0f);
        n.spr.assign (kSympatheticCapacity, 0.0f); n.spi.assign (kSympatheticCapacity, 0.0f);
    }
}

Instrument::~Instrument()
{
    delete incoming.exchange (nullptr);
    delete outgoing.exchange (nullptr);
}

//==============================================================================
void Instrument::offer (std::shared_ptr<const Prepared> next)
{
    delete incoming.exchange (new std::shared_ptr<const Prepared> (std::move (next)));
}

void Instrument::collect()
{
    delete outgoing.exchange (nullptr);
}

void Instrument::takeOffer()
{
    // The builder collects what was handed back before it offers again, so the slot is free.
    if (outgoing.load (std::memory_order_acquire) != nullptr)
        return;
    std::shared_ptr<const Prepared>* box = incoming.exchange (nullptr);
    if (box == nullptr)
        return;

    std::swap (retired, prepared);       // retired: the model the lanes fade out with
    prepared.swap (*box);                // box: the one before, for the builder to free
    outgoing.store (box, std::memory_order_release);

    // Every sympathetic bank starts afresh with the new tables; held keys keep their dampers up.
    const PianoModel& model = *prepared->model;
    for (int i = 0; i < kNumNotes; ++i)
    {
        BodyNote& b = notes[(size_t) i];
        const NoteModel& note = model.notes[i];
        std::fill (b.szr.begin(), b.szr.end(), 0.0f); std::fill (b.szi.begin(), b.szi.end(), 0.0f);
        const auto count = (size_t) prepared->sympatheticCount[i];
        std::copy_n (note.sympathetic.poleRe.begin(), count, b.spr.begin());
        std::copy_n (note.sympathetic.poleIm.begin(), count, b.spi.begin());
        b.sympatheticLive = false;
        b.damper = b.damperTarget = note.hasDamper ? (b.heldLanes > 0 ? 0.0f : 1.0f - bodyLift) : 0.0f;
        b.appliedDamper = -1.0f;
        b.checkCountdown = i % kCheckChunks;
    }
    anySympathetic = false;
}

void Instrument::want (const Wanted& w)
{
    wantedSequence.fetch_add (1, std::memory_order_release);          // odd: being written
    wantedValues[0].store (w.sampleRate, std::memory_order_relaxed);
    wantedValues[1].store (w.a4, std::memory_order_relaxed);
    wantedValues[2].store (w.stretch, std::memory_order_relaxed);
    wantedValues[3].store (w.unison, std::memory_order_relaxed);
    wantedValues[4].store (w.sustain, std::memory_order_relaxed);
    wantedSequence.fetch_add (1, std::memory_order_release);
}

bool Instrument::readWanted (Wanted& w) const
{
    const uint32_t before = wantedSequence.load (std::memory_order_acquire);
    if (before & 1u)
        return false;
    w.sampleRate = wantedValues[0].load (std::memory_order_relaxed);
    w.a4 = wantedValues[1].load (std::memory_order_relaxed);
    w.stretch = wantedValues[2].load (std::memory_order_relaxed);
    w.unison = wantedValues[3].load (std::memory_order_relaxed);
    w.sustain = wantedValues[4].load (std::memory_order_relaxed);
    std::atomic_thread_fence (std::memory_order_acquire);
    return wantedSequence.load (std::memory_order_relaxed) == before;
}

void Instrument::openBlock (int blockSize)
{
    if (blockOpen && blockSamples == blockSize)
        return;
    blockSize = std::clamp (blockSize, 1, kMaxBlock);
    for (auto& region : regionHistory)
        std::fill (region + kChunk, region + kChunk + blockSize, 0.0f);
    blockOpen = true;
    blockSamples = blockSize;
}

float Instrument::unit()
{
    random ^= random << 13; random ^= random >> 17; random ^= random << 5;      // xorshift32
    return (float) (int32_t) random * (1.0f / 2147483648.0f);
}

//==============================================================================
void Instrument::keyDown (int note)
{
    BodyNote& b = notes[(size_t) note];
    ++b.heldLanes;
    b.damper = b.damperTarget = 0.0f;       // the key has lifted the damper well before the hammer arrives
}

void Instrument::keyUp (int note, float lift, float dampersLift)
{
    BodyNote& b = notes[(size_t) note];
    if (b.heldLanes > 0)
        --b.heldLanes;
    if (prepared == nullptr || b.heldLanes > 0 || b.heldBySostenuto || ! prepared->model->notes[note].hasDamper)
        return;
    b.damperTarget = 1.0f - dampersLift;
    const float seconds = 0.06f - 0.045f * std::clamp (lift, 0.0f, 127.0f) / 127.0f;
    b.damperRate = 1.0f / (seconds * (float) prepared->model->sampleRate);
}

namespace
{
    /** How far the dampers have left the strings for a sustain pedal position: over the middle of the travel. */
    float dampersLift (float sustain)
    {
        const float t = std::clamp ((sustain - 0.3f) / 0.4f, 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }
}

void Instrument::pedalEvent (Pedal pedal, float value, int sample)
{
    PedalEvent e;
    e.sample = std::max (0, sample);
    e.pedal = pedal;
    e.value = std::clamp (value, 0.0f, 1.0f);
    if (numPedalEvents == kMaxPedalEvents)
        --numPedalEvents;                            // too many in one block: the last one gives way
    int at = numPedalEvents++;
    while (at > 0 && pedalEvents[(size_t) at - 1].sample > e.sample)
    {
        pedalEvents[(size_t) at] = pedalEvents[(size_t) at - 1];
        --at;
    }
    pedalEvents[(size_t) at] = e;
}

Instrument::Pedals Instrument::pedalsAt (int sample) const
{
    float sustain = sustainStart, soft = softStart;
    bool sostenuto = sostenutoStart;
    for (int i = 0; i < numPedalEvents && pedalEvents[(size_t) i].sample <= sample; ++i)
    {
        const PedalEvent& e = pedalEvents[(size_t) i];
        if (e.pedal == kSustainPedal) sustain = e.value;
        else if (e.pedal == kSostenutoPedal) sostenuto = e.value >= 0.5f;
        else soft = e.value;
    }
    Pedals p;
    p.lift = dampersLift (sustain);
    p.sostenuto = sostenuto;
    p.soft = soft;
    return p;
}

int Instrument::nextPedalEvent (int sample) const
{
    for (int i = 0; i < numPedalEvents; ++i)
        if (pedalEvents[(size_t) i].sample > sample)
            return pedalEvents[(size_t) i].sample;
    return kMaxBlock;
}

void Instrument::syncPedals (Lane& v, int sample)
{
    const Pedals p = pedalsAt (sample);
    if (p.sostenuto && ! v.lastSostenuto && v.keyDown)
        v.heldBySostenuto = true;
    const bool sostenutoReleased = ! p.sostenuto && v.lastSostenuto;
    if (sostenutoReleased)
        v.heldBySostenuto = false;
    if ((p.lift != v.lastLift || sostenutoReleased) && ! v.keyDown && ! v.heldBySostenuto
        && v.prepared->model->notes[v.note].hasDamper)
    {
        v.damperTarget = 1.0f - p.lift;
        v.damperRate = 1.0f / (0.03f * (float) v.prepared->model->sampleRate);
    }
    v.lastLift = p.lift;
    v.lastSostenuto = p.sostenuto;
}

//==============================================================================
/** A knock on the body: a half-sine-squared force pulse whose impulse grows with the speed of the blow as
    measured, spread over `seconds` (a slower key lands with a longer, duller pulse), and never quite the same
    twice. */
void Instrument::startKnock (Lane& v, double speed, double seconds, float spread, int delaySamples, float actionNoise)
{
    const PianoModel& model = *v.prepared->model;
    seconds *= 1.0 + 0.15 * unit();
    const double impulse = std::pow (speed, (double) model.knockPower) * kKnockSeconds / seconds;
    v.knockLength = std::max (4, (int) (seconds * model.sampleRate));
    v.knock = actionNoise * model.knockNewtons * model.notes[v.note].knockGain * (float) impulse * (1.0f + spread * unit());
    v.knockPosition = -delaySamples;
}

void Instrument::strike (Lane& v, int note, float velocity, const Blow& blow, int fadeSamples, int sample)
{
    if (prepared == nullptr || note < 0 || note >= kNumNotes)
    {
        clear (v);
        return;
    }
    if (v.note >= 0 && (v.note != note || v.prepared != prepared.get()))
        fadeOut (v, fadeSamples);
    if (Lane* other = owners[(size_t) note]; other != nullptr && other != &v)
        clear (*other);          // takeOver was not asked: the other lane's strings are dropped

    const PianoModel& model = *prepared->model;
    const NoteModel& nm = model.notes[note];
    const int count = prepared->mainCount[note];
    if (v.note < 0)
    {
        v.prepared = prepared.get();
        v.note = note;
        std::fill_n (v.zr.begin(), count, 0.0f); std::fill_n (v.zi.begin(), count, 0.0f);
        std::fill_n (v.boardRe.begin(), prepared->boardCount, 0.0f);
        std::fill_n (v.boardIm.begin(), prepared->boardCount, 0.0f);
        v.active = 0; v.contact = false; v.knock = 0.0f;
        v.boardActive = 0; v.caseActive = false;
        std::memset (v.longFilter, 0, sizeof (v.longFilter)); v.longDc = 0.0f; v.longFade = 0.0f;
        v.keyDown = false; v.heldBySostenuto = false;
        v.damper = v.damperTarget = nm.hasDamper ? 1.0f : 0.0f;
        v.damperRate = 0.0f;
        v.appliedDamper = -1.0f;
        v.checkCountdown = note % kCheckChunks;
    }
    std::copy_n (nm.main.poleRe.begin(), count, v.pr.begin());
    std::copy_n (nm.main.poleIm.begin(), count, v.pi.begin());
    v.appliedDamper = -1.0f;

    if (! v.keyDown)
        keyDown (note);
    v.keyDown = true;
    v.damper = v.damperTarget = 0.0f;
    // A note struck now is not held by a sostenuto pressed before it.
    const Pedals pedals = pedalsAt (sample);
    v.lastLift = pedals.lift;
    v.lastSostenuto = pedals.sostenuto;

    const float shaped = 64.0f + (std::clamp (velocity, 1.0f, 127.0f) - 64.0f) * blow.dynamics;
    // The soft pedal shifts the hammers onto less compacted felt: a gentler, slightly slower blow.
    const double speed = hammerVelocity (shaped) * (1.0 - 0.2 * pedals.soft);
    v.hardness = (1.0f - 0.45f * pedals.soft) * blow.hardness;

    // The hammer starts touching the string where the string is now, so a note that is still
    // sounding is struck as it moves.
    float string = 0.0f;
    for (int r = 0; r < count; ++r)
        string += nm.main.hammerRe[(size_t) r] * v.zr[(size_t) r] - nm.main.hammerIm[(size_t) r] * v.zi[(size_t) r];
    v.stringAtHammer = string;
    v.hammerPosition = string;
    v.hammerVelocity = speed;
    v.lastCompression = 0.0;
    v.contact = true;
    v.contactSamples = 0;
    if (v.active == 0)
        v.longFade = 0.0f;       // a string at rest: its longitudinal sound comes in with the note
    v.active = count;
    v.audible = true;
    owners[(size_t) note] = &v;

    // The key reaches the keybed as the hammer arrives; a slow key lands with a longer, duller pulse.
    startKnock (v, speed, std::clamp (kKnockSeconds * std::pow (kKnockSpeed / speed, 0.2), 0.0006, 0.0015), 0.2f, 0,
                blow.actionNoise);
}

void Instrument::announceStrike (int note, int sample)
{
    if (note >= 0 && note < kNumNotes && owners[(size_t) note] != nullptr)
        handOver[(size_t) note] = std::max (0, sample);
}

int Instrument::takeOver (Lane& v, int note, int fadeSamples)
{
    if (prepared == nullptr || note < 0 || note >= kNumNotes)
        return -1;
    Lane* other = owners[(size_t) note];
    if (other == nullptr || other == &v || other->note != note)
        return -1;
    // Strings on an old model, or left behind by a voice that stopped rendering, are dropped.
    if (other->prepared != prepared.get() || (other->renderedBlock != block && other->renderedBlock != block - 1))
    {
        clear (*other);
        return -1;
    }
    if (v.note >= 0)
        fadeOut (v, fadeSamples);
    std::swap (*other, v);           // the strings change hands; each lane keeps its own fade slot
    std::swap (other->fade, v.fade);
    owners[(size_t) note] = &v;
    return v.renderedBlock == block ? v.renderedTo : 0;
}

void Instrument::release (Lane& v, float lift, float actionNoise, int sample)
{
    if (v.note < 0 || v.prepared == nullptr)
        return;
    const PianoModel& model = *v.prepared->model;
    const NoteModel& nm = model.notes[v.note];
    const Pedals pedals = pedalsAt (sample);
    if (v.keyDown && ! v.contact)
    {
        // The key coming back to rest: sooner and harder after a quick release, as a soft, longer pulse.
        const double quick = std::clamp (lift, 0.0f, 127.0f) / 127.0;
        const double speed = 0.034 + 0.105 * quick * quick;
        const double delay = (0.11 - 0.05 * quick) * (1.0 + 0.15 * unit());
        startKnock (v, speed, 0.002, 0.3f, (int) (delay * model.sampleRate), actionNoise);
    }
    if (v.keyDown)
    {
        v.keyDown = false;
        keyUp (v.note, lift, pedals.lift);
    }
    if (! v.heldBySostenuto && nm.hasDamper)
    {
        v.damperTarget = 1.0f - pedals.lift;
        const float seconds = 0.06f - 0.045f * std::clamp (lift, 0.0f, 127.0f) / 127.0f;
        v.damperRate = 1.0f / (seconds * (float) model.sampleRate);
    }
}

void Instrument::clear (Lane& v)
{
    if (v.note >= 0 && owners[(size_t) v.note] == &v)
        owners[(size_t) v.note] = nullptr;
    if (v.keyDown && v.note >= 0)
        keyUp (v.note, 64.0f, pedalsAt (kMaxBlock).lift);
    dropFade (v);
    v.note = -1;
    v.prepared = nullptr;
    v.contact = false; v.active = 0; v.knock = 0.0f;
    v.boardActive = 0; v.caseActive = false;
    v.keyDown = false; v.heldBySostenuto = false;
    v.audible = false;
}

void Instrument::fadeOut (Lane& v, int samples)
{
    int slot = -1;
    for (int i = 0; i < kFadeSlots; ++i)
        if (fades[(size_t) i].lane.note < 0) { slot = i; break; }
    if (slot < 0)
    {
        clear (v);
        return;
    }
    dropFade (v);                        // a fade still running is cut short by the newer one
    if (owners[(size_t) v.note] == &v)
        owners[(size_t) v.note] = nullptr;
    FadeSlot& f = fades[(size_t) slot];
    std::swap (f.lane, v);               // the vectors change hands without copying
    v.fade = slot;
    f.lane.fade = -1;
    if (f.lane.keyDown)
    {
        f.lane.keyDown = false;          // the key belongs to the new note now
        keyUp (f.lane.note, 64.0f, pedalsAt (kMaxBlock).lift);
    }
    f.length = std::max (1, samples);
    f.remaining = f.length;
}

void Instrument::dropFade (Lane& v)
{
    if (v.fade < 0)
        return;
    FadeSlot& f = fades[(size_t) v.fade];
    f.lane.note = -1;
    f.lane.prepared = nullptr;
    f.lane.contact = false; f.lane.active = 0; f.lane.knock = 0.0f;
    f.lane.boardActive = 0; f.lane.caseActive = false;
    f.remaining = 0;
    v.fade = -1;
}

//==============================================================================
void Instrument::applyDamper (Lane& v)
{
    const NoteModel& nm = v.prepared->model->notes[v.note];
    const Bank& bank = nm.main;
    const f4 amount = simd::set (v.damper), one = simd::set (1.0f);
    const int count = v.prepared->mainCount[v.note];
    for (int q = 0; q < count; q += 4)
    {
        const f4 f = simd::fms (one, amount, simd::load (&bank.damping[(size_t) q]));
        simd::store (&v.pr[(size_t) q], simd::mul (simd::load (&bank.poleRe[(size_t) q]), f));
        simd::store (&v.pi[(size_t) q], simd::mul (simd::load (&bank.poleIm[(size_t) q]), f));
    }
    v.appliedDamper = v.damper;
}

void Instrument::applyDamper (BodyNote& b, int index)
{
    const Bank& bank = prepared->model->notes[index].sympathetic;
    const f4 amount = simd::set (b.damper), one = simd::set (1.0f);
    const int count = prepared->sympatheticCount[index];
    for (int q = 0; q < count; q += 4)
    {
        const f4 f = simd::fms (one, amount, simd::load (&bank.damping[(size_t) q]));
        simd::store (&b.spr[(size_t) q], simd::mul (simd::load (&bank.poleRe[(size_t) q]), f));
        simd::store (&b.spi[(size_t) q], simd::mul (simd::load (&bank.poleIm[(size_t) q]), f));
    }
    b.appliedDamper = b.damper;
}

void Instrument::runContact (Lane& v, int n, f4* accL, f4* accR, f4* accOwn, float* longL, float* longR,
                             float (*thumpIn)[kThumpModes], bool& thumpActive)
{
    const PianoModel& model = *v.prepared->model;
    const NoteModel& note = model.notes[v.note];
    const Bank& b = note.main;
    const int count = v.prepared->mainCount[v.note], steps = note.substeps;
    const double dt = 1.0 / (model.sampleRate * steps);
    const double stiffness = (double) note.hammerK * v.hardness;
    const double exponent = note.hammerP, hysteresis = note.hammerHysteresis;
    const double pull = (double) note.stringsHit / note.hammerMass;

    const float longGain = v.phantoms * note.longGain;
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
            const f4 in = simd::set ((float) (dt * f));
            f4 string = simd::set (0.0f);

            if (s + 1 < steps)
            {
                for (int q = 0; q < count; q += 4)
                {
                    const f4 re = simd::add (simd::load (zr + q), in), im = simd::load (zi + q);
                    const f4 pr = simd::load (psr + q), pi = simd::load (psi + q);
                    const f4 nr = simd::fms (simd::mul (re, pr), im, pi);
                    const f4 ni = simd::fma (simd::mul (re, pi), im, pr);
                    simd::store (zr + q, nr); simd::store (zi + q, ni);
                    string = simd::fms (simd::fma (string, simd::load (hr + q), nr), simd::load (hi + q), ni);
                }
            }
            else
            {
                f4 l = simd::set (0.0f), r = l, own = l;
                for (int q = 0; q < count; q += 4)
                {
                    const f4 re = simd::add (simd::load (zr + q), in), im = simd::load (zi + q);
                    const f4 pr = simd::load (psr + q), pi = simd::load (psi + q);
                    const f4 nr = simd::fms (simd::mul (re, pr), im, pi);
                    const f4 ni = simd::fma (simd::mul (re, pi), im, pr);
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
            if (! thumpActive) { std::memset (thumpIn, 0, sizeof (float) * kChunk * kThumpModes); thumpActive = true; }
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
    if (v.contactSamples > (int) (0.05 * model.sampleRate))
        v.contact = false;
}

void Instrument::runFree (Lane& v, int n, f4* accL, f4* accR, f4* accOwn)
{
    const Bank& b = v.prepared->model->notes[v.note].main;
    float* zr = v.zr.data(); float* zi = v.zi.data();
    const float *pr = v.pr.data(), *pi = v.pi.data();
    const float *lr = b.outRe[0].data(), *li = b.outIm[0].data(), *rr = b.outRe[1].data(), *ri = b.outIm[1].data();
    const float *ur = b.busRe.data(), *ui = b.busIm.data();

    for (int q = 0; q < v.active; q += 4)
    {
        f4 re = simd::load (zr + q), im = simd::load (zi + q);
        const f4 ar = simd::load (pr + q), ai = simd::load (pi + q);
        const f4 clr = simd::load (lr + q), cli = simd::load (li + q), crr = simd::load (rr + q), cri = simd::load (ri + q);
        const f4 cur = simd::load (ur + q), cui = simd::load (ui + q);
        for (int i = 0; i < n; ++i)
        {
            const f4 nr = simd::fms (simd::mul (re, ar), im, ai);
            im = simd::fma (simd::mul (re, ai), im, ar);
            re = nr;
            accL[i] = simd::fms (simd::fma (accL[i], clr, re), cli, im);
            accR[i] = simd::fms (simd::fma (accR[i], crr, re), cri, im);
            accOwn[i] = simd::fms (simd::fma (accOwn[i], cur, re), cui, im);
        }
        simd::store (zr + q, re); simd::store (zi + q, im);
    }
}

void Instrument::runSympathetic (BodyNote& b, int index, const float* input, int n, f4* accL, f4* accR)
{
    const Bank& bank = prepared->model->notes[index].sympathetic;
    const int count = prepared->sympatheticCount[index];
    float* zr = b.szr.data(); float* zi = b.szi.data();
    const float *pr = b.spr.data(), *pi = b.spi.data();
    const float *lr = bank.outRe[0].data(), *li = bank.outIm[0].data(), *rr = bank.outRe[1].data(), *ri = bank.outIm[1].data();

    for (int q = 0; q < count; q += 4)
    {
        f4 re = simd::load (zr + q), im = simd::load (zi + q);
        const f4 ar = simd::load (pr + q), ai = simd::load (pi + q);
        const f4 clr = simd::load (lr + q), cli = simd::load (li + q), crr = simd::load (rr + q), cri = simd::load (ri + q);
        for (int i = 0; i < n; ++i)
        {
            re = simd::add (re, simd::set (input[i]));
            const f4 nr = simd::fms (simd::mul (re, ar), im, ai);
            im = simd::fma (simd::mul (re, ai), im, ar);
            re = nr;
            accL[i] = simd::fms (simd::fma (accL[i], clr, re), cli, im);
            accR[i] = simd::fms (simd::fma (accR[i], crr, re), cri, im);
        }
        simd::store (zr + q, re); simd::store (zi + q, im);
    }
}

float Instrument::longitudinalSample (Lane& v, const float* u, int partials)
{
    const LongitudinalFilters& c = v.prepared->longitudinal[v.note];
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

void Instrument::runLongitudinal (Lane& v, int n, float* longL, float* longR)
{
    const NoteModel& note = v.prepared->model->notes[v.note];
    const int partials = (int) note.longIndex.size();
    float zr[kLongPartials + 2], zi[kLongPartials + 2], pr[kLongPartials + 2], pi[kLongPartials + 2], u[kLongPartials + 2];

    // The strongest resonator of each partial, followed from its state at the start of the chunk.
    for (int k = 0; k < partials; ++k)
    {
        const auto r = (size_t) note.longIndex[(size_t) k];
        zr[k] = v.zr[r]; zi[k] = v.zi[r]; pr[k] = v.pr[r]; pi[k] = v.pi[r];
    }
    u[partials] = u[partials + 1] = 0.0f;

    const float gain = v.phantoms * note.longGain;
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

void Instrument::runBoard (Lane& v, int n, const float* knockForce, const float (*thumpIn)[kThumpModes], bool thumpActive,
                           bool bus, f4* accL, f4* accR, f4 (*regionAcc)[kChunk])
{
    const PianoModel& model = *v.prepared->model;
    const Bank& b = model.board;
    const int region = model.notes[v.note].region;
    float* zr = v.boardRe.data(); float* zi = v.boardIm.data();
    const float *pr = b.poleRe.data(), *pi = b.poleIm.data();
    const float *lr = b.outRe[0].data(), *li = b.outIm[0].data(), *rr = b.outRe[1].data(), *ri = b.outIm[1].data();
    const float* knockWeights = model.knock[region].data();
    if (bus)
        for (int r = 0; r < kNumRegions; ++r)
            for (int i = 0; i < n; ++i)
                regionAcc[r][i] = simd::set (0.0f);

    auto run = [&] (int first, int last)
    {
        for (int q = first; q < last; q += 4)
        {
            f4 re = simd::load (zr + q), im = simd::load (zi + q);
            const f4 ar = simd::load (pr + q), ai = simd::load (pi + q);
            const f4 clr = simd::load (lr + q), cli = simd::load (li + q), crr = simd::load (rr + q), cri = simd::load (ri + q);
            const f4 knock = knockForce != nullptr ? simd::load (knockWeights + q) : simd::set (0.0f);

            const bool thump = thumpActive && q < kThumpModes;
            const bool drive = bus && q < model.boardBusCount;
            f4 weights[kNumRegions];
            if (drive)
                for (int r = 0; r < kNumRegions; ++r)
                    weights[r] = simd::load (&model.boardBus[r][(size_t) q]);

            for (int i = 0; i < n; ++i)
            {
                if (knockForce != nullptr)
                    re = simd::fmaScalar (re, knock, knockForce[i]);
                if (thump)
                    re = simd::add (re, simd::load (&thumpIn[i][q]));
                const f4 nr = simd::fms (simd::mul (re, ar), im, ai);
                im = simd::fma (simd::mul (re, ai), im, ar);
                re = nr;
                accL[i] = simd::fms (simd::fma (accL[i], clr, re), cli, im);
                accR[i] = simd::fms (simd::fma (accR[i], crr, re), cri, im);
                if (drive)
                    for (int r = 0; r < kNumRegions; ++r)
                        regionAcc[r][i] = simd::fma (regionAcc[r][i], weights[r], im);
            }
            simd::store (zr + q, re); simd::store (zi + q, im);
        }
    };
    run (0, v.boardActive);
    if (v.caseActive)
        run (v.prepared->plateCount, v.prepared->boardCount);
}

void Instrument::prune (Lane& v)
{
    const float* gate = v.prepared->mainGate[v.note].data();
    int a = v.active;
    while (a > 0 && ! anyAbove (v.zr.data(), v.zi.data(), gate, a - 4, a))
    {
        a -= 4;
        std::memset (&v.zr[(size_t) a], 0, 4 * sizeof (float));
        std::memset (&v.zi[(size_t) a], 0, 4 * sizeof (float));
    }
    v.active = a;
}

void Instrument::pruneBoard (Lane& v)
{
    const float* gate = v.prepared->boardGate.data();
    int a = v.boardActive;
    while (a > 0 && ! anyAbove (v.boardRe.data(), v.boardIm.data(), gate, a - 4, a))
    {
        a -= 4;
        std::memset (&v.boardRe[(size_t) a], 0, 4 * sizeof (float));
        std::memset (&v.boardIm[(size_t) a], 0, 4 * sizeof (float));
    }
    v.boardActive = a;
    const int first = v.prepared->plateCount, last = v.prepared->boardCount;
    if (v.caseActive && ! anyAbove (v.boardRe.data(), v.boardIm.data(), gate, first, last))
    {
        v.caseActive = false;
        std::fill (v.boardRe.begin() + first, v.boardRe.begin() + last, 0.0f);
        std::fill (v.boardIm.begin() + first, v.boardIm.begin() + last, 0.0f);
    }
}

void Instrument::checkAudible (Lane& v)
{
    const float* gate = v.prepared->boardGate.data();
    v.audible = v.contact || v.knock > 0.0f
                || anyAbove (v.zr.data(), v.zi.data(), v.prepared->mainGate[v.note].data(), 0, v.active, kHoldRatio)
                || anyAbove (v.boardRe.data(), v.boardIm.data(), gate, 0, v.boardActive, kHoldRatio)
                || (v.caseActive && anyAbove (v.boardRe.data(), v.boardIm.data(), gate, v.prepared->plateCount,
                                              v.prepared->boardCount, kHoldRatio));
}

//==============================================================================
void Instrument::noteChunk (Lane& v, float* left, float* right, int offset, int n)
{
    const Prepared& p = *v.prepared;
    const PianoModel& model = *p.model;
    const NoteModel& note = model.notes[v.note];
    syncPedals (v, offset);

    const float step = v.damperRate * (float) n;
    if (v.damper < v.damperTarget)
        v.damper = std::min (v.damperTarget, v.damper + step);
    else if (v.damper > v.damperTarget)
        v.damper = std::max (v.damperTarget, v.damper - step);
    if (std::fabs (v.damper - v.appliedDamper) > 1.0e-4f)
        applyDamper (v);
    const bool open = v.damper < kDamperClosed;

    if (! v.busy())
    {
        std::fill (left, left + n, 0.0f);
        std::fill (right, right + n, 0.0f);
        return;
    }

    const f4 zero = simd::set (0.0f);
    f4 accL[kChunk], accR[kChunk], accOwn[kChunk];
    float longL[kChunk], longR[kChunk];
    for (int i = 0; i < n; ++i) { accL[i] = zero; accR[i] = zero; accOwn[i] = zero; longL[i] = 0.0f; longR[i] = 0.0f; }
    alignas (16) float thumpIn[kChunk][kThumpModes];
    bool thumpActive = false;

    if (v.contact)
    {
        runContact (v, n, accL, accR, accOwn, longL, longR, thumpIn, thumpActive);
    }
    else if (v.active > 0)
    {
        if (note.longGain > 0.0f && v.phantoms > 0.0f)
            runLongitudinal (v, n, longL, longR);
        runFree (v, n, accL, accR, accOwn);
    }

    float knockForce[kChunk];
    bool knocking = false;
    if (v.knock > 0.0f)
    {
        std::fill (knockForce, knockForce + n, 0.0f);
        knocking = true;
        const int length = v.knockLength;
        for (int i = 0; i < n && v.knockPosition < length; ++i, ++v.knockPosition)
            if (v.knockPosition >= 0)
            {
                const float s = std::sin ((float) kPi * (float) v.knockPosition / (float) length);
                knockForce[i] += v.knock * s * s;
            }
        if (v.knockPosition >= length)
            v.knock = 0.0f;
    }

    // The soundboard: a knock drives every mode, the blow's push the lowest. The strings of the
    // open notes hear the board's motion at the bridge.
    if (knocking)
    {
        v.boardActive = p.plateCount;
        v.caseActive = p.boardCount > p.plateCount;
    }
    else if (thumpActive)
        v.boardActive = std::max (v.boardActive, std::min (p.plateCount, (note.thumpCount + 3) & ~3));
    const bool bus = anySympathetic || open;
    f4 regionAcc[kNumRegions][kChunk];
    const bool board = v.boardActive > 0 || v.caseActive;
    if (board)
        runBoard (v, n, knocking ? knockForce : nullptr, thumpIn, thumpActive, bus, accL, accR, regionAcc);

    // This note's motion at the bridge, for the sympathetic strings a chunk later.
    BodyNote& b = notes[(size_t) v.note];
    float* own = &ownHistory[(size_t) v.note * (kChunk + kMaxBlock)];
    if (b.ownBlock != block)
    {
        if (b.ownBlock != block - 1)
            std::fill (own, own + kChunk, 0.0f);
        std::fill (own + kChunk, own + kChunk + blockSamples, 0.0f);
        b.ownBlock = block;
    }
    float* regionOut = regionHistory[note.region] + kChunk + offset;
    for (int i = 0; i < n; ++i)
    {
        const float motion = simd::sum (accOwn[i]);
        own[kChunk + offset + i] += motion;
        regionOut[i] += motion;
    }
    if (bus && board)
        for (int r = 0; r < kNumRegions; ++r)
        {
            float* out = regionHistory[r] + kChunk + offset;
            for (int i = 0; i < n; ++i)
                out[i] += simd::sum (regionAcc[r][i]);
        }

    if (--v.checkCountdown <= 0)
    {
        v.checkCountdown = kCheckChunks;
        if (! v.contact)
            prune (v);
        if (! v.contact && ! knocking)
            pruneBoard (v);
        checkAudible (v);
    }

    const float gain = model.outputGain;
    for (int i = 0; i < n; ++i)
    {
        left[i] = gain * (simd::sum (accL[i]) + longL[i]);
        right[i] = gain * (simd::sum (accR[i]) + longR[i]);
    }
}

void Instrument::renderNote (Lane& v, float* left, float* right, int offset, int n, int blockSize)
{
    openBlock (blockSize);
    n = std::min (n, kMaxBlock - offset);
    std::fill (left, left + n, 0.0f);
    std::fill (right, right + n, 0.0f);

    // A new model has taken over: this block fades the note out on the old one, which is kept for it.
    bool fading = false;
    if (v.note >= 0 && v.prepared != prepared.get())
    {
        if (v.prepared == retired.get() && retired != nullptr)
            fading = true;
        else
            clear (v);
    }

    if (v.note >= 0)
    {
        // An announced strike of this note by another voice: the strings stop there and wait for it.
        int stop = offset + n;
        if (const int at = handOver[(size_t) v.note]; at >= offset && at < offset + n && owners[(size_t) v.note] == &v)
            stop = at;
        for (int pos = 0; offset + pos < stop;)
        {
            const int at = offset + pos;
            const int chunk = std::min ({ kChunk - (at & (kChunk - 1)), stop - at, nextPedalEvent (at) - at });
            noteChunk (v, left + pos, right + pos, at, chunk);
            pos += chunk;
        }
        v.renderedBlock = block;
        v.renderedTo = stop;
        if (fading)
        {
            const float scale = 1.0f / (float) blockSize;
            for (int i = 0; i < n; ++i)
            {
                const float g = std::max (0.0f, 1.0f - (float) (offset + i) * scale);
                left[i] *= g;
                right[i] *= g;
            }
            if (offset + n >= blockSize)
                clear (v);
        }
    }
    if (v.fade >= 0)
        renderFade (v, left, right, offset, n, blockSize);
}

void Instrument::renderFade (Lane& v, float* left, float* right, int offset, int n, int blockSize)
{
    FadeSlot& f = fades[(size_t) v.fade];
    if (f.lane.note < 0 || f.lane.prepared == nullptr
        || (f.lane.prepared != prepared.get() && f.lane.prepared != retired.get()))
    {
        dropFade (v);
        return;
    }
    float faded[2][kMaxBlock];
    const int fade = v.fade;
    v.fade = -1;                                 // the slot's own lane never chains a fade
    renderNote (f.lane, faded[0], faded[1], offset, n, blockSize);
    v.fade = fade;
    const float inverseLength = 1.0f / (float) f.length;
    for (int i = 0; i < n; ++i)
    {
        const float g = (float) std::max (0, f.remaining - i) * inverseLength;
        left[i] += faded[0][i] * g;
        right[i] += faded[1][i] * g;
    }
    f.remaining = std::max (0, f.remaining - n);
    if (f.remaining == 0 || f.lane.note < 0)
        dropFade (v);
}

//==============================================================================
void Instrument::renderBody (float* left, float* right, int blockSize)
{
    blockSize = std::clamp (blockSize, 1, kMaxBlock);
    openBlock (blockSize);
    const int count = blockSize;
    std::fill (left, left + count, 0.0f);
    std::fill (right, right + count, 0.0f);

    if (prepared != nullptr)
    {
        const PianoModel& model = *prepared->model;
        const float Ts = (float) (1.0 / model.sampleRate);
        const float amount = sympathetic * Ts;
        const f4 zero = simd::set (0.0f);
        bool sympatheticNow = false;

        // A note that stopped for an announced strike nobody took: the rest of its block plays here,
        // its motion at the bridge in place before the sympathetic strings read it.
        float caughtL[kMaxBlock], caughtR[kMaxBlock];
        bool caught = false;
        for (int index = 0; index < kNumNotes; ++index)
        {
            Lane* v = owners[(size_t) index];
            if (handOver[(size_t) index] < 0 || v == nullptr || v->renderedBlock != block || v->renderedTo >= count)
                continue;
            if (! caught)
            {
                std::fill (caughtL, caughtL + count, 0.0f);
                std::fill (caughtR, caughtR + count, 0.0f);
                caught = true;
            }
            float l[kMaxBlock], r[kMaxBlock];
            const int from = v->renderedTo;
            handOver[(size_t) index] = -1;
            renderNote (*v, l, r, from, count - from, count);
            for (int i = 0; i < count - from; ++i)
            {
                caughtL[from + i] += l[i];
                caughtR[from + i] += r[i];
            }
        }

        for (int t0 = 0; t0 < count;)
        {
            const int n = std::min ({ kChunk - (t0 & (kChunk - 1)), count - t0, nextPedalEvent (t0) - t0 });

            // The pedals at this chunk: a sostenuto pressed holds the notes whose keys are down; its
            // release, and the sustain pedal, set every free damper's target anew.
            const Pedals pedals = pedalsAt (t0);
            const bool sostenutoPressed = pedals.sostenuto && ! bodySostenuto;
            const bool sostenutoReleased = ! pedals.sostenuto && bodySostenuto;
            const bool liftChanged = pedals.lift != bodyLift;
            if (sostenutoPressed || sostenutoReleased || liftChanged)
                for (int index = 0; index < kNumNotes; ++index)
                {
                    BodyNote& b = notes[(size_t) index];
                    if (sostenutoPressed)
                        b.heldBySostenuto = b.heldLanes > 0;
                    else if (sostenutoReleased)
                        b.heldBySostenuto = false;
                    if ((liftChanged || sostenutoReleased) && b.heldLanes == 0 && ! b.heldBySostenuto
                        && model.notes[index].hasDamper)
                    {
                        b.damperTarget = 1.0f - pedals.lift;
                        b.damperRate = 1.0f / (0.03f * (float) model.sampleRate);
                    }
                }
            bodyLift = pedals.lift;
            bodySostenuto = pedals.sostenuto;

            // Bridge motion each region's strings feel: every region's motion one chunk ago.
            float regionIn[kNumRegions][kChunk];
            float regionPeak[kNumRegions] {};
            for (int a = 0; a < kNumRegions; ++a)
                for (int i = 0; i < n; ++i)
                {
                    float sum = 0.0f;
                    for (int b = 0; b < kNumRegions; ++b)
                        sum += model.regionGain[a][b] * regionHistory[b][t0 + i];
                    regionIn[a][i] = amount * sum;
                    regionPeak[a] = std::max (regionPeak[a], std::fabs (regionIn[a][i]));
                }

            f4 accL[kChunk], accR[kChunk];
            for (int i = 0; i < n; ++i) { accL[i] = zero; accR[i] = zero; }

            for (int index = 0; index < kNumNotes; ++index)
            {
                BodyNote& b = notes[(size_t) index];
                const NoteModel& note = model.notes[index];

                const float step = b.damperRate * (float) n;
                if (b.damper < b.damperTarget)
                    b.damper = std::min (b.damperTarget, b.damper + step);
                else if (b.damper > b.damperTarget)
                    b.damper = std::max (b.damperTarget, b.damper - step);
                if (std::fabs (b.damper - b.appliedDamper) > 1.0e-4f)
                    applyDamper (b, index);

                const bool open = b.damper < kDamperClosed;
                const float self = model.regionGain[note.region][note.region] * amount;
                if (prepared->sympatheticCount[index] > 0 && (b.sympatheticLive || (open && regionPeak[note.region] > 0.0f)))
                {
                    // Sympathetic strings hear every region except this note's own struck strings.
                    const float* own = &ownHistory[(size_t) index * (kChunk + kMaxBlock)];
                    const bool thisBlock = b.ownBlock == block, lastBlock = b.ownBlock == block - 1;
                    float input[kChunk];
                    for (int i = 0; i < n; ++i)
                    {
                        const int t = t0 + i;
                        const float motion = thisBlock || (lastBlock && t < kChunk) ? own[t] : 0.0f;
                        input[i] = regionIn[note.region][i] - self * motion;
                    }
                    runSympathetic (b, index, input, n, accL, accR);
                    b.sympatheticLive = true;
                }
                sympatheticNow |= open;

                if (--b.checkCountdown <= 0)
                {
                    b.checkCountdown = kCheckChunks;
                    if (b.sympatheticLive
                        && ! anyAbove (b.szr.data(), b.szi.data(), prepared->sympatheticGate[index].data(), 0,
                                       prepared->sympatheticCount[index])
                        && ! (open && regionPeak[note.region] > 1.0e-18f))
                    {
                        b.sympatheticLive = false;
                        std::fill (b.szr.begin(), b.szr.end(), 0.0f);
                        std::fill (b.szi.begin(), b.szi.end(), 0.0f);
                    }
                }
            }

            const float gain = model.outputGain;
            for (int i = 0; i < n; ++i)
            {
                const float l = gain * simd::sum (accL[i]), r = gain * simd::sum (accR[i]);
                const float mid = 0.5f * (l + r), difference = 0.5f * (l - r) * width;
                left[t0 + i] = mid + difference;
                right[t0 + i] = mid - difference;
            }
            t0 += n;
        }
        anySympathetic = sympatheticNow;

        if (caught)
            for (int i = 0; i < count; ++i)
            {
                const float mid = 0.5f * (caughtL[i] + caughtR[i]), difference = 0.5f * (caughtL[i] - caughtR[i]) * width;
                left[i] += mid + difference;
                right[i] += mid - difference;
            }
    }
    handOver.fill (-1);
    // The pedals where the next block starts.
    for (int i = 0; i < numPedalEvents; ++i)
    {
        const PedalEvent& e = pedalEvents[(size_t) i];
        if (e.pedal == kSustainPedal) sustainStart = e.value;
        else if (e.pedal == kSostenutoPedal) sostenutoStart = e.value >= 0.5f;
        else softStart = e.value;
    }
    numPedalEvents = 0;

    // Close the block: the last chunk of motion stays in front for the next one.
    for (auto& region : regionHistory)
        std::memmove (region, region + count, kChunk * sizeof (float));
    for (int index = 0; index < kNumNotes; ++index)
        if (notes[(size_t) index].ownBlock == block)
        {
            float* own = &ownHistory[(size_t) index * (kChunk + kMaxBlock)];
            std::memmove (own, own + count, kChunk * sizeof (float));
        }
    ++block;
    blockOpen = false;
    takeOffer();
}

} // namespace pm
