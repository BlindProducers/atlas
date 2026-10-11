// SPDX-License-Identifier: GPL-3.0-or-later
#include "spectral_retune_engine.h"
#include "spectral_lock_engine.h"
#include <algorithm>

namespace atlas::spectral_lock::retune {
  namespace {
    constexpr float kPi = 3.14159265358979323846f;
    constexpr float kTwoPi = 2.0f * kPi;
    constexpr float kMixSeconds = 0.005f;
    // Harmonic matching tolerance: 3% of the expected frequency, at least 1.5 bins.
    constexpr float kHarmonicTolerance = 0.03f;

    float wrapPhase(float phase) {
      return phase - kTwoPi * std::floor((phase + kPi) / kTwoPi);
    }
    float hzToMidi(float hz) { return 69.0f + 12.0f * std::log2(std::max(hz, 1.0f) / 440.0f); }
    float dbToGain(float db) { return std::pow(10.0f, db / 20.0f); }
  }

  float Engine::snapSemitones(int scale, int root, float midi) {
    scale = std::clamp(scale, 0, spectral_lock::Engine::numScales - 1);
    root = std::clamp(root, 0, 11);
    const int nearest = int(std::floor(midi + 0.5f));
    const int relative = nearest - root;
    const int octave = int(std::floor(float(relative) / 12.0f));
    const int pitch_class = relative - octave * 12;
    const int out_note = root + octave * 12 + spectral_lock::Engine::scaleTables[scale][pitch_class];
    return float(out_note) - midi;
  }

  Engine::Engine() { prepare(48000.0); }

  void Engine::prepare(double sample_rate) {
    sample_rate_ = sample_rate > 1000.0 ? sample_rate : 48000.0;
    const int order = fftOrderForRate(sample_rate_);
    if (order != fft_order_) {
      fft_order_ = order;
      fft_size_ = 1 << order;
      hop_size_ = fft_size_ / 4;
      num_bins_ = fft_size_ / 2 + 1;
      fft_ = std::make_unique<juce::dsp::FFT>(order);
      window_.assign(size_t(fft_size_), 0.0f);
      for (int i = 0; i < fft_size_; ++i)
        window_[size_t(i)] = 0.5f - 0.5f * std::cos(kTwoPi * float(i) / float(fft_size_));
      for (auto* buffer : { &fft_left_, &fft_right_, &out_fft_left_, &out_fft_right_ })
        buffer->assign(size_t(2 * fft_size_), 0.0f);
      for (auto* buffer : { &in_left_, &in_right_, &out_left_, &out_right_ })
        buffer->assign(size_t(fft_size_), 0.0f);
      for (auto* buffer : { &magnitude_, &previous_magnitude_, &phase_, &previous_phase_, &frequency_,
                            &magnitude_left_, &magnitude_right_, &phase_left_, &phase_right_,
                            &out_phase_left_, &out_phase_right_ })
        buffer->assign(size_t(num_bins_), 0.0f);
      out_phase_frame_left_.assign(size_t(num_bins_), -1);
      out_phase_frame_right_.assign(size_t(num_bins_), -1);
    }
    bin_hz_ = float(sample_rate_ / fft_size_);
    reset();
  }

  void Engine::reset() {
    for (auto* buffer : { &in_left_, &in_right_, &out_left_, &out_right_, &previous_magnitude_, &previous_phase_,
                          &out_phase_left_, &out_phase_right_ })
      std::fill(buffer->begin(), buffer->end(), 0.0f);
    std::fill(out_phase_frame_left_.begin(), out_phase_frame_left_.end(), -1);
    std::fill(out_phase_frame_right_.begin(), out_phase_frame_right_.end(), -1);
    frame_count_ = 0;
    position_ = 0;
    hop_position_ = 0;
    num_peaks_ = 0;
    num_notes_ = 0;
    onset_ = false;
    first_frame_ = true;
    mix_initialised_ = false;
  }

  void Engine::process(const float* in_left, const float* in_right, float* out_left, float* out_right, int count,
                       const Parameters& parameters) {
    parameters_ = parameters;
    const float target_mix = std::clamp(parameters.mix, 0.0f, 1.0f);
    if (!mix_initialised_) {
      mix_ = target_mix;
      mix_initialised_ = true;
    }
    const float mix_coefficient = 1.0f - std::exp(-1.0f / (kMixSeconds * float(sample_rate_)));
    for (int i = 0; i < count; ++i) {
      // The input ring doubles as the dry path, delayed by exactly one frame.
      const float dry_left = in_left_[position_], dry_right = in_right_[position_];
      const float wet_left = out_left_[position_], wet_right = out_right_[position_];
      in_left_[position_] = in_left[i];
      in_right_[position_] = in_right[i];
      out_left_[position_] = 0.0f;
      out_right_[position_] = 0.0f;
      mix_ += (target_mix - mix_) * mix_coefficient;
      out_left[i] = dry_left + (wet_left - dry_left) * mix_;
      out_right[i] = dry_right + (wet_right - dry_right) * mix_;
      position_ = (position_ + 1) & (fft_size_ - 1);
      if (++hop_position_ == hop_size_) {
        hop_position_ = 0;
        processFrame();
      }
    }
  }

  void Engine::processFrame() {
    for (int i = 0; i < fft_size_; ++i) {
      const int index = (position_ + i) & (fft_size_ - 1);
      fft_left_[i] = in_left_[index] * window_[i];
      fft_right_[i] = in_right_[index] * window_[i];
    }
    fft_->performRealOnlyForwardTransform(fft_left_.data(), true);
    fft_->performRealOnlyForwardTransform(fft_right_.data(), true);

    analyse();
    groupNotes();
    decideShifts();
    synthesise(out_fft_left_, out_phase_left_, out_phase_frame_left_, 0);
    synthesise(out_fft_right_, out_phase_right_, out_phase_frame_right_, 1);
    ++frame_count_;

    fft_->performRealOnlyInverseTransform(out_fft_left_.data());
    fft_->performRealOnlyInverseTransform(out_fft_right_.data());
    constexpr float kOverlapScale = 2.0f / 3.0f;  // four squared Hann windows sum to 1.5
    for (int i = 0; i < fft_size_; ++i) {
      const int index = (position_ + i) & (fft_size_ - 1);
      const float scale = window_[i] * kOverlapScale;
      out_left_[index] += out_fft_left_[i] * scale;
      out_right_[index] += out_fft_right_[i] * scale;
    }
    first_frame_ = false;
  }

  void Engine::analyse() {
    // Magnitudes and phases per channel and of the linked sum; instantaneous
    // frequency of every bin from the sum's phase advance over one hop.
    const float expected_step = kTwoPi * float(hop_size_) / float(fft_size_);
    float frame_peak = 0.0f;
    float flux = 0.0f, previous_total = 0.0f;
    for (int bin = 0; bin < num_bins_; ++bin) {
      const float real_left = fft_left_[2 * bin], imaginary_left = fft_left_[2 * bin + 1];
      const float real_right = fft_right_[2 * bin], imaginary_right = fft_right_[2 * bin + 1];
      magnitude_left_[bin] = std::sqrt(real_left * real_left + imaginary_left * imaginary_left);
      magnitude_right_[bin] = std::sqrt(real_right * real_right + imaginary_right * imaginary_right);
      phase_left_[bin] = std::atan2(imaginary_left, real_left);
      phase_right_[bin] = std::atan2(imaginary_right, real_right);
      const float real = real_left + real_right, imaginary = imaginary_left + imaginary_right;
      magnitude_[bin] = std::sqrt(real * real + imaginary * imaginary);
      phase_[bin] = std::atan2(imaginary, real);
      const float deviation = wrapPhase(phase_[bin] - previous_phase_[bin] - expected_step * float(bin));
      frequency_[bin] = (float(bin) + deviation / expected_step) * bin_hz_;
      frame_peak = std::max(frame_peak, magnitude_[bin]);
      flux += std::max(0.0f, magnitude_[bin] - previous_magnitude_[bin]);
      previous_total += previous_magnitude_[bin];
      previous_phase_[bin] = phase_[bin];
    }
    // Onset: the spectrum grew by more than the sensitivity allows in one hop.
    const float sensitivity = std::clamp(parameters_.transients, 0.0f, 1.0f);
    const float threshold = 2.0f - 1.6f * sensitivity;
    onset_ = first_frame_ || flux > threshold * (previous_total + 1.0e-3f);
    previous_magnitude_ = magnitude_;

    // Peaks: local maxima above the floor; regions meet at the valleys between them.
    num_peaks_ = 0;
    const float floor = frame_peak * dbToGain(kPeakFloorDb) + 1.0e-6f;
    for (int bin = 2; bin < num_bins_ - 2 && num_peaks_ < kMaxPeaks; ++bin) {
      const float m = magnitude_[bin];
      if (m < floor || m < magnitude_[bin - 1] || m < magnitude_[bin + 1] || m < magnitude_[bin - 2] || m < magnitude_[bin + 2])
        continue;
      Peak& peak = peaks_[num_peaks_++];
      peak.bin = bin;
      peak.frequency = std::max(1.0f, frequency_[bin]);
      peak.magnitude = m;
      peak.note = -1;
      peak.shift = 0.0f;
    }
    for (int p = 0; p < num_peaks_; ++p) {
      Peak& peak = peaks_[p];
      int low = p == 0 ? 0 : peaks_[p - 1].bin;
      if (p > 0) {
        int valley = peaks_[p - 1].bin;
        for (int bin = peaks_[p - 1].bin; bin <= peak.bin; ++bin)
          if (magnitude_[bin] < magnitude_[valley]) valley = bin;
        low = valley + (valley < peak.bin ? 1 : 0);
        peaks_[p - 1].high = std::max(peaks_[p - 1].low, low - 1);
      }
      peak.low = low;
      peak.high = p == num_peaks_ - 1 ? num_bins_ - 1 : peak.bin;
    }
  }

  void Engine::groupNotes() {
    // Greedy from the bottom: each unassigned, strong enough peak inside the
    // range becomes a fundamental and claims the peaks at its harmonics.
    num_notes_ = 0;
    const float lo = std::clamp(std::min(parameters_.rangeLo, parameters_.rangeHi), 20.0f, float(sample_rate_) * 0.45f);
    const float hi = std::clamp(std::max(parameters_.rangeLo, parameters_.rangeHi), lo, float(sample_rate_) * 0.45f);
    float frame_peak = 0.0f;
    for (int p = 0; p < num_peaks_; ++p) frame_peak = std::max(frame_peak, peaks_[p].magnitude);
    const float note_floor = frame_peak * dbToGain(kNoteFloorDb);
    for (int p = 0; p < num_peaks_ && num_notes_ < kMaxNotes; ++p) {
      Peak& candidate = peaks_[p];
      if (candidate.note >= 0 || candidate.magnitude < note_floor || candidate.frequency < lo || candidate.frequency > hi)
        continue;
      Note& note = notes_[num_notes_];
      note.frequency = candidate.frequency;
      note.partials = 1;
      note.shift = 0.0f;
      candidate.note = num_notes_;
      int next = p + 1;
      for (int harmonic = 2; harmonic <= kMaxHarmonics && next < num_peaks_; ++harmonic) {
        const float target = candidate.frequency * float(harmonic);
        if (target > float(sample_rate_) * 0.5f) break;
        const float tolerance = std::max(kHarmonicTolerance * target, 1.5f * bin_hz_);
        int best = -1;
        float best_distance = tolerance;
        for (int q = next; q < num_peaks_; ++q) {
          const float distance = std::abs(peaks_[q].frequency - target);
          if (peaks_[q].frequency > target + tolerance) break;
          if (peaks_[q].note < 0 && distance <= best_distance) { best = q; best_distance = distance; }
        }
        if (best >= 0) {
          peaks_[best].note = num_notes_;
          ++note.partials;
          next = best + 1;
        }
      }
      ++num_notes_;
    }
  }

  void Engine::decideShifts() {
    const Parameters& p = parameters_;
    const float amount = std::clamp(p.amount, 0.0f, 1.0f);
    const float boost = std::max(1.0f, std::min(p.amount, 2.0f));
    const float reach = std::clamp(p.reach, 1.0f, 12.0f);
    const float global = std::round(std::clamp(p.pitch, -24.0f, 24.0f)) +
                         std::clamp(p.bend, -1.0f, 1.0f) * std::clamp(p.bendRange, 1.0f, 24.0f);
    const float lo = std::clamp(std::min(p.rangeLo, p.rangeHi), 20.0f, float(sample_rate_) * 0.45f);
    const float hi = std::clamp(std::max(p.rangeLo, p.rangeHi), lo, float(sample_rate_) * 0.45f);
    auto shiftFor = [&](float hz) {
      const float snap = snapSemitones(p.scale, p.root, hzToMidi(hz));
      return (std::abs(snap) > reach ? 0.0f : snap * amount) + global;
    };
    for (int n = 0; n < num_notes_; ++n)
      notes_[n].shift = shiftFor(notes_[n].frequency);
    for (int q = 0; q < num_peaks_; ++q) {
      Peak& peak = peaks_[q];
      if (peak.note >= 0)
        peak.shift = notes_[peak.note].shift;          // harmonics follow their fundamental
      else if (peak.frequency >= lo && peak.frequency <= hi)
        peak.shift = shiftFor(peak.frequency);         // a lone partial snaps on its own
      else
        peak.shift = 0.0f;                             // outside the range: untouched
      peak.gain = std::abs(peak.shift) > 1.0e-4f ? boost : 1.0f;
    }
  }

  void Engine::synthesise(std::vector<float>& out, std::vector<float>& out_phase,
                          std::vector<int>& out_phase_frame, int channel) {
    const auto& magnitude = channel == 0 ? magnitude_left_ : magnitude_right_;
    const auto& phase = channel == 0 ? phase_left_ : phase_right_;
    std::fill(out.begin(), out.end(), 0.0f);
    const float hop_seconds = float(hop_size_) / float(sample_rate_);
    for (int q = 0; q < num_peaks_; ++q) {
      const Peak& peak = peaks_[q];
      const float ratio = std::exp2(peak.shift / 12.0f);
      const bool shifted = std::abs(peak.shift) > 1.0e-4f;
      // Fractional bin shift: the integer part moves the region, the fraction
      // slides the magnitude lobe between neighbouring bins so the partial
      // lands on its exact frequency, not the nearest bin centre.
      const float delta_bins = shifted ? peak.frequency * (ratio - 1.0f) / bin_hz_ : 0.0f;
      const int delta = int(std::floor(delta_bins));
      const float fraction = delta_bins - float(delta);
      const int out_peak_bin = peak.bin + delta;
      if (out_peak_bin < 1 || out_peak_bin >= num_bins_ - 2)
        continue;
      // The region's peak advances at its new frequency; the other bins keep
      // their phase offsets to the peak, so the partial stays coherent.
      float peak_phase;
      if (!shifted || onset_)
        peak_phase = phase[peak.bin];
      else {
        // Continue from the accumulator this partial wrote last frame; its
        // output bin may have moved by one or two, so look around it. A bin
        // nobody wrote last frame starts from the input phase.
        int accumulator = -1;
        for (int offset : { 0, -1, 1, -2, 2 }) {
          const int bin = out_peak_bin + offset;
          if (bin >= 0 && bin < num_bins_ && out_phase_frame[bin] == frame_count_ - 1) { accumulator = bin; break; }
        }
        peak_phase = accumulator >= 0
            ? wrapPhase(out_phase[accumulator] + kTwoPi * peak.frequency * ratio * hop_seconds)
            : phase[peak.bin];
      }
      if (!shifted) {
        for (int bin = peak.low; bin <= peak.high; ++bin) {
          out[2 * bin] += magnitude[bin] * std::cos(phase[bin]);
          out[2 * bin + 1] += magnitude[bin] * std::sin(phase[bin]);
        }
        continue;
      }
      for (int out_bin = peak.low + delta; out_bin <= peak.high + delta + 1; ++out_bin) {
        if (out_bin < 0 || out_bin >= num_bins_)
          continue;
        const int source = out_bin - delta;            // the bin the lobe value comes from
        const int below = source - 1;
        const float here = source >= peak.low && source <= peak.high ? magnitude[source] : 0.0f;
        const float previous = below >= peak.low && below <= peak.high ? magnitude[below] : 0.0f;
        const float value = (here * (1.0f - fraction) + previous * fraction) * peak.gain;
        if (value <= 0.0f)
          continue;
        const int phase_source = std::clamp(here >= previous ? source : below, peak.low, peak.high);
        const float bin_phase = peak_phase + (phase[phase_source] - phase[peak.bin]);
        out[2 * out_bin] += value * std::cos(bin_phase);
        out[2 * out_bin + 1] += value * std::sin(bin_phase);
      }
      out_phase[out_peak_bin] = wrapPhase(peak_phase);
      out_phase_frame[out_peak_bin] = frame_count_;
    }
    // Bins that belong to no region (below the first peak, when the frame has
    // no peaks) pass through unchanged.
    if (num_peaks_ == 0) {
      for (int bin = 0; bin < num_bins_; ++bin) {
        out[2 * bin] = magnitude[bin] * std::cos(phase[bin]);
        out[2 * bin + 1] = magnitude[bin] * std::sin(phase[bin]);
      }
    }
  }
}
