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
      history_.reset(new double[384 * kMaximumOversampling]);
    capacity_ = required;
    reset();
  }
}
void ReverbFilterCore::release() {
  memory_.reset();
  history_.reset();
  capacity_ = 0;
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
  fir_size_ = 384 * oversampling;
  main_size_ = main; ap_size_ = ap; low_size_ = low;
  ap_base_ = 16 * main; low_base_ = ap_base_ + 6 * ap;
  reset();
}
void ReverbFilterCore::reset() {
  valid_ = fdn_pos_ = ap_pos_ = low_pos_ = fir_pos_ = 0;
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
double ReverbFilterCore::read(int base, int position, int size, double delay) const {
  // Rounding at the processing rate changes the resonant modes. Round on the
  // JSFX/host sample grid first, then express that delay in processing samples.
  const int offset = static_cast<int>(std::floor(std::clamp(delay / oversampling_,
      1.0, double(size / oversampling_ - 2)) + .5)) * oversampling_;
  if (offset > valid_) return 0;
  int index = position - offset;
  if (index < 0) index += size;
  return memory_[base + index];
}
double ReverbFilterCore::allpass(double input, int stage, double delay, double coefficient) {
  const int base = ap_base_ + stage * ap_size_;
  const double output = read(base, ap_pos_, ap_size_, delay) - coefficient * input;
  memory_[base + ap_pos_] = static_cast<float>(reverbClean(input + coefficient * output));
  return output;
}
double ReverbFilterCore::tick(double input, double cutoff_hz) {
  using namespace reverb_filter_data;
  cutoff_hz = std::clamp(cutoff_hz, 8.0, 20000.0);
  const double log_cut = std::log(cutoff_hz);
  const double network_log = cutoff_hz < 50 ? std::log(12.4) +
      reverbUnit(log_cut, std::log(8.0), std::log(50.0)) * (std::log(50.0) - std::log(12.4)) : log_cut;
  const double scale = rate_ / std::exp(network_log);
  damp_ += damp_step_; boost_ += boost_step_;
  double diffused = allpass(input, 0, .023 * scale, .82);
  diffused = allpass(diffused, 1, .037 * scale, .82);
  diffused = allpass(diffused, 2, .053 * scale, .82);
  double sum = 0, wet = 0;
  auto& damping = damping_[fir_pos_ % oversampling_];
  for (int i = 0; i < 16; ++i) {
    const double value = read(i * main_size_, fdn_pos_, main_size_, ratio[i] * scale);
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
  wet = allpass(wet, 3, .23 * scale, .5);
  wet = allpass(wet, 4, .31 * scale, .5);
  wet = allpass(wet, 5, .41 * scale, .5);

  history_[fir_pos_] = input;
  if (cutoff_hz > 300) {
    static constexpr double frequencies[] { 400, 800, 1600, 3200, 6400, 12800 };
    const double* kernels[] { fir400.data(), fir800.data(), fir1600.data(), fir3200.data(), fir6400.data(), fir12800.data() };
    static constexpr int sizes[] { 384, 320, 192, 128, 128, 128 };
    int a = 0;
    while (a < 5 && cutoff_hz >= frequencies[a + 1]) ++a;
    const int b = cutoff_hz < 400 ? 0 : std::min(5, a + 1);
    const double t = b == a ? 0 : reverbUnit(log_cut, std::log(frequencies[a]), std::log(frequencies[b]));
    const double amount = cutoff_hz < 400 ? reverbUnit(log_cut, std::log(300.0), std::log(400.0)) : 1;
    double correction = 0;
    const int count = std::min(sizes[a], valid_ / oversampling_ + 1);
    for (int i = 0; i < count; ++i) {
      const double coefficient = kernels[a][i] * (1 - t) + (i < sizes[b] ? kernels[b][i] * t : 0);
      int index = fir_pos_ - i * oversampling_;
      if (index < 0) index += fir_size_;
      correction += coefficient * history_[index];
    }
    wet += amount * correction;
  }
  memory_[low_base_ + low_pos_] = static_cast<float>(input);
  const double low_amount = 1 - reverbUnit(log_cut, std::log(8.0), std::log(50.0));
  if (low_amount > 0) {
    double low = 0;
    for (int i = 0; i < 32; ++i)
      low += low_coeff[i] * read(low_base_, low_pos_, low_size_, low_ratio[i] * rate_ / cutoff_hz);
    wet += low_amount * low;
  }
  double compensation = 0;
  if (cutoff_hz <= 200) compensation = .59;
  else if (cutoff_hz < 400) compensation = .59 + reverbUnit(log_cut, std::log(200.0), std::log(400.0)) * (.14 - .59);
  else if (cutoff_hz < 800) compensation = .14 * (1 - reverbUnit(log_cut, std::log(400.0), std::log(800.0)));
  if (++fdn_pos_ == main_size_) fdn_pos_ = 0;
  if (++ap_pos_ == ap_size_) ap_pos_ = 0;
  if (++low_pos_ == low_size_) low_pos_ = 0;
  if (++fir_pos_ == fir_size_) fir_pos_ = 0;
  valid_ = std::min(valid_ + 1, std::max(fir_size_, main_size_));
  return wet * std::exp(compensation / 8.68588963806504) * boost_;
}
}
