// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "synth_module.h"
#include "tape_controls.h"
#include "tape_noise_library.h"
#include "cut_filter.h"
#include <array>
#include <limits>
#include <vector>

namespace vital {
class TapeModule final : public SynthModule {
 public:
  explicit TapeModule(std::string prefix = "", std::shared_ptr<TapeNoiseLibrary> library = nullptr);
  void init() override;
  void processWithInput(const poly_float* input, int count) override;
  void setSampleRate(int rate) override;
  void setOversampleAmount(int amount) override;
  void hardReset() override;
  void enable(bool enabled) override;
  Processor* clone() const override { VITAL_ASSERT(false); return nullptr; }
 private:
  struct CachedValue {
    double input = std::numeric_limits<double>::quiet_NaN(), value = 0;
    template<class Transform> double get(double next, Transform transform) {
      if (next != input) { input = next; value = transform(next); }
      return value;
    }
  };
  struct Biquad {
    std::array<double, 6> c { 1, 0, 0, 1, 0, 0 };
    double z1 = 0, z2 = 0;
    double tick(double x) {
      const double y = c[0] * x / c[3] + z1;
      z1 = (c[1] * x - c[4] * y) / c[3] + z2;
      z2 = (c[2] * x - c[5] * y) / c[3];
      return y;
    }
  };
  struct Lane {
    Biquad drive_tone, noise_tone;
    CachedValue drive_gain, output_gain, wow_depth, flutter_depth, noise_pitch, noise_gain;
    double drive_tone_value = -1, noise_tone_value = 1000;
    double dry_envelope = 0, wet_envelope = 0, auto_gain = 1;
    double wow_phase = 0, flutter_phase = 0, delay = 12;
    double noise_position = 0, fading_position = 0, noise_envelope = 0;
    int dropout_wait = 1, dropout_remaining = 0, dropout_length = 1;
    uint32_t random = 0x428e3b71;
  };
  struct Filters { CutFilter hp { true }, lp { false }; };
  void prepareDelay();
  void configureFilters(Filters& filters, int hp, int lp);
  poly_float filterAndWidth(poly_float audio, Filters& filters, int width, int hp, int lp);
  float delaySample(int lane, double delay) const;
  static float noiseSample(const TapeNoiseLibrary::Clip& clip, int channel, double position);
  static void advanceNoise(const TapeNoiseLibrary::Clip& clip, double& position, double increment);
  std::string prefix_;
  std::shared_ptr<TapeNoiseLibrary> library_;
  std::array<Output*, tape::kNumControls> controls_ {};
  std::array<poly_float, tape::kNumControls> current_ {}, target_ {};
  std::array<float, tape::kNumControls> smoothing_ {};
  std::array<Lane, poly_float::kSize> lanes_ {};
  Filters noise_filters_, global_filters_;
  std::vector<poly_float> delay_;
  int write_ = 0, tone_counter_ = 0, prepared_rate_ = 0;
  double envelope_step_ = 0, auto_step_ = 0, delay_step_ = 0, attack_ = 0, release_ = 0;
  const TapeNoiseLibrary::Clip* clip_ = nullptr;
  const TapeNoiseLibrary::Clip* fading_clip_ = nullptr;
  int fade_remaining_ = 0, fade_length_ = 1;
  bool ready_ = false, noise_was_enabled_ = false, motion_was_active_ = false;
};
}
