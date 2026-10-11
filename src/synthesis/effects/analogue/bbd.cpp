// SPDX-License-Identifier: GPL-3.0-or-later
#include "bbd.h"

#include <algorithm>
#include <cmath>

namespace atlas::analogue {
  namespace {
    constexpr double kTwoPi = 6.28318530717958647692;
    // Caps the clock so CPU stays bounded; 16 ticks per sample covers a
    // 10 ms delay through 8192 stages at 44.1 kHz.
    constexpr double kMaxTicksPerSample = 16.0;
    constexpr double kMinClock = 50.0;

    // Cubic soft clip normalized to the chip's headroom: unity slope at zero,
    // reaching the rail with zero slope at 1.5x headroom.
    double bucketCurve(double x) {
      x = std::clamp(x, -1.5, 1.5);
      return x - (4.0 / 27.0) * x * x * x;
    }
  }

  double noiseForSignalToNoise(double signal_to_noise_db, const AnalogFilter& output_filter, double clock_hz) {
    // Held white noise of variance s^2 at the clock rate has the two-sided
    // density s^2 T sinc^2(f T); integrate it through the output filter.
    const double period = 1.0 / clock_hz;
    const double step = clock_hz / 2000.0;
    double integral = 0.0;
    for (double hz = 0.5 * step; hz < 8.0 * clock_hz; hz += step) {
      const double x = kTwoPi * 0.5 * hz * period;
      const double hold = std::sin(x) / x;
      integral += 2.0 * period * hold * hold * std::norm(output_filter.responseAtHz(hz)) * step;
    }
    const double target = std::sqrt(0.5) * std::pow(10.0, -signal_to_noise_db / 20.0);
    return target / std::sqrt(std::max(integral, 1.0e-30));
  }

  void BbdLine::configure(const BbdChip& chip, const AnalogFilter& input_filter,
                          const AnalogFilter& output_filter, double sample_rate) {
    chip_ = chip;
    input_filter_ = input_filter;
    output_filter_ = output_filter;
    sample_rate_ = sample_rate;
    period_ = 1.0 / sample_rate;
    output_dc_gain_ = output_filter.dcGain();

    const auto build = [&](const AnalogFilter& filter, std::vector<Term>& terms, std::vector<Complex>& table, bool input) {
      terms.assign(filter.count, Term {});
      table.assign(static_cast<size_t>(filter.count) * (kTableSize + 1), Complex());
      for (int i = 0; i < filter.count; ++i) {
        const Complex pole = filter.poles[i];
        terms[i].pole_step = std::exp(pole * period_);
        const double weight = filter.paired[i] ? 2.0 : 1.0;
        for (int k = 0; k <= kTableSize; ++k) {
          const double d = static_cast<double>(k) / kTableSize;
          table[static_cast<size_t>(i) * (kTableSize + 1) + k] = input
              ? weight * period_ * filter.residues[i] * std::exp(pole * (d * period_))
              : weight * filter.residues[i] / pole * std::exp(pole * ((1.0 - d) * period_));
        }
      }
    };
    build(input_filter_, input_terms_, input_table_, true);
    build(output_filter_, output_terms_, output_table_, false);
    // A power-of-two FIFO lets grouped ticks wrap with a mask.
    int bucket_count = 1;
    while (bucket_count < std::max(1, chip_.stages / 2)) bucket_count *= 2;
    if (chip_.max_events_per_sample > 0.0 && bucket_count != chip_.stages / 2)
      chip_.max_events_per_sample = 0.0;  // grouping needs an exact power-of-two chain
    buckets_.assign(std::max(1, chip_.stages / 2), 0.0);
    bucket_mask_ = bucket_count - 1;
    const double loss = chip_.stages * chip_.transfer_inefficiency;
    transfer_coefficient_ = loss / (1.0 + loss);
    setDelay(delay());
    reset();
  }

  void BbdLine::reset() {
    std::fill(buckets_.begin(), buckets_.end(), 0.0);
    for (auto& term : input_terms_) term.state = 0.0;
    for (auto& term : output_terms_) term.state = 0.0;
    held_ = 0.0;
    transfer_state_ = 0.0;
    write_ = 0;
    next_tick_ = 0.0;
    clock_phase_ = 0.0;
    clock_ = target_clock_;
    updateGroup();
    refresh_counter_ = 0;
    random_ = 0x6d2b79f5u;
  }

  void BbdLine::setDelay(double seconds) {
    const double max_clock = kMaxTicksPerSample * sample_rate_;
    const double clock = 0.5 * chip_.stages / std::max(seconds, 1.0e-6);
    target_clock_ = std::clamp(clock, kMinClock, max_clock);
  }

  void BbdLine::updateGroup() {
    group_ = 1;
    if (chip_.max_events_per_sample > 0.0) {
      const double limit = chip_.max_events_per_sample * sample_rate_;
      while (clock_ / group_ > limit && group_ < 64) group_ *= 2;
    }
    samples_per_tick_ = group_ * sample_rate_ / clock_;
    group_transfer_coefficient_ = std::pow(transfer_coefficient_, group_);
    group_noise_ = 1.0 / std::sqrt(static_cast<double>(group_));
  }

  void BbdLine::refresh() {
    // Clock feedthrough only exists in the model while it is representable;
    // fade it out approaching Nyquist instead of letting it alias.
    const double nyquist_fade = std::clamp((0.45 * sample_rate_ - clock_) / (0.05 * sample_rate_), 0.0, 1.0);
    leakage_gain_ = chip_.clock_leakage > 0.0 && nyquist_fade > 0.0
        ? chip_.clock_leakage * nyquist_fade * std::abs(output_filter_.responseAtHz(clock_)) : 0.0;
  }

  double BbdLine::randomGaussian() {
    // Unit-variance sum of two uniforms. After the output filter averages
    // several buckets the hiss is Gaussian in effect, at half the cost.
    double sum = 0.0;
    for (int i = 0; i < 2; ++i) {
      random_ ^= random_ << 13;
      random_ ^= random_ >> 17;
      random_ ^= random_ << 5;
      sum += static_cast<double>(random_) * (1.0 / 4294967296.0);
    }
    return (sum - 1.0) * 2.4494897427831781;
  }

  double BbdLine::tick(double delta) {
    const double position = delta * kTableSize;
    const int index = std::min(static_cast<int>(position), kTableSize - 1);
    const double fraction = position - index;
    constexpr size_t kStride = kTableSize + 1;

    double sampled = 0.0;
    for (size_t i = 0; i < input_terms_.size(); ++i) {
      const Complex* g = &input_table_[i * kStride + index];
      const Complex gain = g[0] + fraction * (g[1] - g[0]);
      const Complex& x = input_terms_[i].state;
      sampled += gain.real() * x.real() - gain.imag() * x.imag();
    }

    const double headroom = chip_.headroom;
    double charge = headroom * bucketCurve(sampled / headroom);
    charge += chip_.even_distortion * charge * charge / headroom;

    double out;
    if (group_ == 1) {
      out = buckets_[write_];
      buckets_[write_] = charge;
      if (++write_ == static_cast<int>(buckets_.size()))
        write_ = 0;
    }
    else {
      out = 0.0;
      for (int i = 0; i < group_; ++i) {
        out += buckets_[write_];
        buckets_[write_] = charge;
        write_ = (write_ + 1) & bucket_mask_;
      }
      out /= group_;
    }
    transfer_state_ += (out - transfer_state_) * (1.0 - group_transfer_coefficient_);
    out = transfer_state_;
    if (chip_.noise > 0.0 && noise_scale_ > 0.0)
      out += chip_.noise * noise_scale_ * group_noise_ * randomGaussian();

    const double step = out - held_;
    held_ = out;
    for (size_t i = 0; i < output_terms_.size(); ++i) {
      const Complex* h = &output_table_[i * kStride + index];
      output_terms_[i].state += step * (h[0] + fraction * (h[1] - h[0]));
    }
    return out;
  }

  double BbdLine::process(double input) {
    if (target_clock_ != clock_) {
      clock_ = target_clock_;
      updateGroup();
    }
    if (refresh_counter_-- <= 0) {
      refresh();
      refresh_counter_ = kRefreshInterval - 1;
    }

    for (auto& term : output_terms_)
      term.state *= term.pole_step;
    while (next_tick_ < 1.0) {
      tick(next_tick_);
      next_tick_ += samples_per_tick_;
    }
    next_tick_ -= 1.0;
    for (auto& term : input_terms_)
      term.state = term.pole_step * term.state + input;

    double out = output_dc_gain_ * held_;
    for (const auto& term : output_terms_)
      out += term.state.real();

    if (leakage_gain_ > 0.0 && leakage_scale_ > 0.0) {
      clock_phase_ += clock_ * period_;
      clock_phase_ -= std::floor(clock_phase_);
      out += leakage_gain_ * leakage_scale_ * std::sin(kTwoPi * clock_phase_);
    }
    return out;
  }
}
