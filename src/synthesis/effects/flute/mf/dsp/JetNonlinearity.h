#pragma once
// Jet / labium interaction: flow into the bore as a function of jet deflection.
//
// Physical picture (Coltman; Fletcher; Verge & Fabre): the jet leaves the flue,
// is deflected transversely by the acoustic flow at the embouchure, and the
// deflection grows as the disturbance convects to the labium edge. The edge
// splits the jet; the fraction entering the bore is a smooth, bounded function
// of the deflection relative to the edge. For a jet with a Bickley velocity
// profile, that fraction is exactly a tanh of the deflection divided by the jet
// half-width:   Q_in = W b U [ tanh((eta - y0)/b) + const ].
//
// So we use tanh (not STK's x^3 - x polynomial): it is the physically derived
// shape, it is smooth and strictly bounded for any input (so the loop can never
// run away), and its derivative is largest at the operating point, giving a
// clean Hopf-like onset. The labium offset y0 makes it asymmetric, which
// produces the even harmonics characteristic of flutes. The static flow at
// eta = 0 is subtracted so that a pressure step does not inject a DC thump.

#include <cmath>

namespace mf::dsp {

struct JetNonlinearity {
    float gain = 1.0f;    // 1 / jet half-width (receptivity), set by engine
    float offset = 0.3f;  // labium offset (in half-widths)

    // Returns the bounded jet "split" in (-1 - tanh(y0), 1 - tanh(y0)), zero at
    // eta = 0. Multiply by jet velocity to get the flow.
    float operator()(float eta) const {
        return std::tanh(gain * eta + offset) - std::tanh(offset);
    }
    // Small-signal slope at eta = 0 (for the analytic gain estimate).
    float slope() const {
        const float t = std::tanh(offset);
        return gain * (1.0f - t * t);
    }
};

} // namespace mf::dsp
