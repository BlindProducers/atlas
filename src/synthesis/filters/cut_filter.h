#pragma once

#include "poly_utils.h"
#include "synth_constants.h"

#include <array>
#include <cmath>

namespace vital {

  // One to eight Butterworth poles: the complete filter, rather than each
  // individual pole, is -3 dB at the displayed cutoff. Fixed storage and TPT
  // states keep low cutoffs usable at oversampled processing rates.
  class CutFilter {
    public:
      explicit CutFilter(bool high_pass) : high_pass_(high_pass) { reset(constants::kFullMask); }

      void reset(poly_mask mask) {
        one_state_ = utils::maskLoad(one_state_, 0.0f, mask);
        for (auto& section : sections_) {
          section.s1 = utils::maskLoad(section.s1, 0.0f, mask);
          section.s2 = utils::maskLoad(section.s2, 0.0f, mask);
        }
      }

      void configure(poly_float midi, int slope, int sample_rate) {
        midi = utils::clamp(midi, 8.0f, 136.0f);
        const int order = utils::iclamp(slope, 0, 7) + 1;
        if (initialized_ && order == order_ && sample_rate == sample_rate_ &&
            !poly_float::notEqual(midi, midi_).anyMask())
          return;

        const bool order_changed = order != order_;
        order_ = order;
        sample_rate_ = sample_rate;
        midi_ = midi;
        poly_float c;
        for (int lane = 0; lane < poly_float::kSize; ++lane) {
          const double frequency = utils::midiNoteToFrequency(midi[lane]);
          double g = std::tan(kPi * std::min(frequency / sample_rate, 0.499));
          // Approach bypass continuously in the final semitone at the open
          // end. Do not switch the whole filter on at 8.5 or 135.5 semitones.
          const double edge = high_pass_ ? std::min(1.0f, midi[lane] - 8.0f)
                                         : std::min(1.0f, 136.0f - midi[lane]);
          const double value = high_pass_ ? (g * edge) / (1.0 + g * edge) : g / (g + edge);
          c.set(lane, static_cast<float>(value));
        }
        target_one_ = c;
        bypass_ = high_pass_ ? poly_float::equal(c, 0.0f) : poly_float::equal(c, 1.0f);
        const bool immediate = !initialized_ || order_changed;
        const int ramp = std::max(1, sample_rate / 200); // 5 ms, independent of host block size.
        if (order_changed)
          reset(constants::kFullMask);
        if (immediate)
          one_ = c;
        delta_one_ = (c - one_) / ramp;
        for (int index = 0; index < order / 2; ++index) {
          auto& section = sections_[index];
          // Least resonant pair first to keep intermediate levels bounded.
          const int pair = order / 2 - 1 - index;
          section.k = 2.0f * std::sin(kPi * (2 * pair + 1) / (2 * order));
          const poly_float complement = poly_float(1.0f) - c;
          const poly_float denominator = c * complement * (section.k - 2.0f) + 1.0f;
          section.target = { complement * complement / denominator,
                             c * complement / denominator, c * c / denominator };
          if (immediate)
            section.a = section.target;
          for (int j = 0; j < 3; ++j)
            section.delta[j] = (section.target[j] - section.a[j]) / ramp;
        }
        remaining_ = immediate ? 0 : ramp;
        initialized_ = true;
      }

      force_inline poly_float tick(poly_float input) {
        if (remaining_ > 0) {
          --remaining_;
          one_ = remaining_ ? one_ + delta_one_ : target_one_;
          for (int index = 0; index < order_ / 2; ++index) {
            auto& section = sections_[index];
            for (int j = 0; j < 3; ++j)
              section.a[j] = remaining_ ? section.a[j] + section.delta[j] : section.target[j];
          }
          if (remaining_ == 0)
            reset(bypass_);
        }
        if (remaining_ == 0 && !(~bypass_).anyMask())
          return input;
        poly_float sample = input;
        if (order_ & 1) {
          const poly_float delta = (sample - one_state_) * one_;
          const poly_float low = one_state_ + delta;
          one_state_ = low + delta;
          sample = high_pass_ ? sample - low : low;
        }
        // Trapezoidal state-variable sections, following Andrew Simper's
        // bounded form: https://cytomic.com/files/dsp/SvfLinearTrapOptimised2.pdf
        for (int index = 0; index < order_ / 2; ++index) {
          auto& s = sections_[index];
          const poly_float v3 = sample - s.s2;
          const poly_float v1 = s.a[0] * s.s1 + s.a[1] * v3;
          const poly_float v2 = s.s2 + s.a[1] * s.s1 + s.a[2] * v3;
          s.s1 = v1 * 2.0f - s.s1;
          s.s2 = v2 * 2.0f - s.s2;
          sample = high_pass_ ? sample - v1 * s.k - v2 : v2;
        }
        if (remaining_ == 0 && bypass_.anyMask())
          reset(bypass_);
        return remaining_ ? sample : utils::maskLoad(sample, input, bypass_);
      }

    private:
      struct Section {
        std::array<poly_float, 3> a{}, target{}, delta{};
        poly_float s1 = 0.0f, s2 = 0.0f;
        float k = 1.0f;
      };
      std::array<Section, 4> sections_;
      poly_float one_state_ = 0.0f, one_ = 0.0f, target_one_ = 0.0f, delta_one_ = 0.0f, midi_ = 0.0f;
      poly_mask bypass_ = 0;
      int order_ = 1, sample_rate_ = 0, remaining_ = 0;
      bool high_pass_, initialized_ = false;
  };
}
