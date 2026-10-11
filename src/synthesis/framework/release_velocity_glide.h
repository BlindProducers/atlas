#pragma once

#include "processor.h"
#include "futils.h"

#include <algorithm>
#include <array>

namespace vital {
  // Like pitch portamento, this source advances in elapsed audio samples at
  // control rate. Each cloned voice owns its ramp and release state.
  class ReleaseVelocityGlide : public Processor {
    public:
      enum { kLift, kRetrigger, kMode, kTime, kCurve, kTarget, kNumInputs };

      ReleaseVelocityGlide() : Processor(kNumInputs, 1, true) { }
      Processor* clone() const override { return new ReleaseVelocityGlide(*this); }

      void hardReset() override {
        current_ = start_ = target_ = 0.0f;
        position_.fill(0.0);
        released_.fill(false);
      }

      void process(int num_samples) override {
        const auto* lift = input(kLift)->source;
        const auto* retrigger = input(kRetrigger)->source;
        const bool glide = input(kMode)->at(0)[0] >= 0.5f;
        const bool full_amount = input(kTarget)->at(0)[0] >= 0.5f;
        const double seconds = std::clamp(double(input(kTime)->at(0)[0]), 0.0, 10.0);
        const float curve = std::clamp(input(kCurve)->at(0)[0], -8.0f, 8.0f);
        const poly_mask note_on = retrigger->trigger_mask &
                                  poly_float::equal(retrigger->trigger_value, kVoiceOn);

        for (int lane = 0; lane < poly_float::kSize; ++lane) {
          if (note_on[lane]) {
            current_.set(lane, 0.0f);
            start_.set(lane, 0.0f);
            target_.set(lane, 0.0f);
            position_[lane] = 0.0;
            released_[lane] = false;
          }
          // A zero-velocity note-off is still a release. Track the event,
          // rather than testing the velocity value, including while the
          // MIDI-velocity target is selected so switching targets works.
          if (lift->trigger_mask[lane])
            released_[lane] = true;
          const float target = full_amount ? (released_[lane] ? 1.0f : 0.0f)
                                           : lift->buffer[0][lane];
          if (!glide || seconds <= 0.0) {
            current_.set(lane, target);
            target_.set(lane, target);
            position_[lane] = 1.0;
            continue;
          }

          int samples = num_samples;
          if (lift->trigger_mask[lane] || target != target_[lane]) {
            start_.set(lane, current_[lane]);
            target_.set(lane, target);
            position_[lane] = 0.0;
            if (lift->trigger_mask[lane])
              samples -= std::clamp(static_cast<int>(lift->trigger_offset[lane]), 0, num_samples);
          }
          position_[lane] = std::min(1.0, position_[lane] +
              samples / (seconds * std::max(1, getSampleRate())));
          const float phase = static_cast<float>(position_[lane]);
          const float shaped = curve == 0.0f ? phase : futils::powerScale(phase, curve);
          current_.set(lane, position_[lane] >= 1.0 ? target_[lane]
              : start_[lane] + (target_[lane] - start_[lane]) * shaped);
        }
        output()->buffer[0] = current_;
        output()->trigger_value = current_;
      }

    private:
      poly_float current_ = 0.0f;
      poly_float start_ = 0.0f;
      poly_float target_ = 0.0f;
      std::array<double, poly_float::kSize> position_ {};
      std::array<bool, poly_float::kSize> released_ {};
  };
}
