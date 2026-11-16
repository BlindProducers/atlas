#pragma once
// One-pole lowpass  H(z) = (1 - a) / (1 - a z^-1),  unity gain at DC.
//
// Used for (a) the bore end-reflection loss filter (radiation from the open end
// reflects low frequencies almost fully and lets high frequencies escape), (b)
// the embouchure "velocity" leaky integrator that drives the jet, and (c) noise
// band-limiting. The analytic response helpers are used by the engine's pitch
// compensation.

#include <cmath>
#include <complex>

namespace mf::dsp {

class OnePoleLowpass {
public:
    // Pole from a -3 dB-ish cutoff in Hz (bilinear-free impulse-invariant map).
    static double poleForCutoff(double cutoffHz, double sampleRate) {
        return std::exp(-2.0 * 3.14159265358979323846 * cutoffHz / sampleRate);
    }

    void setPole(float a) { a_ = a; }
    float pole() const { return a_; }
    void reset() { z_ = 0.0f; }

    float process(float x) {
        z_ = x + a_ * (z_ - x);  // = (1-a) x + a z
        return z_;
    }

    static std::complex<double> response(double a, double w) {
        return (1.0 - a) / (1.0 - a * std::polar(1.0, -w));
    }

private:
    float a_ = 0.0f;
    float z_ = 0.0f;
};

} // namespace mf::dsp
