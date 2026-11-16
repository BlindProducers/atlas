#pragma once
// DC blocker  H(z) = (1 - z^-1) / (1 - R z^-1).
//
// The jet nonlinearity is asymmetric (labium offset) and so produces a DC flow
// component. Inside a feedback loop with near-unity gain at DC, that would
// accumulate, so the loop contains this high-pass. Its phase lead at f0 is
// included in the analytic pitch compensation.

#include <cmath>
#include <complex>

namespace mf::dsp {

class DcBlocker {
public:
    static double poleForCutoff(double cutoffHz, double sampleRate) {
        return std::exp(-2.0 * 3.14159265358979323846 * cutoffHz / sampleRate);
    }
    void setPole(float R) { R_ = R; }
    void reset() { x1_ = 0.0f; y1_ = 0.0f; }

    float process(float x) {
        const float y = x - x1_ + R_ * y1_;
        x1_ = x;
        y1_ = y;
        return y;
    }

    static std::complex<double> response(double R, double w) {
        const auto zi = std::polar(1.0, -w);
        return (1.0 - zi) / (1.0 - R * zi);
    }

private:
    float R_ = 0.999f;
    float x1_ = 0.0f, y1_ = 0.0f;
};

} // namespace mf::dsp
