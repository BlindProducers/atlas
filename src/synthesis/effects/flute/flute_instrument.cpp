// SPDX-License-Identifier: GPL-3.0-or-later
// The original plugin's processor (plugin/src/PluginProcessor.cpp in Modeled
// Flute), without its reverb, output gain and meter: parameters smoothed the
// same way, the block cut at every event, the MIDI handled the same way. Kept
// close to it so the two can be compared line by line.
#include "flute_instrument.h"

#include "multisample_midi_state.h"

#include <algorithm>
#include <cmath>

namespace atlas::flute {

namespace {

constexpr double kSmoothSec = 0.02;
constexpr int kChunk = mf::FluteVoice::kControlBlock;

// Parameter snapshots are compared exactly on purpose (change detection).
bool sameParams (const mf::PerformerParams& a, const mf::PerformerParams& b)
{
    return a.tuningA4 == b.tuningA4 && a.tone.air == b.tone.air && a.tone.brightness == b.tone.brightness
        && a.tone.attack == b.tone.attack && a.jetRatio == b.jetRatio && a.dynamicsSource == b.dynamicsSource
        && a.levelDb == b.levelDb && a.dynamicRangeDb == b.dynamicRangeDb && a.vibratoRateHz == b.vibratoRateHz
        && a.vibratoDepth == b.vibratoDepth && a.vibratoDelaySec == b.vibratoDelaySec
        && a.vibratoFadeSec == b.vibratoFadeSec && a.releaseSec == b.releaseSec && a.humanize == b.humanize
        && a.seed == b.seed;
}

} // namespace

// ---- LinearSmoother ----

void Instrument::LinearSmoother::reset (double sampleRate, double seconds)
{
    steps_ = std::max (1, static_cast<int> (std::floor (seconds * sampleRate)));
    countdown_ = 0;
    current_ = target_;
}

void Instrument::LinearSmoother::setTarget (float v)
{
    if (v == target_)
        return;
    target_ = v;
    countdown_ = steps_;
    step_ = (target_ - current_) / static_cast<float> (steps_);
}

float Instrument::LinearSmoother::skip (int n)
{
    if (countdown_ <= 0 || n <= 0)
        return current_;
    if (n >= countdown_)
    {
        countdown_ = 0;
        current_ = target_;
        return current_;
    }
    current_ += step_ * static_cast<float> (n);
    countdown_ -= n;
    return current_;
}

// ---- Instrument ----

Instrument::Instrument()
{
    keyOf_.fill (-1);
    prepare (kMaxSampleRate);
}

// The original's prepareToPlay. The voice's delay lines were allocated for
// kMaxSampleRate when the instrument was built, so preparing for a lower rate
// reuses them (std::vector::assign below the capacity).
void Instrument::prepare (double sampleRate)
{
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    voice_.prepare (sampleRate_, std::max (kMaxBlock, kChunk));
    for (auto* s : { &air_, &brightness_, &vibratoDepth_, &tuning_ })
        s->reset (sampleRate_, kSmoothSec);
    sustainDown_ = false;
    sustained_.fill (false);
    keyOf_.fill (-1);
    pitchWheel_ = 0.0f;
    expression_ = 1.0f;
    wheel_ = 0.0f;
    readTargets (params_, true);
    perf_ = voice_.params();
    applyVoiceParams (0);
    voice_.reset();
    updateBend();
}

void Instrument::noteOn (int note, float velocity, int sample)
{
    if (numNotes_ >= kMaxNoteEvents)
        return;
    notes_[static_cast<size_t> (numNotes_++)] = { sample, note, velocity, NoteEvent::kOn };
}

void Instrument::noteOff (int note, int sample)
{
    if (numNotes_ >= kMaxNoteEvents)
        return;
    notes_[static_cast<size_t> (numNotes_++)] = { sample, note, 0.0f, NoteEvent::kOff };
}

void Instrument::allNotesOff (int sample)
{
    if (numNotes_ >= kMaxNoteEvents)
        return;
    notes_[static_cast<size_t> (numNotes_++)] = { sample, 0, 0.0f, NoteEvent::kAllOff };
}

void Instrument::beginBlock (const vital::MultiSampleMidiState* midi, int oversample, double sampleRate,
                             float pitchWheel, float bendRange, bool enabled)
{
    midi_ = midi;
    oversample_ = std::max (1, oversample);
    blockRate_ = sampleRate;
    // Atlas maps the wheel's 14 bits onto -1 to 1 whole, so its centre is a
    // hair above 0; the original's centre is 0 exactly.
    pitchWheel_ = std::isfinite (pitchWheel) && std::fabs (pitchWheel) >= 1.0e-4f ? std::clamp (pitchWheel, -1.0f, 1.0f) : 0.0f;
    bendRange_ = std::isfinite (bendRange) ? std::max (0.0f, bendRange) : 2.0f;
    enabled_ = enabled;
}

void Instrument::claim (int voiceId, int sample)
{
    if (voiceId == owner_)
        return;
    // Two claims in one block: the lane that carried the audio when the block
    // started hands over to the last one.
    if (! pendingHandover_)
        previousOwner_ = owner_;
    owner_ = voiceId;
    handoverSample_ = std::max (0, sample);
    pendingHandover_ = true;
}

// ---- Parameters (the original's readTargets and applyVoiceParams) ----

void Instrument::readTargets (const Params& p, bool snap)
{
    params_ = p;
    auto set = [snap] (LinearSmoother& s, float v)
    {
        if (snap) s.setCurrentAndTarget (v);
        else s.setTarget (v);
    };
    set (air_, p.air);
    set (brightness_, p.brightness);
    set (vibratoDepth_, p.vibratoDepth);
    set (tuning_, p.tuningA4);
    voice_.setRegisterOverlap (p.overlap);
}

void Instrument::applyVoiceParams (int numSamplesAdvanced)
{
    mf::PerformerParams p = perf_;
    auto adv = [numSamplesAdvanced] (LinearSmoother& s) { return numSamplesAdvanced > 0 ? s.skip (numSamplesAdvanced) : s.current(); };
    p.tone.air = adv (air_);
    p.tone.brightness = adv (brightness_);
    p.vibratoDepth = adv (vibratoDepth_);
    p.tuningA4 = static_cast<double> (adv (tuning_));
    p.tone.attack = params_.attack;
    p.vibratoRateHz = params_.vibratoRateHz;
    p.vibratoDelaySec = params_.vibratoDelaySec;
    p.humanize = params_.humanize;
    p.dynamicRangeDb = params_.dynamicRangeDb;
    p.releaseSec = params_.releaseSec;
    switch (params_.dynamicsSource)
    {
        case kControllerOnly: p.dynamicsSource = mf::DynamicsSource::Controller; break;
        case kVelocityOnly: p.dynamicsSource = mf::DynamicsSource::Velocity; break;
        default: p.dynamicsSource = mf::DynamicsSource::Auto; break;
    }
    if (! sameParams (p, perf_) || numSamplesAdvanced == 0)
    {
        perf_ = p;
        voice_.setParams (p);
    }
}

// While a smoothed parameter moves, the voice runs in control-block chunks so
// the change is spread over 20 ms; otherwise in one call.
void Instrument::renderVoice (float* out, int numSamples)
{
    const bool moving = air_.isSmoothing() || brightness_.isSmoothing() || vibratoDepth_.isSmoothing()
                     || tuning_.isSmoothing();
    if (! moving)
    {
        applyVoiceParams (-1);
        voice_.process (out, numSamples);
        return;
    }
    for (int pos = 0; pos < numSamples; pos += kChunk)
    {
        const int n = std::min (kChunk, numSamples - pos);
        applyVoiceParams (n);
        voice_.process (out + pos, n);
    }
}

// ---- MIDI (the original's handleMidi) ----

void Instrument::updateBend()
{
    voice_.setPitchBend (pitchWheel_ * bendRange_);
}

int Instrument::keyFor (int note) const
{
    return std::clamp (note + params_.transpose, 0, 127);
}

// Releases keys held by the sustain pedal. The sounding note goes last, so the
// voice does not slur back through the other sustained keys.
void Instrument::releaseSustained()
{
    const int sounding = voice_.lastControls().note;
    for (int n = 0; n < 128; ++n)
    {
        if (sustained_[static_cast<size_t> (n)] && n != sounding)
        {
            sustained_[static_cast<size_t> (n)] = false;
            voice_.noteOff (n);
        }
    }
    if (sounding >= 0 && sounding < 128 && sustained_[static_cast<size_t> (sounding)])
    {
        sustained_[static_cast<size_t> (sounding)] = false;
        voice_.noteOff (sounding);
    }
}

void Instrument::handleNote (const NoteEvent& event)
{
    if (event.kind == NoteEvent::kAllOff)
    {
        sustainDown_ = false;
        sustained_.fill (false);
        keyOf_.fill (-1);
        voice_.allNotesOff();
        return;
    }
    if (event.note < 0 || event.note > 127)
        return;
    auto& played = keyOf_[static_cast<size_t> (event.note)];
    if (event.kind == NoteEvent::kOn)
    {
        const int key = keyFor (event.note);
        played = static_cast<signed char> (key);
        sustained_[static_cast<size_t> (key)] = false;
        voice_.noteOn (key, event.velocity);
    }
    else
    {
        const int key = played >= 0 ? played : keyFor (event.note);
        played = -1;
        if (sustainDown_) sustained_[static_cast<size_t> (key)] = true;
        else voice_.noteOff (key);
    }
}

void Instrument::handleController (int cc, int value)
{
    const float v = static_cast<float> (value) / 127.0f;
    const int dynamics = kControllerNumbers[std::clamp (params_.dynamicsController, 0, 2)];
    if (cc == dynamics)
        voice_.setDynamics (v);
    else if (cc == 11)
    {
        expression_ = v;
        voice_.setExpression (v);
    }
    else if (cc == 1)
    {
        wheel_ = v;
        voice_.setVibrato (v);
    }
    else if (cc == 64)
    {
        const bool down = value >= 64;
        if (sustainDown_ && ! down) releaseSustained();
        sustainDown_ = down;
    }
    else if (cc == 120 || cc == 123) // all sound off / all notes off
    {
        sustainDown_ = false;
        sustained_.fill (false);
        keyOf_.fill (-1);
        voice_.allNotesOff();
    }
}

// ---- The block (the original's processBlock) ----

void Instrument::render (const Params& p, int numSamples)
{
    const int n = std::clamp (numSamples, 0, kMaxBlock);
    handedOver_ = pendingHandover_;
    pendingHandover_ = false;
    if (blockRate_ > 0.0 && blockRate_ != sampleRate_)
    {
        params_ = p;
        prepare (blockRate_);
    }

    if (! enabled_)
    {
        // A slot switched off lets its keys go, so switching it on starts from rest.
        if (voice_.performer().heldKeys() > 0 || sustainDown_)
            handleNote ({ 0, 0, 0.0f, NoteEvent::kAllOff });
        numNotes_ = 0;
        std::fill (buffer_.begin(), buffer_.begin() + n, 0.0f);
        return;
    }

    readTargets (p, false);
    updateBend();

    // All Controllers Off (CC 121) leaves no trace in the block's events: a
    // controller the state no longer holds has returned to its default.
    const vital::MultiSampleMidiState* midi = midi_;
    if (midi != nullptr)
    {
        if (sustainDown_ && ! midi->received[64])
        {
            releaseSustained();
            sustainDown_ = false;
        }
        if (expression_ != 1.0f && ! midi->received[11])
        {
            expression_ = 1.0f;
            voice_.setExpression (1.0f);
        }
        if (wheel_ != 0.0f && ! midi->received[1])
        {
            wheel_ = 0.0f;
            voice_.setVibrato (0.0f);
        }
    }

    // The block, cut at every event's sample; the controllers' host-rate
    // offsets count in the voices' samples here. A controller and a key at
    // the same sample: the controller first.
    float* out = buffer_.data();
    int pos = 0;
    int nextNote = 0;
    int nextCc = 0;
    const int numCcs = midi != nullptr ? midi->num_events : 0;
    while (nextNote < numNotes_ || nextCc < numCcs)
    {
        const int noteAt = nextNote < numNotes_ ? std::clamp (notes_[static_cast<size_t> (nextNote)].sample, 0, n) : n + 1;
        const int ccAt = nextCc < numCcs
                           ? std::clamp (midi->events[static_cast<size_t> (nextCc)].offset * oversample_, 0, n)
                           : n + 1;
        const bool controller = ccAt <= noteAt;
        const int at = controller ? ccAt : noteAt;
        if (at > pos)
        {
            renderVoice (out + pos, at - pos);
            pos = at;
        }
        if (controller)
        {
            const auto& event = midi->events[static_cast<size_t> (nextCc++)];
            handleController (event.cc, static_cast<int> (event.value + 0.5f));
        }
        else
            handleNote (notes_[static_cast<size_t> (nextNote++)]);
    }
    if (pos < n)
        renderVoice (out + pos, n - pos);
    numNotes_ = 0;
}

} // namespace atlas::flute
