// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "synth_module.h"
#include <array>
#include <cstdint>
#include <memory>
#include <string>

namespace atlas::particles { class Engine; }

namespace vital {
  class NoteMonitor;

  // Atlas controls around the Particles engine (src/synthesis/effects/particles):
  // a granular texture processor recording into a buffer of up to 7.4 s.
  // Mode and Quality are menus; the rest are modulation destinations. Each
  // note-on fires a grain, as does Trigger rising through one half. Only the
  // left and right lanes reach the host, so those are processed and lanes 2-3
  // mirror them.
  class ParticlesModule : public SynthModule {
    public:
      explicit ParticlesModule(const NoteMonitor* notes = nullptr, std::string prefix = "");
      ~ParticlesModule() override;
      void init() override;
      void processWithInput(const poly_float* input, int samples) override;
      void hardReset() override;
      void enable(bool on) override;
      void setSampleRate(int rate) override;
      void setOversampleAmount(int amount) override;
      int getLatencySamples() const override;
      Processor* clone() const override { VITAL_ASSERT(false); return nullptr; }

      // Room every shell reserves for the dry signal while Particles is being
      // built: the latency at 8 times 192 kHz is about 3,200 samples.
      static constexpr int kMaxLatencySamples = 4096;

      enum ModControl {
        kInputGain, kPosition, kSize, kPitch, kDensity, kTexture, kMix, kSpread, kFeedback, kReverb, kFreeze,
        kTrigger, kNumModControls
      };
      enum BaseControl { kMode, kQuality, kNumBaseControls };
      static const char* modControlSuffix(int control);
      static const char* baseControlSuffix(int control);

      const atlas::particles::Engine& getEngine() const { return *engine_; }

    private:
      void prepareEngine();

      std::unique_ptr<atlas::particles::Engine> engine_;
      const NoteMonitor* notes_;
      std::string prefix_;
      std::array<Output*, kNumModControls> mod_controls_ {};
      std::array<Value*, kNumBaseControls> base_controls_ {};
      int prepared_rate_ = 0;
      bool reset_pending_ = true;
      bool trigger_high_ = false;
      uint32_t note_on_count_ = 0;
  };
}
