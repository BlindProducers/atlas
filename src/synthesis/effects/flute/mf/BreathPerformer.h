#pragma once
// Virtual breath (M4): the default Performer, tuned for FluteEngine.
// See src/BreathPerformer.cpp for the model and the measurements behind each
// constant, and docs/milestones/M4-report.md for the results.

#include "mf/NoteStack.h"
#include "mf/Performer.h"

#include <cstdint>

namespace mf {

class BreathPerformer final : public Performer {
public:
    // Shape of a tongued attack, in units of the steady breath (1 = target):
    // a raised-cosine rise to 1 + overshoot in onsetSec, then an exponential
    // decay of the overshoot with time constant decaySec.
    struct AttackShape {
        float onsetSec = 0.01f;
        float overshoot = 1.0f;
        float decaySec = 0.06f;
    };

    BreathPerformer();
    void prepare(double sampleRate) override;
    void reset() override;
    void setParams(const PerformerParams& p) override;
    const PerformerParams& params() const { return params_; }

    void noteOn(int note, float velocity) override;
    void noteOff(int note) override;
    void allNotesOff() override;
    void setDynamics(float v) override;
    void setExpression(float v) override;
    void setVibrato(float v) override;
    // Pitch bend in semitones (MIDI pitch bend x bend range). Keeps the register.
    void setPitchBend(float semitones);
    float pitchBend() const { return bend_; }
    PerformerOutput advance(int numSamples) override;

    // --- Inspection (tests, tools) ---
    int soundingNote() const { return note_; }
    int heldKeys() const { return stack_.size(); }
    float dynamic() const { return dyn_; }          // smoothed dynamic level 0..1 (before expression)
    float envelope() const { return env_; }         // breath envelope (1 = steady)
    float vibratoDepthNow() const { return vibDepthNow_; }

    // --- Mappings (pure functions, exposed for tests and the report) ---
    // Breath pressure for a dynamic level (0 = pp .. 1 = ff) on a note: a
    // geometric range that rises with pitch, as a player blows harder for
    // higher notes (0.30..1.10 at C4, 0.42..1.40 at C7).
    static float pressureFor(double note, float dynamic);
    // FluteEngine's steady output level (dBFS RMS, default tone, jet 0.32) at
    // a pressure, interpolated from a table measured with
    // `mf_render --tuning-table` (37 notes x 5 pressures).
    static float engineLevelDb(double note, float pressure);
    // Output gain (dB) that puts the steady level at
    // levelDb + dynamicRangeDb * (dynamic - 0.5) on every note.
    static float gainDbFor(const PerformerParams& p, double note, float dynamic);
    // Level change of the engine with Brightness (dB re brightness 0.5), removed by gainDbFor.
    static float brightnessLevelDb(double note, float brightness);
    // Attack shape from the Attack macro (0..1), the note velocity (0..1)
    // and the register (1..3) of the note.
    static AttackShape attackShape(float attack, float velocity, int harmonic);

private:
    void startAttack(int note, float velocity);
    void changeNote(int note, float velocity);
    void startRelease();
    float velocityDynamic(float velocity);
    float random();      // uniform -1..1 (deterministic)
    double noteHz(int note) const;
    bool controllerMode() const;

    PerformerParams params_{};
    double sampleRate_ = 48000.0;
    NoteStack stack_;

    enum class Phase { Idle, Attack, Release } phase_ = Phase::Idle;
    int note_ = -1;
    double freqHz_ = 440.0;
    AttackShape shape_{};
    double attackT_ = 0.0;      // time since the tongued attack started
    float releaseFrom_ = 0.0f;  // envelope at the start of the release
    double releaseT_ = 0.0;
    float env_ = 0.0f;
    float attackFrom_ = 0.0f;  // envelope the attack rises from (re-tonguing during a release)

    // Dynamics
    float dynCc_ = 0.5f, expr_ = 1.0f, wheel_ = 0.0f;
    float bend_ = 0.0f;         // pitch bend, semitones
    double tuneRef_ = 440.0;    // tuning when freqHz_ was set (live tuning changes scale it)
    bool ccSeen_ = false;
    float dynTarget_ = 0.5f, dyn_ = 0.5f, dynTau_ = 0.025f;
    float noteOffset_ = 0.0f;   // humanisation of the level, per note

    // Register slur push (upward register changes)
    double pushT_ = 1e9;

    // Vibrato
    double vibPhase_ = 0.0;
    double vibT_ = 0.0;         // time since the automatic vibrato was (re)started
    float slowA_ = 0.0f, slowB_ = 0.0f, slowTargetA_ = 0.0f, slowTargetB_ = 0.0f;
    double slowT_ = 0.0;
    float vibDepthNow_ = 0.0f;

    float lastGain_ = 1.0f;
    bool pendTongued_ = false, pendLegato_ = false, pendRegister_ = false;
    std::uint32_t rng_ = 0x5EEDu;
};

} // namespace mf
