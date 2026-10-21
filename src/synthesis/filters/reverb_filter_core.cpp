// SPDX-License-Identifier: GPL-3.0-or-later
#include "reverb_filter_core.h"
#include "reverb_filter_data.h"
#include "double_pair.h"
#include <algorithm>
#include <cassert>
#include <cmath>

namespace vital {
namespace {
double reverbUnit(double x, double a, double b) { return std::clamp((x - a) / (b - a), 0.0, 1.0); }
int reverbSize(double rate, double ratio, int oversampling) {
  return (static_cast<int>(std::ceil(rate / oversampling * ratio / 8.0)) + 4) * oversampling;
}
force_inline double_pair reverbClean(double_pair x) {
  return select(lessThan(abs(x), double_pair::splat(1.0e-20)), double_pair::splat(0.0), x);
}
force_inline bool zeroBits(double x) { return x == 0.0 && !std::signbit(x); }
// Eight running sums instead of one, so the multiply-adds don't wait on each
// other, over taps `stride` doubles apart: one lane's...
double dot(const double* a, const double* b, int count, int stride) {
  double sums[8] {};
  int i = 0;
  for (; i + 8 <= count; i += 8) {
    for (int j = 0; j < 8; ++j)
      sums[j] += a[(i + j) * stride] * b[(i + j) * stride];
  }
  for (; i < count; ++i)
    sums[0] += a[i * stride] * b[i * stride];
  return ((sums[0] + sums[1]) + (sums[2] + sums[3])) + ((sums[4] + sums[5]) + (sums[6] + sums[7]));
}
// ...and two adjacent lanes' at once, with the same sums for each.
force_inline double_pair dotPair(const double* a, const double* b, int count, int stride) {
  double_pair sums[8];
  for (auto& sum : sums) sum = double_pair::splat(0.0);
  int i = 0;
  for (; i + 8 <= count; i += 8) {
    for (int j = 0; j < 8; ++j)
      sums[j] = sums[j] + double_pair::load(a + (i + j) * stride) * double_pair::load(b + (i + j) * stride);
  }
  for (; i < count; ++i)
    sums[0] = sums[0] + double_pair::load(a + i * stride) * double_pair::load(b + i * stride);
  return ((sums[0] + sums[1]) + (sums[2] + sums[3])) + ((sums[4] + sums[5]) + (sums[6] + sums[7]));
}
}

// The pairs one tick processes, and the bits of every input, delay read and
// damping state each pair saw (zero bits throughout mean the pair was silent).
struct ReverbFilterCore::Active {
  bool pair[kLanes / 2] {};
  bool all = false;           // both pairs, with offsets in common, valid as far back as min_valid
  int min_valid = 0;
  double_pair activity[kLanes / 2];
};

void ReverbFilterCore::reserve(double maximum_rate) {
  const int required = kLanes * (kLines * reverbSize(maximum_rate, 1.36, kMaximumOversampling) +
                                 6 * reverbSize(maximum_rate, .42, kMaximumOversampling) +
                                     reverbSize(maximum_rate, .55, kMaximumOversampling));
  if (required > capacity_) {
    // Deliberately uninitialised: valid_ guards every read. Unused models do
    // not touch megabytes of delay pages, and a note reset is constant time.
    memory_.reset(new float[required]);
    if (history_ == nullptr)
      history_.reset(new double[kFirTaps * kMaximumOversampling * kLanes]);
    if (fir_coefficients_ == nullptr)
      fir_coefficients_.reset(new double[kFirTaps * kLanes]);
    capacity_ = required;
    for (auto& terms : terms_) terms.cutoff = -1;
    reset();
  }
}
void ReverbFilterCore::release() {
  memory_.reset();
  history_.reset();
  fir_coefficients_.reset();
  capacity_ = 0;
  for (auto& terms : terms_) terms.cutoff = -1;
  reset();
}
void ReverbFilterCore::setSampleRate(double rate, int oversampling) {
  assert(oversampling >= 1 && oversampling <= kMaximumOversampling);
  const int main = reverbSize(rate, 1.36, oversampling), ap = reverbSize(rate, .42, oversampling),
            low = reverbSize(rate, .55, oversampling);
  assert(capacity_ >= kLanes * (kLines * main + 6 * ap + low));
  if (rate_ == rate && main_size_ == main && oversampling_ == oversampling) return;
  rate_ = rate;
  oversampling_ = oversampling;
  fir_size_ = kFirTaps * oversampling;
  main_size_ = main; ap_size_ = ap; low_size_ = low;
  ap_base_ = kLines * main; low_base_ = ap_base_ + 6 * ap;
  for (auto& terms : terms_) terms.cutoff = -1;
  reset();
}
void ReverbFilterCore::setPairs(int pairs) {
  pairs = std::clamp(pairs, 1, kLanes / 2);
  if (pairs == pairs_) return;
  pairs_ = pairs;
  reset();
}
void ReverbFilterCore::reset() {
  fdn_pos_ = ap_pos_ = low_pos_ = fir_phase_ = fir_newest_ = 0;
  for (int lane = 0; lane < kLanes; ++lane) resetLane(lane);
}
void ReverbFilterCore::resetLane(int lane) {
  valid_[lane] = 0;
  silent_[lane] = 0;
  for (auto& phase : damping_)
    for (auto& line : phase) line[lane] = 0;
  damp_[lane] = boost_[lane] = 1;
  fresh_[lane] = true;
}
void ReverbFilterCore::beginBlock(int lane, double resonance, double damp, int samples) {
  resonance = std::clamp(resonance, 0.0, 1.0);
  damp = std::clamp(damp, 0.0, 1.0);
  const double inverse = 1.0 / std::max(1, samples);
  for (int i = 0; i < kLines; ++i) {
    const double target = std::exp(.11512925464970229 * (-11 + 10.92 * resonance) * reverb_filter_data::ratio[i]);
    if (fresh_[lane]) gains_[i][lane] = target;
    gain_steps_[i][lane] = (target - gains_[i][lane]) * inverse;
  }
  gains_checked_ = false;
  const double damp_hz = 20000 * std::exp(std::log(500.0 / 20000) * damp);
  const double host_rate = rate_ / oversampling_;
  const double target_damp = damp <= .000001 ? 1 : 1 - std::exp(-6.283185307179586 * damp_hz / host_rate);
  damp_step_[lane] = (target_damp - damp_[lane]) * inverse;
  boost_step_[lane] = (std::exp(2 * resonance / 8.68588963806504) - boost_[lane]) * inverse;
  fresh_[lane] = false;
}
int ReverbFilterCore::delayOffset(int size, double delay) const {
  // Rounding at the processing rate changes the resonant modes. Round on the
  // JSFX/host sample grid first, then express that delay in processing samples.
  return static_cast<int>(std::floor(std::clamp(delay / oversampling_,
      1.0, double(size / oversampling_ - 2)) + .5)) * oversampling_;
}
float* ReverbFilterCore::at(int base, int position, int size, int offset) const {
  int index = position - offset;
  if (index < 0) index += size;
  return memory_.get() + (base + index) * kLanes;
}
// The active lanes of a delay line `offset(lane)` samples back, zero for a lane
// that has not written that far since its reset.
template<typename Offset>
force_inline void ReverbFilterCore::readLanes(const Active& active, int base, int position, int size, Offset offset,
                                              double* values) const {
  const int first = offset(0);
  if (active.all && first <= active.min_valid) {
    double_pair low, high;
    double_pair::loadFloats(at(base, position, size, first), low, high);
    low.store(values);
    high.store(values + 2);
    return;
  }
  for (int pair = 0; pair < kLanes / 2; ++pair) {
    if (!active.pair[pair]) continue;
    const int a = 2 * pair, b = a + 1;
    const int offset_a = offset(a);
    if (pair_uniform_[pair]) {
      if (offset_a > std::max(valid_[a], valid_[b])) {
        values[a] = values[b] = 0;
        continue;
      }
      if (offset_a <= std::min(valid_[a], valid_[b])) {
        double_pair::loadFloats(at(base, position, size, offset_a) + a).store(values + a);
        continue;
      }
    }
    for (int lane : { a, b }) {
      const int lane_offset = offset(lane);
      values[lane] = lane_offset > valid_[lane] ? 0 : at(base, position, size, lane_offset)[lane];
    }
  }
}
void ReverbFilterCore::allpass(const Active& active, double* values, int stage, double coefficient) {
  const int base = ap_base_ + stage * ap_size_;
  alignas(16) double delayed[kLanes];
  readLanes(active, base, ap_pos_, ap_size_, [&](int lane) { return terms_[lane].allpass_offsets[stage]; }, delayed);
  const double_pair c = double_pair::splat(coefficient);
  double_pair stored[2] { double_pair::splat(0.0), double_pair::splat(0.0) };
  for (int pair = 0; pair < 2; ++pair) {
    if (!active.pair[pair]) continue;
    const double_pair input = double_pair::load(values + 2 * pair);
    const double_pair read = double_pair::load(delayed + 2 * pair);
    const double_pair output = read - c * input;
    stored[pair] = reverbClean(input + c * output);
    output.store(values + 2 * pair);
    const_cast<Active&>(active).activity[pair] = bitOr(active.activity[pair], read);
  }
  double_pair::storeFloats(memory_.get() + (base + ap_pos_) * kLanes, stored[0], stored[1]);
}
double ReverbFilterCore::fir(int lane, int count) const {
  const double* reversed = fir_coefficients_.get() + lane;
  const double* history = history_.get() + fir_phase_ * kFirTaps * kLanes + lane;
  const int newest = fir_newest_;
  const int unwrapped = std::min(count, newest + 1);
  double correction = dot(reversed + (kFirTaps - unwrapped) * kLanes, history + (newest - unwrapped + 1) * kLanes,
                          unwrapped, kLanes);
  if (count > unwrapped)
    correction += dot(reversed + (kFirTaps - count) * kLanes, history + (newest - count + 1 + kFirTaps) * kLanes,
                      count - unwrapped, kLanes);
  return correction;
}
void ReverbFilterCore::updateCutoffTerms(int lane, double cutoff_hz) {
  using namespace reverb_filter_data;
  CutoffTerms& terms = terms_[lane];
  terms.cutoff = cutoff_hz;
  const double log_cut = std::log(cutoff_hz);
  terms.log_cut = log_cut;
  const double network_log = cutoff_hz < 50 ? std::log(12.4) +
      reverbUnit(log_cut, std::log(8.0), std::log(50.0)) * (std::log(50.0) - std::log(12.4)) : log_cut;
  const double scale = rate_ / std::exp(network_log);
  terms.allpass_offsets[0] = delayOffset(ap_size_, .023 * scale);
  terms.allpass_offsets[1] = delayOffset(ap_size_, .037 * scale);
  terms.allpass_offsets[2] = delayOffset(ap_size_, .053 * scale);
  terms.allpass_offsets[3] = delayOffset(ap_size_, .23 * scale);
  terms.allpass_offsets[4] = delayOffset(ap_size_, .31 * scale);
  terms.allpass_offsets[5] = delayOffset(ap_size_, .41 * scale);
  for (int i = 0; i < kLines; ++i)
    terms.network_offsets[i] = delayOffset(main_size_, ratio[i] * scale);

  terms.fir_taps = 0;
  terms.fir_amount = 0;
  if (cutoff_hz > 300) {
    static constexpr double frequencies[] { 400, 800, 1600, 3200, 6400, 12800 };
    const double* kernels[] { fir400.data(), fir800.data(), fir1600.data(), fir3200.data(), fir6400.data(), fir12800.data() };
    static constexpr int sizes[] { 384, 320, 192, 128, 128, 128 };
    int a = 0;
    while (a < 5 && cutoff_hz >= frequencies[a + 1]) ++a;
    const int b = cutoff_hz < 400 ? 0 : std::min(5, a + 1);
    const double t = b == a ? 0 : reverbUnit(log_cut, std::log(frequencies[a]), std::log(frequencies[b]));
    terms.fir_amount = cutoff_hz < 400 ? reverbUnit(log_cut, std::log(300.0), std::log(400.0)) : 1;
    terms.fir_taps = sizes[a];
    for (int i = 0; i < sizes[a]; ++i)
      fir_coefficients_[(kFirTaps - 1 - i) * kLanes + lane] = kernels[a][i] * (1 - t) + (i < sizes[b] ? kernels[b][i] * t : 0);
  }
  terms.low_amount = 1 - reverbUnit(log_cut, std::log(8.0), std::log(50.0));
  // Worked out at every cutoff (only read below 50 Hz), so lanes that agree
  // on the cutoff agree on every offset.
  for (int i = 0; i < kLowTaps; ++i)
    terms.low_offsets[i] = delayOffset(low_size_, low_ratio[i] * rate_ / cutoff_hz);
  double compensation = 0;
  if (cutoff_hz <= 200) compensation = .59;
  else if (cutoff_hz < 400) compensation = .59 + reverbUnit(log_cut, std::log(200.0), std::log(400.0)) * (.14 - .59);
  else if (cutoff_hz < 800) compensation = .14 * (1 - reverbUnit(log_cut, std::log(400.0), std::log(800.0)));
  terms.gain = std::exp(compensation / 8.68588963806504);
  gain_[lane] = terms.gain;
  fir_amount_[lane] = terms.fir_amount;
  low_amount_[lane] = terms.low_amount;
}
void ReverbFilterCore::compareLanes() {
  auto same = [this](int a, int b) {
    const CutoffTerms &one = terms_[a], &two = terms_[b];
    return one.allpass_offsets == two.allpass_offsets && one.network_offsets == two.network_offsets &&
           one.low_offsets == two.low_offsets;
  };
  pair_uniform_[0] = same(0, 1);
  pair_uniform_[1] = same(2, 3);
  uniform_ = pair_uniform_[0] && pair_uniform_[1] && same(0, 2);
}
void ReverbFilterCore::tick(const double* input, const double* cutoff_hz, double* output) {
  using namespace reverb_filter_data;
  const int lanes = 2 * pairs_;
  bool changed = false;
  for (int lane = 0; lane < lanes; ++lane) {
    const double cutoff = std::clamp(cutoff_hz[lane], 8.0, 20000.0);
    if (cutoff != terms_[lane].cutoff) {
      updateCutoffTerms(lane, cutoff);
      changed = true;
    }
  }
  if (changed) compareLanes();
  if (!gains_checked_) {
    for (int pair = 0; pair < kLanes / 2; ++pair) {
      gains_moving_[pair] = false;
      for (int i = 0; i < kLines; ++i)
        gains_moving_[pair] = gains_moving_[pair] || gain_steps_[i][2 * pair] != 0 || gain_steps_[i][2 * pair + 1] != 0;
    }
    gains_checked_ = true;
  }

  // A lane is idle once it has been silent for longer than any delay; its
  // pair runs unless both lanes are idle and stay silent.
  const int cap = std::max(fir_size_, main_size_);
  Active active;
  for (int pair = 0; pair < pairs_; ++pair) {
    for (int lane : { 2 * pair, 2 * pair + 1 }) {
      if (!zeroBits(input[lane])) silent_[lane] = 0;
      active.pair[pair] = active.pair[pair] || silent_[lane] < cap;
    }
    active.activity[pair] = double_pair::load(input + 2 * pair);
  }
  // (An idle pair's lanes are as valid as ever; the one load serves both.)
  active.all = pairs_ == 2 && uniform_;
  active.min_valid = std::min(std::min(valid_[0], valid_[1]), std::min(valid_[2], valid_[3]));

  // The parameter ramps run in every lane, idle or not, so a lane that wakes
  // up continues them exactly.
  double_pair damp[2];
  for (int pair = 0; pair < pairs_; ++pair) {
    damp[pair] = double_pair::load(damp_ + 2 * pair) + double_pair::load(damp_step_ + 2 * pair);
    damp[pair].store(damp_ + 2 * pair);
    (double_pair::load(boost_ + 2 * pair) + double_pair::load(boost_step_ + 2 * pair)).store(boost_ + 2 * pair);
    if (gains_moving_[pair]) {
      for (int i = 0; i < kLines; ++i)
        (double_pair::load(gains_[i] + 2 * pair) + double_pair::load(gain_steps_[i] + 2 * pair)).store(gains_[i] + 2 * pair);
    }
  }

  alignas(16) double diffused[kLanes];
  std::copy_n(input, kLanes, diffused);
  allpass(active, diffused, 0, .82);
  allpass(active, diffused, 1, .82);
  allpass(active, diffused, 2, .82);

  double_pair sum[2], wet[2];
  sum[0] = sum[1] = wet[0] = wet[1] = double_pair::splat(0.0);
  auto& damping = damping_[fir_phase_];
  for (int i = 0; i < kLines; ++i) {
    alignas(16) double values[kLanes];
    readLanes(active, i * main_size_, fdn_pos_, main_size_,
              [&](int lane) { return terms_[lane].network_offsets[i]; }, values);
    for (int pair = 0; pair < 2; ++pair) {
      if (!active.pair[pair]) continue;
      const double_pair value = double_pair::load(values + 2 * pair);
      double_pair state = double_pair::load(damping[i] + 2 * pair);
      state = state + damp[pair] * (value - state);
      state.store(damping[i] + 2 * pair);
      sum[pair] = sum[pair] + state;
      wet[pair] = i < 8 ? wet[pair] - value : wet[pair] + value;
      active.activity[pair] = bitOr(active.activity[pair], bitOr(value, state));
    }
  }
  // Each line stores input_sign[i] * diffused * .25 + gain * (damping - sum * .125).
  // input_sign is +-1, so its two products and the sum's eighth are worked
  // out once rather than per line.
  const double_pair quarter = double_pair::splat(.25), eighth = double_pair::splat(.125), zero = double_pair::splat(0.0);
  double_pair excitation[2][2], mean[2];
  for (int pair = 0; pair < 2; ++pair) {
    const double_pair input_pair = double_pair::load(diffused + 2 * pair);
    excitation[pair][0] = double_pair::splat(-1.0) * input_pair * quarter;
    excitation[pair][1] = double_pair::splat(1.0) * input_pair * quarter;
    mean[pair] = sum[pair] * eighth;
  }
  for (int i = 0; i < kLines; ++i) {
    const int positive = input_sign[i] > 0;
    double_pair stored[2] { zero, zero };
    for (int pair = 0; pair < 2; ++pair) {
      if (!active.pair[pair]) continue;
      const double_pair gain = double_pair::load(gains_[i] + 2 * pair);
      const double_pair state = double_pair::load(damping[i] + 2 * pair);
      stored[pair] = reverbClean(excitation[pair][positive] + gain * (state - mean[pair]));
    }
    double_pair::storeFloats(memory_.get() + (i * main_size_ + fdn_pos_) * kLanes, stored[0], stored[1]);
  }
  alignas(16) double out[kLanes] {};
  const double_pair wet_scale = double_pair::splat(2.2844642888941586 * .25);
  for (int pair = 0; pair < 2; ++pair) {
    if (active.pair[pair]) (wet[pair] * wet_scale).store(out + 2 * pair);
  }
  allpass(active, out, 3, .5);
  allpass(active, out, 4, .5);
  allpass(active, out, 5, .5);

  // Tap i reads the input i host samples back: the same sub-sample phase,
  // `newest - i` in that phase's ring. The window is at most two contiguous
  // runs (before and after the ring wraps), each a forward dot product with
  // the reversed kernel.
  const int newest = fir_newest_;
  std::copy_n(input, kLanes, history_.get() + (fir_phase_ * kFirTaps + newest) * kLanes);
  for (int pair = 0; pair < 2; ++pair) {
    if (!active.pair[pair]) continue;
    const int a = 2 * pair, b = a + 1;
    const int taps_a = terms_[a].fir_taps, taps_b = terms_[b].fir_taps;
    const int count_a = std::min(taps_a, valid_[a] / oversampling_ + 1);
    const int count_b = std::min(taps_b, valid_[b] / oversampling_ + 1);
    if (taps_a > 0 && taps_a == taps_b && count_a == count_b) {
      const double* reversed = fir_coefficients_.get() + a;
      const double* ring = history_.get() + fir_phase_ * kFirTaps * kLanes + a;
      const int unwrapped = std::min(count_a, newest + 1);
      double_pair correction = dotPair(reversed + (kFirTaps - unwrapped) * kLanes,
                                       ring + (newest - unwrapped + 1) * kLanes, unwrapped, kLanes);
      if (count_a > unwrapped)
        correction = correction + dotPair(reversed + (kFirTaps - count_a) * kLanes,
                                          ring + (newest - count_a + 1 + kFirTaps) * kLanes, count_a - unwrapped, kLanes);
      (double_pair::load(out + a) + double_pair::load(fir_amount_ + a) * correction).store(out + a);
    }
    else {
      if (taps_a > 0) out[a] += fir_amount_[a] * fir(a, count_a);
      if (taps_b > 0) out[b] += fir_amount_[b] * fir(b, count_b);
    }
  }
  double_pair::storeFloats(memory_.get() + (low_base_ + low_pos_) * kLanes,
                           double_pair::load(input), double_pair::load(input + 2));
  bool low_needed = false;
  for (int lane = 0; lane < lanes; ++lane) low_needed = low_needed || (active.pair[lane / 2] && low_amount_[lane] > 0);
  if (low_needed) {
    double_pair low[2] { double_pair::splat(0.0), double_pair::splat(0.0) };
    for (int i = 0; i < kLowTaps; ++i) {
      alignas(16) double values[kLanes];
      readLanes(active, low_base_, low_pos_, low_size_, [&](int lane) { return terms_[lane].low_offsets[i]; }, values);
      const double_pair coefficient = double_pair::splat(low_coeff[i]);
      for (int pair = 0; pair < 2; ++pair) {
        if (active.pair[pair]) low[pair] = low[pair] + coefficient * double_pair::load(values + 2 * pair);
      }
    }
    const double_pair zero = double_pair::splat(0.0);
    for (int pair = 0; pair < 2; ++pair) {
      if (!active.pair[pair]) continue;
      const double_pair amount = double_pair::load(low_amount_ + 2 * pair);
      const double_pair current = double_pair::load(out + 2 * pair);
      select(greaterThan(amount, zero), current + amount * low[pair], current).store(out + 2 * pair);
    }
  }
  for (int pair = 0; pair < 2; ++pair) {
    if (active.pair[pair])
      (double_pair::load(out + 2 * pair) * double_pair::load(gain_ + 2 * pair) * double_pair::load(boost_ + 2 * pair))
          .store(output + 2 * pair);
    else
      output[2 * pair] = output[2 * pair + 1] = 0;
  }
  if (pairs_ == 1) {
    output[2] = output[0];
    output[3] = output[1];
  }

  if (++fdn_pos_ == main_size_) fdn_pos_ = 0;
  if (++ap_pos_ == ap_size_) ap_pos_ = 0;
  if (++low_pos_ == low_size_) low_pos_ = 0;
  if (++fir_phase_ == oversampling_) {
    fir_phase_ = 0;
    if (++fir_newest_ == kFirTaps) fir_newest_ = 0;
  }
  for (int pair = 0; pair < pairs_; ++pair) {
    const int a = 2 * pair, b = a + 1;
    // Silent so far: count on. Once both lanes reach the longest delay, every
    // delay slot and FIR tap they can read holds zero, and while skipped they
    // keep writing zeros, so they wake to the history they would have had.
    if (active.pair[pair]) {
      silent_[a] = active.activity[pair].firstIsZeroBits() ? std::min(silent_[a] + 1, cap) : 0;
      silent_[b] = active.activity[pair].secondIsZeroBits() ? std::min(silent_[b] + 1, cap) : 0;
    }
    valid_[a] = std::min(valid_[a] + 1, cap);
    valid_[b] = std::min(valid_[b] + 1, cap);
  }
}
}
