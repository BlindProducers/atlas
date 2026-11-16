// FluteVoice (M4): BreathPerformer -> FluteEngine, output gain, register-slur
// overlap, output safety limiter. Original implementation.
//
// Register-slur overlap. When a slur changes register (C5 -> C6, C#5 -> D5 ...)
// the resonance that was sounding stops being supported (the register vent
// opens, the jet shortens) within ~10 ms, while the new one needs 30-70 ms to
// build up from what is left in the tube. M2 measured a 10-24 dB loudness gap.
// Players hide it with breath and timing, and on a real flute both regimes
// briefly coexist during the change. Here:
//   1. at the slur, the second engine takes a copy of the running state
//      (FluteEngine::copyStateFrom: identical output, so no discontinuity) and
//      goes on playing the OLD note with the current breath;
//   2. the main engine gets the new note as before (it dips and builds up);
//   3. once the new note has dipped and come back to within 7 dB of the old
//      one (5 ms power followers; "armed" after it fell 10 dB below), or after
//      120 ms at most, or after 40 ms if it never dipped, the two are
//      crossfaded (equal power, raised cosine, 20 ms); then the copy stops.
// The new pitch is heard as soon as it has built up, the same time as
// without the overlap, but the gap in between is filled by the old note.
// Measured on C5 -> C6 at pressure 0.8: gap -24 dB -> -3 dB relative to the
// new note (0 dB relative to the quieter of the two). See M4-report.md.
// Another slur during the overlap: before the fade has started the new note
// simply replaces the one building up (a note too short to speak is skipped,
// as on a real flute); once fading, the fade is completed quickly (8 ms).

#include "mf/FluteVoice.h"

#include "mf/dsp/Denormals.h"

#include <algorithm>
#include <cmath>

namespace mf {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kFollowerTau = 0.005;
constexpr float kArmRatio = 0.316f;     // -10 dB (amplitude): the new note has dipped
constexpr float kGoRatio = 0.447f;      // -7 dB: it is back, start the fade
constexpr double kNoDipGoSec = 0.040;   // never dipped: fade after 40 ms
constexpr double kMaxHoldSec = 0.120;   // fade at the latest after 120 ms
constexpr double kFadeSec = 0.020;
constexpr double kQuickFadeSec = 0.008;
constexpr double kGainTau = 0.010;

constexpr float kLimitKnee = 0.7f;      // linear below -3 dBFS
constexpr float kLimitCeiling = 0.99f;

inline float softLimit(float x) {
    const float a = std::fabs(x);
    if (a <= kLimitKnee) return x;
    const float range = kLimitCeiling - kLimitKnee;
    return std::copysign(kLimitKnee + range * std::tanh((a - kLimitKnee) / range), x);
}

} // namespace

FluteVoice::FluteVoice() { engines_[1].setNoiseSeed(0x2468ACEu); }

void FluteVoice::prepare(double sampleRate, int maxBlockSize) {
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    for (auto& e : engines_) e.prepare(sampleRate_, kControlBlock);
    performer_.prepare(sampleRate_);
    bufOld_.assign(static_cast<std::size_t>(std::max(kControlBlock, 1)), 0.0f);
    (void)maxBlockSize;
    followerCoeff_ = static_cast<float>(1.0 - std::exp(-1.0 / (kFollowerTau * sampleRate_)));
    reset();
}

void FluteVoice::reset() {
    for (auto& e : engines_) e.reset();
    performer_.reset();
    cur_ = 0;
    xf_ = Overlap{};
    outPower_ = 0.0f;
    lastOut_ = PerformerOutput{};
    gain_ = std::pow(10.0f, BreathPerformer::gainDbFor(performer_.params(), 69.0, 0.5f) / 20.0f);
}

std::uint64_t FluteVoice::nonFiniteResetCount() const {
    return engines_[0].nonFiniteResetCount() + engines_[1].nonFiniteResetCount();
}

void FluteVoice::process(float* out, int numSamples) {
    dsp::ScopedFlushDenormals noDenormals;
    for (int pos = 0; pos < numSamples; pos += kControlBlock) {
        const int n = std::min(kControlBlock, numSamples - pos);
        processChunk(out + pos, n);
    }
}

void FluteVoice::startOverlap() {
    const int old = cur_;
    cur_ = 1 - cur_;
    engines_[cur_].copyStateFrom(engines_[old], 0x9E3779B9u * ++seedCounter_);
    xf_.active = true;
    xf_.oldCtl = lastOut_.controls;
    xf_.oldGain = gain_;
    xf_.basePressureAtSplit = lastOut_.basePressure;
    xf_.t = 0.0;
    // Both followers start from the running output level (identical signals
    // at the split; starting from 0 made the ratio meaningless for ~1 ms).
    xf_.eOld = xf_.eNew = outPower_;
    xf_.armed = false;
    xf_.w = 0.0f;
    xf_.rate = 0.0f;
    ++overlaps_;
}

void FluteVoice::processChunk(float* out, int n) {
    const PerformerOutput o = performer_.advance(n);

    if (xf_.active && (o.legatoChange || o.tongued)) {
        if (xf_.rate > 0.0f || o.tongued) {
            xf_.rate = std::max(xf_.rate, static_cast<float>(1.0 / (kQuickFadeSec * sampleRate_)));
        } else {
            xf_.armed = false;     // the note building up changed: wait for the new one
            xf_.t = 0.0;
        }
    } else if (o.registerChange && overlapEnabled_ && !engines_[cur_].isSilent()) {
        startOverlap();
    }

    FluteEngine& eng = engines_[cur_];
    eng.setControls(o.controls);
    eng.process(out, n);

    // Gain: one-pole smoothing towards the performer's target (10 ms: a note
    // change must not step the level of the ringing tube), ramped linearly
    // over the chunk.
    const float g0 = gain_;
    const float g1 = g0 + (o.gain - g0) * static_cast<float>(1.0 - std::exp(-n / (kGainTau * sampleRate_)));
    const float dg = (g1 - g0) / static_cast<float>(n);

    if (!xf_.active) {
        for (int i = 0; i < n; ++i) {
            out[i] = softLimit(out[i] * (g0 + dg * static_cast<float>(i + 1)));
            outPower_ += followerCoeff_ * (out[i] * out[i] - outPower_);
        }
    } else {
        FluteEngine& old = engines_[1 - cur_];
        EngineControls oc = xf_.oldCtl;
        // The old note follows the breath (release, dynamics, vibrato envelope)
        // relative to the moment of the split.
        const float rel = xf_.basePressureAtSplit > 0.0f ? o.basePressure / xf_.basePressureAtSplit : 0.0f;
        oc.pressure = xf_.oldCtl.pressure * std::clamp(rel, 0.0f, 4.0f);
        old.setControls(oc);
        old.process(bufOld_.data(), n);
        const double dt = 1.0 / sampleRate_;
        for (int i = 0; i < n; ++i) {
            const float b = out[i] * (g0 + dg * static_cast<float>(i + 1));
            const float a = bufOld_[static_cast<std::size_t>(i)] * xf_.oldGain;
            xf_.eOld += followerCoeff_ * (a * a - xf_.eOld);
            xf_.eNew += followerCoeff_ * (b * b - xf_.eNew);
            if (xf_.rate == 0.0f) {
                const float r2 = xf_.eNew / (xf_.eOld + 1e-20f);
                if (!xf_.armed && r2 < kArmRatio * kArmRatio) xf_.armed = true;
                const bool go = (xf_.armed && r2 >= kGoRatio * kGoRatio) || (!xf_.armed && xf_.t >= kNoDipGoSec) ||
                                xf_.t >= kMaxHoldSec;
                if (go) xf_.rate = static_cast<float>(1.0 / (kFadeSec * sampleRate_));
            }
            xf_.w = std::min(1.0f, xf_.w + xf_.rate);
            const float s = 0.5f - 0.5f * std::cos(static_cast<float>(kPi) * xf_.w);
            const float th = 0.5f * static_cast<float>(kPi) * s;
            out[i] = softLimit(std::cos(th) * a + std::sin(th) * b);
            outPower_ += followerCoeff_ * (out[i] * out[i] - outPower_);
            xf_.t += dt;
        }
        if (xf_.w >= 1.0f) {
            old.reset();
            xf_.active = false;
        }
    }
    gain_ = g1;
    lastOut_ = o;
}

} // namespace mf
