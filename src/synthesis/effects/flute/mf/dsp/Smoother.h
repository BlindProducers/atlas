#pragma once
// One-pole parameter smoother (exponential approach to the target).
// Controls are typically updated once per block; per-sample smoothing removes
// zipper noise and makes the output independent of the block size.

#include <cmath>

namespace mf::dsp {

class Smoother {
public:
    void setTimeConstant(double seconds, double sampleRate) {
        coeff_ = static_cast<float>(std::exp(-1.0 / (seconds * sampleRate)));
    }
    void setTarget(float t) { target_ = t; }
    void snap(float v) { target_ = v; value_ = v; }
    float target() const { return target_; }
    float value() const { return value_; }

    float next() {
        value_ = target_ + coeff_ * (value_ - target_);
        return value_;
    }

private:
    float coeff_ = 0.0f;
    float target_ = 0.0f;
    float value_ = 0.0f;
};

} // namespace mf::dsp
