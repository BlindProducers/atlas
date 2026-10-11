// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "synth_module.h"
#include <array>
#include <memory>
namespace vital {
class SpectralLockModule : public SynthModule {
public:
  explicit SpectralLockModule(std::string prefix = "");
  ~SpectralLockModule() override;
  void init() override;
  void processWithInput(const poly_float* input, int samples) override;
  void hardReset() override;
  void enable(bool on) override;
  void setSampleRate(int rate) override;
  void setOversampleAmount(int amount) override;
  // Filterbank mode has no latency; Spectral mode is one STFT frame, about
  // 170 ms at any processing rate (8192 samples at 48 kHz, 32768 at most).
  int getLatencySamples() const override;
  static int spectralLatencyForRate(double sample_rate);
  static constexpr int kMaxSpectralLatencySamples = 32768;
  Processor* clone() const override { VITAL_ASSERT(false); return nullptr; }
private:
  void prepare();
  void resetEngines(int mode);
  struct Engines;
  std::unique_ptr<Engines> engines_;
  std::string prefix_;
  std::array<Output*, 15> controls_ {};
  Value *root_ = nullptr, *scale_ = nullptr, *mute_low_ = nullptr, *mute_high_ = nullptr, *mode_ = nullptr;
  int last_mode_ = -1;
};
}
