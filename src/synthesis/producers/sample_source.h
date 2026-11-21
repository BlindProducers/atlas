/* Copyright 2013-2019 Matt Tytel
 *
 * vital is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * vital is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with vital.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "processor.h"
#include "nlohmann/json.hpp"
#include "utils.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include "cut_filter.h"
#include "slot_modulation.h"

using json = nlohmann::json;

namespace vital {

  // A slice of the Sample type's Slicer mode as it is edited and saved:
  // where it begins, in samples of the file, and its own pitch offset in
  // semitones, gain in dB and direction.
  struct SampleSlice {
    int start = 0;
    mono_float pitch = 0.0f;
    mono_float gain = 0.0f;
    bool reverse = false;
  };

  class Sample {
    public:
      static constexpr int kDefaultSampleLength = 44100;
      static constexpr int kUpsampleTimes = 1;
      static constexpr int kBufferSamples = 4;
      static constexpr int kMinSize = 4;
      // The most samples a file keeps per channel, two minutes at 48 kHz; a
      // longer file keeps its start.
      static constexpr int kMaxLength = 5760000;

      struct SampleData {
        SampleData(int l, int sr, bool s) : length(l), sample_rate(sr), stereo(s) { }
        
        int length;
        int sample_rate;
        bool stereo;
        mono_float peak = 0.0f;   // the loudest absolute sample, for Normalize
        std::vector<std::unique_ptr<mono_float[]>> left_buffers;
        std::vector<std::unique_ptr<mono_float[]>> left_loop_buffers;
        std::vector<std::unique_ptr<mono_float[]>> right_buffers;
        std::vector<std::unique_ptr<mono_float[]>> right_loop_buffers;

        JUCE_LEAK_DETECTOR(SampleData)
      };
    
      // `default_variant` picks which built-in default (a white noise of its
      // own) the sample starts with and init() returns to: an oscillator
      // slot's index, so no two slots of an instance play the same noise.
      explicit Sample(int default_variant = 0);

      using LoadCallback = std::function<void(const mono_float*, const mono_float*, int, int)>;

      void loadSample(const mono_float* buffer, int size, int sample_rate);
      void loadSample(const mono_float* left_buffer, const mono_float* right_buffer, int size, int sample_rate);
      // loadSample in two steps: building a long file's buffers takes a while
      // and touches nothing the audio thread reads, so it can run before
      // processing pauses for loadData to swap them in. A mono file has no
      // right buffer.
      static std::unique_ptr<SampleData> createData(const mono_float* left_buffer, const mono_float* right_buffer,
                                                    int size, int sample_rate);
      void loadData(std::unique_ptr<SampleData> data);
      // True while the sample is the built-in default from init(); presets skip it.
      bool isDefault() const { return is_default_; }
      void setLoadCallback(LoadCallback callback) { load_callback_ = std::move(callback); }
      void setName(const std::string& name) { name_ = name; }
      std::string getName() const { return name_; }
      void setLastBrowsedFile(const std::string& path) { last_browsed_file_ = path; }
      std::string getLastBrowsedFile() const { return last_browsed_file_; }

      force_inline int originalLength() const { return current_data_->length; }
      force_inline int upsampleLength() { return originalLength() * (1 << kUpsampleTimes); }
      force_inline int sampleRate() const { return current_data_->sample_rate; }

      force_inline int activeLength() const { return active_audio_data_.load()->length * (1 << kUpsampleTimes); }
      force_inline int activeSampleRate() const { return active_audio_data_.load()->sample_rate; }
      force_inline mono_float activePeak() const { return active_audio_data_.load()->peak; }

      force_inline const mono_float* buffer() const { return current_data_->left_buffers[kUpsampleTimes].get() + 1; }
      // The file's channels at its own rate, originalLength() samples each;
      // the right one is null for a mono file. Message thread.
      const mono_float* leftChannel() const { return current_data_->left_buffers[kUpsampleTimes].get() + kBufferSamples; }
      const mono_float* rightChannel() const {
        return current_data_->stereo ? current_data_->right_buffers[kUpsampleTimes].get() + kBufferSamples : nullptr;
      }
      void init();

      int getActiveIndex(mono_float delta) {
        int octaves = utils::ilog2(std::max<int>(delta, 1));
        return std::min(octaves, (int)active_audio_data_.load()->left_buffers.size() - 1);
      }

      force_inline const mono_float* getActiveLeftBuffer(int index) {
        VITAL_ASSERT(index >= 0 && index < active_audio_data_.load()->left_buffers.size());

        return active_audio_data_.load()->left_buffers[index].get();
      }

      force_inline const mono_float* getActiveLeftLoopBuffer(int index) {
        VITAL_ASSERT(index >= 0 && index < active_audio_data_.load()->left_loop_buffers.size());

        return active_audio_data_.load()->left_loop_buffers[index].get();
      }

      force_inline const mono_float* getActiveRightBuffer(int index) {
        if (active_audio_data_.load()->stereo) {
          VITAL_ASSERT(index >= 0 && index < active_audio_data_.load()->right_buffers.size());
          return active_audio_data_.load()->right_buffers[index].get();
        }
        return getActiveLeftBuffer(index);
      }

      force_inline const mono_float* getActiveRightLoopBuffer(int index) {
        if (active_audio_data_.load()->stereo) {
          VITAL_ASSERT(index >= 0 && index < active_audio_data_.load()->right_loop_buffers.size());
          return active_audio_data_.load()->right_loop_buffers[index].get();
        }
        return getActiveLeftLoopBuffer(index);
      }

      force_inline void markUsed() {
        active_audio_data_ = current_data_;
        active_slices_ = current_slices_.load(std::memory_order_acquire);
      }
      force_inline void markUnused() {
        active_audio_data_ = nullptr;
        active_slices_ = nullptr;
      }

      // The Slicer mode's slices: where each begins, in samples of the file,
      // ascending from 0; a slice ends where the next begins, the last at the
      // end of the file. Loading a file resets them to an even grid of
      // kDefaultSliceDivisions. setSlices runs on the message thread with
      // processing paused, since the audio thread reads the set it finds.
      static constexpr int kMaxSlices = 128;
      static constexpr int kMinSliceLength = 64;
      static constexpr int kDefaultSliceDivisions = 16;
      static constexpr mono_float kMaxSlicePitch = 24.0f;   // semitones, either way
      static constexpr mono_float kMinSliceGain = -24.0f;   // dB
      static constexpr mono_float kMaxSliceGain = 12.0f;
      using Slice = SampleSlice;
      // The slices for the audio thread, with each gain as an amplitude.
      struct Slices {
        int count = 0;
        int starts[kMaxSlices] = {};
        mono_float pitches[kMaxSlices] = {};
        mono_float gains[kMaxSlices] = {};
        mono_float reverses[kMaxSlices] = {};   // 1 for a slice that plays backwards
      };
      void setSlices(const std::vector<Slice>& slices);
      // `pitches` runs beside `starts`; a slice without one plays at the file's pitch.
      void setSlices(const std::vector<int>& starts, const std::vector<mono_float>& pitches = {});
      // The even grid a new file starts with.
      void resetSlices();
      std::vector<Slice> sliceList() const;
      std::vector<int> slices() const;
      std::vector<mono_float> slicePitches() const;
      // The slices as a preset holds them: {"starts": [...], "pitches": [...],
      // "gains": [...], "reverses": [...]}; reading also takes the earlier
      // plain list of starts.
      json slicesToJson() const;
      void slicesFromJson(const json& data);
      force_inline const Slices* activeSlices() const { return active_slices_.load(std::memory_order_relaxed); }

      json stateToJson();
      void jsonToState(json data);

    protected:
      std::string name_;
      std::string last_browsed_file_;
      SampleData* current_data_;
      std::atomic<SampleData*> active_audio_data_;
      // Shared while it is the built-in default (see init), which every
      // instance's samples of a variant hold; a loaded file's is this sample's alone.
      std::shared_ptr<SampleData> data_;
      std::unique_ptr<Slices> slices_;
      std::atomic<Slices*> current_slices_ { nullptr };
      std::atomic<Slices*> active_slices_ { nullptr };
      bool is_default_ = false;
      int default_variant_ = 0;
      LoadCallback load_callback_;

      void installData(std::shared_ptr<SampleData> data);

      JUCE_LEAK_DETECTOR(Sample)
  };

  class SampleSource : public Processor {
    public:
      static constexpr mono_float kMaxTranspose = 96.0f;
      static constexpr mono_float kMinTranspose = -96.0f;
      static constexpr mono_float kMaxAmplitude = 1.41421356237f;

      static constexpr int kNumDownsampleTaps = 55;
      static constexpr int kNumUpsampleTaps = 52;

      enum {
        kReset,
        kRetrigger,
        kVoiceEvent,
        kMidi,
        kKeytrack,
        kRootKey,
        kLevel,
        kRandomPhase,
        kTranspose,
        kTransposeQuantize,
        kTransposeQuantizeKey,
        kTransposeQuantizeScale,
        kTransposeQuantizeMode,
        kTune,
        kLoop,
        kBounce,
        kReverse,
        kNormalize,
        kPlaybackMode,
        kStart,
        kEnd,
        kLoopStart,
        kLoopEnd,
        kLoopShift,
        kLoopCrossfade,
        kLowCutoff,
        kHighCutoff,
        kPan,
        kNoteCount,
        kNotePressed,
        kFilterSlope,
        kDistortionType,
        kDistortionAmount,
        kSecondaryDistortionType,
        kSecondaryDistortionAmount,
        kActiveVoices,
        kBeatsPerSecond,
        kStretchMode,
        kBpm,
        kStretchLength,
        kFormants,
        kMode,
        kSliceLoop,
        kPlayThrough,
        kSliceStretchMode,
        kStutter,
        kStutterChance,
        kSliceDecay,
        kNumInputs
      };

      enum {
        kRaw,
        kLevelled,
        kNumOutputs
      };

      // Stretch mode: Off resamples, so the length follows the pitch; Tempo
      // stretches the file's beats (Sample BPM) to the host tempo; Fixed keeps
      // the file's own length, times Stretch length. Both shift the pitch
      // through the stretcher instead.
      enum StretchMode {
        kStretchOff,
        kStretchTempo,
        kStretchFixed,
        kNumStretchModes
      };
      // Mode: Classic plays the file between Start and End; Slicer plays the
      // slice a key picks, slice 1 on the root key and one per semitone up.
      enum Mode {
        kClassic,
        kSlicer,
        kNumModes
      };
      // Stutter: how many times a slice's first part plays over, inside the
      // slice's length (Off, 2, 3, 4, 6, 8, 12, 16).
      static constexpr int kNumStutterChoices = 8;
      static constexpr int kVoicesPerProcessor = poly_float::kSize / 2;

      SampleSource();
      explicit SampleSource(std::shared_ptr<Sample> sample);
      SampleSource(const SampleSource& other);
      ~SampleSource() override;

      virtual void process(int num_samples) override;
      virtual Processor* clone() const override { return new SampleSource(*this); }
      Sample* getSample() { return sample_.get(); }
      force_inline Output* getPhaseOutput() const { return phase_output_.get(); }
      SlotModulation& modulation() { return modulation_; }

      // The stretchers of this source and of every voice copy made from it: a
      // stereo one per voice, held only while a stretch mode is selected and
      // configured for the rate they met. Both allocate or free, so they run
      // on the message thread with processing paused, or offline.
      void setStretchMemory(bool reserve);
      // Whether a copy met a stretch mode in realtime without stretchers for
      // its rate (none yet, or the rate changed) since the last call.
      bool takeStretchMemoryRequest() { return stretch_shared_->requested.exchange(false, std::memory_order_acq_rel); }

    private:
      enum PlaybackMode {
        kRetriggerPerNote,
        kContinuousGlobal,
        kContinuousPerNote,
        kNumPlaybackModes
      };

      struct StretchState;   // the voices' stretchers and the buffers that feed them
      // What a source and its voice copies share.
      struct StretchShared {
        std::mutex mutex;
        std::vector<SampleSource*> sources;
        bool reserved = false;
        std::atomic<bool> requested { false };
      };

      poly_float snapTranspose(poly_float input_midi, poly_float transpose, int quantize);
      // Runs with stretch_shared_->mutex held.
      void reserveStretchVoices();
      // The stretchers for this block, or null to play plain: the mode is Off,
      // or they are not there yet (realtime asks the message thread for them).
      StretchState* stretchStateForBlock(int mode);
      void fadeFromPrevious(poly_float* raw_output, int num_samples, int lane, int start);

      poly_float pan_amplitude_;
      poly_float normalize_gain_ = 1.0f;
      int transpose_quantize_;
      poly_float last_quantized_transpose_;
      poly_float slice_index_;      // the slice each lane's note picked, -1 for none
      poly_float slice_pitch_;      // that slice's pitch offset, in semitones
      poly_float slice_gain_;       // that slice's gain, as an amplitude
      poly_mask slice_reverse_mask_; // the lanes whose slice plays backwards
      poly_float stutter_count_;    // how many times the lane's pass plays its slice's first part, 0 for once
      poly_float stutter_remaining_; // repeats still to come, -1 once they are over or without stutter
      // Each lane's copy of its voice's Stutter Chance sequence. Both lanes of
      // a voice start from one seed and step it at their own pass ends, so the
      // two channels always roll alike.
      std::array<uint32_t, poly_float::kSize> stutter_seed_ {};
      poly_mask reverse_mask_;      // which lanes read backwards last block
      poly_float sample_index_;
      poly_float sample_fraction_;
      poly_float global_sample_index_;
      poly_float global_sample_fraction_;
      poly_float phase_inc_;
      poly_mask bounce_mask_;
      CutFilter low_cut_{true};
      CutFilter high_cut_{false};
      poly_float last_output_;
      std::shared_ptr<cr::Output> phase_output_;
      utils::RandomGenerator random_generator_;
      SlotModulation modulation_;

      std::shared_ptr<Sample> sample_;
      std::shared_ptr<StretchShared> stretch_shared_;
      std::unique_ptr<StretchState> stretch_;
      int stretch_rate_ = 0;        // the rate the stretchers are configured for
      bool stretching_ = false;     // whether the last block went through them

      JUCE_LEAK_DETECTOR(SampleSource)
  };
} // namespace vital
