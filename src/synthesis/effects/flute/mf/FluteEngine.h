#pragma once
// Jet-drive + waveguide-bore concert-flute model (M1 core, M2 fingering & registers).
// See src/FluteEngine.cpp for the signal flow and the physics of each block.

#include "mf/Engine.h"
#include "mf/dsp/DcBlocker.h"
#include "mf/dsp/DelayLine.h"
#include "mf/dsp/JetNonlinearity.h"
#include "mf/dsp/Noise.h"
#include "mf/dsp/OnePole.h"
#include "mf/dsp/Smoother.h"

#include <cstdint>

namespace mf {

class FluteEngine final : public Engine {
public:
    // Safe control ranges. setControls() clamps into these.
    static constexpr float kMinPressure = 0.0f, kMaxPressure = 2.5f;
    static constexpr float kMinFrequency = 100.0f, kMaxFrequency = 4500.0f; // also <= fs/8
    // jetRatio control range. The EFFECTIVE ratio (after pressure coupling) is
    // clamped per register, see FluteEngine.cpp.
    static constexpr float kMinJetRatio = 0.15f, kMaxJetRatio = 0.55f;
    static constexpr int kMaxHarmonic = 3;
    static constexpr float kMinNoise = 0.0f, kMaxNoise = 0.5f;

    FluteEngine();
    ~FluteEngine() override;
    void prepare(double sampleRate, int maxBlockSize) override;
    void reset() override;
    void setControls(const EngineControls& c) override;
    void process(float* out, int numSamples) override;

    // --- Extras (not part of the swappable interface) ---
    // Seed of the breath-noise generator; applied now and on every reset().
    void setNoiseSeed(std::uint32_t seed);
    // Copies the complete running state of another FluteEngine (delay lines,
    // filters, smoothers, controls), so this engine continues exactly where the
    // other one is. Used by FluteVoice to keep the old note sounding during a
    // register-changing slur (M4). Real-time safe when both engines were
    // prepare()d at the same sample rate (the buffers then have equal sizes and
    // std::vector copy-assignment reuses the existing storage). The breath-noise
    // generator is re-seeded with noiseSeed so the two copies decorrelate.
    void copyStateFrom(const FluteEngine& other, std::uint32_t noiseSeed);
    // Number of times the safety guard found a non-finite / runaway state and
    // reset the model. Should stay 0; exposed for tests.
    std::uint64_t nonFiniteResetCount() const { return nonFiniteResets_; }
    // True while the engine is fully at rest (outputs exact zeros, ~no CPU).
    bool isSilent() const { return silent_; }
    // Target bore loop delay (samples) chosen by the pitch compensation.
    double boreDelayTarget() const { return boreDelayTarget_; }
    // Jet delay target (samples), after the pressure coupling.
    double jetDelayTarget() const { return jetDelayTarget_; }
    // Register in use: harmonic k of the fingered tube (1, 2 or 3).
    int activeHarmonic() const { return harmonic_; }
    // The automatic register rule used when controls.harmonic == 0.
    static int autoHarmonic(double frequencyHz);
    double sampleRate() const { return sampleRate_; }

private:
    void clearState();
    void updateTargets(bool snap);
    void updateJet();  // pressure-coupled jet delay + its phase at f0
    void setNoiseBand(double pressure);
    double boreDelayFor(double amplitude) const;
    double ampEstimate() const;

    // Pitch-compensation terms computed in updateTargets().
    struct Compensation {
        double boreBase = 100.0;   // P + linear filter phase correction (samples)
        double linearGain = 0.9;   // |B(w0)| of the bore loop
        double cosPhi = 1.0, sinPhi = 0.0; // phase of the jet path at f0
        double w0 = 0.06;          // rad/sample
        double period = 800.0;     // sounding period P (samples)
        double velPhase = 0.0;     // arg V(w0)
    } comp_;
    int harmonic_ = 1;
    float jetPressure_ = -1.0f; // pressure the jet delay was last computed for
    float ampMs_ = 0.0f, ampCoeff_ = 0.001f;
    dsp::DelayLine vHist_;     // embouchure velocity history (periodic-amplitude estimate)
    int retuneCounter_ = 0;

    double sampleRate_ = 48000.0;
    EngineControls controls_{};  // clamped targets
    bool haveControls_ = false;
    bool snapNext_ = true;

    // Building blocks
    dsp::DelayLine bore_;       // round-trip bore waveguide
    dsp::DelayLine jet_;        // jet travel (flue -> labium)
    dsp::OnePoleLowpass endLp_; // end-reflection loss
    dsp::DcBlocker dc_;
    dsp::DcBlocker rad_;        // radiation (output) highpass
    float radNorm_ = 1.0f;
    float splitRest_ = 0.0f;   // tanh(y0) for the current jet offset
    float chiffEnv_ = 0.0f, chiffDecay_ = 0.99f, lastP_ = 0.0f;
    dsp::OnePoleLowpass velLp_; // embouchure velocity (leaky integrator)
    dsp::OnePoleLowpass noiseHp_, noiseLp1_, noiseLp2_;
    dsp::JetNonlinearity jetNl_;
    dsp::WhiteNoise noise_;
    std::uint32_t noiseSeed_ = 0x1234567u;

    // Smoothed per-sample parameters
    dsp::Smoother pressure_, boreDelay_, jetDelay_, endPole_, velPole_, velNorm_, wallGain_;
    dsp::Smoother noiseGain_;
    dsp::Smoother ventDelay_, ventW1_, ventW2_; // register vent comb (taps at +D, +2D)
    dsp::Smoother jetK_, jetY0_, chiff_;        // per-register jet shape, onset chiff amount

    // Targets computed by updateTargets()
    double boreDelayTarget_ = 100.0;
    double jetDelayTarget_ = 30.0;

    float lastSplit_ = 0.0f;   // previous jet split (for noise modulation)
    int quietCount_ = 0;
    bool silent_ = true;
    std::uint64_t nonFiniteResets_ = 0;
};

} // namespace mf
