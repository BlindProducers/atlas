// Virtual breath (M4): keyboard events -> breath pressure, pitch and tone for
// FluteEngine. Original implementation.
//
// ---------------------------------------------------------------------------
// Articulation (PLAN "Polyphony": strictly monophonic, last-note priority).
// * A key pressed while another is held is a SLUR: the fingering changes, the
//   breath goes on. Releasing the sounding key while others are held slurs
//   back to the most recent of them.
// * A key pressed with no other key held is TONGUED: the breath restarts
//   (from zero after silence; during the release of the previous note it
//   dips to 35 % of where it was, a legato "d" tonguing), then rises with an
//   overshoot. Releasing the last key starts the breath release.
//
// Tongued attack (envelope e, 1 = steady breath):
//     e = (1 + os) (1 - cos(pi t / T_on)) / 2                  t < T_on
//     e = 1 + os exp(-(t - T_on) / tau)                        after
// A real player's mouth pressure overshoots at a tongued start (the pressure
// built up behind the tongue). In the model the overshoot is what makes the
// tube speak quickly: the oscillation grows from the breath noise at a rate
// set by how far the loop gain is above threshold. Measured during M4 on C4 (pressure
// 0.7, quick onset): no overshoot 173 ms to 90 % level; overshoot 1.0 116 ms;
// 2.5 83 ms. Upper registers speak about twice as fast. The Attack macro
// sets the onset time and the overshoot; velocity accents the overshoot;
// the overshoot decays more slowly in the low register, which needs it longer.
//
// Dynamics. The dynamic level D (0 = pp, 1 = ff; velocity or CC1/CC11) sets
// * the breath pressure, a geometric range rising with pitch (pressureFor):
//   it sets the tone colour, which brightens with pressure (M3);
// * the output gain, so that the steady level is levelDb + range (D - 0.5)
//   on every note. FluteEngine alone is ~14 dB louder at C7 than at C4 for
//   the same pressure (radiation +6 dB/octave, M3) and its level rises only
//   ~7 dB from pressure 0.3 to 1.4 (amplitude ~ sqrt(p), the jet velocity).
//   A player's lip opening and air volume also grow with dynamics, which the
//   jet-velocity-only model does not have; the gain stands for that and for
//   the level calibration (engineLevelDb: measured table, see below).
//
// Vibrato is a modulation of the dynamic level, as a breath (diaphragm)
// vibrato is: it moves the pressure (and so the brightness) and the level
// together, plus a small in-phase pitch modulation (pitch rises with blowing
// pressure on a real flute; the engine's pitch compensation removes that, so
// it is added explicitly). The automatic part fades in after a delay on each
// tongued note and continues through slurs; the mod wheel adds to it.
//
// Register slurs. A slur that changes register (e.g. C5 -> C6) needs the new
// resonance of the tube to build up. On upward register slurs the breath gets
// a short push (+50 % for 60 ms, raised sine), as players "support" the
// upper note. On its own it takes ~3 dB off (C#5 -> D5: -21 -> -18 dB);
// with FluteVoice's overlap it makes upward register slurs 1-4 dB shallower
// (C#5 -> D5: -4.9 -> -1.0 dB) but downward ones up to 3 dB deeper (M4).
// The rest of the gap is filled by FluteVoice, which keeps the old note
// sounding on a second engine until the new one has built up.
// ---------------------------------------------------------------------------

#include "mf/BreathPerformer.h"

#include "mf/FluteEngine.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace mf {
namespace {

constexpr double kPi = 3.14159265358979323846;

// --- Pressure range per note (dynamic 0 .. 1), linear in pitch C4 .. C7 ---
constexpr float kPLoC4 = 0.30f, kPLoC7 = 0.42f;
constexpr float kPHiC4 = 1.10f, kPHiC7 = 1.40f;
constexpr float kMinDyn = -0.2f, kMaxDyn = 1.2f;   // with vibrato on top

// --- Level table: FluteEngine sustain RMS (dBFS) at default tone (noise 0.1,
// brightness 0.5, jet 0.32), measured by `mf_render --tuning-table`
// (renders/m3/reports/tuning_table.csv), rows C4 (60) .. C7 (96).
constexpr std::array<float, 5> kTableP = {0.3f, 0.5f, 0.7f, 1.0f, 1.4f};
constexpr float kLevel[37][5] = {
    {-28.3f, -25.7f, -24.3f, -22.9f, -21.6f}, {-27.8f, -25.2f, -23.8f, -22.4f, -21.1f},
    {-27.2f, -24.6f, -23.2f, -21.9f, -20.6f}, {-26.6f, -24.1f, -22.8f, -21.4f, -20.1f},
    {-26.1f, -23.7f, -22.3f, -20.9f, -19.7f}, {-25.6f, -23.2f, -21.8f, -20.4f, -19.2f},
    {-25.1f, -22.7f, -21.4f, -20.0f, -18.8f}, {-24.7f, -22.3f, -20.9f, -19.6f, -18.4f},
    {-24.2f, -21.9f, -20.5f, -19.2f, -18.0f}, {-23.8f, -21.5f, -20.1f, -18.8f, -17.6f},
    {-23.4f, -21.1f, -19.7f, -18.4f, -17.2f}, {-23.0f, -20.7f, -19.4f, -18.1f, -16.9f},
    {-22.6f, -20.3f, -19.0f, -17.7f, -16.5f}, {-22.3f, -20.0f, -18.7f, -17.3f, -16.2f},
    {-27.0f, -23.3f, -21.5f, -19.8f, -18.3f}, {-26.5f, -22.9f, -21.0f, -19.3f, -17.9f},
    {-26.2f, -22.5f, -20.7f, -19.0f, -17.5f}, {-25.6f, -21.9f, -20.1f, -18.4f, -17.0f},
    {-24.8f, -21.3f, -19.5f, -17.8f, -16.4f}, {-24.1f, -20.6f, -18.9f, -17.2f, -15.8f},
    {-23.3f, -20.0f, -18.2f, -16.6f, -15.2f}, {-22.6f, -19.4f, -17.7f, -16.1f, -14.7f},
    {-21.9f, -18.8f, -17.1f, -15.5f, -14.1f}, {-21.2f, -18.1f, -16.5f, -14.9f, -13.5f},
    {-20.6f, -17.5f, -15.9f, -14.3f, -13.0f}, {-19.9f, -16.9f, -15.3f, -13.8f, -12.4f},
    {-22.4f, -18.3f, -16.4f, -14.6f, -13.1f}, {-21.6f, -17.6f, -15.7f, -14.0f, -12.5f},
    {-20.7f, -17.0f, -15.1f, -13.4f, -11.9f}, {-19.9f, -16.3f, -14.5f, -12.8f, -11.4f},
    {-19.2f, -15.7f, -13.9f, -12.2f, -10.8f}, {-18.5f, -15.1f, -13.3f, -11.7f, -10.3f},
    {-17.7f, -14.4f, -12.7f, -11.1f, -9.7f},  {-17.0f, -13.8f, -12.1f, -10.6f, -9.2f},
    {-16.4f, -13.3f, -11.6f, -10.0f, -8.7f},  {-15.7f, -12.6f, -11.0f, -9.5f, -8.1f},
    {-15.1f, -12.1f, -10.5f, -9.0f, -7.6f},
};

// --- Attack (see header comment) ---
// M4 listening fix ("the breath attack is way too sharp"): the first version
// rose in 11 ms to ~2.7x the steady breath and restarted from zero on every
// re-tongued note. Measured on ten isolated notes C4..C7 (default Attack,
// velocity 80): level rise 3.5 dB/ms mean, chiff (4-12 kHz, first 40 ms)
// +7.9 dB over the steady breath noise (+15 dB on E4), level overshoot
// +1.9 dB; in fast_tongued every note re-attacked from zero with its own
// chiff (+6.6 dB mean). Now: a rounder 24 ms onset, a smaller but longer
// overshoot, less chiff, and legato re-tonguing. See M4-report "Fix after
// listening" for the before/after numbers.
constexpr float kOnsetSlowSec = 0.060f, kOnsetFastSec = 0.010f;   // Attack 0 .. 1
constexpr float kOvershootMin = 0.1f, kOvershootSpan = 1.6f;      // Attack 0 .. 1 (was 0.3 + 2.4 a)
constexpr float kOvershootDecay[3] = {0.150f, 0.100f, 0.080f};    // per register (was 80/50/40 ms)
// The upper registers need a stronger push to speak quickly, and there the
// chiff is small (the jet noise lies below their harmonics).
constexpr float kOvershootUpper = 1.6f;
constexpr float kMaxPeakPressure = 2.4f;
// Re-tongued during a release: the breath dips to 35 % of where it was
// (instead of 0) and the overshoot is halved.
constexpr float kRetongueFloor = 0.35f, kRetongueOvershoot = 0.5f;
// Chiff amount from the Attack macro: 0.5 a^1.5 (0.18 at the default 0.5;
// ToneMacros' 0.8 a gave 0.4). Engine boost of the onset noise: +4.8 dB at
// the default instead of +8.3 dB, +9.5 dB at Attack 1.
constexpr float kChiffScale = 0.5f, kChiffExp = 1.5f;                          // engine max 2.5

// --- Dynamics smoothing ---
constexpr float kDynTauController = 0.025f;  // CC steps (1/127) and zipper
constexpr float kDynTauLegato = 0.040f;      // velocity mode: glide to the new note's level

// --- Register slur push (upward register changes only; measured: it helps
// upward changes, and with the overlap made downward ones up to 3 dB worse) ---
constexpr double kPushSec = 0.060;
constexpr float kPushAmount = 0.5f;

// --- Vibrato ---
constexpr float kVibDyn = 0.12f;     // dynamic-scale amplitude at depth 1
constexpr float kVibCents = 10.0f;   // pitch amplitude at depth 1

// --- Humanisation at humanize = 1 ---
constexpr float kHumanDyn = 0.03f;   // +- per-note level offset (dynamic scale)
constexpr float kHumanOnset = 0.15f; // +- relative onset time
constexpr float kHumanRate = 0.04f;  // +- relative vibrato rate (slow drift)
constexpr float kHumanDepth = 0.15f; // +- relative vibrato depth (slow drift)
constexpr double kSlowStepSec = 0.35, kSlowTau = 0.30;

constexpr float kEnvOff = 1.0e-3f;   // release finished below this (-60 dB)

float noteFraction(double note) { return static_cast<float>(std::clamp((note - 60.0) / 36.0, 0.0, 1.0)); }

float onePoleCoeff(double seconds, double tau) { return tau > 0.0 ? static_cast<float>(1.0 - std::exp(-seconds / tau)) : 1.0f; }

} // namespace

BreathPerformer::BreathPerformer() { reset(); }

void BreathPerformer::prepare(double sampleRate) {
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    reset();
}

void BreathPerformer::reset() {
    stack_.clear();
    phase_ = Phase::Idle;
    note_ = -1;
    env_ = 0.0f;
    attackT_ = releaseT_ = 0.0;
    pushT_ = 1e9;
    vibPhase_ = 0.0;
    vibT_ = 0.0;
    slowA_ = slowB_ = slowTargetA_ = slowTargetB_ = 0.0f;
    slowT_ = 0.0;
    dynCc_ = 0.5f;
    expr_ = 1.0f;
    wheel_ = 0.0f;
    bend_ = 0.0f;
    ccSeen_ = false;
    dynTarget_ = dyn_ = 0.5f;
    noteOffset_ = 0.0f;
    rng_ = params_.seed ? params_.seed : 0x5EEDu;
    pendTongued_ = pendLegato_ = pendRegister_ = false;
    lastGain_ = std::pow(10.0f, gainDbFor(params_, 69.0, 0.5f) / 20.0f);
    vibDepthNow_ = 0.0f;
}

void BreathPerformer::setParams(const PerformerParams& p) {
    const bool seedChanged = p.seed != params_.seed;
    params_ = p;
    params_.tone.air = std::clamp(p.tone.air, 0.0f, 1.0f);
    params_.tone.brightness = std::clamp(p.tone.brightness, 0.0f, 1.0f);
    params_.tone.attack = std::clamp(p.tone.attack, 0.0f, 1.0f);
    params_.vibratoDepth = std::clamp(p.vibratoDepth, 0.0f, 1.0f);
    params_.vibratoRateHz = std::clamp(p.vibratoRateHz, 0.5f, 12.0f);
    params_.humanize = std::clamp(p.humanize, 0.0f, 1.0f);
    params_.releaseSec = std::clamp(p.releaseSec, 0.01f, 5.0f);
    params_.dynamicRangeDb = std::clamp(p.dynamicRangeDb, 0.0f, 40.0f);
    if (!(p.tuningA4 > 300.0 && p.tuningA4 < 600.0)) params_.tuningA4 = 440.0;
    if (seedChanged) rng_ = params_.seed ? params_.seed : 0x5EEDu;
}

// ---------------------------------------------------------------------------
// Mappings

float BreathPerformer::pressureFor(double note, float dynamic) {
    const float u = noteFraction(note);
    const float lo = kPLoC4 + (kPLoC7 - kPLoC4) * u;
    const float hi = kPHiC4 + (kPHiC7 - kPHiC4) * u;
    const float d = std::clamp(dynamic, kMinDyn, kMaxDyn);
    return lo * std::pow(hi / lo, d);
}

float BreathPerformer::engineLevelDb(double note, float pressure) {
    const double n = std::clamp(note, 60.0, 96.0);
    const int i0 = std::min(static_cast<int>(std::floor(n)) - 60, 35);
    const float fn = static_cast<float>(n - 60.0 - i0);
    // Linear in log pressure (level ~ 10 log10 p), extrapolated at the ends.
    const float lp = std::log(std::max(pressure, 0.05f));
    int j = 0;
    while (j < 3 && lp > std::log(kTableP[static_cast<std::size_t>(j + 1)])) ++j;
    const float l0 = std::log(kTableP[static_cast<std::size_t>(j)]);
    const float l1 = std::log(kTableP[static_cast<std::size_t>(j + 1)]);
    const float fp = (lp - l0) / (l1 - l0);
    auto row = [&](int r) { return kLevel[r][j] + (kLevel[r][j + 1] - kLevel[r][j]) * fp; };
    // Within a register the level is smooth in pitch; across a register
    // boundary (73|74, 85|86) the table steps, so do not blend across it.
    const int r1 = std::min(i0 + 1, 36);
    if (FluteEngine::autoHarmonic(440.0 * std::pow(2.0, (60 + i0 - 69) / 12.0)) !=
        FluteEngine::autoHarmonic(440.0 * std::pow(2.0, (60 + r1 - 69) / 12.0)))
        return row(fn < 0.5f ? i0 : r1);
    return row(i0) + (row(r1) - row(i0)) * fn;
}

float BreathPerformer::brightnessLevelDb(double note, float brightness) {
    // Steady mezzo-forte level change vs brightness 0.5 (M5 measurement, Air 0.5,
    // mf_tests_plugin "[measure]"): brightness 0 and 1, per note. Air changes the
    // level by 0.2 dB at most and is not corrected.
    static constexpr float kNote[] = {60, 64, 67, 69, 72, 74, 77, 81, 84};
    static constexpr float kDark[] = {-0.61f, -0.86f, -1.06f, -1.19f, -0.93f, -0.52f, 0.0f, 0.0f, 0.0f};
    static constexpr float kBright[] = {0.35f, 0.57f, 0.74f, 0.85f, 1.00f, 0.99f, 1.05f, 0.27f, 0.0f};
    const float n = static_cast<float>(std::clamp(note, 60.0, 84.0));
    int i = 0;
    while (i < 7 && n > kNote[i + 1]) ++i;
    const float u = (n - kNote[i]) / (kNote[i + 1] - kNote[i]);
    const float b = std::clamp(brightness, 0.0f, 1.0f);
    const float* t = b < 0.5f ? kDark : kBright;
    const float edge = t[i] + (t[i + 1] - t[i]) * u;
    return edge * std::abs(b - 0.5f) * 2.0f;
}

float BreathPerformer::gainDbFor(const PerformerParams& p, double note, float dynamic) {
    const float d = std::clamp(dynamic, kMinDyn, kMaxDyn);
    const float target = p.levelDb + p.dynamicRangeDb * (d - 0.5f);
    return target - engineLevelDb(note, pressureFor(note, d)) - brightnessLevelDb(note, p.tone.brightness);
}

BreathPerformer::AttackShape BreathPerformer::attackShape(float attack, float velocity, int harmonic) {
    const float a = std::clamp(attack, 0.0f, 1.0f);
    const float v = std::clamp(velocity, 0.0f, 1.0f);
    AttackShape s;
    // Onset: 60 ms (soft "hoo") .. 10 ms (hard "too"), geometric; 24 ms at 0.5.
    // A harder key strike (velocity) shortens it by up to 30 %.
    s.onsetSec = kOnsetSlowSec * std::pow(kOnsetFastSec / kOnsetSlowSec, a) * (1.3f - 0.6f * v);
    s.overshoot = (kOvershootMin + kOvershootSpan * a) * (0.4f + 1.2f * v);
    s.decaySec = kOvershootDecay[std::clamp(harmonic, 1, 3) - 1];
    if (harmonic >= 2) s.overshoot *= kOvershootUpper;
    return s;
}

// ---------------------------------------------------------------------------
// Events

double BreathPerformer::noteHz(int note) const { return params_.tuningA4 * std::pow(2.0, (note - 69) / 12.0); }

bool BreathPerformer::controllerMode() const {
    return params_.dynamicsSource == DynamicsSource::Controller ||
           (params_.dynamicsSource == DynamicsSource::Auto && ccSeen_);
}

float BreathPerformer::random() {
    std::uint32_t x = rng_;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_ = x;
    return static_cast<float>(static_cast<std::int32_t>(x >> 8) - 8388608) * (1.0f / 8388608.0f);
}

float BreathPerformer::velocityDynamic(float velocity) { return std::clamp(velocity, 0.0f, 1.0f); }

void BreathPerformer::noteOn(int note, float velocity) {
    if (note < 0 || note > 127) return;
    velocity = std::clamp(velocity, 0.0f, 1.0f);
    const bool slur = !stack_.empty() && phase_ == Phase::Attack;
    stack_.push(note, velocity);
    if (slur) changeNote(note, velocity);
    else startAttack(note, velocity);
}

void BreathPerformer::noteOff(int note) {
    const bool wasTop = stack_.top() == note;
    if (!stack_.remove(note)) return;
    if (stack_.empty()) startRelease();
    else if (wasTop && phase_ == Phase::Attack) changeNote(stack_.top(), stack_.topVelocity());
}

void BreathPerformer::allNotesOff() {
    stack_.clear();
    if (phase_ == Phase::Attack) startRelease();
}

void BreathPerformer::setDynamics(float v) {
    dynCc_ = std::clamp(std::isfinite(v) ? v : 0.5f, 0.0f, 1.0f);
    ccSeen_ = true;
}
void BreathPerformer::setExpression(float v) { expr_ = std::clamp(std::isfinite(v) ? v : 1.0f, 0.0f, 1.0f); }
void BreathPerformer::setVibrato(float v) { wheel_ = std::clamp(std::isfinite(v) ? v : 0.0f, 0.0f, 1.0f); }
void BreathPerformer::setPitchBend(float semitones) {
    bend_ = std::clamp(std::isfinite(semitones) ? semitones : 0.0f, -24.0f, 24.0f);
}

void BreathPerformer::startAttack(int note, float velocity) {
    const bool fromIdle = phase_ == Phase::Idle;
    note_ = note;
    freqHz_ = noteHz(note);
    tuneRef_ = params_.tuningA4;
    const int k = FluteEngine::autoHarmonic(freqHz_);
    shape_ = attackShape(params_.tone.attack, velocity, k);
    const float h = params_.humanize;
    shape_.onsetSec *= 1.0f + kHumanOnset * h * random();
    noteOffset_ = kHumanDyn * h * random();
    // Keep the peak pressure inside the engine's range.
    const float d = controllerMode() ? dynCc_ : velocityDynamic(velocity) + noteOffset_;
    const float p = pressureFor(note, d);
    shape_.overshoot = std::clamp(shape_.overshoot, 0.0f, std::max(0.0f, kMaxPeakPressure / p - 1.0f));
    if (!controllerMode()) {
        dynTarget_ = dyn_ = std::clamp(velocityDynamic(velocity) + noteOffset_, 0.0f, 1.0f);
    } else if (fromIdle) {
        dynTarget_ = dyn_ = dynCc_;
    }
    dynTau_ = kDynTauController;
    phase_ = Phase::Attack;
    attackT_ = 0.0;
    // Re-tongued during the release of the previous note (a gap shorter than
    // the release): the tongue only interrupts the air briefly, so the breath
    // dips to a fraction of where it was and the accent is smaller.
    attackFrom_ = fromIdle ? 0.0f : kRetongueFloor * env_;
    if (!fromIdle) shape_.overshoot *= kRetongueOvershoot;
    env_ = attackFrom_;
    pushT_ = 1e9;
    vibT_ = 0.0;
    pendTongued_ = true;
}

void BreathPerformer::changeNote(int note, float velocity) {
    if (note == note_) return;
    const int kOld = FluteEngine::autoHarmonic(freqHz_);
    note_ = note;
    freqHz_ = noteHz(note);
    tuneRef_ = params_.tuningA4;
    const int kNew = FluteEngine::autoHarmonic(freqHz_);
    pendLegato_ = true;
    if (kNew != kOld) pendRegister_ = true;
    if (kNew > kOld) pushT_ = 0.0;
    if (!controllerMode()) {
        noteOffset_ = kHumanDyn * params_.humanize * random();
        dynTarget_ = std::clamp(velocityDynamic(velocity) + noteOffset_, 0.0f, 1.0f);
        dynTau_ = kDynTauLegato;
    }
}

void BreathPerformer::startRelease() {
    if (phase_ != Phase::Attack) return;
    phase_ = Phase::Release;
    releaseFrom_ = env_;
    releaseT_ = 0.0;
}

// ---------------------------------------------------------------------------
// Control-rate update

PerformerOutput BreathPerformer::advance(int numSamples) {
    // 1. Envelope at the current time.
    switch (phase_) {
    case Phase::Idle: env_ = 0.0f; break;
    case Phase::Attack: {
        const double t = attackT_;
        const double on = std::max(1e-4, static_cast<double>(shape_.onsetSec));
        const double os = shape_.overshoot;
        const double e0 = attackFrom_;
        env_ = static_cast<float>(t < on ? e0 + (1.0 + os - e0) * (0.5 - 0.5 * std::cos(kPi * t / on))
                                         : 1.0 + os * std::exp(-(t - on) / shape_.decaySec));
        break;
    }
    case Phase::Release: {
        // Exponential to -40 dB in releaseSec.
        const double tau = params_.releaseSec / std::log(100.0);
        env_ = static_cast<float>(releaseFrom_ * std::exp(-releaseT_ / tau));
        if (env_ < kEnvOff) {
            env_ = 0.0f;
            phase_ = Phase::Idle;
        }
        break;
    }
    }

    // 2. Vibrato depth and value.
    const float h = params_.humanize;
    const double vt = vibT_ - params_.vibratoDelaySec;
    const float fade = vt <= 0.0 ? 0.0f
                                 : static_cast<float>(params_.vibratoFadeSec > 0.0f
                                                          ? std::min(1.0, vt / params_.vibratoFadeSec)
                                                          : 1.0);
    const float autoDepth = params_.vibratoDepth * (0.5f - 0.5f * std::cos(static_cast<float>(kPi) * fade));
    vibDepthNow_ = std::clamp((autoDepth + wheel_) * (1.0f + kHumanDepth * h * slowB_), 0.0f, 1.0f);
    const float vib = static_cast<float>(std::sin(vibPhase_)) * vibDepthNow_;

    // 3. Dynamics -> pressure, gain.
    PerformerOutput out;
    const double noteF = note_ >= 0 ? static_cast<double>(note_) : 69.0;
    const float d = dyn_ * expr_ + kVibDyn * vib;
    const float push = pushT_ < kPushSec ? kPushAmount * static_cast<float>(std::sin(kPi * pushT_ / kPushSec)) : 0.0f;
    const float pBase = phase_ == Phase::Idle ? 0.0f : pressureFor(noteF, d) * env_;
    if (phase_ != Phase::Idle) lastGain_ = std::pow(10.0f, gainDbFor(params_, noteF, d) / 20.0f);

    EngineControls c;
    c.pressure = pBase * (1.0f + push);
    // Pitch bend and live tuning changes scale the sounding pitch; the register
    // stays the one of the fingered note (a bend must not flip the register).
    const double tune = tuneRef_ > 0.0 ? params_.tuningA4 / tuneRef_ : 1.0;
    const double bendRatio = bend_ != 0.0f ? std::pow(2.0, bend_ / 12.0) : 1.0;
    c.frequencyHz = static_cast<float>(freqHz_ * tune * bendRatio * std::pow(2.0, kVibCents * vib / 1200.0));
    c.jetRatio = params_.jetRatio;
    c.harmonic = 0.0f;
    if (bend_ != 0.0f || tune != 1.0) c.harmonic = static_cast<float>(FluteEngine::autoHarmonic(freqHz_));
    c = applyToneMacros(c, params_.tone);
    c.chiff = kChiffScale * std::pow(params_.tone.attack, kChiffExp);
    out.controls = c;
    out.gain = lastGain_;
    out.basePressure = pBase;
    out.note = phase_ == Phase::Idle ? -1 : note_;
    out.breathing = pBase > 0.0f;
    out.tongued = pendTongued_;
    out.legatoChange = pendLegato_ && !pendTongued_;
    out.registerChange = out.legatoChange && pendRegister_;
    pendTongued_ = pendLegato_ = pendRegister_ = false;

    // 4. Advance the clock.
    const double dt = static_cast<double>(std::max(0, numSamples)) / sampleRate_;
    if (phase_ == Phase::Attack) attackT_ += dt;
    if (phase_ == Phase::Release) releaseT_ += dt;
    pushT_ += dt;
    vibT_ += dt;
    const double rate = params_.vibratoRateHz * (1.0 + kHumanRate * h * slowA_);
    vibPhase_ = std::fmod(vibPhase_ + 2.0 * kPi * rate * dt, 2.0 * kPi);
    if (controllerMode()) dynTarget_ = dynCc_;
    dyn_ += (dynTarget_ - dyn_) * onePoleCoeff(dt, dynTau_);
    slowT_ += dt;
    if (slowT_ >= kSlowStepSec) {
        slowT_ -= kSlowStepSec;
        slowTargetA_ = random();
        slowTargetB_ = random();
    }
    const float ks = onePoleCoeff(dt, kSlowTau);
    slowA_ += (slowTargetA_ - slowA_) * ks;
    slowB_ += (slowTargetB_ - slowB_) * ks;
    return out;
}

} // namespace mf
