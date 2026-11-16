#pragma once
// Swappable "performer" interface (M4): the virtual player between the
// keyboard and the engine. It turns note events and continuous controllers
// into the engine's continuous controls (breath pressure, pitch, embouchure,
// tone) plus an output gain. See BreathPerformer for the implementation and
// docs/milestones/M4-report.md for the measurements behind it.
//
// Real-time contract: prepare() may allocate; everything else must not
// allocate, lock or throw. Events take effect at the next advance() call, so
// a caller that wants sample-accurate timing splits its block at the events.
// Nothing looks ahead: zero added latency.

#include "mf/Engine.h"
#include "mf/ToneMacros.h"

#include <cstdint>

namespace mf {

// Where the dynamic level comes from.
enum class DynamicsSource {
    Auto,       // note velocity until the first dynamics controller arrives, then the controller
    Velocity,   // velocity of each note (legato notes glide to their own velocity)
    Controller, // setDynamics() only (CC1 or CC11, mapped by the plug-in)
};

struct PerformerParams {
    double tuningA4 = 440.0;
    ToneMacros tone{};             // Air / Brightness / Attack (0..1, 0.5 = default)
    float jetRatio = 0.32f;        // embouchure, see EngineControls::jetRatio
    DynamicsSource dynamicsSource = DynamicsSource::Auto;
    // Loudness: steady RMS level at mezzo-forte (dynamic 0.5) and the span
    // from dynamic 0 (pp) to 1 (ff). Equal in every register (calibrated).
    float levelDb = -22.0f;
    float dynamicRangeDb = 18.0f;
    // Vibrato (breath-pressure vibrato). depth 0..1; 1 = +-0.12 of the dynamic
    // scale (about +-2 dB) and +-10 cents. The automatic part fades in after
    // `vibratoDelaySec` of each tongued note over `vibratoFadeSec`; the mod
    // wheel (setVibrato) adds to it immediately.
    float vibratoRateHz = 5.0f;
    float vibratoDepth = 0.35f;
    float vibratoDelaySec = 0.20f;
    float vibratoFadeSec = 0.45f;
    // Breath release after the last key is let go: time to -40 dB.
    float releaseSec = 0.15f;
    // Humanisation 0..1: small random variations of dynamics, attack time and
    // vibrato rate/depth from note to note (deterministic for a given seed).
    float humanize = 0.5f;
    std::uint32_t seed = 0x5EEDu;
};

struct PerformerOutput {
    EngineControls controls{};   // targets for Engine::setControls
    float gain = 1.0f;           // linear output gain (level calibration + breath volume)
    float basePressure = 0.0f;   // controls.pressure without the slur push
    int note = -1;               // sounding MIDI note (-1 = none)
    bool breathing = false;      // breath pressure > 0
    // Since the previous advance():
    bool tongued = false;        // a tongued (re)attack started
    bool legatoChange = false;   // a slurred note change happened (finger change while blowing)
    bool registerChange = false; // ... and it changes the register (harmonic of the tube)
};

class Performer {
public:
    virtual ~Performer() = default;
    virtual void prepare(double sampleRate) = 0;
    virtual void reset() = 0;
    virtual void setParams(const PerformerParams& p) = 0;

    // velocity 0..1 (MIDI velocity / 127). Overlap with a held key = slur,
    // otherwise a tongued attack. Last-note priority.
    virtual void noteOn(int note, float velocity) = 0;
    // Releasing the sounding key falls back (slurred) to the previous held key.
    virtual void noteOff(int note) = 0;
    virtual void allNotesOff() = 0;

    virtual void setDynamics(float v) = 0;   // 0..1 (CC1 or CC11 / 127)
    virtual void setExpression(float v) = 0; // 0..1, scales the dynamic level (default 1)
    virtual void setVibrato(float v) = 0;    // 0..1, mod wheel: extra vibrato depth

    // Returns the controls for the current time (after all events so far) and
    // advances the performer's clock by numSamples.
    virtual PerformerOutput advance(int numSamples) = 0;
};

} // namespace mf
