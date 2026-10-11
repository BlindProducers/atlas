// SPDX-License-Identifier: GPL-3.0-or-later
// Spectral Lock's Spectral mode: polyphonic pitch mapping of the material
// itself. Each frame's spectrum is split into peak regions (one per partial);
// partials are grouped into notes by harmonic structure, every note is moved
// as a whole to the nearest scale note (so a bass and its harmonics keep their
// timbre), inharmonic partials inside the range snap on their own, and
// anything outside Range Low/High that belongs to no note passes unchanged.
// Shifting is a phase-locked vocoder on peak regions: a region keeps its
// internal phase relations and advances at the new frequency; unshifted
// regions keep their input phases exactly. On onsets every phase is reset to
// the input so attacks stay sharp. The frame is about 170 ms at any rate the
// effect runs at (8192 points at 44.1/48 kHz, 16384 at 88.2/96 kHz, 32768 at
// 176.4 kHz and above), so bins stay near 5.9 Hz and notes a semitone apart
// separate down to about 400 Hz whatever the oversampling. Latency is one frame.
#pragma once
#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <vector>

namespace atlas::spectral_lock::retune {
  struct Parameters {
    float amount = 1.0f;      // 0..1 how far a note moves toward its scale note; 1..2 also boosts retuned partials
    float pitch = 0.0f;       // semitones, applied to everything retuned
    float bend = 0.0f;        // -1..1 of bendRange
    float bendRange = 2.0f;
    float rangeLo = 65.0f;    // Hz: fundamentals and lone partials below are left alone
    float rangeHi = 2200.0f;  // Hz: and above (a note's harmonics follow their fundamental)
    float mix = 1.0f;
    float transients = 0.5f;  // 0..1 onset sensitivity for phase resets
    float reach = 3.0f;       // semitones a note may be moved; farther notes are left alone
    int root = 0;
    int scale = 0;
  };

  class Engine {
    public:
      static constexpr int kMinFftOrder = 13;
      static constexpr int kMaxFftOrder = 15;
      static constexpr int kMaxLatencySamples = 1 << kMaxFftOrder;
      // Frame order for a processing rate: about 8192 points per 48 kHz.
      static int fftOrderForRate(double sample_rate) {
        const double ratio = sample_rate > 1000.0 ? sample_rate / 48000.0 : 1.0;
        return std::clamp(kMinFftOrder + int(std::lround(std::log2(ratio))), kMinFftOrder, kMaxFftOrder);
      }
      static int latencyForRate(double sample_rate) { return 1 << fftOrderForRate(sample_rate); }
      static constexpr int kMaxPeaks = 512;
      static constexpr int kMaxNotes = 24;
      static constexpr int kMaxHarmonics = 32;
      static constexpr float kPeakFloorDb = -66.0f;   // below the frame's loudest peak
      static constexpr float kNoteFloorDb = -42.0f;   // a fundamental must be this strong

      struct Note {
        float frequency = 0.0f;   // measured fundamental, Hz
        float shift = 0.0f;       // semitones applied
        int partials = 0;
      };

      // Semitones from `midi` to the scale note the source's tables choose.
      static float snapSemitones(int scale, int root, float midi);

      Engine();
      // Allocates only when the frame size changes; call off the audio thread
      // for a new rate. reset() never allocates.
      void prepare(double sample_rate);
      void reset();
      int latencySamples() const { return fft_size_; }
      int fftSize() const { return fft_size_; }
      void process(const float* in_left, const float* in_right, float* out_left, float* out_right, int count,
                   const Parameters& parameters);
      double sampleRate() const { return sample_rate_; }
      // Analysis of the most recent frame, for tests and displays.
      int numNotes() const { return num_notes_; }
      const Note& note(int index) const { return notes_[index]; }
      int numPeaks() const { return num_peaks_; }
      bool lastFrameWasOnset() const { return onset_; }

    private:
      struct Peak {
        int bin = 0;              // magnitude maximum
        int low = 0, high = 0;    // region of bins, inclusive
        float frequency = 0.0f;   // instantaneous, Hz
        float magnitude = 0.0f;
        int note = -1;            // index into notes_, or -1
        float shift = 0.0f;       // semitones
        float gain = 1.0f;        // Amount above 1 boosts retuned partials
      };

      void processFrame();
      void analyse();
      void groupNotes();
      void decideShifts();
      void synthesise(std::vector<float>& fft, std::vector<float>& out_phase,
                      std::vector<int>& out_phase_frame, int channel);

      Parameters parameters_;
      double sample_rate_ = 48000.0;
      int fft_order_ = 0;
      int fft_size_ = 0;
      int hop_size_ = 0;
      int num_bins_ = 0;
      float bin_hz_ = 0.0f;
      std::unique_ptr<juce::dsp::FFT> fft_;
      std::vector<float> window_;
      std::vector<float> fft_left_, fft_right_, out_fft_left_, out_fft_right_;
      std::vector<float> in_left_, in_right_, out_left_, out_right_;
      int position_ = 0;
      int hop_position_ = 0;
      float mix_ = 1.0f;
      bool mix_initialised_ = false;

      // Analysis of the linked sum.
      std::vector<float> magnitude_, previous_magnitude_, phase_, previous_phase_;
      std::vector<float> frequency_;                      // instantaneous Hz per bin
      std::vector<float> magnitude_left_, magnitude_right_, phase_left_, phase_right_;
      // Phase accumulators live at each partial's output peak bin, stamped with
      // the frame that wrote them so a stale or overwritten bin is never trusted.
      std::vector<float> out_phase_left_, out_phase_right_;
      std::vector<int> out_phase_frame_left_, out_phase_frame_right_;
      int frame_count_ = 0;
      std::array<Peak, kMaxPeaks> peaks_ {};
      std::array<Note, kMaxNotes> notes_ {};
      int num_peaks_ = 0;
      int num_notes_ = 0;
      bool onset_ = false;
      bool first_frame_ = true;
  };
}
