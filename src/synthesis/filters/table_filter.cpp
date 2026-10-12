// SPDX-License-Identifier: GPL-3.0-or-later
#include "table_filter.h"

#include "futils.h"
#include "poly_utils.h"
#include "utils.h"
#include "wavetable.h"

#include <algorithm>
#include <cmath>

namespace vital {
  using namespace atlas::filter_table;

  namespace {
    constexpr float kMinCutoffHz = 10.0f;
    constexpr float kMaxCutoffFraction = 0.45f;    // of the sample rate
    constexpr float kCutoffChange = 0.003f;        // relative, about 5 cents
    constexpr float kResonanceChange = 0.01f;
    constexpr float kFrameChange = 0.02f;

    force_inline poly_float voiceMask(int voice) {
      return voice == 0 ? poly_float(1.0f, 1.0f, 0.0f, 0.0f) : poly_float(0.0f, 0.0f, 1.0f, 1.0f);
    }
  }

  // Dot product of the last kBlock inputs (newest[-m]) with a head kernel, on
  // four accumulators so the FMAs pipeline.
  force_inline poly_float TableFilter::headDot(const poly_float* newest, const float* head) {
    poly_float a = 0.0f, b = 0.0f, c = 0.0f, d = 0.0f;
    for (int m = 0; m < kBlock; m += 4) {
      a = utils::mulAdd(a, newest[-m], head[m]);
      b = utils::mulAdd(b, newest[-m - 1], head[m + 1]);
      c = utils::mulAdd(c, newest[-m - 2], head[m + 2]);
      d = utils::mulAdd(d, newest[-m - 3], head[m + 3]);
    }
    return (a + b) + (c + d);
  }

  TableFilter::TableFilter() : Processor(kTableNumInputs, 1), source_(std::make_shared<Source>()) { }

  TableFilter::TableFilter(const TableFilter& other) :
      Processor(other), SynthFilter(other), mono_(other.mono_), source_(other.source_) {
    if (other.allocated_) allocate();
  }

  TableFilter::~TableFilter() = default;

  PhaseMode TableFilter::modeForStyle(int style, bool mono) {
    if (mono) return static_cast<PhaseMode>(std::clamp(style, 0, kNumPhaseModes - 1));
    return style <= 0 ? kMinimum : kRaw;
  }

  float TableFilter::resonanceFromPercent(float percent) {
    return std::exp2(4.0f * (std::clamp(percent, 0.0f, 1.0f) - 0.5f));
  }

  void TableFilter::defaultFrame(Frame& frame) {
    // A gentle low-pass: flat to Cutoff (harmonic 24), then 12 dB per octave down.
    for (int n = 1; n <= kNumHarmonics; ++n) {
      const float gain = n <= 24 ? 1.0f : (24.0f / n) * (24.0f / n);
      frame.harmonics[n] = gain / (kTilt * n);
    }
    frame.harmonics[0] = 0.0f;
  }

  void TableFilter::allocate() {
    size_ = kernelSize();
    partitions_ = size_ / kBlock;
    spectrum_floats_ = 2 * kBlock + 2;
    const int order = mono_ ? kFxOrder : kVoiceOrder;
    builder_ = std::make_unique<KernelBuilder>(order, order);
    fft_ = atlas::sharedFft(kBlockOrder + 1);
    history_.assign(4 * kBlock, 0.0f);      // written twice, so the last 2 * kBlock samples are contiguous
    for (auto& lane : fdl_) lane.assign(partitions_ * spectrum_floats_, 0.0f);
    for (auto& tail : tail_) tail.assign(kBlock, 0.0f);
    scratch_.assign(4 * kBlock, 0.0f);
    accumulator_.assign(spectrum_floats_, 0.0f);
    for (Voice& voice : voices_) {
      voice.kernel.assign(size_, 0.0f);
      for (auto& head : voice.head) head.assign(kBlock, 0.0f);
      for (auto& spectra : voice.spectra) spectra.assign(partitions_ * spectrum_floats_, 0.0f);
      voice.valid = false;
      voice.crossfading = false;
      voice.since_build = 1 << 30;
    }
    history_position_ = 0;
    block_fill_ = 0;
    fdl_position_ = 0;
    allocated_ = true;
  }

  void TableFilter::init() {
    Processor::init();
    if (!mono_ && !allocated_) allocate();
  }

  void TableFilter::prepare() {
    if (!allocated_) allocate();
  }

  int TableFilter::latencyForStyle(int style) const {
    return KernelBuilder::latencyForMode(modeForStyle(style, mono_), kernelSize());
  }

  int TableFilter::getLatencySamples() const {
    return latencyForStyle(static_cast<int>(input(kStyle)->at(0)[0] + 0.5f));
  }

  void TableFilter::loadFrame(float position, Frame& out, int& version) const {
    Wavetable* table = source_->table.load();
    if (table == nullptr) {
      defaultFrame(out);
      version = 0;
      return;
    }
    // Vital's protocol: mark the table in use while reading so an edit on the
    // message thread waits for us, then release it.
    table->markUsed();
    const Wavetable::WavetableData* data = table->getAllActiveData();
    if (data == nullptr) {
      table->markUnused();
      defaultFrame(out);
      version = 0;
      return;
    }
    version = data->version;
    const int frames = std::max(1, data->num_frames);
    const float clamped = std::clamp(position, 0.0f, float(frames - 1));
    const int a = int(clamped), b = std::min(a + 1, frames - 1);
    const float t = clamped - a;
    const auto* amplitudes_a = reinterpret_cast<const mono_float*>(data->frequency_amplitudes[a]);
    const auto* amplitudes_b = reinterpret_cast<const mono_float*>(data->frequency_amplitudes[b]);
    const auto* phases_a = reinterpret_cast<const std::complex<float>*>(data->normalized_frequencies[a]);
    const auto* phases_b = reinterpret_cast<const std::complex<float>*>(data->normalized_frequencies[b]);
    const float scale = 2.0f / kFrameSize;
    out.harmonics[0] = 0.0f;
    for (int n = 1; n <= kNumHarmonics; ++n) {
      const std::complex<float> value_a = phases_a[n] * (amplitudes_a[2 * n] * scale);
      const std::complex<float> value_b = phases_b[n] * (amplitudes_b[2 * n] * scale);
      out.harmonics[n] = value_a + (value_b - value_a) * t;
    }
    table->markUnused();
  }

  int TableFilter::activeVersion() const {
    Wavetable* table = source_->table.load();
    if (table == nullptr) return 0;
    table->markUsed();
    const Wavetable::WavetableData* data = table->getAllActiveData();
    const int version = data != nullptr ? data->version : 0;
    table->markUnused();
    return version;
  }

  bool TableFilter::parametersChanged(const Voice& voice, const Parameters& p, float frame, int version) const {
    if (!voice.valid || voice.table_version != version || voice.built.mode != p.mode) return true;
    if (std::abs(std::log(p.cutoff_hz / voice.built.cutoff_hz)) > kCutoffChange) return true;
    if (std::abs(p.resonance - voice.built.resonance) > kResonanceChange * voice.built.resonance) return true;
    return std::abs(frame - voice.built_frame) > kFrameChange;
  }

  void TableFilter::buildKernel(int index, const Parameters& p, float frame, int version) {
    Voice& voice = voices_[index];
    int loaded_version = 0;
    loadFrame(frame, frame_, loaded_version);
    builder_->build(frame_, p, voice.kernel.data());
    const int next = voice.valid ? 1 - voice.current : voice.current;
    std::copy_n(voice.kernel.begin(), kBlock, voice.head[next].begin());
    for (int partition = 1; partition < partitions_; ++partition) {
      std::copy_n(voice.kernel.begin() + partition * kBlock, kBlock, scratch_.begin());
      std::fill(scratch_.begin() + kBlock, scratch_.end(), 0.0f);
      fft_->performRealOnlyForwardTransform(scratch_.data(), true);
      std::copy_n(scratch_.begin(), spectrum_floats_, voice.spectra[next].begin() + partition * spectrum_floats_);
    }
    voice.crossfading = voice.valid;
    voice.current = next;
    voice.built = p;
    voice.built_frame = frame;
    voice.table_version = loaded_version;
    voice.since_build = 0;
    voice.valid = true;
  }

  void TableFilter::computeTail() {
    const int active_voices = mono_ ? 1 : kNumVoices;
    // Analyse the block that just completed, per lane: the newest sample sits at
    // index (position - 1) + 2 * kBlock, the oldest of the window 2 * kBlock - 1 before it.
    const poly_float* window = history_.data() + ((history_position_ - 1) & (2 * kBlock - 1)) + 1;
    for (int voice = 0; voice < active_voices; ++voice) {
      for (int channel = 0; channel < 2; ++channel) {
        const int lane = 2 * voice + channel;
        for (int i = 0; i < 2 * kBlock; ++i)
          scratch_[i] = window[i][lane];
        std::fill(scratch_.begin() + 2 * kBlock, scratch_.end(), 0.0f);
        fft_->performRealOnlyForwardTransform(scratch_.data(), true);
        std::copy_n(scratch_.begin(), spectrum_floats_, fdl_[lane].begin() + fdl_position_ * spectrum_floats_);
      }
    }
    // The next block's tail from partitions 1 .. partitions - 1, for the
    // current kernel set and, while crossfading, the previous one.
    for (int voice = 0; voice < active_voices; ++voice) {
      const Voice& state = voices_[voice];
      for (int set = 0; set < (state.crossfading ? 2 : 1); ++set) {
        const std::vector<float>& spectra = state.spectra[set == 0 ? state.current : 1 - state.current];
        for (int channel = 0; channel < 2; ++channel) {
          const int lane = 2 * voice + channel;
          std::fill(accumulator_.begin(), accumulator_.end(), 0.0f);
          for (int partition = 1; partition < partitions_; ++partition) {
            const int slot = (fdl_position_ - (partition - 1) + partitions_) % partitions_;
            const float* x = fdl_[lane].data() + slot * spectrum_floats_;
            const float* k = spectra.data() + partition * spectrum_floats_;
            for (int bin = 0; bin < spectrum_floats_; bin += 2) {
              accumulator_[bin] += x[bin] * k[bin] - x[bin + 1] * k[bin + 1];
              accumulator_[bin + 1] += x[bin] * k[bin + 1] + x[bin + 1] * k[bin];
            }
          }
          std::copy_n(accumulator_.begin(), spectrum_floats_, scratch_.begin());
          std::fill(scratch_.begin() + spectrum_floats_, scratch_.end(), 0.0f);
          fft_->performRealOnlyInverseTransform(scratch_.data());
          for (int i = 0; i < kBlock; ++i)
            tail_[set][i].set(lane, scratch_[kBlock + i]);
        }
      }
    }
    fdl_position_ = (fdl_position_ + 1) % partitions_;
  }

  void TableFilter::process(int num_samples) {
    processWithInput(input(kAudio)->source->buffer, num_samples);
  }

  void TableFilter::processWithInput(const poly_float* audio_in, int num_samples) {
    poly_float* audio_out = output()->buffer;
    if (!allocated_ || num_samples <= 0) {
      utils::copyBuffer(audio_out, audio_in, std::max(num_samples, 0));
      return;
    }
    filter_state_.loadSettings(this);
    const poly_float frame_input = input(kFrame)->at(0);
    const int active_voices = mono_ ? 1 : kNumVoices;
    const float sample_rate = float(getSampleRate());
    const int style = static_cast<int>(filter_state_.style);

    // A new note on a voice: drop the previous note's history and tails and
    // invalidate its kernel so one is built now from the new note's cutoff,
    // instead of playing the old note's response for the next few blocks.
    const poly_mask reset_mask = getResetMask(kReset);
    if (reset_mask.anyMask()) reset(reset_mask);

    // Per-voice targets from the newest cutoff in this block (the envelope has
    // moved by then; the first sample still holds the previous note's value right
    // after a note-on). A voice without a kernel builds one now, others at the
    // next block boundary if they changed and the rate limit allows.
    const poly_float midi_cutoff = filter_state_.midi_cutoff_buffer[num_samples - 1];
    Parameters targets[kNumVoices];
    float frames[kNumVoices] = {};
    const int version = activeVersion();
    for (int voice = 0; voice < active_voices; ++voice) {
      const int lane = 2 * voice;
      Parameters& p = targets[voice];
      p.cutoff_hz = std::clamp(utils::midiNoteToFrequency(midi_cutoff[lane]),
                               kMinCutoffHz, kMaxCutoffFraction * sample_rate);
      p.resonance = resonanceFromPercent(filter_state_.resonance_percent[lane]);
      p.mode = modeForStyle(style, mono_);
      p.sample_rate = sample_rate;
      frames[voice] = frame_input[lane];
      if (!voices_[voice].valid) {
        buildKernel(voice, p, frames[voice], version);
        // After a note-on the envelope is moving fastest and this block's cutoff
        // may still be the previous note's: let the next boundary rebuild at once.
        if (reset_mask[lane] || reset_mask[lane + 1]) voices_[voice].since_build = kMinSamplesBetweenBuilds;
      }
    }

    const int mask = 2 * kBlock - 1;
    for (int i = 0; i < num_samples; ++i) {
      const int position = history_position_ & mask;
      history_[position] = audio_in[i];
      history_[position + 2 * kBlock] = audio_in[i];
      const poly_float* newest = history_.data() + position + 2 * kBlock;   // newest[-m] is m samples back
      // Direct head: taps 0 .. kBlock - 1, per voice kernel, crossfaded when a
      // new kernel was installed at the last block boundary.
      poly_float current = 0.0f, previous = 0.0f;
      bool crossfading = false;
      for (int voice = 0; voice < active_voices; ++voice) {
        const Voice& state = voices_[voice];
        const poly_float lane_mask = voiceMask(voice);
        const poly_float sum = headDot(newest, state.head[state.current].data());
        current += sum * lane_mask;
        if (state.crossfading) {
          crossfading = true;
          previous += headDot(newest, state.head[1 - state.current].data()) * lane_mask;
        }
        else
          previous += sum * lane_mask;
      }
      poly_float result = current + tail_[0][block_fill_];
      if (crossfading) {
        const float weight = float(block_fill_ + 1) / float(kBlock);
        poly_float weights = 1.0f;
        for (int voice = 0; voice < active_voices; ++voice)
          if (voices_[voice].crossfading)
            weights = utils::maskLoad(weights, weight, poly_float::greaterThan(voiceMask(voice), poly_float(0.5f)));
        const poly_float old_result = previous + tail_[1][block_fill_];
        result = utils::interpolate(old_result, result, weights);
      }
      if (mono_) result = poly_float(result[0], result[1], result[0], result[1]);
      audio_out[i] = result;
      ++history_position_;
      if (++block_fill_ == kBlock) {
        block_fill_ = 0;
        for (int voice = 0; voice < active_voices; ++voice) {
          Voice& state = voices_[voice];
          state.crossfading = false;
          state.since_build += kBlock;
          if (state.since_build >= kMinSamplesBetweenBuilds && parametersChanged(state, targets[voice], frames[voice], version))
            buildKernel(voice, targets[voice], frames[voice], version);
        }
        computeTail();
      }
    }
  }

  void TableFilter::reset(poly_mask reset_mask) {
    if (!allocated_) return;
    for (poly_float& sample : history_) sample = utils::maskLoad(sample, 0.0f, reset_mask);
    for (auto& tail : tail_)
      for (poly_float& sample : tail) sample = utils::maskLoad(sample, 0.0f, reset_mask);
    for (int lane = 0; lane < poly_float::kSize; ++lane) {
      if (reset_mask[lane]) std::fill(fdl_[lane].begin(), fdl_[lane].end(), 0.0f);
    }
    // The voice's kernel belongs to the note that just ended; the next process
    // call builds a fresh one (without a crossfade, the history is empty).
    for (int voice = 0; voice < kNumVoices; ++voice) {
      if (reset_mask[2 * voice] || reset_mask[2 * voice + 1]) {
        voices_[voice].valid = false;
        voices_[voice].crossfading = false;
      }
    }
  }

  void TableFilter::hardReset() {
    reset(constants::kFullMask);
    for (Voice& voice : voices_) voice.crossfading = false;
  }
} // namespace vital
