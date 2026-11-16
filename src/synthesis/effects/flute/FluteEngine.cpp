// Jet-drive + waveguide-bore flute model (M1 "core oscillator", M2 "fingering
// & registers", M3 "tone & noise").
//
// Original implementation. Design informed by the STK Flute (Cook & Scavone),
// Coltman's jet-drive experiments, J.O. Smith's CCRMA waveguide wind pages and
// the Verge/Fabre jet-drive literature; no STK code is used. See THIRD_PARTY.md.
//
// ---------------------------------------------------------------------------
// Signal flow (one sample):
//
//        breath noise (band-limited, ∝ pressure, modulated by the jet, + chiff)
//                         |  (in-tube share)                      (direct share)
//   +--> -kf * V(z) --> (+) --> [ jet delay Mj ] --> tanh split --> x sqrt(p) = q
//   |   (embouchure                                                          |
//   |    velocity)                                                           v
//   y <-- DC <-- g * LP <-- [ bore delay Nb (round trip) ] <-- (+) <-- vortex(y)
//   |                                                   (+ register vent comb)
//   +--> (+ direct noise) --> radiation d/dt --> gain, soft limit --> output
//
// * Bore: a cylindrical tube open at both ends (embouchure hole and foot). A
//   pressure wave travels to the foot, reflects with inversion through a
//   frequency-dependent loss (lowpass "LP": the tone-hole cutoff, fixed in Hz
//   around 2 kHz and set by brightness, see kEndCutoffHz) and a broadband wall
//   loss g, travels back and reflects again,
//   with inversion, at the open embouchure. The two inversions cancel, so ONE
//   delay line holding the full round trip with a positive loop gain g*LP
//   models the whole tube; its resonances are all the harmonics k*f0 of an
//   open-open pipe (unlike a clarinet's odd harmonics). A DC blocker in the
//   loop stops the asymmetric jet's DC flow accumulating.
//
// * Vortex loss: flow separating at the sharp embouchure edges sheds vortices,
//   a loss growing with |u| u. Since M3 it shares the amplitude limiting with
//   the jet's saturation (see kVortexLoss), so louder playing is brighter.
//
// * Embouchure velocity: the jet is deflected by the acoustic VELOCITY through
//   the embouchure hole, not by the pressure. The hole's air plug is an
//   inertance (p = L dv/dt), so velocity is a leaky time-integral of pressure.
//   We model it as a one-pole lowpass V(z) with its corner at 0.5*f0,
//   normalised to unit gain at f0. Besides being physically motivated, its
//   ~-63 deg lag at f0 and 1/k rolloff over harmonics make the classic jet
//   ratio (~0.32 of the period) select the FUNDAMENTAL register (loop phase
//   error ~1 deg at 0.32), and keep upper harmonics from winning the mode
//   competition (no squeal two octaves up).
//
// * Jet: the disturbance convects along the jet at about half the jet velocity
//   and reaches the labium Mj = jetRatio * period later (fractional delay).
//   The excitation entering the jet delay is (turbulence - kf * velocity); the
//   blowing pressure enters as the jet velocity U = sqrt(p) (Bernoulli), which
//   scales the flow the jet delivers and hence the loop gain (threshold of
//   oscillation around p ~ 0.1-0.2, harmonics grow with pressure).
//
// * Jet nonlinearity (labium split): q = U * (tanh(K*eta + y0) - tanh(y0)),
//   the Bickley-jet flow-split law (see dsp/JetNonlinearity.h). Bounded, smooth,
//   asymmetric (even harmonics), zero static flow (no thump on a pressure step).
//   K and y0 depend on the register (kJetGain, kLabiumOffset; M3).
//
// * Noise: white xorshift noise, band-limited (HP 200 Hz, 2x LP whose corner
//   follows the jet velocity, 3 kHz at p = 0.7), amplitude ∝ pressure *
//   (noiseGain + tiny seed floor), modulated by the instantaneous jet split
//   (70 % depth) so the turbulence pulses with the tone, as in real flutes,
//   and boosted briefly at sudden onsets (chiff). It is heard three ways: as a
//   turbulent flow source at the labium (added to q, filtered by the bore
//   resonances), as a small perturbation of the jet deflection (seeds the
//   onset, natural jitter), and directly at the mouth-hole (output only). The
//   in-tube share is capped at noiseGain 0.1 (pitch stability, see
//   kNoiseFlowScale). Level is normalised for sample rate. Zero pressure ->
//   zero noise.
//
// * Radiation (M3): the output is the time derivative of the flow at the
//   mouth-hole (bore wave plus direct noise), +6 dB/octave up to 5.75 kHz.
//
// ---------------------------------------------------------------------------
// Fingering and registers (M2).
// * Fingering = tube length. frequencyHz may change at any time: the bore and
//   jet delays glide to their new targets with a 5 ms time constant (the
//   Lagrange interpolator is stateless, so modulating it is click-free). The
//   wave already in the tube is stretched or compressed by the moving read
//   point, which carries the energy of the old note into the new one.
// * Register = which resonance of the fingered tube sounds: harmonic k, with
//   the tube fundamental F = f0 / k (k = 1, 2, 3: fundamental, octave,
//   twelfth). Chosen explicitly (controls.harmonic) or by the fingering-chart
//   rule autoHarmonic(). For register k the round-trip delay is ~k periods of
//   f0, the jet delay is set relative to the SOUNDING period (the player's
//   shorter jet), the wall loss is that of the longer tube, and a register
//   vent comb (see kVentDepth) keeps the tube's lower modes from taking over.
// * Pressure -> jet coupling: blowing harder speeds up the jet and shortens
//   its delay, ratio_eff = jetRatio * (0.7 / p)^0.25 (see kJetCoupling),
//   clamped to a measured safe range per register.
// ---------------------------------------------------------------------------
// Pitch compensation (analytic, harmonic balance at f0).
// Write the loop as  w = DFv*y + q,  y = B(z) w,  q = J(z) y  with
//     B(z) = g * LP(z) * DC(z) * Lagrange_Nb(z)          (bore round trip)
//     J(z) = -kf * c * V(z) * Lagrange_Mj(z)             (jet path)
//     DFv  = describing function of the vortex loss at the current amplitude
//     c    = describing-function gain of the jet (real: memoryless nonlinearity)
// The characteristic equation is B DFv (1 + J/DFv) = 1. In steady state the
// nonlinearities settle until the magnitude condition holds; the PHASE of J is
// fixed by the filters and the jet delay. With phi = arg J(w0) and
// g'' = |B(w0)| DFv(A):
//     |1 + |J| e^{j phi}| = 1/g''  ->  |J| = -cos phi + sqrt(cos^2 phi - 1 + 1/g''^2)
// so theta = arg(1 + J) (the jet's pull on the frequency) is known, and the
// phase condition
//     -w0 * pd_Lagrange(Nb) + arg(g LP DC)(w0) + theta = -2 pi
// gives Nb directly (in register k the right-hand side is -2 pi k, so Nb ~ k P;
// the vent comb is exactly transparent at f0 and adds nothing). The jet phase
// phi is re-evaluated every 16 samples because the pressure coupling moves Mj. pd_Lagrange is evaluated exactly from the interpolator's
// coefficients. The only signal-dependent input is the amplitude A (a 20 ms RMS
// follower of the fundamental-weighted velocity v), re-solved every 16
// samples, with |J| capped by the jet's small-signal gain below threshold;
// everything else is closed-form phase response at f0. No tuning table.
// ---------------------------------------------------------------------------

#include "mf/FluteEngine.h"
#include "mf/dsp/Denormals.h"

#include <algorithm>
#include <cmath>
#include <complex>

namespace mf {
namespace {

constexpr double kPi = 3.14159265358979323846;

// --- Bore ---
// Broadband (visco-thermal wall) loss per round trip. Wall losses grow with
// tube length and with sqrt(f), so per period they scale ~ 1/sqrt(f0):
//   g_wall = exp(-kWallLoss * sqrt(440 / f0))   (0.93 at A4, ~0.91 at C4)
// giving a bore Q of the order of 30-40, as measured on real flutes.
constexpr double kWallLoss = 0.0726;
// Nonlinear (vortex-shedding) loss at the embouchure edges, ~ |u| u. Applied
// as a smooth compressor w = y / (1 + kVortex |y|). M1 used 0.15, which made
// it the main amplitude limiter and kept the jet in the same mildly nonlinear
// range at every dynamic (the tone barely brightened when blown harder). At
// 0.06 the jet's own saturation shares the limiting, so a loud note drives the
// jet further into saturation and gains upper harmonics (M3, see M3-report).
constexpr float kVortexLoss = 0.06f;
constexpr double kDcCutoffHz = 20.0;
// End / tone-hole reflection lowpass (M3). Above roughly 2 kHz the lattice of
// open tone holes stops reflecting sound back up the tube (the tone-hole
// cutoff; Wolfe et al. measure about 2 kHz on modern flutes), so resonances
// above it are weak whatever the note. The corner is therefore FIXED IN HZ:
// 2 kHz at brightness 0.5, 1.41 / 2.83 kHz (half an octave) at brightness
// 0 / 1. It never goes below 3 f0, so the fundamental is barely damped: at
// 2 f0 (-1 dB per round trip at f0) soft top-octave notes failed to start
// with non-default jets; 3 f0 costs ~0.5 dB of reference match (upper-register
// H3 a little strong). A full octave of brightness range (1 kHz) sat so close
// to the middle notes' fundamental that soft notes no longer started. M1 used
// a corner proportional to f0, which gave every note the same spectrum.
constexpr double kEndCutoffHz = 2000.0;
constexpr double kEndCutoffOctaves = 0.5;    // +- range over brightness 0..1
constexpr double kEndMinRatio = 3.0;         // corner >= 3 f0
constexpr double kMaxEndCutoff = 0.40;       // and <= 0.4 fs

// --- Embouchure velocity / jet ---
constexpr double kVelocityCornerRatio = 0.5; // leaky-integrator corner, x f0
constexpr double kFeedback = 1.0;            // kf
// Jet nonlinearity per register (M3, fitted to the UNSW reference spectra,
// see docs/research/unsw_flute_spectra.csv): K = receptivity (1 / jet
// half-width), y0 = offset of the jet from the labium edge in half-widths.
// The offset sets the even harmonics (Fletcher & Douglas): in the low register
// the jet is well off-centre (strong H2), in the upper registers the player
// centres it on the edge (H2 about -25 dB) and the jet is driven more gently.
// The small-signal gain K sech^2(y0) is about the same in every register
// (0.36, 0.53, 0.54), so the oscillation threshold barely changes.
constexpr float kJetGain[3] = {1.0f, 0.55f, 0.55f};
constexpr float kLabiumOffset[3] = {1.1f, 0.2f, 0.12f};

// --- Noise ---
constexpr float kNoiseSeedFloor = 0.002f;    // keeps a tiny seed when noiseGain = 0
// Turbulence spectrum: its upper corner scales with the jet velocity (the
// Strouhal number of the jet's eddies, f ~ U / h), 5 kHz at pressure 0.7.
constexpr double kNoiseLpRefHz = 3000.0;
// Part of the breath noise is heard DIRECTLY from the mouth-hole, not only
// after filtering by the tube's resonances; this fills the gaps between the
// harmonics, as on the reference recordings (M3). In flow units.
constexpr float kNoiseDirect = 4.0f;
// Chiff (onset turbulence): when the breath rises quickly, the jet is briefly
// turbulent and edge-tone-like before the tube locks in. Modelled as a boost
// of the breath noise driven by the RELATIVE rise rate of the pressure
// (d ln p / dt, integrated with a 15 ms decay and capped at 1), so it depends
// on how sudden the attack is, not on how loud: noise x (1 + kChiffBoost *
// chiff * env), at most +14 dB over the steady breath noise at chiff = 1.
// Slow swells (d ln p / dt * 15 ms << 1) produce almost none.
constexpr float kChiffBoost = 4.0f;
constexpr double kChiffTau = 0.015;
// The turbulence enters three ways: as a small perturbation of the jet
// deflection (seeds the onset, adds natural jitter), as a turbulent flow
// source at the labium that the tube filters (resonant breath), and directly
// at the mouth-hole (kNoiseDirect). Keeping the in-tube parts moderate
// matters: M3 measured that strong in-tube turbulence (about 2.5x the M2
// level) kicks long-jet notes into a second oscillation regime about 60 cents
// sharp. So the in-tube level is capped at the default noiseGain (0.1): more
// "Air" than that models air that misses the edge, which is heard directly
// but does not drive the tube.
constexpr float kNoiseJetScale = 2.0f;       // in jet-deflection units
constexpr float kNoiseFlowScale = 4.0f;      // in flow units
constexpr float kNoiseRefGain = 0.1f;
constexpr float kNoiseModDepth = 0.7f;       // jet modulation depth of the noise
// Noise level and bandwidth vs blowing pressure (M3 listening fix): level ~
// (p / 0.7)^kNoiseLevelExp, bandwidth ~ (p / 0.7)^kNoiseBandExp, both 1 at
// p = 0.7 (where the level matches the reference). With level ~ p and
// bandwidth ~ sqrt(p) the noise rose ~20 dB from pp to ff while the tone rose
// ~6 dB, so loud notes turned noisy and buzzy (the noise pulses with the jet).
constexpr float kNoiseLevelExp = 0.5f;
constexpr double kNoiseBandExp = 0.25;
constexpr double kNoiseHpHz = 200.0;

// --- Output / safety ---
// Radiation (M3). The wave y returning to the open embouchure is proportional
// to the acoustic flow there; the sound radiated from an open pipe end is
// proportional to the time derivative of that flow (+6 dB/octave) up to
// ka ~ 1 (a = 9.5 mm bore radius -> 5.75 kHz), where it levels off. Modelled
// as a one-pole highpass at 5.75 kHz, normalised to unity gain at 1 kHz.
constexpr double kRadiationHz = 5750.0;
constexpr double kRadiationNormHz = 1000.0;
constexpr float kOutputGain = 0.07f;         // M1: 0.12; radiation raised the peaks (M3)
constexpr float kLimitKnee = 0.7f;           // linear below -3 dBFS (M1: -6)
constexpr float kLimitCeiling = 0.99f;       // asymptote, never reaches 0 dBFS
constexpr float kRunawayLimit = 1.0e3f;      // internal state sanity bound
constexpr float kQuietThreshold = 1.0e-7f;   // ~ -140 dB re full internal scale

// --- Registers and jet/pressure coupling (M2) ---
// Register k: the fingered tube's fundamental is F = f0 / k and the note
// sounds on its k-th resonance. The bore delay is ~k periods of the sounding
// pitch; the jet delay (embouchure) is set relative to the SOUNDING period,
// i.e. the player shortens the jet by k for the upper registers.
// Automatic rule (controls.harmonic = 0), after the Boehm-flute fingering
// chart: 1st register C4..C#5, 2nd (octave) D5..C#6, 3rd (twelfth) from D6.
// Boundaries are the geometric midpoints C#5/D5 and C#6/D6 at A4 = 440 Hz.
constexpr double kAutoReg2Hz = 570.61, kAutoReg3Hz = 1141.22;
// Jet velocity is ~sqrt(p) (Bernoulli), so at a fixed lip-to-edge distance
// the jet delay would scale as p^-0.5. A player compensates with the lips:
//   ratio_eff = jetRatio * (kJetPressureRef / p)^kJetCoupling
// M2 kept a quarter of the effect (0.25). M3 measured that this shortened jet
// DARKENS loud notes (it moves the harmonics' phase away from the jet's
// optimum) and cancels the brightening with dynamics, so the player is now
// modelled as compensating fully: kJetCoupling = 0 (the jet ratio holds).
constexpr double kJetPressureRef = 0.7;
constexpr double kJetCoupling = 0.0;
constexpr double kJetCouplingMinP = 0.25;   // below: the coupling is frozen
// Safe EFFECTIVE jet-ratio range per register (index k-1), measured with
// `mf_render --sweep` (see M2-report.md): inside it the loop locks onto the
// requested register at every pressure 0.3..1.4.
// Register vent. In register k the tube's lower resonances F, 2F, ..
// (k-1)F would otherwise compete with the sounding mode kF and usually win at
// soft pressures (they lose less and the embouchure velocity favours them).
// On a real flute the fingering helps: upper-register fingerings open vent
// holes near the pressure nodes of the wanted mode, the principle of a
// clarinet's speaker key. Modelled as a comb on the wave returning from the
// bore, built from extra taps of the bore delay line D = one sounding period
// apart:   r = (1 - a) r(Nb) + a/k * sum_{i<k} r(Nb + i D).
// Its zeros sit on the tube modes that are not multiples of kF, and it is
// exactly transparent (gain 1, phase 0) on f0 and all its harmonics, so the
// tone and the pitch compensation of the sounding note are unchanged. The
// depth a < 1 leaves the lower resonances weakly present (they still colour
// the breath noise), like a small real vent hole.
constexpr double kVentDepth[3] = {0.0, 0.8, 0.8};

constexpr double kEffJetMin[3] = {0.20, 0.20, 0.20};
constexpr double kEffJetMax[3] = {0.42, 0.46, 0.46};

// --- Smoothing ---
constexpr double kPressureTau = 0.003;       // 3 ms: no zipper, onset stays crisp
constexpr double kParamTau = 0.005;
constexpr double kGlideTau = 0.005;          // bore/jet delay glide on note changes
constexpr double kVentTau = 0.005;           // register vent open/close
constexpr double kJetTau = 0.005;            // jet delay glide
constexpr double kAmpTau = 0.020;            // amplitude follower for the pitch compensation
constexpr int kRetuneInterval = 16;          // samples between compensation updates
constexpr double kMaxJetPull = 0.6;          // rad, bound on the compensated jet phase pull

inline float softLimit(float x) {
    const float a = std::fabs(x);
    if (a <= kLimitKnee) return x;
    const float range = kLimitCeiling - kLimitKnee;
    return std::copysign(kLimitKnee + range * std::tanh((a - kLimitKnee) / range), x);
}

} // namespace

FluteEngine::FluteEngine() {
    jetNl_.gain = kJetGain[0];
    jetNl_.offset = kLabiumOffset[0];
}
FluteEngine::~FluteEngine() = default;

void FluteEngine::prepare(double sampleRate, int /*maxBlockSize*/) {
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    // Size for the lowest supported pitch (100 Hz) plus compensation headroom.
    // Longest read: register-3 bore (<= one 100 Hz period) plus two vent taps.
    const auto maxBore = static_cast<std::size_t>(std::ceil(sampleRate_ / kMinFrequency * 2.0)) + 16;
    bore_.allocate(maxBore);
    vHist_.allocate(static_cast<std::size_t>(std::ceil(sampleRate_ / kMinFrequency)) + 16);
    jet_.allocate(static_cast<std::size_t>(std::ceil(sampleRate_ / kMinFrequency * 0.6)) + 16);

    pressure_.setTimeConstant(kPressureTau, sampleRate_);
    ampCoeff_ = static_cast<float>(1.0 - std::exp(-1.0 / (kAmpTau * sampleRate_)));
    for (auto* s : {&endPole_, &velPole_, &velNorm_, &wallGain_, &noiseGain_, &jetK_, &jetY0_, &chiff_})
        s->setTimeConstant(kParamTau, sampleRate_);
    for (auto* s : {&boreDelay_, &ventDelay_}) s->setTimeConstant(kGlideTau, sampleRate_);
    jetDelay_.setTimeConstant(kJetTau, sampleRate_);
    for (auto* s : {&ventW1_, &ventW2_}) s->setTimeConstant(kVentTau, sampleRate_);

    dc_.setPole(static_cast<float>(dsp::DcBlocker::poleForCutoff(kDcCutoffHz, sampleRate_)));
    {
        const double R = dsp::DcBlocker::poleForCutoff(std::min(kRadiationHz, 0.45 * sampleRate_), sampleRate_);
        rad_.setPole(static_cast<float>(R));
        radNorm_ = static_cast<float>(
            1.0 / std::abs(dsp::DcBlocker::response(R, 2.0 * kPi * kRadiationNormHz / sampleRate_)));
    }
    chiffDecay_ = static_cast<float>(std::exp(-1.0 / (kChiffTau * sampleRate_)));
    noiseHp_.setPole(static_cast<float>(dsp::OnePoleLowpass::poleForCutoff(kNoiseHpHz, sampleRate_)));
    setNoiseBand(kJetPressureRef);
    reset();
}

void FluteEngine::clearState() {
    bore_.clear();
    jet_.clear();
    endLp_.reset();
    dc_.reset();
    rad_.reset();
    velLp_.reset();
    noiseHp_.reset();
    noiseLp1_.reset();
    noiseLp2_.reset();
    lastSplit_ = 0.0f;
    chiffEnv_ = 0.0f;
    lastP_ = 0.0f;
    quietCount_ = 0;
    ampMs_ = 0.0f;
    vHist_.clear();
    retuneCounter_ = 0;
}

void FluteEngine::reset() {
    clearState();
    noise_.setSeed(noiseSeed_);
    controls_.pressure = 0.0f;  // reset == stop: silent until the next setControls()
    pressure_.snap(0.0f);
    noiseGain_.snap(controls_.noiseGain);
    silent_ = true;
    snapNext_ = true;
    updateTargets(true);
}

void FluteEngine::setNoiseSeed(std::uint32_t seed) {
    noiseSeed_ = seed;
    noise_.setSeed(seed);
}

void FluteEngine::copyStateFrom(const FluteEngine& other, std::uint32_t noiseSeed) {
    if (&other == this) return;
    const std::uint32_t keepSeed = noiseSeed_;
    *this = other;
    noiseSeed_ = keepSeed;
    noise_.setSeed(noiseSeed);
}

void FluteEngine::setControls(const EngineControls& in) {
    EngineControls c = in;
    auto finiteOr = [](float v, float fallback) { return std::isfinite(v) ? v : fallback; };
    const float fMax = std::min(kMaxFrequency, static_cast<float>(sampleRate_ / 8.0));
    c.pressure = std::clamp(finiteOr(c.pressure, 0.0f), kMinPressure, kMaxPressure);
    c.frequencyHz = std::clamp(finiteOr(c.frequencyHz, 440.0f), kMinFrequency, fMax);
    c.jetRatio = std::clamp(finiteOr(c.jetRatio, 0.32f), kMinJetRatio, kMaxJetRatio);
    c.noiseGain = std::clamp(finiteOr(c.noiseGain, 0.04f), kMinNoise, kMaxNoise);
    c.brightness = std::clamp(finiteOr(c.brightness, 0.5f), 0.0f, 1.0f);
    c.harmonic = std::clamp(std::round(finiteOr(c.harmonic, 0.0f)), 0.0f, static_cast<float>(kMaxHarmonic));
    c.chiff = std::clamp(finiteOr(c.chiff, 0.0f), 0.0f, 1.0f);

    const bool shapeChanged = !haveControls_ || c.frequencyHz != controls_.frequencyHz ||
                              c.jetRatio != controls_.jetRatio || c.brightness != controls_.brightness ||
                              c.harmonic != controls_.harmonic;
    controls_ = c;
    haveControls_ = true;
    pressure_.setTarget(c.pressure);
    noiseGain_.setTarget(c.noiseGain);
    chiff_.setTarget(c.chiff);
    if (silent_) {
        noiseGain_.snap(c.noiseGain);
        chiff_.snap(c.chiff);
    }
    if (shapeChanged || snapNext_) updateTargets(snapNext_ || silent_);
    snapNext_ = false;
}

int FluteEngine::autoHarmonic(double f) { return f < kAutoReg2Hz ? 1 : (f < kAutoReg3Hz ? 2 : 3); }

// Computes filter coefficients and the compensated delays for the current
// controls (see "Pitch compensation" above). Called only when the pitch, jet
// ratio, register or brightness change; a handful of trig calls, no allocation.
void FluteEngine::updateTargets(bool snap) {
    const double fs = sampleRate_;
    const double f0 = std::clamp(static_cast<double>(controls_.frequencyHz), static_cast<double>(kMinFrequency),
                                 std::min(static_cast<double>(kMaxFrequency), fs / 8.0));
    const double P = fs / f0;          // target period in samples
    const double w = 2.0 * kPi / P;    // rad/sample

    // Register: harmonic k of a tube of k periods. The tube may not be longer
    // than the one for the lowest supported pitch (buffer size).
    int k = controls_.harmonic > 0.0f ? static_cast<int>(controls_.harmonic) : autoHarmonic(f0);
    while (k > 1 && f0 / k < kMinFrequency) --k;
    harmonic_ = k;

    // End / tone-hole reflection lowpass: fixed in Hz, set by brightness.
    const double endHz = std::max(kEndCutoffHz * std::pow(2.0, kEndCutoffOctaves * 2.0 * (controls_.brightness - 0.5)),
                                  kEndMinRatio * f0);
    const double endPole = dsp::OnePoleLowpass::poleForCutoff(std::min(endHz, kMaxEndCutoff * fs), fs);
    const double velPole = dsp::OnePoleLowpass::poleForCutoff(kVelocityCornerRatio * f0, fs);
    const double dcPole = dsp::DcBlocker::poleForCutoff(kDcCutoffHz, fs);
    // Wall loss over the round trip of a tube k/f0 long, evaluated at f0.
    const double gWall = std::exp(-kWallLoss * k * std::sqrt(440.0 / f0));

    const auto Hv = dsp::OnePoleLowpass::response(velPole, w);
    const double velNorm = 1.0 / std::abs(Hv);
    const auto Hloop = gWall * dsp::OnePoleLowpass::response(endPole, w) *
                       dsp::DcBlocker::response(dcPole, w);

    // Linear part of the bore compensation: the end filter, DC blocker and
    // interpolator phase at f0, for a loop of k periods (phase -2 pi k). The
    // Lagrange phase-delay error is evaluated from its coefficients (a tiny
    // correction, O(w^4)).
    const double kP = k * P;
    const double lagErr = dsp::DelayLine::phaseDelayAt(kP, w) - kP;
    comp_.boreBase = kP + std::arg(Hloop) / w - lagErr;
    comp_.linearGain = std::abs(Hloop) * dsp::DelayLine::magnitudeAt(kP, w);
    jetK_.setTarget(kJetGain[k - 1]);
    jetY0_.setTarget(kLabiumOffset[k - 1]);
    comp_.w0 = w;
    comp_.period = P;
    comp_.velPhase = std::arg(Hv);
    updateJet();
    const double Nb = boreDelayFor(ampEstimate());
    boreDelayTarget_ = Nb;
    boreDelay_.setTarget(static_cast<float>(Nb));
    const double a = kVentDepth[k - 1];
    ventDelay_.setTarget(static_cast<float>(P));
    ventW1_.setTarget(static_cast<float>(k >= 2 ? a / k : 0.0));
    ventW2_.setTarget(static_cast<float>(k >= 3 ? a / k : 0.0));
    endPole_.setTarget(static_cast<float>(endPole));
    velPole_.setTarget(static_cast<float>(velPole));
    velNorm_.setTarget(static_cast<float>(velNorm));
    wallGain_.setTarget(static_cast<float>(gWall));
    if (snap) {
        for (auto* s : {&boreDelay_, &jetDelay_, &endPole_, &velPole_, &velNorm_, &wallGain_, &ventDelay_, &ventW1_,
                        &ventW2_, &jetK_, &jetY0_})
            s->snap(s->target());
        jetNl_.gain = jetK_.value();
        jetNl_.offset = jetY0_.value();
        splitRest_ = std::tanh(jetNl_.offset);
    }
}

// Turbulence band: the lowpass corner follows the jet velocity sqrt(p).
void FluteEngine::setNoiseBand(double p) {
    const double fc = kNoiseLpRefHz * std::pow(std::max(p, 0.02) / kJetPressureRef, kNoiseBandExp);
    const auto lp = static_cast<float>(
        dsp::OnePoleLowpass::poleForCutoff(std::clamp(fc, 500.0, 0.4 * sampleRate_), sampleRate_));
    noiseLp1_.setPole(lp);
    noiseLp2_.setPole(lp);
}

// Harmonic-balance estimate of the steady-state loop: the vortex loss lowers
// the effective bore gain at amplitude A by its describing function
// DF(A) ~= 1 / (1 + 8 kVortex A / (3 pi)), so the jet must supply
// |J| = -cos phi + sqrt(cos^2 phi - 1 + 1/g''^2), whose phase pull theta is
// then cancelled by lengthening the bore by theta / w0 samples.
// Below the oscillation threshold (and early in the onset) the jet cannot
// supply that much: |J| is capped by its small-signal gain U * K sech^2(y0) kf
// (V is normalised at f0), which is what then sets the resonance frequency.
double FluteEngine::boreDelayFor(double amplitude) const {
    const double df = 1.0 / (1.0 + 8.0 * kVortexLoss * amplitude / (3.0 * kPi));
    const double g = comp_.linearGain * df;
    const double c = comp_.cosPhi, s = comp_.sinPhi;
    const double jSmall = std::sqrt(static_cast<double>(pressure_.value())) * jetNl_.slope() * kFeedback / df;
    const double J = std::min(jSmall, -c + std::sqrt(std::max(0.0, c * c - 1.0 + 1.0 / (g * g))));
    const double theta = std::clamp(std::atan2(J * s, 1.0 + J * c), -kMaxJetPull, kMaxJetPull);
    return std::clamp(comp_.boreBase + theta / comp_.w0, dsp::DelayLine::minDelay() + 1.0, bore_.maxDelay());
}

// Jet delay from the embouchure (jetRatio), the register and the current
// blowing pressure, and its phase at f0 for the compensation. Re-run every
// kRetuneInterval samples because the pressure moves.
void FluteEngine::updateJet() {
    jetPressure_ = pressure_.value();
    const double p = std::max(static_cast<double>(jetPressure_), kJetCouplingMinP);
    setNoiseBand(jetPressure_);
    const int r = harmonic_ - 1;
    const double ratio = std::clamp(controls_.jetRatio * std::pow(kJetPressureRef / p, kJetCoupling),
                                    kEffJetMin[r], kEffJetMax[r]);
    const double w = comp_.w0;
    const double Mj = std::clamp(ratio * comp_.period, dsp::DelayLine::minDelay(), jet_.maxDelay());
    const double phi = kPi + comp_.velPhase - w * dsp::DelayLine::phaseDelayAt(Mj, w);
    comp_.cosPhi = std::cos(phi);
    comp_.sinPhi = std::sin(phi);
    jetDelayTarget_ = Mj;
    jetDelay_.setTarget(static_cast<float>(Mj));
}

double FluteEngine::ampEstimate() const { return std::sqrt(2.0 * static_cast<double>(ampMs_)); }

void FluteEngine::process(float* out, int numSamples) {
    dsp::ScopedFlushDenormals ftz;

    const float noiseNorm = static_cast<float>(std::sqrt(sampleRate_ / 48000.0)); // keep band power fs-invariant

    for (int n = 0; n < numSamples; ++n) {
        // At rest with no breath: exact zeros, state is already cleared.
        if (silent_) {
            if (pressure_.target() <= 0.0f) {
                out[n] = 0.0f;
                continue;
            }
            silent_ = false;
        }

        // --- Smoothed controls ---
        float p = pressure_.next();
        if (pressure_.target() <= 0.0f && p < 1.0e-9f) {
            pressure_.snap(0.0f);
            p = 0.0f;
        }
        const float U = std::sqrt(p);  // jet velocity (Bernoulli)
        const float Nb = boreDelay_.next();
        const float Mj = jetDelay_.next();
        endLp_.setPole(endPole_.next());
        velLp_.setPole(velPole_.next());
        const float vNorm = velNorm_.next();
        const float nGain = noiseGain_.next();
        const float gWall = wallGain_.next();
        // Register-dependent jet shape (smoothed across register changes).
        {
            const float k = jetK_.next(), y0 = jetY0_.next();
            jetNl_.gain = k;
            if (y0 != jetNl_.offset) {
                jetNl_.offset = y0;
                splitRest_ = std::tanh(y0);
            }
        }

        // --- Bore: wave returning to the embouchure after one round trip ---
        float returning = bore_.read(Nb);
        // Register vent comb (inactive, and skipped, in the 1st register).
        const float w1 = ventW1_.next(), w2 = ventW2_.next();
        const float D = ventDelay_.next();
        if (w1 > 1.0e-6f || ventW1_.target() > 0.0f) {
            float acc = (1.0f - w1 - w2) * returning + w1 * bore_.read(Nb + D);
            if (w2 > 1.0e-6f || ventW2_.target() > 0.0f) acc += w2 * bore_.read(Nb + 2.0f * D);
            returning = acc;
        }
        const float y = dc_.process(gWall * endLp_.process(returning));

        // --- Embouchure acoustic velocity (leaky integral of pressure) ---
        const float v = vNorm * velLp_.process(y);

        // --- Breath turbulence: band-limited, ∝ pressure, jet-modulated ---
        const float white = noise_.next();
        const float hp = white - noiseHp_.process(white);
        const float band = noiseLp2_.process(noiseLp1_.process(hp));
        const float mod = (1.0f - kNoiseModDepth) + kNoiseModDepth * 0.5f * (1.0f + lastSplit_);
        // Chiff: burst following the rising breath (15 ms decay).
        chiffEnv_ = std::min(1.0f, chiffEnv_ * chiffDecay_ + std::max(p - lastP_, 0.0f) / std::max(p, 0.05f));
        lastP_ = p;
        const float burst = 1.0f + kChiffBoost * chiff_.next() * chiffEnv_;
        const float pNoise = static_cast<float>(kJetPressureRef) *
                             std::pow(p * static_cast<float>(1.0 / kJetPressureRef), kNoiseLevelExp);
        const float turb = noiseNorm * pNoise * (nGain + kNoiseSeedFloor) * mod * burst * band;
        // In-tube share, capped at the default noiseGain (see kNoiseFlowScale).
        const float turbIn = turb * std::min(1.0f, kNoiseRefGain / (nGain + kNoiseSeedFloor));

        // --- Jet: excitation travels to the labium, then splits ---
        const float excitation = kNoiseJetScale * turbIn - static_cast<float>(kFeedback) * v;
        const float eta = jet_.read(Mj);
        jet_.write(excitation);
        const float splitDev = jetNl_(eta);       // tanh(K eta + y0) - tanh(y0)
        lastSplit_ = splitDev + splitRest_;       // raw split in (-1, 1)
        const float q = U * splitDev + kNoiseFlowScale * turbIn;  // jet flow + turbulent flow into the bore

        // --- Close the loop ---
        const float wIn = y / (1.0f + kVortexLoss * std::fabs(y)) + q;  // vortex loss + jet flow
        if (!(std::fabs(wIn) < kRunawayLimit)) {  // also catches NaN / Inf
            ++nonFiniteResets_;
            clearState();
            out[n] = 0.0f;
            continue;
        }
        bore_.write(wIn);

        // --- Radiation: d/dt of the flow at the mouth-hole (bore wave plus
        // the breath noise heard directly), then gain and soft limit.
        out[n] = softLimit(kOutputGain * radNorm_ * rad_.process(y + kNoiseDirect * turb));

        // Track the oscillation amplitude and re-solve the pitch compensation
        // every kRetuneInterval samples (fixed internal clock, so the result
        // does not depend on the host block size).
        // Periodic amplitude: v correlated with itself one period earlier. The
        // tone's power survives, breath noise (uncorrelated over a period) does
        // not, so a breathy tone is not mistaken for a louder one (M3).
        const float vPrev = vHist_.read(std::max(comp_.period, dsp::DelayLine::minDelay()));
        vHist_.write(v);
        ampMs_ += ampCoeff_ * (std::max(v * vPrev, 0.0f) - ampMs_);
        if (++retuneCounter_ >= kRetuneInterval) {
            retuneCounter_ = 0;
            // The jet only needs re-solving while the breath moves.
            if (std::fabs(pressure_.value() - jetPressure_) > 1.0e-4f) updateJet();
            boreDelayTarget_ = boreDelayFor(ampEstimate());
            boreDelay_.setTarget(static_cast<float>(boreDelayTarget_));
        }

        // --- Silence gate: once breath is gone and everything has decayed
        // below ~-140 dB for longer than the whole loop, clear to exact zero.
        if (p == 0.0f && std::fabs(wIn) < kQuietThreshold && std::fabs(excitation) < kQuietThreshold) {
            if (++quietCount_ > static_cast<int>(Nb + Mj) + 8) {
                clearState();
                for (auto* s : {&boreDelay_, &jetDelay_, &endPole_, &velPole_, &velNorm_, &wallGain_, &noiseGain_,
                                &ventDelay_, &ventW1_, &ventW2_, &jetK_, &jetY0_, &chiff_})
                    s->snap(s->target());
                silent_ = true;
            }
        } else {
            quietCount_ = 0;
        }
    }
}

} // namespace mf
