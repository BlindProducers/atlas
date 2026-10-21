// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "synth_module.h"
#include <array>
#include <memory>
#include <string>

namespace atlas::maim { class Engine; }

namespace vital {
  // Atlas controls around the Maim engine (src/synthesis/effects/maim):
  // audio through a circuit-bent MP3 encoder, Opus, GSM or an LPC vocoder.
  // Encoder decides which rows the editor shows and, with the LPC rate, the
  // latency. Only the left and right lanes reach the host, so those are
  // processed and lanes 2-3 mirror them.
  class MaimModule : public SynthModule {
    public:
      explicit MaimModule(std::string prefix = "");
      ~MaimModule() override;
      void init() override;
      void processWithInput(const poly_float* input, int samples) override;
      void hardReset() override;
      void enable(bool on) override;
      void setSampleRate(int rate) override;
      void setOversampleAmount(int amount) override;
      int getLatencySamples() const override;
      Processor* clone() const override { VITAL_ASSERT(false); return nullptr; }

      // The delay for an Encoder and LPC Sample Rate value at a processing rate.
      static int latencyFor(float encoder, float lpc_rate, int sample_rate);
      // Room every shell reserves for the dry signal while Maim is being
      // built: the longest latency any setting has at 8 times 48 kHz.
      static constexpr int kMaxLatencySamples = 32768;

      enum ModControl {
        kTurbo, kTilt, kError, kMix, kShift, kLevelShift, kFeedback, kButterflyStraight, kButterflyCrossed,
        kLossPeriod, kLossWidth, kLossJitter, kDrive, kMakeup, kLowCut, kHighCut, kNumModControls
      };
      enum BaseControl {
        kEncoder, kBitrate, kLpcRate, kBandStep, kBandInvert, kWindowStep, kBandShift, kBandReverse, kLossMode,
        kNumBaseControls
      };
      static const char* modControlSuffix(int control);
      static const char* baseControlSuffix(int control);

      const atlas::maim::Engine& getEngine() const { return *engine_; }

    private:
      void prepareEngine();

      std::unique_ptr<atlas::maim::Engine> engine_;
      std::string prefix_;
      std::array<Output*, kNumModControls> mod_controls_ {};
      std::array<Value*, kNumBaseControls> base_controls_ {};
      int prepared_rate_ = 0;
      bool reset_pending_ = true;
  };
}
