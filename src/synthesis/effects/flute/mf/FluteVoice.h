#pragma once
// One playable flute (M4): BreathPerformer -> FluteEngine, with the output
// gain and the register-slur overlap. This is what the plug-in (M5) drives:
// it forwards MIDI to the event methods (splitting its block at event times)
// and calls process(). See src/FluteVoice.cpp.

#include "mf/BreathPerformer.h"
#include "mf/FluteEngine.h"

#include <cstdint>
#include <vector>

namespace mf {

class FluteVoice {
public:
    // Controls are updated every kControlBlock samples (0.67 ms at 48 kHz);
    // the engine smooths them further (3-5 ms).
    static constexpr int kControlBlock = 32;

    FluteVoice();
    // Allocates (non-real-time).
    void prepare(double sampleRate, int maxBlockSize);
    void reset();
    void setParams(const PerformerParams& p) { performer_.setParams(p); }
    const PerformerParams& params() const { return performer_.params(); }
    // Register-slur overlap on/off (on by default; off = M2/M3 behaviour).
    void setRegisterOverlap(bool on) { overlapEnabled_ = on; }

    void noteOn(int note, float velocity) { performer_.noteOn(note, velocity); }
    void noteOff(int note) { performer_.noteOff(note); }
    void allNotesOff() { performer_.allNotesOff(); }
    void setDynamics(float v) { performer_.setDynamics(v); }
    void setExpression(float v) { performer_.setExpression(v); }
    void setVibrato(float v) { performer_.setVibrato(v); }
    void setPitchBend(float semitones) { performer_.setPitchBend(semitones); }

    // Writes numSamples mono samples (overwrites). Real-time safe.
    void process(float* out, int numSamples);

    // --- Inspection ---
    BreathPerformer& performer() { return performer_; }
    const FluteEngine& engine() const { return engines_[cur_]; }
    bool overlapActive() const { return xf_.active; }
    std::uint64_t overlapCount() const { return overlaps_; }
    std::uint64_t nonFiniteResetCount() const;
    bool isSilent() const { return !xf_.active && engines_[cur_].isSilent() && !lastOut_.breathing; }
    const PerformerOutput& lastControls() const { return lastOut_; }

private:
    void processChunk(float* out, int n);
    void startOverlap();

    BreathPerformer performer_;
    FluteEngine engines_[2];
    int cur_ = 0;
    double sampleRate_ = 48000.0;
    std::vector<float> bufOld_;
    PerformerOutput lastOut_{};
    float gain_ = 1.0f;       // gain applied at the end of the previous chunk (current engine)
    bool overlapEnabled_ = true;
    std::uint64_t overlaps_ = 0;
    std::uint32_t seedCounter_ = 1;

    // Register-slur overlap: the old note keeps sounding on the other engine
    // (a copy of the running state) until the new note has built up.
    struct Overlap {
        bool active = false;
        EngineControls oldCtl{};
        float oldGain = 1.0f;
        float basePressureAtSplit = 0.0f;
        double t = 0.0;        // seconds since the split
        float eOld = 0.0f, eNew = 0.0f; // 5 ms power followers (post gain)
        bool armed = false;
        float w = 0.0f;        // fade position 0..1
        float rate = 0.0f;     // fade increment per sample (0 = not started)
    } xf_;
    float followerCoeff_ = 0.004f;
    float outPower_ = 0.0f;   // 5 ms power follower of the output
};

} // namespace mf
