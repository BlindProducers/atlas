#include "jsfx_ports.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace vital::jsfx {
  namespace {
    constexpr double pi = 3.1415926535897932384626433832795;
    constexpr double twopi = 2 * pi;
    const std::array<std::array<double, 24>, 1025>& sincTable() {
      static const auto table = [] {
        std::array<std::array<double, 24>, 1025> result {};
        for (int row = 0; row <= 1024; ++row) {
          double sum = 0;
          for (int tap = 0; tap < 24; ++tap) {
            const double x = row / 1024.0 - (tap - 11);
            const double sinc = std::abs(x) < 1.0e-12 ? 1 : std::sin(pi * x) / (pi * x);
            const double wx = x / 12;
            const double window = std::abs(wx) < 1.0e-12 ? 1 : std::sin(pi * wx) / (pi * wx);
            result[row][tap] = std::abs(x) >= 12 ? 0 : sinc * window;
            sum += result[row][tap];
          }
          for (auto& weight : result[row]) weight /= sum;
        }
        return result;
      }();
      return table;
    }
  }

  void HostRate::prepare(int factor) {
    factor_ = std::clamp(factor, 1, kMaxFactor);
    if (factor_ > 1) {
      const int length = kSpan * factor_;
      const double cutoff = 0.475 / factor_;
      for (int i = 0; i <= length; ++i) {
        const double x = i - length / 2;
        const double window = 0.42 - 0.5 * std::cos(twopi * i / length) +
                              0.08 * std::cos(2 * twopi * i / length);
        taps_[i] = window * (x == 0 ? 2 * cutoff : std::sin(twopi * cutoff * x) / (pi * x));
      }
      // Each interpolation phase has unity DC gain. Their sum also gives
      // the decimator unity gain, without a phase-dependent level ripple.
      for (int phase = 0; phase < factor_; ++phase) {
        double sum = 0;
        for (int tap = phase; tap <= length; tap += factor_) sum += taps_[tap];
        for (int tap = phase; tap <= length; tap += factor_) taps_[tap] /= sum * factor_;
      }
    }
    reset();
  }
  void HostRate::reset() {
    input_.fill({}); output_.fill({});
    phase_ = input_position_ = 0; output_position_ = kSpan;
  }

  void Memory::prepare(size_t size) {
    if (size == size_) { clear(); return; }
    // No value-initialisation of these large reservations. The small epoch
    // table and validity bits guarantee we never read indeterminate storage.
    auto data = std::unique_ptr<double[]>(new double[size]);
    const size_t pages = (size + kPage - 1) / kPage;
    auto written = std::unique_ptr<uint64_t[]>(new uint64_t[pages * (kPage / 64)]);
    epochs_.assign(pages, 0);
    data_ = std::move(data);
    written_ = std::move(written);
    size_ = size;
    epoch_ = 1;
  }
  void Memory::clear() {
    if (++epoch_ == 0) {
      std::fill(epochs_.begin(), epochs_.end(), 0);
      epoch_ = 1;
    }
  }
  double Memory::read(size_t index) const {
    if (index >= size_ || epochs_[index / kPage] != epoch_ ||
        !(written_[index / 64] & (uint64_t(1) << (index % 64)))) return 0;
    return data_[index];
  }
  void Memory::write(size_t index, double value) {
    if (index >= size_) return;
    const size_t page = index / kPage;
    if (epochs_[page] != epoch_) {
      std::fill_n(written_.get() + page * (kPage / 64), kPage / 64, uint64_t(0));
      epochs_[page] = epoch_;
    }
    data_[index] = value;
    written_[index / 64] |= uint64_t(1) << (index % 64);
  }

  Quorax::Quorax() { (void)sincTable(); }
  void Quorax::prepare(double rate) {
    if (sample_rate_ == rate) return;
    sample_rate_ = rate;
    length_ = static_cast<int>(std::ceil(rate * 0.230)) + 64;
    buffer_.prepare(2 * length_);
    size_coefficient_ = std::exp(-1 / (0.010 * rate));
    gain_coefficient_ = std::exp(-1 / (0.005 * rate));
    reset();
  }
  void Quorax::reset() {
    buffer_.clear(); position_ = 0; phase_ = 0.123291835; initial_ = true;
    for (auto& filter : hp_) filter.clear();
    for (auto& filter : lp_) filter.clear();
  }
  double Quorax::Biquad::process(double input, int pair) {
    const double output = b0 * input + z1[pair];
    z1[pair] = b1 * input - a1 * output + z2[pair];
    z2[pair] = b2 * input - a2 * output;
    return output;
  }
  void Quorax::coefficients(std::array<Biquad, 4>& filters, double frequency, bool highpass) {
    const double omega = twopi * frequency / sample_rate_;
    for (int section = 0; section < sections_; ++section) {
      auto& f = filters[section];
      const double q = 1 / (2 * std::cos((2 * section + 1) * pi / (4 * sections_)));
      const double alpha = std::sin(omega) / (2 * q);
      const double a0 = 1 + alpha;
      const double cosine = std::cos(omega);
      f.b0 = (highpass ? 1 + cosine : 1 - cosine) * 0.5 / a0;
      f.b1 = (highpass ? -(1 + cosine) : 1 - cosine) / a0;
      f.b2 = f.b0;
      f.a1 = -2 * cosine / a0; f.a2 = (1 - alpha) / a0;
    }
  }
  void Quorax::setParameters(const Parameters& p) {
    const bool filters_changed = initial_ || p.low_cut != parameters_.low_cut ||
        p.high_cut != parameters_.high_cut || p.slope != parameters_.slope;
    parameters_ = p;
    sections_ = std::clamp(p.slope + 1, 1, 4);
    gain_target_ = std::pow(10.0, p.output / 20);
    if (initial_) { size_ = p.size; mix_ = p.mix / 100; gain_ = gain_target_; initial_ = false; }
    if (filters_changed) {
      coefficients(hp_, std::max(5.0, std::min(p.low_cut, sample_rate_ * 0.45)), true);
      coefficients(lp_, std::max(20.0, std::min(p.high_cut, sample_rate_ * 0.45)), false);
    }
  }
  double Quorax::readDelay(double delay, int pair) const {
    const int integer = static_cast<int>(std::floor(delay));
    const double table_position = (delay - integer) * 1024;
    const int row = static_cast<int>(std::floor(table_position));
    const double fraction = table_position - row;
    const auto& table = sincTable();
    double sum = 0;
    for (int tap = 0; tap < 24; ++tap) {
      int index = position_ - integer - (tap - 11);
      if (index < 0) index += length_;
      if (index >= length_) index -= length_;
      const double weight = table[row][tap] + (table[std::min(row + 1, 1024)][tap] - table[row][tap]) * fraction;
      sum += buffer_.read(pair * length_ + index) * weight;
    }
    return sum;
  }
  Frame Quorax::process(const Frame& input) {
    size_ = parameters_.size + size_coefficient_ * (size_ - parameters_.size);
    mix_ = parameters_.mix / 100 + gain_coefficient_ * (mix_ - parameters_.mix / 100);
    gain_ = gain_target_ + gain_coefficient_ * (gain_ - gain_target_);
    const double d1 = (15 + size_) * 0.001 * sample_rate_, d2 = d1 * 1.18, d3 = d2 * 1.18, d4 = d3 * 1.18;
    const double sine = std::sin(phase_), cosine = std::cos(phase_);
    Frame output;
    for (int pair = 0; pair < 2; ++pair) {
      buffer_.write(pair * length_ + position_, (input[2 * pair] + input[2 * pair + 1]) * 0.5);
      double side = 0.5 * (sine * (readDelay(d1, pair) - readDelay(d3, pair)) +
                           cosine * (readDelay(d2, pair) - readDelay(d4, pair)));
      if (parameters_.low_cut > 0)
        for (int section = 0; section < sections_; ++section) side = hp_[section].process(side, pair);
      if (parameters_.high_cut < 20000)
        for (int section = 0; section < sections_; ++section) side = lp_[section].process(side, pair);
      output[2 * pair] = (input[2 * pair] + side * mix_) * gain_;
      output[2 * pair + 1] = (input[2 * pair + 1] - side * mix_) * gain_;
    }
    if (parameters_.low_cut <= 0) for (auto& f : hp_) f.clear();
    if (parameters_.high_cut >= 20000) for (auto& f : lp_) f.clear();
    if (++position_ >= length_) position_ = 0;
    phase_ -= twopi * parameters_.motion / sample_rate_;
    if (phase_ < 0) phase_ += twopi;
    return output;
  }

  void Erosion::prepare(double rate) { sample_rate_ = rate; reset(); }
  void Erosion::reset() {
    buffer_.fill(Frame {}); x1_.fill(0); x2_.fill(0); y1_.fill(0); y2_.fill(0);
    position_ = 0; phase_ = 0; random_ = 0x853c49e6748fea9bull;
  }
  void Erosion::setParameters(const Parameters& p) {
    parameters_ = p;
    // Match the original RBJ octave-bandwidth expression. A sub-Nyquist clamp
    // only guards unsupported low sample rates / extreme modulation values.
    const double omega = twopi * std::min(p.frequency, sample_rate_ * 0.499) / sample_rate_;
    double sine = std::sin(omega);
    if (std::abs(sine) < 1.0e-12) sine = 0.00001;
    const double exponent = std::log(2.0) / 2 * p.width * omega / sine;
    const double alpha = sine * std::sinh(std::min(exponent, 700.0));
    b0_ = alpha / (1 + alpha); b2_ = -b0_;
    a1_ = -2 * std::cos(omega) / (1 + alpha); a2_ = (1 - alpha) / (1 + alpha);
  }
  double Erosion::noise() {
    random_ ^= random_ >> 12; random_ ^= random_ << 25; random_ ^= random_ >> 27;
    return static_cast<double>((random_ * 2685821657736338717ull) >> 11) * (2.0 / 9007199254740992.0) - 1;
  }
  double Erosion::read(double position, int lane) const {
    position -= std::floor(position / 4096) * 4096;
    const int i = static_cast<int>(std::floor(position));
    const double frac = position - i;
    const double y0 = buffer_[(i - 1) & 4095][lane], y1 = buffer_[i][lane];
    const double y2 = buffer_[(i + 1) & 4095][lane], y3 = buffer_[(i + 2) & 4095][lane];
    const double c1 = 0.5 * (y2 - y0), c2 = y0 - 2.5 * y1 + 2 * y2 - 0.5 * y3;
    const double c3 = 0.5 * (y3 - y0) + 1.5 * (y1 - y2);
    return ((c3 * frac + c2) * frac + c1) * frac + y1;
  }
  Frame Erosion::process(const Frame& input, const Frame* noise_override) {
    Frame modulator {}, result;
    if (parameters_.mode == 0) {
      for (int lane = 0; lane < 4; ++lane) {
        const double raw = noise_override ? (*noise_override)[lane] : noise();
        const double filtered = b0_ * raw + b2_ * x2_[lane] - a1_ * y1_[lane] - a2_ * y2_[lane];
        x2_[lane] = x1_[lane]; x1_[lane] = raw; y2_[lane] = y1_[lane]; y1_[lane] = filtered;
        modulator[lane] = filtered;
      }
      for (int pair = 0; pair < 2; ++pair)
        modulator[2 * pair + 1] = modulator[2 * pair + 1] * parameters_.stereo / 100 +
                                 modulator[2 * pair] * (1 - parameters_.stereo / 100);
    }
    else {
      phase_ += parameters_.frequency / sample_rate_;
      if (phase_ > 1) phase_ -= 1;
      modulator.fill(std::sin(phase_ * twopi));
    }
    buffer_[position_] = input;
    for (int lane = 0; lane < 4; ++lane)
      result[lane] = read(position_ - std::max(1.0, 64 + modulator[lane] * parameters_.amount * 0.15), lane);
    position_ = (position_ + 1) & 4095;
    return result;
  }

  void Floaty::prepare(double rate) {
    if (sample_rate_ == rate) return;
    sample_rate_ = rate;
    delay_capacity_ = std::max(kMinDelayCapacity, static_cast<int>(std::ceil(rate * kMaxDelaySeconds)));
    pair_capacity_ = static_cast<size_t>(delay_capacity_) * 8 + 32;
    buffer_.prepare(pair_capacity_ * 2);
    reset();
  }
  void Floaty::reset() {
    buffer_.clear(); low0_.fill(0); low1_.fill(0); high0_.fill(0); high1_.fill(0);
    delay_ = -1; play_ = 0; time_ = 0; clamp_ = 2; resetting_ = true;
  }
  void Floaty::setParameters(const Parameters& p, double bpm) {
    parameters_ = p;
    const double milliseconds = p.sync > 0 && bpm > 0 ? 1000 * p.sync * 60 / 8 / bpm : p.delay;
    // The original's 0 ms case addresses negative memory / divides by zero.
    const int delay = static_cast<int>(std::clamp(milliseconds * sample_rate_ / 1000,
                                                2.0, static_cast<double>(delay_capacity_)));
    length_ = delay * 4;
    if (delay != delay_ || std::abs(p.playback - old_playback_) >= 0.00001 ||
        std::abs(p.warp_rate - old_rate_) >= 0.00001 || std::abs(p.offset - old_offset_) >= 0.00001) {
      play_ = 0; left_ = delay - 2; right_ = static_cast<int>(p.offset * left_ / 100);
      time_ = 0; resetting_ = true;
      delay_ = delay; old_playback_ = p.playback; old_rate_ = p.warp_rate; old_offset_ = p.offset;
    }
    rate_ = twopi * p.warp_rate / sample_rate_;
    lc_ = std::pow(0.5, 5 - p.cutoff / 25); lr_ = std::pow(0.5, p.resonance / 40 - 0.6);
    hc_ = std::pow(0.5, 4.8); hr_ = std::pow(0.5, 3 - p.resonance / 40);
    target_clamp_ = std::pow(2.0, p.clamp / 6);
  }
  Frame Floaty::process(const Frame& input) {
    play_ += resetting_ ? parameters_.playback : parameters_.playback + parameters_.warp / 100 * std::sin(time_);
    if (!resetting_) time_ += rate_;
    if (play_ >= length_) resetting_ = false;
    if (play_ >= length_) play_ -= length_;
    if (play_ < 0) play_ += length_;
    const int p0 = static_cast<int>(play_), p1 = p0 + 1 == length_ ? 0 : p0 + 1;
    const double fraction = play_ - p0;
    // EEL's % operator truncates operands to integers, unlike std::fmod.
    const double overlap_left = std::min(1.0, std::abs((p0 % length_) - (left_ % length_)) / 128.0);
    const double overlap_right = std::min(1.0, std::abs((p0 % length_) - (right_ % length_)) / 128.0);
    // JSFX != uses a 1e-5 tolerance, which is audible in a resonant loop.
    if (std::abs(clamp_ - target_clamp_) >= 0.00001) clamp_ = 0.8 * clamp_ + 0.2 * target_clamp_;
    Frame result;
    for (int lane = 0; lane < 4; ++lane) {
      const size_t base = (lane / 2) * pair_capacity_ + (lane % 2) * length_;
      double wet = buffer_.read(base + p0) * (1 - fraction) + buffer_.read(base + p1) * fraction;
      wet *= lane % 2 == 0 ? overlap_left : overlap_right;
      wet = std::clamp(wet, -clamp_, clamp_);
      if (parameters_.cutoff > 0 && parameters_.resonance > 0) {
        low0_[lane] = (1 - lr_ * lc_) * low0_[lane] - lc_ * low1_[lane] + lc_ * wet;
        low1_[lane] = (1 - lr_ * lc_) * low1_[lane] + lc_ * low0_[lane];
        wet = low1_[lane];
        high0_[lane] = (1 - hr_ * hc_) * high0_[lane] - hc_ * high1_[lane] + hc_ * wet;
        high1_[lane] = (1 - hr_ * hc_) * high1_[lane] + hc_ * high0_[lane];
        wet -= high1_[lane];
      }
      buffer_.write(base + (lane % 2 == 0 ? left_ : right_), wet * parameters_.feedback / 100 + input[lane]);
      result[lane] = wet * parameters_.mix / 100 + input[lane] * (1 - parameters_.mix / 100);
    }
    if (++left_ >= length_) left_ -= length_;
    if (++right_ >= length_) right_ -= length_;
    return result;
  }
}
