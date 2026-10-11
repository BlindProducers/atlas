// SPDX-License-Identifier: GPL-3.0-or-later
#include "echo_engine.h"
#include "tape_saturation.h"

#include <algorithm>
#include <cmath>

namespace atlas::analogue::echo {
  namespace {
    constexpr double kPi = 3.14159265358979323846;
    constexpr double kFadeSeconds = 0.03;
    constexpr double kSmoothingSeconds = 0.02;
    constexpr double kHeadSeconds = 0.01;
    constexpr double kRatioSeconds = 0.05;

    // Head 1 delay limits. The RE-201 spans 69-177 ms and a Memory Man about
    // 550 ms; both extend beyond the hardware, where the coupling of time and
    // tone keeps degrading the repeats.
    constexpr double kTapeMinDelay = 0.02, kTapeMaxDelay = 1.0;
    constexpr double kBbdMinDelay = 0.01, kBbdMaxDelay = 1.2;

    // RE-201 head times measured by Universal Audio: 69/131/189 ms at the
    // fastest speed, 177/337/489 ms at the slowest.
    constexpr std::array<double, kNumHeads> kFreeRatios { 1.0, 1.90, 2.75 };
    constexpr std::array<double, kNumHeads> kSyncedRatios { 1.0, 2.0, 3.0 };

    // Tape units: speed 1 moves head 1's distance past the heads in 100 ms.
    constexpr double kReferenceDelay = 0.1;
    // Record-to-head-1 distance: about 25 mm (UA's times with the service
    // notes' 12-40 cm/s), so physical speed = 0.025 m / head 1 delay.
    constexpr double kHeadOneMetres = 0.025;
    // Playback spacing loss exp(-2 pi f d / v): -3 dB at 0.0552 v / d. Head
    // and tape spacing 2 um when new, up to 8 um worn (Age).
    constexpr double kSpacingMetres = 2.0e-6, kWornSpacingMetres = 6.0e-6;
    // RE-201 S/N 60 dB (A-weighted): raw hiss per head before playback loss.
    constexpr double kTapeHiss = 1.2e-3;
    // RE-201 motor: delay rising (tape slowing) 1.89 s, falling 1.07 s
    // (Downing & Terjesen); Inertia 50% is the hardware.
    constexpr double kSlowingSeconds = 1.89, kSpeedingSeconds = 1.07;

    // Deluxe Memory Man: 8192 stages (two MN3005, S/N 75 dB, THD 1%) between
    // NE570 halves; strong treble cut (cutoffs undocumented: 3 kHz 4th-order
    // sections, a third of its clock at full delay).
    constexpr int kBbdStages = 8192;
    constexpr double kBbdFilterHz = 3000.0;
    constexpr double kBbdSignalToNoise = 75.0;
    constexpr double kBbdRectifierSeconds = 0.01;

    // Bass / Treble: RE-201 Baxandall, +/-10 dB at 100 Hz and 5 kHz.
    constexpr double kBassHz = 200.0, kTrebleHz = 3000.0;

    double coefficientFor(double seconds, double rate) {
      return seconds > 0.0 ? -std::expm1(-1.0 / (seconds * rate)) : 1.0;
    }

    // RBJ shelving biquads, S = 1.
    void shelf(double* coefficients, double hz, double gain_db, double rate, bool high) {
      const double a = std::pow(10.0, gain_db / 40.0);
      const double w = 2.0 * kPi * std::min(hz, 0.45 * rate) / rate;
      const double cosw = std::cos(w), alpha = std::sin(w) / 2.0 * std::sqrt(2.0);
      const double root = 2.0 * std::sqrt(a) * alpha;
      double b0, b1, b2, a0, a1, a2;
      if (high) {
        b0 = a * ((a + 1) + (a - 1) * cosw + root);
        b1 = -2 * a * ((a - 1) + (a + 1) * cosw);
        b2 = a * ((a + 1) + (a - 1) * cosw - root);
        a0 = (a + 1) - (a - 1) * cosw + root;
        a1 = 2 * ((a - 1) - (a + 1) * cosw);
        a2 = (a + 1) - (a - 1) * cosw - root;
      }
      else {
        b0 = a * ((a + 1) - (a - 1) * cosw + root);
        b1 = 2 * a * ((a - 1) - (a + 1) * cosw);
        b2 = a * ((a + 1) - (a - 1) * cosw - root);
        a0 = (a + 1) + (a - 1) * cosw + root;
        a1 = -2 * ((a - 1) + (a + 1) * cosw);
        a2 = (a + 1) + (a - 1) * cosw - root;
      }
      coefficients[0] = b0 / a0; coefficients[1] = b1 / a0; coefficients[2] = b2 / a0;
      coefficients[3] = a1 / a0; coefficients[4] = a2 / a0;
    }

    // Backstop above the loop saturation: transparent below 1, never past 2.
    double safety(double x) {
      const double magnitude = std::abs(x);
      if (magnitude <= 1.0) return x;
      return std::copysign(1.0 + std::tanh(magnitude - 1.0), x);
    }
  }

  Range tapeRange() { return { kTapeMinDelay, kTapeMaxDelay }; }
  Range bbdRange() { return { kBbdMinDelay, kBbdMaxDelay }; }
  std::array<double, kNumHeads> headRatios(bool synced) { return synced ? kSyncedRatios : kFreeRatios; }

  Engine::Engine() { prepare(sample_rate_); }

  void Engine::prepare(double sample_rate) {
    sample_rate_ = sample_rate;
    smoothing_ = coefficientFor(kSmoothingSeconds, sample_rate);
    fade_length_ = std::max(1, static_cast<int>(kFadeSeconds * sample_rate));
    // Longest head at the slowest speed, plus margin for wow.
    const double longest = kSyncedRatios[2] * kTapeMaxDelay * 1.1 + 0.05;
    const size_t size = static_cast<size_t>(std::ceil(longest * sample_rate)) + 8;
    tape_audio_.assign(size, 0.0f);
    tape_position_.assign(size, 0.0);

    const auto filter = toPartialFractions(butterworthLowPass(4, kBbdFilterHz));
    BbdChip chip;
    chip.stages = kBbdStages;
    chip.headroom = 2.5;
    chip.even_distortion = 0.1;
    chip.noise = noiseForSignalToNoise(kBbdSignalToNoise, filter, 100000.0);
    chip.clock_leakage = 3.0e-3;
    // At short delays the 8192-stage clock runs far above anything the 3 kHz
    // filters pass; group its ticks to at most two events per sample.
    chip.max_events_per_sample = 2.0;
    for (auto& line : lines_)
      line.configure(chip, filter, filter, sample_rate);
    compressor_.prepare(sample_rate, kBbdRectifierSeconds, 0.33);
    for (auto& expander : expanders_)
      expander.prepare(sample_rate, kBbdRectifierSeconds, 0.33);
    tone_bass_ = tone_treble_ = 1.0e9;
    reset();
  }

  void Engine::reset() {
    const auto& p = parameters_;
    current_medium_ = p.medium;
    fading_medium_ = -1;
    fade_remaining_ = 0;
    feedback_ = p.feedback; drive_ = p.drive; mix_ = p.mix; width_ = p.width;
    wow_ = p.wow; flutter_ = p.flutter; noise_ = p.noise; age_ = p.age;
    for (int h = 0; h < kNumHeads; ++h) pan_[h] = p.pans[h];

    // Tape: start as if the motor had been running at the set speed.
    speed_ = kReferenceDelay / std::clamp(p.time, kTapeMinDelay, kTapeMaxDelay);
    ratio_ = headRatios(p.synced);
    const size_t size = tape_audio_.size();
    std::fill(tape_audio_.begin(), tape_audio_.end(), 0.0f);
    for (size_t i = 0; i < size; ++i)
      tape_position_[i] = (static_cast<double>(i) - static_cast<double>(size - 1)) * speed_;
    write_ = size - 1;
    valid_from_ = 0;
    position_ = 0.0;
    for (int h = 0; h < kNumHeads; ++h) {
      heads_[h] = Head {};
      heads_[h].read = 0;
      heads_[h].gain = p.heads[h] ? 1.0 : 0.0;
    }
    feedback_select_ = 2.0;
    tape_feedback_ = 0.0;
    record_highpass_ = OnePole {};
    wow_phase_ = flutter_phase_ = drift_ = 0.0;
    drift_filter_ = OnePole {};

    for (auto& line : lines_) line.reset();
    compressor_.reset();
    for (auto& expander : expanders_) expander.reset();
    bbd_rate_ = 1.0 / std::clamp(p.time, kBbdMinDelay, kBbdMaxDelay);
    bbd_delay_ = { 1.0 / bbd_rate_, 1.0 / bbd_rate_ };
    for (auto& line : lines_) { line.setDelay(1.0 / bbd_rate_); line.reset(); }
    bbd_feedback_ = 0.0;
    bbd_wow_phase_ = { 0.0, 0.25 };
    bbd_flutter_phase_ = { 0.0, 0.4 };
    bbd_drift_ = {};
    bbd_highpass_ = OnePole {};
    control_countdown_ = 0;
    for (auto& tone : tone_) tone = Tone {};
    tone_bass_ = tone_treble_ = 1.0e9;
    configureTone(p.bass_db, p.treble_db);
    random_ = 0x2545f491u;
    primed_ = true;
  }

  void Engine::setParameters(const Parameters& parameters) {
    parameters_ = parameters;
    parameters_.medium = std::clamp(parameters.medium, 0, kNumMedia - 1);
    if (parameters_.medium != current_medium_) {
      // The outgoing machine stops recording and fades; the incoming one
      // starts recording from silence with a short input fade, so its first
      // repeats rise smoothly instead of replaying stale audio.
      if (fading_medium_ < 0) {
        fading_medium_ = current_medium_;
        fade_remaining_ = fade_length_;
      }
      current_medium_ = parameters_.medium;
      if (fading_medium_ == current_medium_) {
        fading_medium_ = -1;
        fade_remaining_ = 0;
      }
      else
        clearMedium(current_medium_);
    }
  }

  void Engine::clearMedium(int medium) {
    if (medium == kTape) {
      // Blank the tape without touching megabytes on the audio thread: the
      // positions stay (the motor kept turning), older audio reads as silence.
      valid_from_ = write_ + 1;
      for (auto& head : heads_) { head.loss1 = head.loss2 = OnePole {}; }
      tape_feedback_ = 0.0;
      record_highpass_ = OnePole {};
    }
    else {
      for (auto& line : lines_) line.reset();
      compressor_.reset();
      for (auto& expander : expanders_) expander.reset();
      bbd_feedback_ = 0.0;
      bbd_highpass_ = OnePole {};
    }
  }

  void Engine::configureTone(double bass_db, double treble_db) {
    if (bass_db == tone_bass_ && treble_db == tone_treble_) return;
    tone_bass_ = bass_db;
    tone_treble_ = treble_db;
    double low[5], high[5];
    shelf(low, kBassHz, bass_db, sample_rate_, false);
    shelf(high, kTrebleHz, treble_db, sample_rate_, true);
    for (auto& tone : tone_) {
      tone.bass.b0 = low[0]; tone.bass.b1 = low[1]; tone.bass.b2 = low[2];
      tone.bass.a1 = low[3]; tone.bass.a2 = low[4];
      tone.treble.b0 = high[0]; tone.treble.b1 = high[1]; tone.treble.b2 = high[2];
      tone.treble.a1 = high[3]; tone.treble.a2 = high[4];
    }
  }

  double Engine::random() {
    random_ ^= random_ << 13; random_ ^= random_ >> 17; random_ ^= random_ << 5;
    return random_ * (1.0 / 4294967296.0);
  }

  double Engine::gaussian() { return (random() + random() - 1.0) * 2.4494897427831781; }

  double Engine::inertiaCoefficient(double target, double current) const {
    const double scale = parameters_.inertia / 0.5;
    return coefficientFor((target < current ? kSlowingSeconds : kSpeedingSeconds) * scale, sample_rate_);
  }

  double Engine::saturate(double x) const {
    // Drive raises the level onto the tape; the playback level is scaled back
    // so quiet signals repeat at unity and Feedback stays the loop gain.
    const double gain = tape_saturation::driveGain(drive_);
    return tape_saturation::shape(x * gain) / gain;
  }

  double Engine::tapeRead(std::uint64_t index, double fraction) const {
    const size_t size = tape_audio_.size();
    const auto at = [&](std::uint64_t i) { return i < valid_from_ ? 0.0 : static_cast<double>(tape_audio_[i % size]); };
    const double y0 = at(index - 1), y1 = at(index), y2 = at(index + 1), y3 = at(index + 2);
    return ((( -0.5 * y0 + 1.5 * y1 - 1.5 * y2 + 0.5 * y3) * fraction +
             (y0 - 2.5 * y1 + 2.0 * y2 - 0.5 * y3)) * fraction + (-0.5 * y0 + 0.5 * y2)) * fraction + y1;
  }

  // Finds where the tape now under a head was recorded: the history index r
  // with position(r) <= target < position(r + 1). Positions only increase.
  double Engine::locate(Head& head, double target) {
    const size_t size = tape_audio_.size();
    const auto position = [&](std::uint64_t i) { return tape_position_[i % size]; };
    const std::uint64_t oldest = write_ - (size - 4);
    const std::uint64_t newest = write_ - 2;  // leave room for the cubic read
    std::uint64_t r = std::clamp(head.read, oldest, newest);
    if (position(r) > target) {
      // A head moved away from the record head: search backwards.
      std::uint64_t low = oldest, high = r;
      while (high - low > 1) {
        const std::uint64_t middle = low + (high - low) / 2;
        (position(middle) > target ? high : low) = middle;
      }
      r = low;
    }
    else if (r < newest && position(r + 1) <= target) {
      // Usually one or two steps; a sudden speed-up searches instead.
      int steps = 0;
      while (r < newest && position(r + 1) <= target && ++steps < 8) ++r;
      if (r < newest && position(r + 1) <= target) {
        std::uint64_t low = r, high = newest;
        while (high - low > 1) {
          const std::uint64_t middle = low + (high - low) / 2;
          (position(middle) > target ? high : low) = middle;
        }
        r = low;
      }
    }
    head.read = r;
    const double start = position(r), span = position(r + 1) - start;
    const double fraction = span > 0.0 ? std::clamp((target - start) / span, 0.0, 1.0) : 0.0;
    return tapeRead(r, fraction);
  }

  void Engine::renderTape(const float* mono, float* wet_left, float* wet_right, int count, double gain_start, double gain_end) {
    const auto& p = parameters_;
    const size_t size = tape_audio_.size();
    const double reference = kReferenceDelay * sample_rate_;
    const double target_speed = kReferenceDelay / std::clamp(p.time, kTapeMinDelay, kTapeMaxDelay);
    const double speed_coefficient = inertiaCoefficient(target_speed, speed_);
    const auto ratios = headRatios(p.synced);
    const double ratio_coefficient = coefficientFor(kRatioSeconds, sample_rate_);
    const double head_coefficient = coefficientFor(kHeadSeconds, sample_rate_);
    const double drift_coefficient = coefficientFor(1.0 / (2.0 * kPi * 1.0), sample_rate_);
    const double drift_scale = std::sqrt(sample_rate_ / (kPi * 1.0));
    const double highpass = coefficientFor(1.0 / (2.0 * kPi * 40.0), sample_rate_);
    const double hiss = kTapeHiss * (noise_ / 0.5) * (noise_ / 0.5) * (1.0 + age_);
    const double spacing = kSpacingMetres + kWornSpacingMetres * age_;
    const double contact_coefficient = coefficientFor(0.15, sample_rate_);

    // The furthest active head feeds back.
    int furthest = -1;
    for (int h = 0; h < kNumHeads; ++h) if (p.heads[h]) furthest = h;

    for (int n = 0; n < count; ++n) {
      speed_ += (target_speed - speed_) * speed_coefficient;
      for (int h = 0; h < kNumHeads; ++h)
        ratio_[h] += (ratios[h] - ratio_[h]) * ratio_coefficient;

      // Transport: drift below 1 Hz, wow and flutter at rates that follow the
      // capstan and pinch roller, so they slow with the tape.
      drift_ = drift_filter_.lowpass(gaussian(), drift_coefficient) * drift_scale;
      const double wobble = wow_ * (0.010 * std::sin(2.0 * kPi * wow_phase_) + 0.005 * drift_) +
                            flutter_ * 0.004 * std::sin(2.0 * kPi * flutter_phase_);
      const double speed = speed_ * (1.0 + wobble);
      wow_phase_ += 0.8 * speed_ / sample_rate_; wow_phase_ -= std::floor(wow_phase_);
      flutter_phase_ += 7.0 * speed_ / sample_rate_; flutter_phase_ -= std::floor(flutter_phase_);
      position_ += speed;

      // Playback loss at this speed: spacing loss as two one-poles.
      const double metres_per_second = kHeadOneMetres / (kReferenceDelay / speed);
      const double loss_hz = 0.0552 * metres_per_second / spacing / 0.6436;
      const double loss = coefficientFor(1.0 / (2.0 * kPi * std::min(loss_hz, 0.45 * sample_rate_)), sample_rate_);

      double heads[kNumHeads];
      for (int h = 0; h < kNumHeads; ++h) {
        Head& head = heads_[h];
        head.gain += ((p.heads[h] ? 1.0 : 0.0) - head.gain) * head_coefficient;
        double value = locate(head, position_ - ratio_[h] * reference) + hiss * gaussian();
        value = head.loss2.lowpass(head.loss1.lowpass(value, loss), loss);

        // Age: uneven tape contact and dropouts.
        if (age_ > 0.0) {
          if (random() < 4.0 / sample_rate_)
            head.contact_target = 1.0 - 0.3 * age_ * random();
          head.contact += (head.contact_target - head.contact) * contact_coefficient;
          if (head.dropout_remaining > 0) {
            const double progress = 1.0 - double(head.dropout_remaining--) / head.dropout_length;
            value *= 1.0 - 0.9 * age_ * (0.5 - 0.5 * std::cos(2.0 * kPi * progress));
          }
          else if (--head.dropout_wait <= 0) {
            head.dropout_wait = static_cast<int>((0.5 + 6.0 * random()) * sample_rate_ / (0.2 + age_));
            head.dropout_length = std::max(16, static_cast<int>((0.01 + 0.06 * random()) * sample_rate_));
            head.dropout_remaining = head.dropout_length;
          }
          value *= head.contact;
        }
        heads[h] = value;
      }

      // Feedback follows the furthest active head, crossfading when it changes.
      feedback_select_ += ((furthest < 0 ? feedback_select_ : furthest) - feedback_select_) * head_coefficient;
      const int lower = std::clamp(static_cast<int>(std::floor(feedback_select_)), 0, kNumHeads - 1);
      const int upper = std::min(lower + 1, kNumHeads - 1);
      const double blend = feedback_select_ - lower;
      const double source = furthest < 0 ? 0.0 : heads[lower] * (1.0 - blend) + heads[upper] * blend;
      tape_feedback_ = tone_[2].tick(source);

      const double input_gain = gain_start + (gain_end - gain_start) * (n + 1) / count;
      double record = mono[n] * input_gain + feedback_ * tape_feedback_;
      record -= record_highpass_.lowpass(record, highpass);
      record = saturate(record);
      ++write_;
      tape_audio_[write_ % size] = static_cast<float>(record);
      tape_position_[write_ % size] = position_;

      double left = 0.0, right = 0.0;
      for (int h = 0; h < kNumHeads; ++h) {
        pan_[h] += (p.pans[h] - pan_[h]) * smoothing_;
        const double angle = (std::clamp(pan_[h], -1.0, 1.0) + 1.0) * 0.25 * kPi;
        left += heads[h] * heads_[h].gain * std::cos(angle);
        right += heads[h] * heads_[h].gain * std::sin(angle);
      }
      wet_left[n] = static_cast<float>(left);
      wet_right[n] = static_cast<float>(right);
    }
  }

  void Engine::renderBbd(const float* mono, float* wet_left, float* wet_right, int count, double gain_start, double gain_end) {
    const auto& p = parameters_;
    const double target_rate = 1.0 / std::clamp(p.time, kBbdMinDelay, kBbdMaxDelay);
    const double rate_coefficient = inertiaCoefficient(target_rate, bbd_rate_);
    const double highpass = coefficientFor(1.0 / (2.0 * kPi * 60.0), sample_rate_);
    const double drift_coefficient = coefficientFor(1.0 / (2.0 * kPi * 1.0), sample_rate_);
    const double drift_scale = std::sqrt(sample_rate_ / (kPi * 1.0));
    const double noise_scale = (noise_ / 0.5) * (noise_ / 0.5) * (1.0 + 3.0 * age_);
    for (auto& line : lines_) line.setNoiseScale(noise_scale, noise_scale);
    // Age: the expanders drift from the compressor (breathing), in opposite
    // directions on the two sides.
    expanders_[0].setExpanderOffset(1.0 + 0.25 * age_);
    expanders_[1].setExpanderOffset(1.0 - 0.2 * age_);

    for (int n = 0; n < count; ++n) {
      bbd_rate_ += (target_rate - bbd_rate_) * rate_coefficient;
      // Clock wobble: wow, flutter and drift, decorrelated across the sides by Width.
      std::array<double, 2> drift;
      for (int side = 0; side < 2; ++side)
        drift[side] = bbd_drift_[side].lowpass(gaussian(), drift_coefficient) * drift_scale;
      if (--control_countdown_ <= 0) {
        control_countdown_ = BbdLine::kRefreshInterval;
        for (int side = 0; side < 2; ++side) {
          const double own_drift = side == 0 ? drift[0] : (1.0 - width_) * drift[0] + width_ * drift[1];
          const double wow_phase = bbd_wow_phase_[0] + (side ? 0.25 * width_ : 0.0);
          const double flutter_phase = bbd_flutter_phase_[0] + (side ? 0.4 * width_ : 0.0);
          const double wobble = wow_ * (0.008 * std::sin(2.0 * kPi * wow_phase) + 0.004 * own_drift) +
                                flutter_ * 0.003 * std::sin(2.0 * kPi * flutter_phase);
          bbd_delay_[side] = (1.0 / bbd_rate_) * (1.0 + wobble);
          lines_[side].setDelay(bbd_delay_[side]);
        }
      }
      bbd_wow_phase_[0] += 0.5 / sample_rate_; bbd_wow_phase_[0] -= std::floor(bbd_wow_phase_[0]);
      bbd_flutter_phase_[0] += 5.0 / sample_rate_; bbd_flutter_phase_[0] -= std::floor(bbd_flutter_phase_[0]);

      const double input_gain = gain_start + (gain_end - gain_start) * (n + 1) / count;
      double input = mono[n] * input_gain + feedback_ * bbd_feedback_;
      input -= bbd_highpass_.lowpass(input, highpass);
      const double compressed = compressor_.compress(saturate(input));
      double out[2];
      for (int side = 0; side < 2; ++side)
        out[side] = expanders_[side].expand(lines_[side].process(compressed));
      bbd_feedback_ = tone_[2].tick(0.5 * (out[0] + out[1]));
      wet_left[n] = static_cast<float>(out[0]);
      wet_right[n] = static_cast<float>(out[1]);
    }
  }

  void Engine::process(const float* in_left, const float* in_right, float* out_left, float* out_right, int count) {
    constexpr int kBlock = 128;
    for (int offset = 0; offset < count;) {
      const int block = std::min(kBlock, count - offset);
      float mono[kBlock], wet_left[kBlock], wet_right[kBlock];
      for (int n = 0; n < block; ++n)
        mono[n] = 0.5f * (in_left[offset + n] + in_right[offset + n]);

      const double block_smoothing = 1.0 - std::pow(1.0 - smoothing_, block);
      const auto& p = parameters_;
      feedback_ += (std::clamp(p.feedback, 0.0, 1.2) - feedback_) * block_smoothing;
      drive_ += (p.drive - drive_) * block_smoothing;
      width_ += (p.width - width_) * block_smoothing;
      wow_ += (p.wow - wow_) * block_smoothing;
      flutter_ += (p.flutter - flutter_) * block_smoothing;
      noise_ += (p.noise - noise_) * block_smoothing;
      age_ += (p.age - age_) * block_smoothing;
      configureTone(std::clamp(p.bass_db, -10.0, 10.0), std::clamp(p.treble_db, -10.0, 10.0));

      const auto render = [&](int medium, float* left, float* right, double gain_start, double gain_end) {
        if (medium == kTape) renderTape(mono, left, right, block, gain_start, gain_end);
        else renderBbd(mono, left, right, block, gain_start, gain_end);
      };
      if (fading_medium_ < 0) {
        render(current_medium_, wet_left, wet_right, 1.0, 1.0);
      }
      else {
        // Both machines run through the fade: the new one records with a
        // rising input, the old one with a falling one while its output fades.
        const double start = 1.0 - double(fade_remaining_) / fade_length_;
        const double end = std::min(1.0, 1.0 - double(fade_remaining_ - block) / fade_length_);
        float old_left[kBlock], old_right[kBlock];
        render(current_medium_, wet_left, wet_right, start, end);
        render(fading_medium_, old_left, old_right, 1.0 - start, 1.0 - end);
        for (int n = 0; n < block; ++n) {
          const double t = std::clamp(1.0 - double(fade_remaining_ - n) / fade_length_, 0.0, 1.0);
          const double out_gain = std::cos(0.5 * kPi * t);
          wet_left[n] = static_cast<float>(wet_left[n] + old_left[n] * out_gain);
          wet_right[n] = static_cast<float>(wet_right[n] + old_right[n] * out_gain);
        }
        fade_remaining_ = std::max(0, fade_remaining_ - block);
        if (fade_remaining_ == 0) fading_medium_ = -1;
      }

      for (int n = 0; n < block; ++n) {
        mix_ += (p.mix - mix_) * smoothing_;
        const double left = safety(tone_[0].tick(wet_left[n]));
        const double right = safety(tone_[1].tick(wet_right[n]));
        const double dry_gain = std::cos(0.5 * kPi * mix_), wet_gain = std::sin(0.5 * kPi * mix_);
        out_left[offset + n] = static_cast<float>(dry_gain * in_left[offset + n] + wet_gain * left);
        out_right[offset + n] = static_cast<float>(dry_gain * in_right[offset + n] + wet_gain * right);
      }
      offset += block;
    }
  }
}
