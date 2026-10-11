// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "synth_module.h"

#include <array>

namespace vital {

  // An attenuation LFO per channel. A half-cycle stereo separation turns
  // tremolo into autopan without collapsing the input's stereo image.
  class TremoloPanModule : public SynthModule {
    public:
      TremoloPanModule(const Output* beats_per_second, std::string prefix = "");
      void init() override;
      void processWithInput(const poly_float* audio_in, int num_samples) override;
      void correctToTime(double seconds) override;
      void hardReset() override;
      void enable(bool enable) override;
      Processor* clone() const override { VITAL_ASSERT(false); return nullptr; }

    private:
      enum { kSine, kTriangle, kSquare, kRampUp, kRampDown, kNumShapes };
      static double waveform(int shape, double phase);
      struct Lane {
        double phase = 0.0;
        double frequency = 2.0;
        double stereo_phase = 0.5;
        double offset = 0.0;
        double depth = 1.0;
        double mix = 1.0;
        double wave = 1.0;
      };

      const Output* beats_per_second_;
      std::string prefix_;
      Output* frequency_ = nullptr;
      Value* sync_ = nullptr;
      Value* shape_ = nullptr;
      Output* stereo_phase_ = nullptr;
      Output* offset_ = nullptr;
      Output* depth_ = nullptr;
      Output* smoothing_ = nullptr;
      Output* mix_ = nullptr;
      std::array<Lane, poly_float::kSize> lanes_ {};
      std::array<double, kNumShapes> shape_weights_ {};
      bool initialized_ = false;
      bool has_transport_time_ = false;
      bool timeline_valid_ = false;
      int previous_sync_ = 0;
      double transport_time_ = 0.0;
      double expected_transport_time_ = 0.0;
  };
}
