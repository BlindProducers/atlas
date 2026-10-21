// SPDX-License-Identifier: GPL-3.0-or-later
#include "reverb_filter_core.h"
#include "reverb_filter_data.h"
#include <algorithm>
#include <cassert>
#include <cmath>

namespace vital {
namespace {
double reverbUnit(double x, double a, double b) { return std::clamp((x - a) / (b - a), 0.0, 1.0); }
int reverbSize(double rate, double ratio, int oversampling) {
  return (static_cast<int>(std::ceil(rate / oversampling * ratio / 8.0)) + 4) * oversampling;
}
double reverbClean(double x) { return std::abs(x) < 1.0e-20 ? 0.0 : x; }
// Eight running sums instead of one, so the multiply-adds don't wait on each
// other and the compiler can use SIMD (SSE2 on Windows, NEON on macOS alike:
// four registers of two doubles).
double dot(const double* a, const double* b, int count) {
  double sums[8] {};
  int i = 0;
  for (; i + 8 <= count; i += 8) {
    for (int j = 0; j < 8; ++j)
      sums[j] += a[i + j] * b[i + j];
  }
  for (; i < count; ++i)
    sums[0] += a[i] * b[i];
  return ((sums[0] + sums[1]) + (sums[2] + sums[3])) + ((sums[4] + sums[5]) + (sums[6] + sums[7]));
}
}
void ReverbFilterCore::reserve(double maximum_rate) {
  const int required = 16 * reverbSize(maximum_rate, 1.36, kMaximumOversampling) +
                        6 * reverbSize(maximum_rate, .42, kMaximumOversampling) +
                            reverbSize(maximum_rate, .55, kMaximumOversampling);
  if (required > capacity_) {
    // Deliberately uninitialised: valid_ guards every read. Unused models do
    // not touch megabytes of delay pages, and a note reset is constant time.
    memory_.reset(new float[required]);
    if (history_ == nullptr)
      history_.reset(new double[kFirTaps * kMaximumOversampling]);
    if (fir_coefficients_ == nullptr)
      fir_coefficients_.reset(new double[kFirTaps]);
    capacity_ = required;
    terms_.cutoff = -1;
    reset();
  }
}
void ReverbFilterCore::release() {
  memory_.reset();
  history_.reset();
  fir_coefficients_.reset();
  capacity_ = 0;
  terms_.cutoff = -1;
  reset();
}
void ReverbFilterCore::setSampleRate(double rate, int oversampling) {
  assert(oversampling >= 1 && oversampling <= kMaximumOversampling);
  const int main = reverbSize(rate, 1.36, oversampling), ap = reverbSize(rate, .42, oversampling),
            low = reverbSize(rate, .55, oversampling);
  assert(capacity_ >= 16 * main + 6 * ap + low);
  if (rate_ == rate && main_size_ == main && oversampling_ == oversampling) return;
  rate_ = rate;
  oversampling_ = oversampling;
  fir_size_ = kFirTaps * oversampling;
  main_size_ = main; ap_size_ = ap; low_size_ = low;
  ap_base_ = 16 * main; low_base_ = ap_base_ + 6 * ap;
  terms_.cutoff = -1;
  reset();
}
void ReverbFilterCore::reset() {
  valid_ = fdn_pos_ = ap_pos_ = low_pos_ = fir_phase_ = fir_newest_ = 0;
  for (auto& phase : damping_) phase.fill(0);
  damp_ = boost_ = 1;
  fresh_ = true;
}
void ReverbFilterCore::beginBlock(double resonance, double damp, int samples) {
  resonance = std::clamp(resonance, 0.0, 1.0);
  damp = std::clamp(damp, 0.0, 1.0);
  const double inverse = 1.0 / std::max(1, samples);
  for (int i = 0; i < 16; ++i) {
    const double target = std::exp(.11512925464970229 * (-11 + 10.92 * resonance) * reverb_filter_data::ratio[i]);
    if (fresh_) gains_[i] = target;
    gain_steps_[i] = (target - gains_[i]) * inverse;
  }
  const double damp_hz = 20000 * std::exp(std::log(500.0 / 20000) * damp);
  const double host_rate = rate_ / oversampling_;
  const double target_damp = damp <= .000001 ? 1 : 1 - std::exp(-6.283185307179586 * damp_hz / host_rate);
  damp_step_ = (target_damp - damp_) * inverse;
  boost_step_ = (std::exp(2 * resonance / 8.68588963806504) - boost_) * inverse;
  fresh_ = false;
}
int ReverbFilterCore::delayOffset(int size, double delay) const {
  // Rounding at the processing rate changes the resonant modes. Round on the
  // JSFX/host sample grid first, then express that delay in processing samples.
  return static_cast<int>(std::floor(std::clamp(delay / oversampling_,
      1.0, double(size / oversampling_ - 2)) + .5)) * oversampling_;
}
double ReverbFilterCore::read(int base, int position, int size, int offset) const {
  if (offset > valid_) return 0;
  int index = position - offset;
  if (index < 0) index += size;
  return memory_[base + index];
}
double ReverbFilterCore::allpass(double input, int stage, int offset, double coefficient) {
  const int base = ap_base_ + stage * ap_size_;
  const double output = read(base, ap_pos_, ap_size_, offset) - coefficient * input;
  memory_[base + ap_pos_] = static_cast<float>(reverbClean(input + coefficient * output));
  return output;
}
void ReverbFilterCore::updateCutoffTerms(double cutoff_hz) {
  using namespace reverb_filter_data;
  CutoffTerms& terms = terms_;
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
  for (int i = 0; i < 16; ++i)
    terms.network_offsets[i] = delayOffset(main_size_, ratio[i] * scale);

  terms.fir_taps = 0;
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
      fir_coefficients_[kFirTaps - 1 - i] = kernels[a][i] * (1 - t) + (i < sizes[b] ? kernels[b][i] * t : 0);
  }
  terms.low_amount = 1 - reverbUnit(log_cut, std::log(8.0), std::log(50.0));
  if (terms.low_amount > 0) {
    for (int i = 0; i < kLowTaps; ++i)
      terms.low_offsets[i] = delayOffset(low_size_, low_ratio[i] * rate_ / cutoff_hz);
  }
  double compensation = 0;
  if (cutoff_hz <= 200) compensation = .59;
  else if (cutoff_hz < 400) compensation = .59 + reverbUnit(log_cut, std::log(200.0), std::log(400.0)) * (.14 - .59);
  else if (cutoff_hz < 800) compensation = .14 * (1 - reverbUnit(log_cut, std::log(400.0), std::log(800.0)));
  terms.gain = std::exp(compensation / 8.68588963806504);
}
double ReverbFilterCore::tick(double input, double cutoff_hz) {
  using namespace reverb_filter_data;
  cutoff_hz = std::clamp(cutoff_hz, 8.0, 20000.0);
  if (cutoff_hz != terms_.cutoff)
    updateCutoffTerms(cutoff_hz);
  const CutoffTerms& terms = terms_;
  damp_ += damp_step_; boost_ += boost_step_;
  double diffused = allpass(input, 0, terms.allpass_offsets[0], .82);
  diffused = allpass(diffused, 1, terms.allpass_offsets[1], .82);
  diffused = allpass(diffused, 2, terms.allpass_offsets[2], .82);
  double sum = 0, wet = 0;
  auto& damping = damping_[fir_phase_];
  for (int i = 0; i < 16; ++i) {
    const double value = read(i * main_size_, fdn_pos_, main_size_, terms.network_offsets[i]);
    damping[i] += damp_ * (value - damping[i]);
    sum += damping[i];
    wet += (i < 8 ? -1 : 1) * value;
  }
  for (int i = 0; i < 16; ++i) {
    gains_[i] += gain_steps_[i];
    const double stored = input_sign[i] * diffused * .25 + gains_[i] * (damping[i] - sum * .125);
    memory_[i * main_size_ + fdn_pos_] = static_cast<float>(reverbClean(stored));
  }
  wet *= 2.2844642888941586 * .25;
  wet = allpass(wet, 3, terms.allpass_offsets[3], .5);
  wet = allpass(wet, 4, terms.allpass_offsets[4], .5);
  wet = allpass(wet, 5, terms.allpass_offsets[5], .5);

  // Tap i reads the input i host samples back: the same sub-sample phase,
  // `newest - i` in that phase's ring. The window is at most two contiguous
  // runs (before and after the ring wraps), each a forward dot product with
  // the reversed kernel.
  const int newest = fir_newest_;
  double* history = history_.get() + fir_phase_ * kFirTaps;
  history[newest] = input;
  if (terms.fir_taps > 0) {
    const double* reversed = fir_coefficients_.get();
    const int count = std::min(terms.fir_taps, valid_ / oversampling_ + 1);
    const int unwrapped = std::min(count, newest + 1);
    double correction = dot(reversed + kFirTaps - unwrapped, history + newest - unwrapped + 1, unwrapped);
    if (count > unwrapped)
      correction += dot(reversed + kFirTaps - count, history + newest - count + 1 + kFirTaps, count - unwrapped);
    wet += terms.fir_amount * correction;
  }
  memory_[low_base_ + low_pos_] = static_cast<float>(input);
  if (terms.low_amount > 0) {
    double low = 0;
    for (int i = 0; i < kLowTaps; ++i)
      low += low_coeff[i] * read(low_base_, low_pos_, low_size_, terms.low_offsets[i]);
    wet += terms.low_amount * low;
  }
  if (++fdn_pos_ == main_size_) fdn_pos_ = 0;
  if (++ap_pos_ == ap_size_) ap_pos_ = 0;
  if (++low_pos_ == low_size_) low_pos_ = 0;
  if (++fir_phase_ == oversampling_) {
    fir_phase_ = 0;
    if (++fir_newest_ == kFirTaps) fir_newest_ = 0;
  }
  valid_ = std::min(valid_ + 1, std::max(fir_size_, main_size_));
  return wet * terms.gain * boost_;
}
}
