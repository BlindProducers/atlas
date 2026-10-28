// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "synth_module.h"
#include "drive_dynamics_controls.h"
#include "drive_dynamics/drive_dynamics.h"

#include <array>
#include <string>

namespace vital {
  // Atlas controls around the Saturator, Dynamic Tube, Overdrive and Multiband
  // Dynamics ports (src/synthesis/effects/drive_dynamics). Each control holds
  // its script's slider value, which is passed to the port unchanged; the
  // port runs at the effect chain's rate, as the script would in a project at
  // that rate. Only the left and right lanes reach the host, so those are
  // processed and lanes 2-3 mirror them.
  template<class Core, size_t kNumControls>
  class DriveDynamicsModule : public SynthModule {
    public:
      using Values = std::array<double, kNumControls>;
      using Parameters = typename Core::Parameters;
      struct Control { std::string suffix; float minimum, maximum; bool modulated; };

      DriveDynamicsModule(std::string prefix, std::array<Control, kNumControls> controls,
                          Parameters (*parameters)(const Values&));
      void init() override;
      void processWithInput(const poly_float* input, int samples) override;
      void hardReset() override { reset_pending_ = true; }
      void enable(bool on) override;
      void setSampleRate(int rate) override;
      void setOversampleAmount(int amount) override;
      int getLatencySamples() const override;
      Processor* clone() const override { VITAL_ASSERT(false); return nullptr; }

    private:
      Values readValues() const;

      std::string prefix_;
      std::array<Control, kNumControls> controls_;
      Parameters (*parameters_)(const Values&);
      std::array<Output*, kNumControls> mod_controls_ {};
      std::array<Value*, kNumControls> base_controls_ {};
      Core core_;
      Values applied_ {};
      int prepared_rate_ = 0;
      bool reset_pending_ = true;
  };

  using drive_dynamics::kNumSaturatorControls;
  using drive_dynamics::kNumDynamicTubeControls;
  using drive_dynamics::kNumOverdriveControls;
  using drive_dynamics::kNumMultibandControls;

  class SaturatorModule : public DriveDynamicsModule<atlas::drive_dynamics::Saturator, kNumSaturatorControls> {
    public:
      explicit SaturatorModule(std::string prefix = "");
  };
  class DynamicTubeModule : public DriveDynamicsModule<atlas::drive_dynamics::DynamicTube, kNumDynamicTubeControls> {
    public:
      explicit DynamicTubeModule(std::string prefix = "");
  };
  class OverdriveModule : public DriveDynamicsModule<atlas::drive_dynamics::Overdrive, kNumOverdriveControls> {
    public:
      explicit OverdriveModule(std::string prefix = "");
  };
  class MultibandDynamicsModule
      : public DriveDynamicsModule<atlas::drive_dynamics::MultibandDynamics, kNumMultibandControls> {
    public:
      explicit MultibandDynamicsModule(std::string prefix = "");
  };
}
