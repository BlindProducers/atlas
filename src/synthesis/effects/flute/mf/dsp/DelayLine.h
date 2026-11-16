#pragma once
// Fractional delay line with 3rd-order Lagrange interpolation.
//
// Why Lagrange-3 (FIR) rather than a Thiran allpass:
//  * The bore and jet delays will be modulated (vibrato, legato note changes in
//    M2). A Thiran allpass is recursive, so changing its coefficient produces
//    transients; a 4-tap Lagrange FIR has no state and can be modulated freely.
//  * Lagrange is maximally flat at DC: its phase delay at the fundamental is
//    almost exactly the requested delay (error O(w^4)), and its small residual
//    phase error is computed analytically by phaseDelayAt() and compensated.
//  * Its mild high-frequency rolloff acts as a little extra bore loss, which is
//    harmless (even welcome) in a flute loop.
// The fractional read position is kept in [1, 2) relative to the first tap,
// the region where 3rd-order Lagrange is most accurate.
//
// Convention: read(d) is called BEFORE write() in a sample tick and returns the
// input from d samples ago (d >= 2). The integer case d = k returns x[n-k].

#include <cmath>
#include <complex>
#include <cstddef>
#include <vector>

namespace mf::dsp {

class DelayLine {
public:
    // Allocates (non-real-time). Capacity is rounded up to a power of two.
    void allocate(std::size_t maxDelaySamples) {
        std::size_t size = 16;
        while (size < maxDelaySamples + 8) size <<= 1;
        buffer_.assign(size, 0.0f);
        mask_ = size - 1;
        writePos_ = 0;
    }

    void clear() {
        for (auto& s : buffer_) s = 0.0f;
        writePos_ = 0;
    }

    // Largest delay read() supports for this allocation.
    double maxDelay() const { return static_cast<double>(buffer_.size()) - 6.0; }
    static constexpr double minDelay() { return 2.0; }

    void write(float x) {
        buffer_[writePos_] = x;
        writePos_ = (writePos_ + 1) & mask_;
    }

    float read(double delay) const {
        // Split into integer tap index i (first tap = i samples ago) and the
        // fractional position D = delay - i in [1, 2).
        const double fl = std::floor(delay);
        const auto i = static_cast<std::size_t>(fl) - 1;
        const double D = delay - fl + 1.0;
        float h[4];
        coefficients(D, h);
        // x[n - i - k]; writePos_ is the slot for x[n] (not written yet).
        const std::size_t base = writePos_ - i;  // wraps via mask below
        float acc = 0.0f;
        for (std::size_t k = 0; k < 4; ++k)
            acc += h[k] * buffer_[(base - k) & mask_];
        return acc;
    }

    // Lagrange-3 coefficients for fractional delay D measured from tap 0.
    static void coefficients(double D, float h[4]) {
        const double d1 = D - 1.0, d2 = D - 2.0, d3 = D - 3.0;
        h[0] = static_cast<float>(-d1 * d2 * d3 / 6.0);
        h[1] = static_cast<float>(D * d2 * d3 / 2.0);
        h[2] = static_cast<float>(-D * d1 * d3 / 2.0);
        h[3] = static_cast<float>(D * d1 * d2 / 6.0);
    }

    // Exact phase delay (in samples) of read(delay) at angular frequency w
    // (rad/sample). Used by the analytic pitch compensation.
    static double phaseDelayAt(double delay, double w) {
        const double fl = std::floor(delay);
        const double i = fl - 1.0;
        const double D = delay - fl + 1.0;
        float h[4];
        coefficients(D, h);
        std::complex<double> H{0.0, 0.0};
        for (int k = 0; k < 4; ++k)
            H += static_cast<double>(h[k]) * std::polar(1.0, -w * (i + k));
        // Unwrap around the nominal delay so large delays are handled.
        const double nominal = -w * delay;
        double ph = std::arg(H * std::polar(1.0, -nominal)) + nominal;
        return -ph / w;
    }

    static double magnitudeAt(double delay, double w) {
        const double fl = std::floor(delay);
        const double D = delay - fl + 1.0;
        float h[4];
        coefficients(D, h);
        std::complex<double> H{0.0, 0.0};
        for (int k = 0; k < 4; ++k)
            H += static_cast<double>(h[k]) * std::polar(1.0, -w * k);
        return std::abs(H);
    }

private:
    std::vector<float> buffer_;
    std::size_t mask_ = 0;
    std::size_t writePos_ = 0;
};

} // namespace mf::dsp
