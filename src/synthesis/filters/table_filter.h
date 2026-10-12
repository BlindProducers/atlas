// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "processor.h"
#include "synth_filter.h"
#include "filter_table/filter_table_kernel.h"

#include <atomic>
#include <memory>
#include <vector>

namespace vital {
  class Wavetable;

  // Filter Table: a wavetable frame's spectrum becomes the filter response
  // (see filter_table_kernel.h). The kernel is applied by zero-latency
  // partitioned convolution: the first 128 taps directly, the rest in
  // 128-sample partitions a block ahead. Lanes 0-1 are voice A, lanes 2-3 voice
  // B, each with its own kernel (cutoff, frame, resonance are per voice);
  // kernels are rebuilt at most every 256 samples and crossfaded over a block.
  class TableFilter : public Processor, public SynthFilter {
    public:
      enum { kFrame = SynthFilter::kNumInputs, kTableNumInputs };

      static constexpr int kBlockOrder = 7;
      static constexpr int kBlock = 1 << kBlockOrder;
      static constexpr int kVoiceOrder = 11;             // 2048 taps in the voice filters
      static constexpr int kFxOrder = 12;                // 4096 taps in Filter FX
      static constexpr int kMinSamplesBetweenBuilds = 256;
      static constexpr int kNumVoices = poly_float::kSize / 2;

      // Shared by a prototype and its voice clones: the table they read. The
      // synth owns the Wavetable for the plugin's lifetime.
      struct Source {
        std::atomic<Wavetable*> table { nullptr };
      };

      TableFilter();
      TableFilter(const TableFilter& other);
      ~TableFilter() override;
      Processor* clone() const override { return new TableFilter(*this); }

      // Mono (Filter FX) processes one stereo pair with the longer kernel and
      // offers all four phase modes; voice filters offer Minimum and Raw.
      void setMono(bool mono) { mono_ = mono; }
      bool isMono() const { return mono_; }
      static atlas::filter_table::PhaseMode modeForStyle(int style, bool mono);
      static int numStyles(bool mono) { return mono ? atlas::filter_table::kNumPhaseModes : 2; }
      static float resonanceFromPercent(float percent);   // 0..1 -> 0.25..4, 0.5 is neutral
      static void defaultFrame(atlas::filter_table::Frame& frame);

      void init() override;
      // Allocates the convolution and kernel buffers. Voice filters do this in
      // init(); Filter FX (mono) waits for the model to be selected, since
      // every effect slot owns one. Until then audio passes through.
      void prepare();
      bool isPrepared() const { return allocated_; }
      void process(int num_samples) override;
      void processWithInput(const poly_float* audio_in, int num_samples) override;
      void reset(poly_mask reset_mask) override;
      void hardReset() override;
      void setupFilter(const FilterState&) override { }

      void setWavetable(Wavetable* table) { source_->table.store(table); }
      Wavetable* getWavetable() const { return source_->table.load(); }
      int kernelSize() const { return 1 << (mono_ ? kFxOrder : kVoiceOrder); }
      // Latency for a style in this context (Linear and Original: half the kernel).
      int latencyForStyle(int style) const;
      int getLatencySamples() const;

      // For tests: the parameters of a voice's installed kernel and the kernel itself.
      const atlas::filter_table::Parameters& installedParameters(int voice) const { return voices_[voice].built; }
      const std::vector<float>& installedKernel(int voice) const { return voices_[voice].kernel; }
      bool hasKernel(int voice) const { return voices_[voice].valid; }

    private:
      struct Voice {
        atlas::filter_table::Parameters built;
        float built_frame = -1.0f;
        int table_version = -1;
        bool valid = false;
        int since_build = 1 << 30;
        std::vector<float> kernel;                 // full kernel (time domain)
        std::vector<float> head[2];                // first kBlock taps, current and previous
        std::vector<float> spectra[2];             // partition spectra p = 1 .. partitions - 1
        int current = 0;
        bool crossfading = false;                  // previous set still blended this block
      };

      void allocate();
      void loadFrame(float frame_position, atlas::filter_table::Frame& out, int& version) const;
      int activeVersion() const;
      bool parametersChanged(const Voice& voice, const atlas::filter_table::Parameters& p, float frame, int version) const;
      void buildKernel(int voice, const atlas::filter_table::Parameters& p, float frame, int version);
      void computeTail();
      static poly_float headDot(const poly_float* newest, const float* head);

      bool mono_ = false;
      bool allocated_ = false;
      int size_ = 0, partitions_ = 0, spectrum_floats_ = 0;
      std::shared_ptr<Source> source_;
      std::unique_ptr<atlas::filter_table::KernelBuilder> builder_;
      std::shared_ptr<const juce::dsp::FFT> fft_;
      atlas::filter_table::Frame frame_a_, frame_b_, frame_;
      Voice voices_[kNumVoices];

      // Convolution state.
      std::vector<poly_float> history_;             // 4 * kBlock: a 2 * kBlock ring written twice for contiguous reads
      int history_position_ = 0;
      int block_fill_ = 0;
      std::vector<float> fdl_[poly_float::kSize];   // per lane: partitions_ spectra
      int fdl_position_ = 0;
      std::vector<poly_float> tail_[2];             // next block's tail output, current and previous set
      std::vector<float> scratch_, accumulator_;
      poly_float head_scale_[2];

      JUCE_LEAK_DETECTOR(TableFilter)
  };
} // namespace vital
