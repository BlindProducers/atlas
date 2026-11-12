// SPDX-License-Identifier: GPL-3.0-or-later
#include "distortion_modes.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <math.h>

namespace atlas::distortion_modes {
  namespace {
    // The C library's functions, called through pointers as EEL2 calls them,
    // so the compiler never inlines, folds or fuses them.
    double (*volatile libm_sin)(double) = ::sin;
    double (*volatile libm_cos)(double) = ::cos;
    double (*volatile libm_exp)(double) = ::exp;
    double (*volatile libm_log)(double) = ::log;
    double (*volatile libm_pow)(double, double) = ::pow;
    double (*volatile libm_sqrt)(double) = ::sqrt;

    // $pi
    constexpr double kPi = 3.141592653589793;

    // @init's constants.
    constexpr double OUT_G = 0.999830894;
    constexpr double SUSPEND_SAMPLES = 8192.0;
    constexpr double OUT_CLIP = 4.0;
    constexpr double SILENCE_EPS = 0.0000000001;
    constexpr double OS_C0 = 0.0387502;
    constexpr double OS_C1 = 0.1585135;
    constexpr double OS_C2 = 0.3747362;
    constexpr double OS_C3 = 0.7332992;
    constexpr int OS_DELAY = 3;
    constexpr double XFADE_S = 0.03;
    constexpr double DCB_HZ = 12.0;
    constexpr double ZSQ_GATE = 0.00005;
    constexpr double TUBE_C = 0.989;
    constexpr double D2_C = 43.41748;
    constexpr double D2_S = 0.9968252;
    constexpr double D2_K = 0.0796221;
    constexpr double D2_HI = 0.57;
    constexpr double D2_LO = -0.68;
    constexpr int ASYM_N = 768;
    constexpr double ASYM_A = 0.5;
    constexpr double ASYM_S0 = -5.48375205019277;
    constexpr double ASYM_SC = 67.8708683190007;
    constexpr int ZSQ_N = 256;
    constexpr double ZSQ_A = 0.1;
    constexpr double ZSQ_S0 = 0.0;
    constexpr double ZSQ_SC = 53.2630269176695;

    // The Asym and Zero-Square curves, measured from Serum 2, as the script
    // stores them: three characters per value, each a digit in base 64.
    constexpr const char* kAsymText =
        "cOycHNb-cb3UbvEbmobeAbVNbMNbDBa5oawCamOacLaR7aHcZ8tZxuZmgZbAZPQZDOY25YqSYdXYQIYCkX0rXmcXX2XI5W5j"
        "Wp0WZrWJGV4GVmpVUvVCVUvaUb-UIATzeTeWTIoSyRSbQSDjRrJRSAQ4EQdWQBxPlVPH9OpoOKTNp7NIcMlzMB9Lc2K2ZKOi"
        "JlMI6TINxHffGvZF9WFJSES9DamCe+BjVAe2AAAADvAB-AChACWACaACZACZACZACZACZACZACZACZACZACZACZACZACZACZ"
        "ACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZ"
        "ACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZ"
        "ACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZ"
        "ACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZ"
        "ACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZACZ"
        "ACZACZACZACZACZACZACZACZACZACZACYACaACVACmACKAIUARXAaAAi3AruA0qA9qBGuBP3BZFBiYBrwB1PB+1CIhCSUCcP"
        "CmSCweC6yDFQDP3DaoDlkDwrD7+EHcETHEe-ErEE3XFD4FQpFdpFq5F4aGGMGUPGilGxOHALHPcHfBHu8H-NIP0IgzIyKJD6"
        "JWDJomJ7kKO9KiyK3ELLzLhAL2sMM3MjiM6uNSaNqpODaOctO2kPQ-Pr9QHhQjpRAWRdpR7hSZ-S5CTYrT46UZuU7HVdFV-n"
        "WisXGVXqgYPMY0YZaDaANamzbN1b1QcdDdFNdtreWce-dfotgSJg7vhlciPPi5Fji7kMvk2flgHmJnmy7ncAoE1otXpVkp9a"
        "qk2rL4rycsYhs+FtjIuHmurgvOzvxfwTjw09xVtx1zyVNyz7zR+zvU0L+0n71DM1dw13p2Q12pW3BL3YW3u24Et4Z64uf5Cb"
        "5Vw5of56n6MK6dJ6tk69c7Mx7bl7p473r8E+8Rz8eL8qF81j9Al9LM9VZ9fM9on9xp96U+Co+Kl+SN+Zf+gd+nH+td+zh+5S"
        "++x-D+-I7-Nn-SD-WP-aM-d7-ha-ks-nw-qn-tR-vu-x--0E-19-3r-5O-6m-7z-82-9v-+f--E--g--z--+-----4--o--R"
        "-+x-+K-9b-8k-7m-6h-5V-4B-2o-1H-zg-xy-v+-uE-sE-p+-ny-lg-jJ-gr-eJ-bh-Yz-WA-TI-QL-NJ-KC-G2-Dl-AP+80"
        "+5V+1x+yI+ub+qq+mz+i5+e6+a3+Ww+Sk+OU+KA+Fo+BM98s94I9zg9u09qE9lQ9gZ9bd9We9Rb9MV9HK9B988r83W8x98sh"
        "8nC8hf8b48WO8Qh8Kw8E87-F75L7zN7tM7nI7hB7a37Uq7OZ7IG7Bv67W6066ub6n56hU6at6UD6NW6Gm5-054-5yI5rO5kS"
        "5dT5WS5PP5IJ5BB4524yq4rb4kL4c44Vj4OM4G03-Z3393we3o+3hd3Z63SV3Ku3DH27d2zz2sH2kZ2cr2U72NK2FY19l11x"
        "1t91mH1eQ1WZ1Oh1Gp0+w0220u80nB0fG0XL0PQ0HUz-Yz3czvgznkzfpzXtzPyzH2y-8y4BywHyoNygUyYcyQkyItyA3x5B"
        "xxMxpYxhmxZ0xSDxKTxClw63wzLwrhwj3wcPwUpwNEwFgv9-v2evvAvnjvgIvYuvRXvKCvCuu7cu0Nus-ulzuequXjuQeuJb"
        "uCat7ct0gttmtmvtf6tZHtSXtLptE+s+Ws3wsxMsqrskNsdxsXYsRCsKvsEer+Pr4Erx7rr1rlyrfyrZ0rT6rOCrIMrCaq8r"
        "q2+qxUqruqmKqgpqbKqVvqQWqLBqFuqAep7Rp2HpxApr8pm6ph8pdApYHpTRpOepJupFApAWo7uo3JoymouHopqolRog5ocl"
        "oYUoUFoP5oLvoHooDkn-jn7kn3onzvnv4nsEnoSnkjng2ndMnZlnWAnSenO+nLgnIFnEsnBWm+Cm6xm3im0VmxLmuCmrAmoB";
    constexpr const char* kZeroSquareText =
        "AAAAOzAeCAtkA9CBMjBcGBrrB7SCK8CaqCqaC6PDKIDaFDqID6PEKdEawErKE7rFMSFdBFt4F+4GQAGhRGyrHEQHV+Hn3H58"
        "IMLIenIxPJEDJXEJqUJ9xKRcKlWK5gLN5LijL3dMMoMiFM31NN2NkLN60ORwOpBPAoPYkPw2QJfQifQ74RVpRvySKWSlTTAs"
        "TcgT4wUVdUynVQPVuWWM8WsDXLqXryYMdYtrZPcZxyaUsa4NbcUcBDcmadMadzEeaYfCYfrEgUdg+jhpXiU6jBNjuPkcClKl"
        "l55mp+nayoMWo+opxnqlQrZisOYtDut5euvjvlzwcHxSSyIJy9ezyC0ln1X72Iv23z3k74P644m5e56Cv6kH7DC7fl7508R2"
        "8n08729OF9eq9tt97W+Hu+S8+dG+mS+un+2K+9A-DM-Iz-N5-Sg-Wr-ad-d5-hB-j2-ma-ow-q3-sy-ui-wH-xj-y3-0D-1I"
        "-2H-3A-30-4k-5P-52-6Z-66-7X-7y-8K-8g-80-9H-9X-9m-90-+B-+M-+W-+g-+p-+w-+4-++--E--J--O--T--X--a--e"
        "--h--j--m--o--q--s--u--w--x--y--0--1--2--3--3--4--5--5--6--6--7--7--8--8--8--9--9--9--9--+--+--+"
        "--+--+--+---------------------------------------------------------------------------------------";

    struct Tables {
      double asym[ASYM_N];
      double zsq[ZSQ_N];
    };

    // The script's DECT.
    int digit(char c) {
      if (c >= 'A' && c <= 'Z') return c - 'A';
      if (c >= 'a' && c <= 'z') return c - 'a' + 26;
      if (c >= '0' && c <= '9') return c - '0' + 52;
      return c == '+' ? 62 : 63;
    }

    // dec(): every value is normal, so the flush on the script's assignment changes none.
    void decode(const char* text, double* dst, int n, double lo, double sc) {
      for (int i = 0; i < n; ++i) {
        double v = 0;
        for (int j = 0; j < 3; ++j)
          v = v * 64 + digit(text[3 * i + j]);
        dst[i] = lo + sc * v;
      }
    }

    const Tables& tables() {
      static const Tables decoded = [] {
        Tables result;
        decode(kAsymText, result.asym, ASYM_N, -1.0010668387999, 0.0000069800867104);
        decode(kZeroSquareText, result.zsq, ZSQ_N, 0.00020768541893645, 0.00000375695122969);
        return result;
      }();
      return decoded;
    }

    // EEL2's min, max and fuzzy ==.
    double eelMin(double a, double b) { return a > b ? b : a; }
    double eelMax(double a, double b) { return a < b ? b : a; }
    bool eelEqual(double a, double b) { return std::fabs(a - b) < 0.00001; }

    // asinh_(z)
    double asinh_(double z) {
      return z < 0 ? -libm_log(-z + libm_sqrt(z * z + 1)) : libm_log(z + libm_sqrt(z * z + 1));
    }
  }

  // EEL2 does not group as C does: / binds tighter than *, and - tighter than
  // +, so `a * b / c` is a * (b / c) and `a + b - c` is a + (b - c). The
  // expressions here are bracketed as EEL2 reads the script's.
  //
  // Each `flush` is an assignment EEL2 compiles with its denormal check: one
  // whose value is computed by + or - without a constant, by * without a
  // constant of at least 1, by / without a constant of at most 1, by ^, exp
  // or log, or by a compound operator, or a user function whose last
  // expression is one of them (cr, down2, os_shape and shape). Copies, memory
  // reads, comparisons, cos, and abs, min and max of unchecked values are
  // stored as they are, and so are the arguments a user function receives.
  double Engine::flush(double value) const {
    uint64_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    const int exponent = static_cast<int>((bits >> 52) & 0x7ff);
    return exponent > build_.flush_exponent && exponent != 0x7ff ? value : 0.0;
  }

  Engine::Engine(Eel2Build build, bool unity_wet) : build_(build), wet_gain_(unity_wet ? 2.0 : 1.0) {
    // Decoded here, off the audio thread.
    const Tables& decoded = tables();
    asym_ = decoded.asym;
    zsq_ = decoded.zsq;
  }

  // Catmull-Rom read of table t (n values) at fractional index f, the ends
  // clamped (the script's lm = rm = 0).
  double Engine::cr(const double* t, int n, double f) const {
    f = eelMin(eelMax(f, 0), n - 1);
    const int i = static_cast<int>(eelMin(std::floor(f), n - 2));
    const double u = flush(f - i);
    const double p1 = t[i], p2 = t[i + 1];
    const double p0 = flush(i > 0 ? t[i - 1] : p1);
    const double p3 = flush(i < n - 2 ? t[i + 2] : p2);
    return p1 + (0.5 * u) * ((p2 - p0) +
        u * (((2 * p0 - 5 * p1) + (4 * p2 - p3)) + u * (3 * (p1 - p2) + (p3 - p0))));
  }

  // set_drive(D), for the ported modes.
  void Engine::setDrive(double D) {
    tube_a_ = 1 / (1 - TUBE_C * D) - 1;
    ssat_g_ = 2 * (1 + 16 * D * D);
    double k = 1 + 3 * D;
    tape_g_ = 2 * k; tape_x_ = 1 / (3 * k);
    tape_w_ = flush(kPi * (k / (0.625 * (1 + 9 * D)))); tape_on_ = D > 0;
    stomp_k_ = 2 * (1 + 7 * D); stomp_x_ = 0.4 / (1 + 7 * D); stomp_s_ = 0.2 / (0.5 - stomp_x_);
    d1_m_ = eelMin(1, 8 * D); k = eelMax(0, (D - 0.125) / 0.875); d1_al_ = 11.2 + 50 * k * k * k;
    d2_g_ = flush(D2_C * D);
    zsq_off_ = flush(0.5 * D);
    asym_g_ = 1 + 20 * D * D;
  }

  // shape(x): the static curve of a mode (input at 4x, output 0.25 of it).
  double Engine::shape(double x, int m) const {
    switch (m) {
      case kTube: {
        const double v = eelMin(eelMax(2 * x, -1), 1);
        return (1 + tube_a_) * (v / (1 + tube_a_ * std::fabs(v)));
      }
      case kDiode1: {
        const double a = std::fabs(x);
        return ((1 - d1_m_) * 2) * x + (d1_m_ * x) * ((a + (d1_al_ - 0.7)) / ((x * x + d1_al_ * a) + 0.25));
      }
      case kDiode2: {
        const double v = flush(d2_g_ * x);
        return std::fabs(v) < 2.5 ? D2_S * libm_sin(v) - D2_K * (1 - libm_cos(v)) : (v > 0 ? D2_HI : D2_LO);
      }
      case kZeroSquare: {
        double a = std::fabs(x);
        // no offset on near-silent input
        if (a > ZSQ_GATE) a = flush(a + zsq_off_);
        const double f = flush(cr(zsq_, ZSQ_N, (asinh_(a / ZSQ_A) - ZSQ_S0) * ZSQ_SC));
        return x < 0 ? -f : x > 0 ? f : 0;
      }
      case kAsym:
        return cr(asym_, ASYM_N, (asinh_(asym_g_ * (x / ASYM_A)) - ASYM_S0) * ASYM_SC);
      case kStompBox: {
        const double a = std::fabs(x);
        const double f = flush(a < stomp_x_ ? stomp_k_ * a : 0.8 + (a - stomp_x_) * stomp_s_);
        return x < 0 ? -f : f;
      }
      case kTapeSat: {
        if (!tape_on_) return 2 * x;
        const double a = std::fabs(x);
        const double f = flush(a < tape_x_ ? tape_g_ * a : libm_cos(tape_w_ * (a - 0.5)));
        return x < 0 ? -f : f;
      }
      default: {
        // Soft Sat
        const double v = flush(ssat_g_ * x), a = std::fabs(v);
        const double f = a < 0.5 ? a : a < 1.5 ? 1 - (0.5 * (1.5 - a)) * (1.5 - a) : 1;
        return v < 0 ? -f : f;
      }
    }
  }

  // 4x oversampler: two 2x polyphase allpass half-band stages. State block s:
  // [0, 8) up stage A, [8, 16) up stage B, [16, 24) down stage B, [24, 32)
  // down stage A (x1 to x4, y1 to y4 per stage).
  void Engine::up2(double v, double* s, double& up_a, double& up_b) const {
    double a = v, b = v, t;
    t = s[0]; s[0] = a; a = flush((a - s[4]) * OS_C0 + t); s[4] = a;
    t = s[1]; s[1] = b; b = flush((b - s[5]) * OS_C1 + t); s[5] = b;
    t = s[2]; s[2] = a; a = flush((a - s[6]) * OS_C2 + t); s[6] = a;
    t = s[3]; s[3] = b; b = flush((b - s[7]) * OS_C3 + t); s[7] = b;
    up_a = a; up_b = b;
  }

  double Engine::down2(double e, double o, double* s) const {
    double a = o, b = e, t;
    t = s[0]; s[0] = a; a = flush((a - s[4]) * OS_C0 + t); s[4] = a;
    t = s[1]; s[1] = b; b = flush((b - s[5]) * OS_C1 + t); s[5] = b;
    t = s[2]; s[2] = a; a = flush((a - s[6]) * OS_C2 + t); s[6] = a;
    t = s[3]; s[3] = b; b = flush((b - s[7]) * OS_C3 + t); s[7] = b;
    return 0.5 * (a + b);
  }

  // one channel through the 4x oversampled shaper
  double Engine::osShape(double x, double* s, int m) const {
    double a0, a1, up_a, up_b;
    up2(x, s, a0, a1);
    up2(a0, s + 8, up_a, up_b);
    const double b0 = flush(shape(up_a, m)), b1 = flush(shape(up_b, m));
    up2(a1, s + 8, up_a, up_b);
    const double b2 = flush(shape(up_a, m)), b3 = flush(shape(up_b, m));
    a0 = flush(down2(b0, b1, s + 16)); a1 = flush(down2(b2, b3, s + 16));
    return down2(a0, a1, s + 24);
  }

  // mode_wet(mm, b): the shaper, the DC blocker where the curve is asymmetric,
  // the output scale and the delay that lines the wet signal up with Serum's.
  void Engine::modeWet(int mm, int b, double pre_l, double pre_r, double& w_l, double& w_r) {
    double* B = banks_[static_cast<size_t>(b)].data();
    double sl = flush(osShape(pre_l, B, mm)), sr = flush(osShape(pre_r, B + 32, mm));
    if (mm == kDiode2 || mm == kAsym) {
      double a = sl; sl = flush(a - B[64] + dcb_p_ * B[65]); B[64] = a; B[65] = sl;
      a = sr; sr = flush(a - B[66] + dcb_p_ * B[67]); B[66] = a; B[67] = sr;
    }
    B[68 + wwp_] = sl; B[84 + wwp_] = sr;
    const int d = (wwp_ - OS_DELAY) & 15;
    w_l = flush((0.25 * OUT_G) * B[68 + d]); w_r = flush((0.25 * OUT_G) * B[84 + d]);
  }

  void Engine::setParams() {
    tm_ = static_cast<int>(std::floor(parameters_.mode + 0.5));
    mix_t_ = flush(libm_pow(libm_sin(kPi * (parameters_.mix / 200)), 2));
    // Level scales the whole output, dry included.
    lvl_t_ = flush(libm_pow(2 * parameters_.level, 2));
  }

  void Engine::prepare(double sample_rate) {
    srate_ = sample_rate;
    // REAPER starts a script with every memory slot at zero.
    for (auto& bank : banks_) bank.fill(0);
    dry_l_.fill(0); dry_r_.fill(0);
    dwp_ = 0; wwp_ = 0;
    drive_s_ = parameters_.drive; setDrive(drive_s_ / 100);
    setParams();
    mix_s_ = mix_t_; lvl_g_ = lvl_t_;
    dcb_p_ = flush(libm_exp((-2 * kPi) * (DCB_HZ / srate_)));
    smooth_ = 1 - libm_exp(-1 / (0.02 * srate_));
    xf_step_ = flush(1 / (XFADE_S * srate_));
    silent_run_ = 0;
    cur_m_ = old_m_ = tm_; cur_b_ = 0; xf_ = 1;
  }

  void Engine::setParameters(const Parameters& parameters) {
    parameters_ = parameters;
    setParams();
  }

  void Engine::block(int samples) {
    // A Mode change crossfades from the old mode to the new one; a further
    // change waits for the fade to finish.
    if (!eelEqual(tm_, cur_m_) && xf_ >= 1) {
      old_m_ = cur_m_; cur_b_ = 1 - cur_b_; cur_m_ = tm_; xf_ = 0;
      banks_[static_cast<size_t>(cur_b_)].fill(0);
    }
    // Drive glides to its new value (20 ms) so changes don't click.
    const double drive = parameters_.drive;
    if (!eelEqual(drive_s_, drive)) {
      drive_s_ = flush(drive_s_ + (1 - libm_exp(-static_cast<double>(samples) / (0.02 * srate_))) * (drive - drive_s_));
      if (std::fabs(drive - drive_s_) < 0.01) drive_s_ = drive;
      setDrive(drive_s_ / 100);
    }
  }

  void Engine::wet(double spl0, double spl1, double pre_l, double pre_r, double& wl, double& wr) {
    // Serum suspends the effect after 8192 samples of digital silence and
    // freezes its state until signal returns.
    const bool nz = std::fabs(spl0) > SILENCE_EPS || std::fabs(spl1) > SILENCE_EPS;
    if (nz) silent_run_ = 0;
    else silent_run_ += 1;
    if (silent_run_ > SUSPEND_SAMPLES) {
      wl = 0; wr = 0;
    }
    else {
      modeWet(cur_m_, cur_b_, pre_l, pre_r, wl, wr);
      if (xf_ < 1) {
        xf_ = flush(eelMin(1, xf_ + xf_step_));
        const double g = 0.5 - 0.5 * libm_cos(kPi * xf_);
        double old_l, old_r;
        modeWet(old_m_, 1 - cur_b_, pre_l, pre_r, old_l, old_r);
        wl = flush(g * wl + (1 - g) * old_l); wr = flush(g * wr + (1 - g) * old_r);
      }
    }
    wwp_ = (wwp_ + 1) & 15;
  }

  void Engine::output(double spl0, double spl1, double wl, double wr, double& out_l, double& out_r) {
    // The dry path is delayed by the reported latency (Serum does the same).
    dry_l_[static_cast<size_t>(dwp_)] = spl0; dry_r_[static_cast<size_t>(dwp_)] = spl1;
    const double dryl = dry_l_[static_cast<size_t>((dwp_ + 1) & 7)], dryr = dry_r_[static_cast<size_t>((dwp_ + 1) & 7)];
    dwp_ = (dwp_ + 1) & 7;
    mix_s_ = flush(mix_s_ + smooth_ * (mix_t_ - mix_s_));
    lvl_g_ = flush(lvl_g_ + smooth_ * (lvl_t_ - lvl_g_));
    // Serum's mix law, and its hard clip at +/- 4.
    const double dry_g = 1 - mix_s_ * mix_s_;
    // Atlas's makeup: the wet path, half the input at Drive 0, is doubled
    // (exactly, a power of two) so it matches the dry path.
    wl *= wet_gain_; wr *= wet_gain_;
    out_l = flush(eelMin(eelMax(lvl_g_ * (dry_g * dryl + mix_s_ * wl), -OUT_CLIP), OUT_CLIP));
    out_r = flush(eelMin(eelMax(lvl_g_ * (dry_g * dryr + mix_s_ * wr), -OUT_CLIP), OUT_CLIP));
  }
}
