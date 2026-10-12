// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "synth_module.h"
#include <array>
#include <memory>
#include <optional>

namespace atlas::glitch_delay { class Engine; }

namespace vital {
  class TranceGateClock;

  // Atlas controls, tempo and transport around the Glitch Delay engine
  // (adapted from Astralay): four delay taps with nine random glitch types
  // each, freeze, smear and a tape stop. The taps hear the input summed to
  // mono; the dry signal stays stereo. Only the left and right lanes reach
  // the host, so those are processed and lanes 2-3 mirror them.
  class GlitchDelayModule : public SynthModule {
    public:
      static constexpr int kNumTaps = 4;
      static constexpr double kMaxDelaySeconds = 10.0;

      enum GlobalControl {
        kSync, kGlide, kFreeze, kFreezeSustain, kPlacement, kBufferSize, kBufferSync, kMaxGlitches,
        kLengthMin, kLengthMax, kReproducible, kSeed, kSmearAmount, kSmearSize, kStopTape, kStopTime,
        kStartTime, kMix, kOutputGain, kTapView, kNumGlobalControls
      };

      enum TapControl {
        kEnabled, kTime, kTimeSync, kVolume, kPan, kFeedback, kLowCut, kHighCut,
        kReverseProb,
        kStutterProb, kStutterMin, kStutterMax, kStutterSyncMin, kStutterSyncMax,
        kGrainProb, kGrainSizeMin, kGrainSizeMax, kGrainDensityMin, kGrainDensityMax,
        kPitchProb, kPitchMin, kPitchMax, kPitchSpeedMin, kPitchSpeedMax, kPitchMode,
        kLpcProb, kLpcMin, kLpcMax,
        kCepsProb, kCepsMin, kCepsMax,
        kRingProb, kRingMin, kRingMax,
        kFmProb, kFmRatioMin, kFmRatioMax, kFmIndexMin, kFmIndexMax,
        kCrushProb, kCrushBitsMin, kCrushBitsMax, kCrushRateMin, kCrushRateMax,
        kNumTapControls
      };

      // Parameter id suffixes, after "glitch_delay_" and, for a tap, "tap_N_".
      static const char* globalSuffix(int control);
      static const char* tapSuffix(int control);
      // Whether Atlas modulation can move a control. Switches, choices and the
      // note values that stand in for times while Host Sync is on cannot, as in
      // Astralay; nor can the glitch engine's settings or the output gain.
      static bool isGlobalModulated(int control);
      static bool isTapModulated(int control);

      GlitchDelayModule(const Output* beats_per_second, const TranceGateClock* transport,
                        std::string prefix = "");
      ~GlitchDelayModule() override;
      void init() override;
      void processWithInput(const poly_float* input, int samples) override;
      void hardReset() override;
      void enable(bool on) override;
      void setSampleRate(int rate) override;
      void setOversampleAmount(int amount) override;
      void correctToTime(double seconds) override { seconds_ = seconds; }
      Processor* clone() const override { VITAL_ASSERT(false); return nullptr; }

      const atlas::glitch_delay::Engine& getEngine() const { return *engine_; }

    private:
      struct Control {
        const Output* modulated = nullptr;
        const Value* base = nullptr;
        float min = 0.0f, max = 1.0f;
        float value() const;
      };

      Control createControl(const std::string& name, bool modulated);
      void prepareEngine();
      void updateEngine(int samples);

      std::unique_ptr<atlas::glitch_delay::Engine> engine_;
      const Output* beats_per_second_;
      const TranceGateClock* transport_;
      std::string prefix_;
      std::array<Control, kNumGlobalControls> global_ {};
      std::array<std::array<Control, kNumTapControls>, kNumTaps> taps_ {};
      int prepared_rate_ = 0;
      bool reset_pending_ = true;
      double seconds_ = 0.0;
      // Where the host timeline should be at the next block, in samples at the
      // module's rate, to tell a seek or loop from rounding in the song time.
      std::optional<long long> expected_position_;
  };
}
