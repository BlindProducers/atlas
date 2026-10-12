// SPDX-License-Identifier: GPL-3.0-or-later
// Filter Table kernel design: a wavetable frame's spectrum used as a filter response.
//   - a wavetable frame's harmonic n (cosine amplitude a_n, phase phi_n) sets the
//     response at frequency n * cutoff / 24 to (pi/2) * n * a_n, held flat below
//     harmonic 1 and interpolated linearly in magnitude between harmonics;
//   - Resonance r (0.25 .. 4) raises that response to the power r about its
//     energy-weighted mean level;
//   - Minimum is the minimum-phase kernel of that magnitude, Linear its
//     zero-phase kernel, Original keeps the frame's phases (quadrature, advanced
//     by an eighth of a period), Raw is one period of the tilted waveform;
//   - the kernel is designed once at the table's native period and time-scaled
//     to the requested Cutoff by linear interpolation into a fixed buffer;
//   - Minimum, Linear and Original are smoothed with a [0.1 0.8 0.1] 3-tap,
//     which is the measured high-frequency roll-off; everything is band-limited
//     just below Nyquist.
#pragma once
#include <juce_dsp/juce_dsp.h>
#include "../spectral_common/shared_fft.h"
#include <array>
#include <complex>
#include <limits>
#include <vector>

namespace atlas::filter_table {
  enum PhaseMode { kMinimum, kLinear, kOriginal, kRaw, kNumPhaseModes };

  constexpr int kFrameOrder = 11;
  constexpr int kFrameSize = 1 << kFrameOrder;        // 2048 samples per frame
  constexpr int kNumHarmonics = kFrameSize / 2;       // harmonics 1 .. 1024
  constexpr float kCutoffHarmonic = 24.0f;            // Cutoff is the frequency of harmonic 24
  constexpr float kTilt = 1.5707963267948966f;        // pi / 2
  constexpr float kMinResonance = 0.25f, kMaxResonance = 4.0f;

  // Cosine-convention complex harmonic amplitudes of one frame: index n holds
  // a_n * exp(j phi_n) for n = 1 .. kNumHarmonics; index 0 is unused.
  struct Frame {
    std::array<std::complex<float>, kNumHarmonics + 1> harmonics {};
  };

  // Analyses a 2048-sample single-cycle waveform (allocation free after construction).
  class FrameAnalyser {
    public:
      FrameAnalyser();
      void analyse(const float* waveform, Frame& out);
      // Linear interpolation of two frames' complex harmonics.
      static void interpolate(const Frame& a, const Frame& b, float t, Frame& out);
    private:
      std::shared_ptr<const juce::dsp::FFT> fft_ = sharedFft(kFrameOrder);
      std::vector<float> buffer_;
  };

  struct Parameters {
    float cutoff_hz = 1000.0f;
    float resonance = 1.0f;           // kMinResonance .. kMaxResonance, 1 is neutral
    PhaseMode mode = kMinimum;
    float sample_rate = 48000.0f;
    // Tests only: override the mode's measured sub-sample offset and the band
    // limit edges (fractions of the sample rate) when finite.
    float offset_override = std::numeric_limits<float>::quiet_NaN();
    float band_limit_start = std::numeric_limits<float>::quiet_NaN();
    float band_limit_end = std::numeric_limits<float>::quiet_NaN();
  };

  class KernelBuilder {
    public:
      // Minimum, Linear and Original are designed on the kernel's own grid at
      // the requested Cutoff (bins at fs / size). Raw is one period of the
      // tilted waveform at the table's native period (2048 samples), time-scaled
      // to the Cutoff by linear interpolation.
      static constexpr int kReferencePeriod = kFrameSize;

      // `design_order` is the FFT grid the magnitude is designed on (at least
      // `order`); Minimum phase benefits from a finer grid than the kernel.
      explicit KernelBuilder(int order, int design_order = 0);
      int size() const { return size_; }
      // Writes size() taps. Returns the kernel's latency in samples: 0 for
      // Minimum and Raw, size() / 2 for Linear and Original.
      int build(const Frame& frame, const Parameters& parameters, float* kernel);
      static int latencyForMode(PhaseMode mode, int size) { return mode == kLinear || mode == kOriginal ? size / 2 : 0; }
      static float referenceCutoff(float sample_rate) { return kCutoffHarmonic * sample_rate / kReferencePeriod; }

      // The target magnitude at a frequency (Hz) for tests and displays:
      // tilt, resonance, hold below harmonic 1, interpolation, band limit.
      float magnitudeAt(const Frame& frame, const Parameters& parameters, float hz);

    private:
      void tiltedMagnitudes(const Frame& frame, const Parameters& p);
      float interpolatedMagnitude(float harmonic_position) const;
      void fillGrid(const Parameters& p);
      void finishKernel(float sub_sample_offset, int zero, float* kernel);
      void designMinimum(const Parameters& p, float* kernel);
      void designLinear(const Parameters& p, float* kernel);
      void designOriginal(const Frame& frame, const Parameters& p, float* kernel);
      void designRaw(const Frame& frame, const Parameters& p, float* kernel);
      void smooth(float* kernel) const;
      void fade(float* kernel, bool start, bool end) const;

      int order_, size_, design_order_, design_size_;
      std::shared_ptr<const juce::dsp::FFT> fft_;
      std::shared_ptr<const juce::dsp::FFT> frame_fft_ = sharedFft(kFrameOrder);
      std::array<float, kNumHarmonics + 1> magnitude_ {};      // tilted, resonance applied, band limited
      std::vector<float> grid_;                                 // |H| on the design_size_ / 2 + 1 bins
      std::vector<std::complex<float>> complex_a_, complex_b_;
      std::vector<float> period_;                               // Raw: one tilted period with phases
  };
}
