#pragma once
#include <cstdint>
#include <cmath>

// Ported from xhl/maths.h (Tremus/xhl) - exact same fast approximations for DSP parity

namespace FastMath
{

constexpr float HALF_PI   = 1.5707963267948966f;
constexpr float LN2       = 0.6931471805599453f;
constexpr float SQRT2     = 1.41421356237309504880f;
constexpr float SQRT1_2   = 0.707106781186547524401f;

constexpr float MIDI_NOTE_NUM_20Hz  = 15.486820576352429f;
constexpr float MIDI_NOTE_NUM_20kHz = 135.0762319922975f;

union Fi32
{
    float    f;
    int32_t  i32;
    uint32_t u32;
};

// Paul Mineiro's exp2 approximation
inline float fastexp2 (float p)
{
    float offset = (p < 0) ? 1.0f : 0.0f;
    int     w = (int) p;
    float   z = p - w + offset;
    Fi32    v;
    v.u32 = (uint32_t) ((1 << 23) * (p + 121.2740575f + 27.7280233f / (4.84252568f - z) - 1.49012907f * z));
    return v.f;
}

inline float fastlog2 (float x)
{
    Fi32 vx;
    vx.f = x;
    Fi32 mx;
    mx.u32 = (vx.u32 & 0x007FFFFF) | 0x3f000000;
    float y = (float) vx.u32;
    y *= 1.1920928955078125e-7f;
    return y - 124.22551499f - 1.498030302f * mx.f - 1.72587999f / (0.3520887068f + mx.f);
}

inline float fastlog (float x) { return fastlog2 (x) * LN2; }

inline float fastlog10 (float x)
{
    Fi32 vx;
    vx.f = x;
    Fi32 mx;
    mx.u32 = (vx.u32 & 0x007FFFFF) | 0x3f000000;
    float y = (float) vx.u32;
    y *= 3.588557191657796e-8f;
    return y - 37.39560623879553f - 0.4509520553155725f * mx.f - 0.5195416459062518f / (0.3520887068f + mx.f);
}

inline float fastexp (float x) { return fastexp2 (x * 1.442695040f); }

// https://observablehq.com/@jrus/fasttan
inline float fasttan_normalised (float x)
{
    float y = 1.0f - x * x;
    return x * (-0.0187108f * y + 0.31583526f + 1.27365776f / y);
}

inline float fast_dB_to_gain (float dB) { return fastexp2 (dB * 0.166666667f); }
inline float fast_gain_to_dB (float gain) { return 20.0f * fastlog10 (gain); }

inline float midi_to_Hz (float midi)
{
    float Hz = fastexp2 ((midi - 69.0f) * 0.0833333f) * 440.0f;
    return Hz > 20000.0f ? 20000.0f : Hz;
}

inline float lerpf (float frac, float start, float end) { return start + frac * (end - start); }
inline double lerpd (double frac, double start, double end) { return start + frac * (end - start); }

inline float clampf (float v, float lo, float hi) { return v < lo ? lo : (hi < v ? hi : v); }
inline double clampd (double v, double lo, double hi) { return v < lo ? lo : (hi < v ? hi : v); }

inline double normd (double v, double start, double end) { return (v - start) / (end - start); }

inline int droundi (double d)
{
    // Use standard rounding
    return (int) std::round (d);
}

} // namespace FastMath
