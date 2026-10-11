#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

// Native ports of the scripts in tests/jsfx/fixtures. Double precision retains JSFX's
// arithmetic; conversion to Atlas's float SIMD lanes occurs at the module edge.
// Sources: Quorax Dimension Expander (OpenAI for quorax), BP A-Erosion
// (Alessio / Blind Producers), Floaty (Daniel Arena / remaincalm.org, GPL).
namespace vital::jsfx {
  using Frame = std::array<double, 4>;

  // Erosion's sample offsets and Floaty's filter equations depend on the host
  // sample rate. Run their unchanged cores at that rate inside Atlas's faster
  // FX chain. A Blackman-windowed sinc removes aliases/images at either edge.
  // This adds 64 host samples of latency when oversampling, none at 1x.
  class HostRate {
    public:
      static constexpr int kMaxFactor = 8;
      static constexpr int kSpan = 64;
      void prepare(int factor);
      void reset();
      template<class Core>
      Frame process(const Frame& input, Core& core) {
        if (factor_ == 1) return core.process(input);
        input_[input_position_] = input;
        if (phase_ == 0) {
          Frame down {};
          int position = input_position_;
          for (int tap = 0; tap <= kSpan * factor_; ++tap) {
            for (int lane = 0; lane < 4; ++lane)
              down[lane] += taps_[tap] * input_[position][lane];
            if (--position < 0) position = static_cast<int>(input_.size()) - 1;
          }
          if (++output_position_ > kSpan) output_position_ = 0;
          output_[output_position_] = core.process(down);
        }
        Frame up {};
        int position = output_position_;
        for (int tap = phase_; tap <= kSpan * factor_; tap += factor_) {
          for (int lane = 0; lane < 4; ++lane)
            up[lane] += factor_ * taps_[tap] * output_[position][lane];
          if (--position < 0) position = kSpan;
        }
        if (++input_position_ == static_cast<int>(input_.size())) input_position_ = 0;
        if (++phase_ == factor_) phase_ = 0;
        return up;
      }
    private:
      std::array<double, kSpan * kMaxFactor + 1> taps_ {};
      std::array<Frame, kSpan * kMaxFactor + 1> input_ {};
      std::array<Frame, kSpan + 1> output_ {};
      int factor_ = 1, phase_ = 0, input_position_ = 0, output_position_ = kSpan;
  };

  // Reserve delay address space at preparation without touching every sample
  // of every inactive slot. Reads of unwritten samples return zero. Reset is
  // constant-time, and sample processing never allocates or frees memory.
  class Memory {
    public:
      void prepare(size_t size);
      void clear();
      double read(size_t index) const;
      void write(size_t index, double value);
    private:
      static constexpr size_t kPage = 1024;
      std::unique_ptr<double[]> data_;
      std::unique_ptr<uint64_t[]> written_;
      std::vector<uint32_t> epochs_;
      uint32_t epoch_ = 1;
      size_t size_ = 0;
  };

  class Quorax {
    public:
      struct Parameters {
        double size = 35, mix = 35, output = 0, motion = 0.4;
        double low_cut = 0, high_cut = 20000;
        int slope = 1;
      };
      Quorax();
      void prepare(double sample_rate);
      void reset();
      void setParameters(const Parameters& parameters);
      Frame process(const Frame& input);
    private:
      struct Biquad {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        std::array<double, 2> z1 {}, z2 {};
        double process(double input, int pair);
        void clear() { z1.fill(0); z2.fill(0); }
      };
      void coefficients(std::array<Biquad, 4>& filters, double frequency, bool highpass);
      double readDelay(double delay, int pair) const;
      Parameters parameters_;
      Memory buffer_;
      std::array<Biquad, 4> hp_, lp_;
      double sample_rate_ = 0, phase_ = 0.123291835;
      double size_ = 35, mix_ = 0.35, gain_ = 1;
      double size_coefficient_ = 0, gain_coefficient_ = 0, gain_target_ = 1;
      int length_ = 0, position_ = 0, sections_ = 2;
      bool initial_ = true;
  };

  class Erosion {
    public:
      struct Parameters { int mode = 0; double frequency = 5000, width = 1, amount = 50, stereo = 100; };
      void prepare(double sample_rate);
      void reset();
      void setParameters(const Parameters& parameters);
      Frame process(const Frame& input, const Frame* noise_override = nullptr);
    private:
      double noise();
      double read(double position, int lane) const;
      Parameters parameters_;
      std::array<Frame, 4096> buffer_ {};
      Frame x1_ {}, x2_ {}, y1_ {}, y2_ {};
      double sample_rate_ = 44100, phase_ = 0;
      double b0_ = 0, b2_ = 0, a1_ = 0, a2_ = 0;
      uint64_t random_ = 0x853c49e6748fea9bull;
      int position_ = 0;
  };

  class Floaty {
    public:
      struct Parameters {
        double delay = 250, mix = 40, feedback = 30, warp_rate = 0.1, warp = 0.5;
        double cutoff = 80, resonance = 80, playback = 1, offset = 98, clamp = 6;
        int sync = 0;
      };
      static constexpr double kMaxDelaySeconds = 16;
      // Also cover the original script's complete default JSFX RAM range at
      // low host rates / slow tempos (two channels * four delay lengths).
      static constexpr int kMinDelayCapacity = 1048576;
      void prepare(double sample_rate);
      void reset();
      void setParameters(const Parameters& parameters, double bpm);
      Frame process(const Frame& input);
    private:
      Parameters parameters_;
      Memory buffer_;
      Frame low0_ {}, low1_ {}, high0_ {}, high1_ {};
      size_t pair_capacity_ = 0;
      double sample_rate_ = 0, play_ = 0, time_ = 0, clamp_ = 2;
      double rate_ = 0, lc_ = 0, lr_ = 0, hc_ = 0, hr_ = 0, target_clamp_ = 2;
      double old_playback_ = 0, old_rate_ = 0, old_offset_ = 0;
      int delay_ = -1, length_ = 8, left_ = 0, right_ = 0;
      int delay_capacity_ = kMinDelayCapacity;
      bool resetting_ = true;
  };
}
