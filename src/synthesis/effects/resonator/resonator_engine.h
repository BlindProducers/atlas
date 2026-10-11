// SPDX-License-Identifier: GPL-3.0-or-later
// Modal resonator ported from the Corpus JSFX (Quorax / OpenAI), an original
// approximation of a Corpus-style resonator. Differences from the script:
// - Glide acts on the played pitch in octaves; the LFO and spread follow it
//   directly, so glide no longer flattens fast LFO depth or square edges.
// - Random LFO shapes honour Stereo Phase; 0 degrees keeps both sides equal.
// - Membrane and Plate modes keep their state by (m, n) when Ratio reorders
//   them, and modes entering the bank take their coefficients immediately.
// - The limiter acts on the resonated signal, so 0% Mix is transparent.
// - Silent, fully decayed banks are skipped until new excitation arrives.
// Coefficients and state stay double precision: at long decays the poles lie
// within 1e-6 of the unit circle, where float rounding alters the decay time.
#pragma once

#include <array>
#include <cstdint>

namespace atlas::resonator {
  enum Model { kBeam, kMarimba, kString, kMembrane, kPlate, kPipe, kTube, kNumModels };
  enum Quality { kEco, kNormal, kHigh, kNumQualities };
  enum LfoShape {
    kSine, kSquare, kTriangle, kSawUp, kSawDown, kRandomStepped, kRandomSmooth, kNumLfoShapes
  };

  struct Parameters {
    int model = kBeam;
    int quality = kNormal;
    double pitch = 130.8128;        // Hz, before glide, spread and LFO
    bool released = false;          // tracked keys have all been released
    double decay = 1.2;             // seconds to -60 dB
    double material = -0.2;         // -1 soft .. 1 metallic
    double brightness = 0.0;        // -1 .. 1
    double inharmonics = 0.0;       // -1 compress .. 1 stretch
    double ratio = 1.3;             // Membrane and Plate
    double hit = 0.35;              // 0 center .. 1 edge
    double listen_left = 0.25;
    double listen_right = 0.35;
    double spread = 0.0;            // total stereo detune, cents
    double width = 1.0;
    double radius = 0.5;            // Pipe and Tube
    double opening = 0.0;           // Pipe
    double lfo_amount = 0.0;        // semitones
    int lfo_shape = kSine;
    double lfo_frequency = 0.5;     // Hz
    bool lfo_free = true;           // Spin applies only to the free clock
    double lfo_stereo_phase = 0.0;  // cycles
    double lfo_spin = 0.0;          // -1 .. 1, half an octave either way
    double lfo_offset = 0.0;        // cycles
    bool filter = false;
    double filter_frequency = 1500.0;
    double filter_bandwidth = 3.0;  // octaves
    double bleed = 0.0;
    double excitation = 1.0;        // linear gains
    double wet = 1.0;
    double output = 1.0;
    double mix = 0.35;
    bool limiter = true;
    double glide = 0.010;           // seconds
    double off_decay = 0.0;         // 0 .. 1
  };

  class Engine {
    public:
      static constexpr int kMaxModes = 40;
      static constexpr int kControlPeriod = 32;
      static constexpr double kLimiterCeiling = 0.98;
      static int modeCount(int model, int quality);

      Engine();
      void prepare(double sample_rate);
      void reset();
      void setParameters(const Parameters& parameters);
      // Places both LFO sides at an absolute position, in cycles.
      void alignLfo(double cycles);
      void process(const float* in_left, const float* in_right,
                   float* out_left, float* out_right, int samples);

      // Read-only inspection, used by the regression tests.
      int activeModes() const { return count_; }
      double modeRatio(int mode) const { return ratio_[mode]; }
      int modeM(int mode) const { return m_[mode]; }
      int modeN(int mode) const { return n_[mode]; }
      double modeLevel(int channel, int mode) const {
        const Channel& c = channels_[channel];
        return c.yr[mode] * c.yr[mode] + c.yi[mode] * c.yi[mode];
      }
      double currentPitch(int channel) const { return pitch_[channel]; }
      bool idle() const { return idle_; }

    private:
      struct Channel {
        alignas(16) std::array<double, kMaxModes> re {}, im {}, dre {}, dim {};
        alignas(16) std::array<double, kMaxModes> gain {}, dgain {}, fade {}, dfade {};
        alignas(16) std::array<double, kMaxModes> yr {}, yi {};
      };
      struct Layout {
        int model = -1, count = 0;
        double ratio = 0, decay = 0, material = 0, brightness = 0, inharmonics = 0;
        double hit = 0, listen_left = 0, listen_right = 0, radius = 0, opening = 0;
        bool operator==(const Layout& other) const;
      };

      void buildModes();
      void updateCoefficients();
      void setModeTargets(int mode, bool snap, double gain_mix);
      double lfoValue(int channel) const;
      double resonate(Channel& channel, double input);
      double bandpass(int channel, double input);

      Parameters params_;
      Layout layout_;
      std::array<Channel, 2> channels_;
      alignas(16) std::array<double, kMaxModes> ratio_ {}, decay_ {};
      std::array<std::array<double, kMaxModes>, 2> amplitude_ {};
      std::array<int, kMaxModes> m_ {}, n_ {};
      std::array<bool, kMaxModes> snap_mode_ {};
      int count_ = 0, clock_ = 0;
      bool snap_ = true, idle_ = true;
      double rate_ = 44100.0, norm_ = 0.65;
      double pitch_log_ = 0.0;
      std::array<double, 2> pitch_ {};
      std::array<double, 2> lfo_phase_ {};
      std::array<int64_t, 2> lfo_cycle_ {};

      double smooth_ = 0.0, dc_ = 0.0, release_ = 0.0;
      double mix_ = 0.35, excitation_ = 1.0, wet_ = 1.0, output_ = 1.0;
      double width_ = 1.0, bleed_ = 0.0, filter_mix_ = 0.0, gate_ = 1.0;
      double filter_g_ = 0.0, filter_k_ = 2.0, limiter_gain_ = 1.0;
      std::array<double, 2> input_previous_ {}, input_dc_ {}, wet_previous_ {}, wet_dc_ {};
      std::array<double, 2> ic1_ {}, ic2_ {};
  };
}
