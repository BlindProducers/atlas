#pragma once
// Swappable "controls -> audio" engine interface.
// Any engine (physical model now, DDSP-style later) implements this.
// Real-time contract: prepare() may allocate; reset/setControls/process must not
// allocate, lock, or throw.

namespace mf {

// Continuous performance controls. Values are targets; the engine smooths them
// internally so they may be updated once per block.
struct EngineControls {
    float pressure     = 0.0f;   // normalised blowing pressure, 0 = silent, ~1 = mf/f
    float frequencyHz  = 440.0f; // target SOUNDING pitch; may change while sounding (legato)
    float jetRatio     = 0.32f;  // jet delay as a fraction of the sounding period, at the
                                 // reference pressure (embouchure); see FluteEngine
    float harmonic     = 0.0f;   // register: which harmonic k of the fingered tube sounds
                                 // (tube fundamental = frequencyHz / k). 0 = automatic
                                 // (flute chart: k=1 below D5, 2 for D5-C#6, 3 from D6)
    float noiseGain    = 0.1f;   // breath turbulence amount (relative to pressure)
    float brightness   = 0.5f;   // 0..1, tone-hole/end reflection corner 1.4..2.8 kHz (0 = dark, 1 = bright)
    float chiff        = 0.4f;   // 0..1, turbulent "chiff" burst when the breath rises quickly
};

class Engine {
public:
    virtual ~Engine() = default;
    virtual void prepare(double sampleRate, int maxBlockSize) = 0;
    virtual void reset() = 0;
    virtual void setControls(const EngineControls& c) = 0;
    // Writes numSamples mono samples to out (overwrites).
    virtual void process(float* out, int numSamples) = 0;
};

} // namespace mf
