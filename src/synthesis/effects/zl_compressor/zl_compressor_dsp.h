// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 - zsliu98
// This file is adapted from ZLCompressor (https://github.com/ZL-Audio/ZLCompressor,
// commit c11049e), which is free software under the GNU Affero General Public
// License version 3 as published by the Free Software Foundation, and is
// distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
// without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
// PARTICULAR PURPOSE. See the GNU Affero General Public License for details.
//
// The Analogue Compressor's detector, from ZLCompressor's source/dsp:
// chore/decibels.hpp, compressor/computer/compression_computer.hpp,
// compressor/follower/ps_follower.hpp and compressor/styles/*.hpp. Atlas sets
// every parameter on the audio thread before a block, so the atomics and the
// cross-thread update flags are gone; Highway's vector loops are plain loops
// with the scalar tails' std::log; and only the parts the effect uses are
// here: the compression computer with OutputDiff, the follower without Pump
// and Smooth, and the four styles without the RMS tracker. README.atlas.md in
// this folder lists every change.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace atlas::zl {
  namespace chore {
    inline constexpr double kLogMin = 1e-12;
    inline constexpr double kLogMul = 8.6858896380650365530;

    template <typename FloatType>
    FloatType decibelsToGain(const FloatType value) {
      return std::pow(FloatType(10), value * FloatType(0.05));
    }

    template <typename FloatType>
    FloatType gainToDecibels(const FloatType value) {
      return FloatType(20) * std::log10(std::max(value, FloatType(1e-12)));
    }
  } // namespace chore

  // vector::mag_to_db: magnitude to decibels in place.
  template <typename FloatType>
  void magToDb(FloatType* buffer, size_t num_samples) {
    static constexpr auto kLogMin = static_cast<FloatType>(chore::kLogMin);
    static constexpr auto kLogMul = static_cast<FloatType>(chore::kLogMul);
    for (size_t i = 0; i < num_samples; ++i)
      buffer[i] = kLogMul * std::log(std::max(buffer[i], kLogMin));
  }

  // compression_computer.hpp with OutputDiff = true: the gain change in dB for
  // an input level in dB, zero below the knee and negative above it. Curve
  // bends the line above the knee (0 is a straight ratio).
  template <typename FloatType>
  class CompressionComputer {
    public:
      // Recomputes the curve after a change; true when it did.
      bool prepareBuffer() {
        if (!to_interpolate_)
          return false;
        to_interpolate_ = false;
        interpolate();
        return true;
      }

      FloatType eval(FloatType x) const {
        if (x <= low_th_)
          return FloatType(0);
        if (x < high_th_)
          return (para_mid_g0_[0] * x + para_mid_g0_[1]) * x + para_mid_g0_[2];
        if (x < FloatType(0))
          return (para_high_g0_[0] * x + para_high_g0_[1]) * x + para_high_g0_[2];
        return para_over_g0_[0] * x + para_over_g0_[1];
      }

      void setThreshold(FloatType v) { update(threshold_, v); }
      void setRatio(FloatType v) { update(ratio_, std::max(FloatType(1), v)); }
      void setKneeW(FloatType v) { update(knee_w_, std::max(v, FloatType(0.01))); }
      void setCurve(FloatType v) { update(curve_, std::clamp(v, FloatType(-1), FloatType(1))); }

    private:
      struct LinearCurve {
        FloatType b, c;
        void setPara(FloatType t, FloatType r, FloatType) {
          b = FloatType(1) / r;
          c = t * (FloatType(1) - b);
        }
      };

      struct DownCurve {
        FloatType a, c;
        static constexpr FloatType b { FloatType(0) };
        void setPara(FloatType t, FloatType r, FloatType w) {
          const auto temp = FloatType(0.5) / r;
          a = temp / std::min(t + w, FloatType(-0.0001));
          c = temp * (w - t) + t;
        }
      };

      struct UpCurve {
        FloatType a, c;
        static constexpr FloatType b { FloatType(1) };
        void setPara(FloatType t, FloatType r, FloatType w) {
          const auto temp = FloatType(0.5) * (FloatType(1) - r) / r;
          a = temp / std::min(t + w, FloatType(-0.0001));
          c = temp * (w - t);
        }
      };

      void update(FloatType& target, FloatType value) {
        if (target != value) {
          target = value;
          to_interpolate_ = true;
        }
      }

      void interpolate() {
        low_th_ = threshold_ - knee_w_;
        high_th_ = threshold_ + knee_w_;
        {
          // update mid curve parameters
          const auto a0 = (FloatType(1) / ratio_ - FloatType(1)) / (knee_w_ * FloatType(4));
          const auto a1 = -low_th_;
          para_mid_g0_[0] = a0;
          const auto a0a1 = a0 * a1;
          para_mid_g0_[1] = FloatType(2) * a0a1;
          para_mid_g0_[2] = a0a1 * a1;
        }
        {
          // update high curve parameters
          if (curve_ >= FloatType(0)) {
            const auto alpha = FloatType(1) - curve_, beta = curve_;
            linear_curve_.setPara(threshold_, ratio_, knee_w_);
            down_curve_.setPara(threshold_, ratio_, knee_w_);
            para_high_g0_[2] = alpha * linear_curve_.c + beta * down_curve_.c;
            para_high_g0_[1] = alpha * linear_curve_.b + beta * down_curve_.b;
            para_high_g0_[0] = beta * down_curve_.a;
          }
          else {
            const auto alpha = FloatType(1) + curve_, beta = -curve_;
            linear_curve_.setPara(threshold_, ratio_, knee_w_);
            up_curve_.setPara(threshold_, ratio_, knee_w_);
            para_high_g0_[2] = alpha * linear_curve_.c + beta * up_curve_.c;
            para_high_g0_[1] = alpha * linear_curve_.b + beta * up_curve_.b;
            para_high_g0_[0] = beta * up_curve_.a;
          }
          para_high_g0_[1] -= FloatType(1);
        }
        if (high_th_ <= FloatType(0)) {
          para_over_g0_[0] = para_high_g0_[1];
          para_over_g0_[1] = para_high_g0_[2];
        }
        else {
          para_over_g0_[0] = linear_curve_.b - FloatType(1);
          para_over_g0_[1] = linear_curve_.c;
        }
      }

      LinearCurve linear_curve_ {};
      DownCurve down_curve_ {};
      UpCurve up_curve_ {};
      FloatType threshold_ { -18 }, ratio_ { 2 };
      FloatType knee_w_ { FloatType(0.25) }, curve_ { 0 };
      FloatType low_th_ { 0 }, high_th_ { 0 };
      std::array<FloatType, 3> para_mid_g0_ {}, para_high_g0_ {};
      std::array<FloatType, 2> para_over_g0_ {};
      bool to_interpolate_ = true;
  };

  // ps_follower.hpp with Pump and Smooth off: a one-pole follower that rises
  // with the attack coefficient and falls with the release one. ZL's times are
  // in its own milliseconds: the coefficient is exp(-2 pi 1000 / (rate * ms)).
  template <typename FloatType>
  class PSFollower {
    public:
      void prepare(double sample_rate) {
        exp_factor_ = -2.0 * 3.14159265358979323846 * 1000.0 / sample_rate;
        to_update_ = true;
      }

      void reset(FloatType x) { y_ = x; }

      // Recomputes the coefficients after a change; true when it did.
      bool prepareBuffer() {
        if (!to_update_)
          return false;
        to_update_ = false;
        update();
        return true;
      }

      void copyFrom(const PSFollower& other) {
        attack_ = other.attack_;
        release_ = other.release_;
      }

      FloatType processSample(FloatType x) {
        y_ = x >= y_ ? attack_ * (y_ - x) + x : release_ * (y_ - x) + x;
        return y_;
      }

      void setAttack(FloatType millisecond) { update(attack_time_, std::max(FloatType(0), millisecond)); }
      void setRelease(FloatType millisecond) { update(release_time_, std::max(FloatType(0), millisecond)); }

    private:
      void update(FloatType& target, FloatType value) {
        if (target != value) {
          target = value;
          to_update_ = true;
        }
      }

      void update() {
        const auto attack_time = static_cast<double>(attack_time_);
        const auto release_time = static_cast<double>(release_time_);
        attack_ = attack_time < 0.001 ? FloatType(0) : static_cast<FloatType>(std::exp(exp_factor_ / attack_time));
        release_ = release_time < 0.001 ? FloatType(0) : static_cast<FloatType>(std::exp(exp_factor_ / release_time));
      }

      FloatType y_ {};
      FloatType attack_ {}, release_ {};
      double exp_factor_ = -0.1308996938995747;
      FloatType attack_time_ { 50 }, release_time_ { 100 };
      bool to_update_ = true;
  };

  // styles/*.hpp: four ways to feed the computer and the follower. Each turns
  // a buffer of side-chain samples into the gain change in dB, in place.

  // Clean: feed-forward. The level in dB goes through the computer, then the
  // follower smooths the gain change.
  template <typename FloatType>
  class CleanCompressor {
    public:
      template <typename F>
      static void reset(F& follower) { follower.reset(FloatType(0)); }

      template <typename C, typename F>
      static void process(C& computer, F& follower, FloatType* buffer, size_t num_samples) {
        static constexpr auto kLogMin = static_cast<FloatType>(chore::kLogMin);
        static constexpr auto kLogMul = static_cast<FloatType>(chore::kLogMul);
        for (size_t i = 0; i < num_samples; ++i)
          buffer[i] = std::log(std::max(std::abs(buffer[i]), kLogMin)) * kLogMul;
        for (size_t i = 0; i < num_samples; ++i)
          buffer[i] = -follower.processSample(-computer.eval(buffer[i]));
      }
  };

  // Classic: feedback. The level comes from the previous output sample.
  template <typename FloatType>
  class ClassicCompressor {
    public:
      template <typename F>
      void reset(F& follower) {
        follower.reset(FloatType(0));
        x0_ = FloatType(0);
      }

      template <typename C, typename F>
      void process(C& computer, F& follower, FloatType* buffer, size_t num_samples) {
        for (size_t i = 0; i < num_samples; ++i) {
          const FloatType input_db = chore::gainToDecibels(std::abs(x0_));
          // pass through the computer and the follower
          const auto smooth_reduction_db = -follower.processSample(-computer.eval(input_db));
          // apply the gain on the current sample and save it as the feedback sample for the next
          x0_ = buffer[i] * chore::decibelsToGain(smooth_reduction_db);
          buffer[i] = smooth_reduction_db;
        }
      }

    private:
      FloatType x0_ { FloatType(0) };
  };

  // Optical: the follower smooths the level itself, before the computer, as a
  // light-dependent resistor lags the signal.
  template <typename FloatType>
  class OpticalCompressor {
    public:
      template <typename F>
      static void reset(F& follower) { follower.reset(FloatType(0)); }

      template <typename C, typename F>
      static void process(C& computer, F& follower, FloatType* buffer, size_t num_samples) {
        // pass through the follower
        for (size_t i = 0; i < num_samples; ++i)
          buffer[i] = follower.processSample(std::abs(buffer[i]));
        // transfer to db
        magToDb(buffer, num_samples);
        // pass through the computer
        for (size_t i = 0; i < num_samples; ++i)
          buffer[i] = computer.eval(buffer[i]);
      }
  };

  // Vocal: feedback like Classic, but the follower smooths the linear gain.
  // Atlas resets the follower to unity gain (-1, as it holds the negated
  // gain); ZL resets it to 0, which fades the output in from silence.
  template <typename FloatType>
  class VocalCompressor {
    public:
      template <typename F>
      void reset(F& follower) {
        follower.reset(FloatType(-1));
        x0_ = FloatType(0);
      }

      template <typename C, typename F>
      void process(C& computer, F& follower, FloatType* buffer, size_t num_samples) {
        for (size_t i = 0; i < num_samples; ++i) {
          const FloatType input_db = chore::gainToDecibels(std::abs(x0_));
          // pass through the computer and the follower
          const auto smooth_reduction_gain =
              -follower.processSample(-chore::decibelsToGain(computer.eval(input_db)));
          // apply the gain on the current sample and save it as the feedback sample for the next
          x0_ = buffer[i] * smooth_reduction_gain;
          buffer[i] = smooth_reduction_gain;
        }
        magToDb(buffer, num_samples);
      }

    private:
      FloatType x0_ { FloatType(0) };
  };
} // namespace atlas::zl
