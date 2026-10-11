#pragma once
#include <cmath>

// Polylogarithm Li2 - real dilogarithm with double precision
// Ported from polylogarithm_Li2.h by Alexander Voigt (MIT License)
inline double li2 (double x)
{
    const double PI = 3.1415926535897932;
    const double P[] = {
        0.9999999999999999502e+0,
       -2.6883926818565423430e+0,
        2.6477222699473109692e+0,
       -1.1538559607887416355e+0,
        2.0886077795020607837e-1,
       -1.0859777134152463084e-2
    };
    const double Q[] = {
        1.0000000000000000000e+0,
       -2.9383926818565635485e+0,
        3.2712093293018635389e+0,
       -1.7076702173954289421e+0,
        4.1596017228400603836e-1,
       -3.9801343754084482956e-2,
        8.2743668974466659035e-4
    };

    double y = 0, r = 0, s = 1;

    if (x < -1) {
        const double l = std::log (1 - x);
        y = 1 / (1 - x);
        r = -PI * PI / 6 + l * (0.5 * l - std::log (-x));
        s = 1;
    } else if (x == -1) {
        return -PI * PI / 12;
    } else if (x < 0) {
        const double l = std::log1p (-x);
        y = x / (x - 1);
        r = -0.5 * l * l;
        s = -1;
    } else if (x == 0) {
        return x;
    } else if (x < 0.5) {
        y = x;
        r = 0;
        s = 1;
    } else if (x < 1) {
        y = 1 - x;
        r = PI * PI / 6 - std::log (x) * std::log1p (-x);
        s = -1;
    } else if (x == 1) {
        return PI * PI / 6;
    } else if (x < 2) {
        const double l = std::log (x);
        y = 1 - 1 / x;
        r = PI * PI / 6 - l * (std::log (y) + 0.5 * l);
        s = 1;
    } else {
        const double l = std::log (x);
        y = 1 / x;
        r = PI * PI / 3 - 0.5 * l * l;
        s = -1;
    }

    const double y2 = y * y;
    const double y4 = y2 * y2;
    const double p = P[0] + y * P[1] + y2 * (P[2] + y * P[3]) + y4 * (P[4] + y * P[5]);
    const double q = Q[0] + y * Q[1] + y2 * (Q[2] + y * Q[3]) + y4 * (Q[4] + y * Q[5] + y2 * Q[6]);

    return r + s * y * p / q;
}

// ADAA2 Tanh waveshaper
// Ported from ADAA.h (BSD 3-Clause, jatinchowdhury18)
// Must use double precision

constexpr double ADAA_TOL = 1.0e-5;

struct TanhADAA2
{
    double x1 = 0, x2 = 0;
    double d2 = 0;
    double ad1_x1 = 0;
    double ad2_x0 = 0, ad2_x1 = 0;

    void reset()
    {
        x1 = x2 = d2 = ad1_x1 = ad2_x0 = ad2_x1 = 0;
    }

    // First antiderivative of tanh
    static double AD1 (double x) { return std::log (std::cosh (x)); }

    // Second antiderivative of tanh
    static double AD2 (double x)
    {
        const double expVal = std::exp (-2 * x);
        return 0.5 * ((double) li2 (-expVal) - x * (x + 2.0 * std::log (expVal + 1.0) - 2.0 * std::log (std::cosh (x)))) + (M_PI * M_PI / 24.0);
    }

    double process (double x)
    {
        bool illCond1 = std::fabs (x - x1) < ADAA_TOL;
        ad2_x0 = AD2 (x);
        double d1;
        if (illCond1)
        {
            d1 = AD1 (0.5 * (x + x1));
        }
        else
        {
            d1 = (ad2_x0 - ad2_x1) / (x - x1);
        }

        double y;
        bool illCond2 = std::fabs (x - x2) < ADAA_TOL;
        if (illCond2)
        {
            double xBar  = 0.5 * (x + x2);
            double delta = xBar - x1;
            bool illCond3 = std::fabs (delta) < ADAA_TOL;
            if (illCond3)
            {
                y = std::tanh (0.5 * (xBar + x1));
            }
            else
            {
                double ad1v = AD1 (xBar);
                double ad2v = AD2 (xBar);
                y = (2.0 / delta) * (ad1v + (ad2_x1 - ad2v) / delta);
            }
        }
        else
        {
            y = (2.0 / (x - x2)) * (d1 - d2);
        }

        // Update state
        d2     = d1;
        x2     = x1;
        x1     = x;
        ad2_x1 = ad2_x0;

        return y;
    }
};
