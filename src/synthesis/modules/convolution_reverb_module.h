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

#include "synth_constants.h"
#include "synth_module.h"

#include "nlohmann/json.hpp"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

using json = nlohmann::json;

namespace vital {

  class ConvolutionReverbModule;

  // One IR rendering worker is shared by every convolution slot in a synth
  // instance. ReorderableEffectChain pre-constructs convolution modules for
  // inactive slots, so owning a thread in every module creates dozens of
  // unnecessary threads whenever a plug-in instance is constructed.
  class ConvolutionIrWorker {
    public:
      ConvolutionIrWorker();
      ~ConvolutionIrWorker();

      void registerModule(ConvolutionReverbModule* module);
      void unregisterModule(ConvolutionReverbModule* module);
      void requestWork();

      static int liveWorkerCount();

    private:
      void run();

      std::mutex mutex_;
      std::condition_variable signal_;
      std::vector<ConvolutionReverbModule*> modules_;
      ConvolutionReverbModule* current_module_ = nullptr;
      std::atomic<bool> work_requested_ { false };
      bool shutdown_ = false;
      std::thread thread_;

      static std::atomic<int> live_worker_count_;
  };

  // Convolution reverb based on the BP-Convolution engine: a partitioned
  // convolver with the impulse response re-rendered in the background when
  // the shaping controls (start, end, size, decay, dampening, reverse) move.
  class ConvolutionReverbModule : public SynthModule {
    public:
      static constexpr float kMaxIrSeconds = 30.0f;
      static constexpr int kMaxFilterPoles = 8;

      // Raw impulse response audio, before any shaping is applied.
      struct IrData {
        std::string name;
        int sample_rate = 0;
        std::vector<float> left;
        std::vector<float> right;  // empty for mono impulse responses

        bool stereo() const { return !right.empty(); }
        int length() const { return static_cast<int>(left.size()); }
      };

      // Predelay longer than this is clamped (synced values at slow tempos).
      static constexpr float kMaxPredelaySeconds = 2.0f;
      // Tone tilts the wet signal around this frequency, up to this many dB per side.
      static constexpr float kToneCrossoverHz = 800.0f;
      static constexpr float kToneMaxDb = 6.0f;

      ConvolutionReverbModule(std::string prefix = "",
                              std::shared_ptr<ConvolutionIrWorker> worker = nullptr, bool state_only = false,
                              const Output* beats_per_second = nullptr);
      // Predelay in seconds for the current controls and tempo.
      float predelaySeconds() const;
      virtual ~ConvolutionReverbModule();

      void init() override;
      void processWithInput(const poly_float* audio_in, int num_samples) override;
      void setSampleRate(int sample_rate) override;
      void setOversampleAmount(int oversampling) override;
      void hardReset() override;
      void enable(bool enable) override;

      Processor* clone() const override { VITAL_ASSERT(false); return nullptr; }

      // IR management. These are safe to call from the UI thread; the audio
      // thread is never blocked while an impulse response is prepared.
      bool hasCustomIr() const { return ir_source_->custom.load(); }
      std::string irName() const;
      json irStateToJson() const;
      bool irStateFromJson(const json& data);
      bool loadIrFromFile(const juce::File& file);
      enum class IrLoadResult { idle, loading, loaded, failed };
      struct IrLoadStatus { uint64_t request = 0; IrLoadResult result = IrLoadResult::idle; };
      uint64_t requestIrFile(const juce::File& file);
      IrLoadStatus irLoadStatus() const;
      bool saveIrToFile(const juce::File& file) const;
      void setDefaultIr();

      // UI-thread bookkeeping for folder browsing, like Sample's last file.
      void setLastBrowsedFile(const std::string& path) { last_browsed_file_ = path; }
      std::string getLastBrowsedFile() const { return last_browsed_file_; }

      const std::string& modulePrefix() const { return prefix_; }

      uint64_t irRevision() const { return ir_source_->revision.load(std::memory_order_acquire); }
      void copyIrFrom(const ConvolutionReverbModule& source);
      // Called before publishing prepared DSP to the audio thread. The source
      // remains alive independently of either module, and updates in place.
      void shareIrSourceFrom(const ConvolutionReverbModule& source);
      static std::shared_ptr<IrData> createDefaultIr();
      static std::shared_ptr<IrData> irFromJson(const json& data);
      static json irToJson(const IrData& ir);
      static std::shared_ptr<IrData> irFromAudioFile(const juce::File& file);

    private:
      struct OnePole {
        float z1 = 0.0f;
        float processLowPass(float x, float alpha) {
          z1 += alpha * (x - z1);
          return z1;
        }
        float processHighPass(float x, float alpha) {
          z1 += alpha * (x - z1);
          return x - z1;
        }
        void reset() { z1 = 0.0f; }
      };

      struct PreparedIr {
        juce::AudioBuffer<float> buffer;
        double sample_rate = 0.0;
        uint64_t revision = 0, generation = 0;
      };

      struct IrSource {
        mutable std::mutex mutex;
        std::shared_ptr<IrData> audio;
        std::atomic<bool> custom { false };
        std::atomic<uint64_t> revision { 0 };
      };

      void setIr(std::shared_ptr<IrData> ir, bool custom);
      void prepareConvolution();
      void applyPendingIr();
      void markIrDirty();
      void rebuildIr();
      void loadRequestedIr();
      static std::shared_ptr<IrData> readIrFile(const juce::File& file);
      void resetFilters();

      friend class ConvolutionIrWorker;

      Output* mix_;
      Output* ir_gain_;
      Output* width_;
      Output* low_cutoff_;
      Output* high_cutoff_;
      Value* filter_slope_;
      Value* start_;
      Value* end_;
      Value* size_;
      Value* decay_;
      Value* dampening_;
      Value* reverse_;
      Output* predelay_ = nullptr;
      Value* predelay_sync_ = nullptr;
      Value* predelay_tempo_ = nullptr;
      Value* fade_in_ = nullptr;
      Value* fade_out_ = nullptr;
      Output* tone_ = nullptr;
      const Output* beats_per_second_ = nullptr;
      std::string prefix_;

      bool state_only_;
      std::shared_ptr<IrSource> ir_source_ = std::make_shared<IrSource>();
      uint64_t seen_ir_revision_ = 0; // audio thread only
      // Each live DSP owns JUCE's queue: it only supports one producer, while
      // hosts process different Atlas instances on different audio threads.
      // Preparation/reset of another effect can also enqueue work. State-only
      // slots keep this null, so unused slots do not create loader threads.
      std::unique_ptr<juce::dsp::Convolution> convolution_;
      juce::AudioBuffer<float> wet_buffer_;
      bool prepared_;
      bool ir_build_requested_ = false;
      std::atomic<bool> ir_ready_;

      std::string last_browsed_file_;
      mutable std::mutex file_mutex_;
      juce::File requested_file_;
      IrLoadStatus file_status_;

      std::shared_ptr<ConvolutionIrWorker> worker_;
      std::atomic<bool> ir_dirty_;
      std::atomic<uint64_t> render_generation_ { 0 };

      // The worker renders and shapes IR audio, but JUCE requires the actual
      // loadImpulseResponse() call to be synchronised with process(). The
      // audio thread takes this buffer with a non-blocking try-lock.
      std::mutex pending_ir_mutex_;
      std::optional<PreparedIr> pending_ir_;

      std::atomic<float> last_start_;
      std::atomic<float> last_end_;
      std::atomic<float> last_size_;
      std::atomic<float> last_decay_;
      std::atomic<float> last_dampening_;
      std::atomic<float> last_reverse_;
      std::atomic<float> last_fade_in_ { 0.0f };
      std::atomic<float> last_fade_out_ { 0.0f };

      // Predelay: a delay line on the signal feeding the convolver, so it can
      // move while playing without rebuilding the impulse response.
      std::vector<float> predelay_buffer_[kNumChannels];
      int predelay_write_ = 0;
      float current_predelay_samples_ = 0.0f;
      OnePole tone_filter_[kNumChannels];
      float current_tone_ = 0.0f;

      OnePole low_pass_[kNumChannels][kMaxFilterPoles];
      OnePole high_pass_[kNumChannels][kMaxFilterPoles];
      poly_float mix_amount_;
      poly_float current_wet_gain_;

      JUCE_LEAK_DETECTOR(ConvolutionReverbModule)
  };
} // namespace vital
