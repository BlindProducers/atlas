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

#include "convolution_reverb_module.h"

#include "futils.h"
#include "poly_utils.h"
#include "synth_constants.h"

#include <algorithm>
#include <cmath>
#include <chrono>

namespace vital {

  namespace {
    constexpr int kConvolutionHeadSize = 512;
    constexpr int kDefaultIrSampleRate = 48000;
    constexpr float kDefaultIrSeconds = 1.5f;
    constexpr float kMinIrSamples = 100;
    constexpr int kMaxIrSampleRate = 768000;

    bool finiteIrSample(const float* value) {
      // The synthesis unity build uses fast-math, so inspect the IEEE bits.
      uint32_t bits;
      std::memcpy(&bits, value, sizeof(bits));
      return (bits & 0x7f800000u) != 0x7f800000u;
    }

    const std::shared_ptr<ConvolutionReverbModule::IrData>& sharedDefaultIr() {
      static std::shared_ptr<ConvolutionReverbModule::IrData> default_ir =
          ConvolutionReverbModule::createDefaultIr();
      return default_ir;
    }
  }

  std::atomic<int> ConvolutionIrWorker::live_worker_count_ { 0 };

  ConvolutionIrWorker::ConvolutionIrWorker() {
    // State-only slots also register here so file loading can run off the UI
    // thread. Leave room for the four chains and their live prepared engines.
    modules_.reserve(128);
    thread_ = std::thread([this] { run(); });
    live_worker_count_.fetch_add(1, std::memory_order_relaxed);
  }

  ConvolutionIrWorker::~ConvolutionIrWorker() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      shutdown_ = true;
    }
    signal_.notify_all();
    if (thread_.joinable())
      thread_.join();
    live_worker_count_.fetch_sub(1, std::memory_order_relaxed);
  }

  void ConvolutionIrWorker::registerModule(ConvolutionReverbModule* module) {
    if (module == nullptr)
      return;
    std::lock_guard<std::mutex> lock(mutex_);
    VITAL_ASSERT(std::find(modules_.begin(), modules_.end(), module) == modules_.end());
    modules_.push_back(module);
  }

  void ConvolutionIrWorker::unregisterModule(ConvolutionReverbModule* module) {
    std::unique_lock<std::mutex> lock(mutex_);
    modules_.erase(std::remove(modules_.begin(), modules_.end(), module), modules_.end());
    signal_.wait(lock, [this, module] { return current_module_ != module; });
  }

  void ConvolutionIrWorker::requestWork() {
    // Never take the registry mutex on the audio thread. The bounded wait in
    // run() covers notifications arriving just before the worker goes to sleep.
    work_requested_.store(true, std::memory_order_release);
    signal_.notify_one();
  }

  int ConvolutionIrWorker::liveWorkerCount() {
    return live_worker_count_.load(std::memory_order_relaxed);
  }

  void ConvolutionIrWorker::run() {
    std::unique_lock<std::mutex> lock(mutex_);
    size_t next_module = 0;
    while (!shutdown_) {
      signal_.wait_for(lock, std::chrono::milliseconds(20), [this] {
        return shutdown_ || work_requested_.load(std::memory_order_acquire);
      });
      if (shutdown_)
        break;

      if (!work_requested_.exchange(false, std::memory_order_acq_rel)) continue;
      while (!shutdown_) {
        ConvolutionReverbModule* module = nullptr;
        for (size_t offset = 0; offset < modules_.size(); ++offset) {
          const size_t index = (next_module + offset) % modules_.size();
          auto* candidate = modules_[index];
          if (candidate->ir_dirty_.exchange(false, std::memory_order_acq_rel)) {
            module = candidate;
            next_module = index + 1;
            break;
          }
        }

        if (module == nullptr) {
          // A request arriving while we scan either leaves this true and is
          // handled immediately, or arrives after the exchange and satisfies
          // the outer wait predicate. In both cases no dirty IR is stranded.
          if (work_requested_.exchange(false, std::memory_order_acq_rel))
            continue;
          break;
        }

        current_module_ = module;
        lock.unlock();
        module->loadRequestedIr();
        if (!module->state_only_) module->rebuildIr();
        lock.lock();
        current_module_ = nullptr;
        signal_.notify_all();
      }
    }

    current_module_ = nullptr;
    signal_.notify_all();
  }

  ConvolutionReverbModule::ConvolutionReverbModule(
      std::string prefix, std::shared_ptr<ConvolutionIrWorker> worker, bool state_only,
      const Output* beats_per_second) :
      SynthModule(0, 1), mix_(nullptr), ir_gain_(nullptr), width_(nullptr),
      low_cutoff_(nullptr), high_cutoff_(nullptr), filter_slope_(nullptr),
      start_(nullptr), end_(nullptr), size_(nullptr), decay_(nullptr),
      dampening_(nullptr), reverse_(nullptr), beats_per_second_(beats_per_second), prefix_(prefix),
      state_only_(state_only),
      convolution_(state_only ? nullptr : std::make_unique<juce::dsp::Convolution>(
          juce::dsp::Convolution::NonUniform { kConvolutionHeadSize })),
      prepared_(false), ir_ready_(false),
      worker_(worker ? std::move(worker) : std::make_shared<ConvolutionIrWorker>()),
      ir_dirty_(false),
      last_start_(0.0f), last_end_(1.0f), last_size_(1.0f), last_decay_(1.0f),
      last_dampening_(0.0f), last_reverse_(0.0f),
      mix_amount_(0.0f), current_wet_gain_(1.0f) {
    ir_source_->audio = sharedDefaultIr();
    if (!state_only_) {
      wet_buffer_.setSize(kNumChannels, kMaxBufferSize);
    }
    worker_->registerModule(this);
  }

  ConvolutionReverbModule::~ConvolutionReverbModule() {
    worker_->unregisterModule(this);
  }

  void ConvolutionReverbModule::init() {
    mix_ = createMonoModControl(prefix_ + "convolution_reverb_dry_wet");
    ir_gain_ = createMonoModControl(prefix_ + "convolution_reverb_ir_gain");
    width_ = createMonoModControl(prefix_ + "convolution_reverb_width");
    low_cutoff_ = createMonoModControl(prefix_ + "convolution_reverb_low_cutoff");
    high_cutoff_ = createMonoModControl(prefix_ + "convolution_reverb_high_cutoff");
    filter_slope_ = createBaseControl(prefix_ + "convolution_reverb_filter_slope");
    start_ = createBaseControl(prefix_ + "convolution_reverb_ir_start");
    end_ = createBaseControl(prefix_ + "convolution_reverb_ir_end");
    size_ = createBaseControl(prefix_ + "convolution_reverb_ir_size");
    decay_ = createBaseControl(prefix_ + "convolution_reverb_ir_decay");
    dampening_ = createBaseControl(prefix_ + "convolution_reverb_ir_dampening");
    reverse_ = createBaseControl(prefix_ + "convolution_reverb_ir_reverse");
    predelay_ = createMonoModControl(prefix_ + "convolution_reverb_predelay");
    predelay_sync_ = createBaseControl(prefix_ + "convolution_reverb_predelay_sync");
    predelay_tempo_ = createBaseControl(prefix_ + "convolution_reverb_predelay_tempo");
    fade_in_ = createBaseControl(prefix_ + "convolution_reverb_fade_in");
    fade_out_ = createBaseControl(prefix_ + "convolution_reverb_fade_out");
    tone_ = createMonoModControl(prefix_ + "convolution_reverb_tone");

    {
      std::lock_guard<std::mutex> lock(ir_source_->mutex);
      if (ir_source_->audio == nullptr)
        ir_source_->audio = sharedDefaultIr();
    }
    SynthModule::init();
  }

  void ConvolutionReverbModule::setSampleRate(int sample_rate) {
    SynthModule::setSampleRate(sample_rate);
    prepareConvolution();
  }

  void ConvolutionReverbModule::setOversampleAmount(int oversampling) {
    SynthModule::setOversampleAmount(oversampling);
    prepareConvolution();
  }

  void ConvolutionReverbModule::prepareConvolution() {
    if (state_only_) return;
    const int max_block_size = kMaxBufferSize * getOversampleAmount();
    wet_buffer_.setSize(kNumChannels, max_block_size, false, false, true);
    const size_t predelay_size = static_cast<size_t>(kMaxPredelaySeconds * getSampleRate()) + max_block_size + 4;
    for (auto& line : predelay_buffer_)
      line.assign(predelay_size, 0.0f);
    predelay_write_ = 0;

    juce::dsp::ProcessSpec spec;
    spec.sampleRate = getSampleRate();
    spec.maximumBlockSize = static_cast<juce::uint32>(max_block_size);
    spec.numChannels = kNumChannels;
    convolution_->prepare(spec);
    prepared_ = true;

    // Only rebuild an impulse response that was already in use. The effect
    // chain constructs many convolution instances; building IRs for all of
    // them up front makes plugin instantiation take seconds.
    if (ir_ready_.load())
      markIrDirty();
  }

  float ConvolutionReverbModule::predelaySeconds() const {
    const int sync = predelay_sync_ != nullptr ? utils::iclamp(static_cast<int>(predelay_sync_->value() + 0.5f), 0, 3) : 0;
    float seconds = 0.0f;
    if (sync == 0)
      seconds = predelay_ != nullptr ? predelay_->buffer[0][0] : 0.0f;
    else {
      const int tempo = predelay_tempo_ != nullptr ?
          utils::iclamp(static_cast<int>(predelay_tempo_->value() + 0.5f), 1, constants::kNumSyncedFrequencyRatios - 1) : 10;
      const float beats_per_second = beats_per_second_ != nullptr ? beats_per_second_->buffer[0][0] : 2.0f;
      // A synced ratio is cycles per beat; its length is the inverse.
      seconds = 1.0f / (constants::kSyncedFrequencyRatios[tempo] * utils::max(beats_per_second, 0.01f));
      if (sync == 2) seconds *= 1.5f;
      else if (sync == 3) seconds *= 2.0f / 3.0f;
    }
    return utils::clamp(seconds, 0.0f, kMaxPredelaySeconds);
  }

  void ConvolutionReverbModule::processWithInput(const poly_float* audio_in, int num_samples) {
    if (state_only_ || num_samples <= 0) return;
    SynthModule::process(num_samples);

    const uint64_t revision = irRevision();
    if (revision != seen_ir_revision_) {
      seen_ir_revision_ = revision;
      markIrDirty();
    }

    const float start = start_->value();
    const float end = end_->value();
    const float size = size_->value();
    const float decay = decay_->value();
    const float dampening = dampening_->value();
    const float reverse = reverse_->value();
    const float fade_in = fade_in_->value();
    const float fade_out = fade_out_->value();
    if (fade_in != last_fade_in_.load(std::memory_order_relaxed) ||
        fade_out != last_fade_out_.load(std::memory_order_relaxed)) {
      last_fade_in_.store(fade_in, std::memory_order_relaxed);
      last_fade_out_.store(fade_out, std::memory_order_relaxed);
      markIrDirty();
    }
    if (start != last_start_.load(std::memory_order_relaxed) ||
        end != last_end_.load(std::memory_order_relaxed) ||
        size != last_size_.load(std::memory_order_relaxed) ||
        decay != last_decay_.load(std::memory_order_relaxed) ||
        dampening != last_dampening_.load(std::memory_order_relaxed) ||
        reverse != last_reverse_.load(std::memory_order_relaxed)) {
      last_start_.store(start, std::memory_order_relaxed);
      last_end_.store(end, std::memory_order_relaxed);
      last_size_.store(size, std::memory_order_relaxed);
      last_decay_.store(decay, std::memory_order_relaxed);
      last_dampening_.store(dampening, std::memory_order_relaxed);
      last_reverse_.store(reverse, std::memory_order_relaxed);
      markIrDirty();
    }

    poly_float current_mix = mix_amount_;
    const poly_float target_mix = utils::clamp(mix_->buffer[0], 0.0f, 1.0f);
    const poly_float delta_mix = (target_mix - current_mix) * (1.0f / num_samples);

    poly_float wet_gain = current_wet_gain_;
    const poly_float target_wet_gain = futils::dbToMagnitude(ir_gain_->buffer[0]);
    const poly_float delta_wet_gain = (target_wet_gain - wet_gain) * (1.0f / num_samples);

    poly_float* audio_out = output()->buffer;

    // First time this instance actually processes audio, kick off the IR
    // build in the background. Until it lands, pass the dry signal through.
    if (!ir_ready_.load() && !ir_build_requested_ && prepared_) {
      ir_build_requested_ = true;
      markIrDirty();
    }

    if (prepared_)
      applyPendingIr();

    if (!ir_ready_.load() || !prepared_) {
      // An asynchronous IR build must never turn a 100% wet effect into a
      // temporary mute. Keep the dry path intact until JUCE has accepted an
      // IR, then its own convolver crossfade will transition to the new one.
      utils::copyBuffer(audio_out, audio_in, num_samples);
      mix_amount_ = target_mix;
      current_wet_gain_ = target_wet_gain;
      return;
    }

    float* left = wet_buffer_.getWritePointer(0);
    float* right = wet_buffer_.getWritePointer(1);
    const int line_size = static_cast<int>(predelay_buffer_[0].size());
    const float target_predelay = predelaySeconds() * getSampleRate();
    if (line_size > 0 && (target_predelay > 0.5f || current_predelay_samples_ > 0.5f)) {
      // Glide the delay across the block so a change never clicks.
      const float start_delay = current_predelay_samples_;
      const float step = (target_predelay - start_delay) / num_samples;
      for (int i = 0; i < num_samples; ++i) {
        predelay_buffer_[0][static_cast<size_t>(predelay_write_)] = audio_in[i][0];
        predelay_buffer_[1][static_cast<size_t>(predelay_write_)] = audio_in[i][1];
        const float delay = utils::clamp(start_delay + step * (i + 1), 0.0f, static_cast<float>(line_size - 2));
        float read = static_cast<float>(predelay_write_) - delay;
        if (read < 0.0f) read += line_size;
        const int index = static_cast<int>(read);
        const int next = index + 1 >= line_size ? 0 : index + 1;
        const float fraction = read - index;
        left[i] = predelay_buffer_[0][static_cast<size_t>(index)] * (1.0f - fraction) +
                  predelay_buffer_[0][static_cast<size_t>(next)] * fraction;
        right[i] = predelay_buffer_[1][static_cast<size_t>(index)] * (1.0f - fraction) +
                   predelay_buffer_[1][static_cast<size_t>(next)] * fraction;
        if (++predelay_write_ >= line_size) predelay_write_ = 0;
      }
      current_predelay_samples_ = target_predelay;
    }
    else {
      for (int i = 0; i < num_samples; ++i) {
        left[i] = audio_in[i][0];
        right[i] = audio_in[i][1];
        if (line_size > 0) {
          // Keep the line current so turning predelay up never replays old audio.
          predelay_buffer_[0][static_cast<size_t>(predelay_write_)] = audio_in[i][0];
          predelay_buffer_[1][static_cast<size_t>(predelay_write_)] = audio_in[i][1];
          if (++predelay_write_ >= line_size) predelay_write_ = 0;
        }
      }
      current_predelay_samples_ = target_predelay;
    }

    juce::dsp::AudioBlock<float> block(wet_buffer_);
    juce::dsp::AudioBlock<float> sub_block = block.getSubBlock(0, static_cast<size_t>(num_samples));
    juce::dsp::ProcessContextReplacing<float> context(sub_block);
    convolution_->process(context);

    const float sample_rate = getSampleRate();
    const int num_poles = utils::iclamp(static_cast<int>(filter_slope_->value() + 0.5f) + 1,
                                        1, kMaxFilterPoles);
    const float high_cutoff_hz = utils::midiNoteToFrequency(high_cutoff_->buffer[0][0]);
    const float low_cutoff_hz = utils::midiNoteToFrequency(low_cutoff_->buffer[0][0]);
    const float lp_alpha = 1.0f - std::exp(-2.0f * kPi * utils::min(high_cutoff_hz, sample_rate * 0.49f) / sample_rate);
    const float hp_alpha = 1.0f - std::exp(-2.0f * kPi * utils::min(low_cutoff_hz, sample_rate * 0.49f) / sample_rate);
    const float stereo_width = utils::clamp(width_->buffer[0][0], 0.0f, 2.0f);
    // Tone: a tilt around kToneCrossoverHz, darker below zero, brighter above.
    const float target_tone = utils::clamp(tone_->buffer[0][0], -1.0f, 1.0f);
    const float tone_step = (target_tone - current_tone_) / num_samples;
    float tone = current_tone_;
    const float tone_alpha = 1.0f - std::exp(-2.0f * kPi * kToneCrossoverHz / sample_rate);
    const bool tone_active = std::abs(target_tone) > 1.0e-4f || std::abs(current_tone_) > 1.0e-4f;

    for (int i = 0; i < num_samples; ++i) {
      current_mix += delta_mix;
      wet_gain += delta_wet_gain;

      float l = left[i];
      float r = right[i];
      for (int p = 0; p < num_poles; ++p) {
        l = high_pass_[0][p].processHighPass(low_pass_[0][p].processLowPass(l, lp_alpha), hp_alpha);
        r = high_pass_[1][p].processHighPass(low_pass_[1][p].processLowPass(r, lp_alpha), hp_alpha);
      }
      if (tone_active) {
        tone += tone_step;
        const float low_gain = utils::dbToMagnitude(-tone * kToneMaxDb);
        const float high_gain = utils::dbToMagnitude(tone * kToneMaxDb);
        const float low_l = tone_filter_[0].processLowPass(l, tone_alpha);
        const float low_r = tone_filter_[1].processLowPass(r, tone_alpha);
        l = low_l * low_gain + (l - low_l) * high_gain;
        r = low_r * low_gain + (r - low_r) * high_gain;
      }

      const float mid = (l + r) * 0.5f;
      const float side = (l - r) * 0.5f * stereo_width;

      poly_float wet(mid + side, mid - side, mid + side, mid - side);
      audio_out[i] = utils::interpolate(audio_in[i], wet * wet_gain, current_mix);
    }

    mix_amount_ = target_mix;
    current_wet_gain_ = target_wet_gain;
    current_tone_ = target_tone;
  }

  void ConvolutionReverbModule::hardReset() {
    if (convolution_) convolution_->reset();
    resetFilters();
    mix_amount_ = 0.0f;
    current_wet_gain_ = 1.0f;
  }

  void ConvolutionReverbModule::enable(bool enable) {
    SynthModule::enable(enable);
    if (enable) {
      if (!ir_ready_.load())
        markIrDirty();
    }
    else {
      if (convolution_) convolution_->reset();
      resetFilters();
    }
  }

  void ConvolutionReverbModule::resetFilters() {
    for (auto& line : predelay_buffer_)
      std::fill(line.begin(), line.end(), 0.0f);
    current_predelay_samples_ = predelaySeconds() * getSampleRate();
    for (auto& filter : tone_filter_)
      filter.reset();
    for (int channel = 0; channel < kNumChannels; ++channel) {
      for (int pole = 0; pole < kMaxFilterPoles; ++pole) {
        low_pass_[channel][pole].reset();
        high_pass_[channel][pole].reset();
      }
    }
  }

  std::string ConvolutionReverbModule::irName() const {
    std::lock_guard<std::mutex> lock(ir_source_->mutex);
    return ir_source_->audio ? ir_source_->audio->name : "";
  }

  void ConvolutionReverbModule::setIr(std::shared_ptr<IrData> ir, bool custom) {
    if (ir == nullptr || ir->length() < kMinIrSamples || ir->sample_rate <= 0)
      return;

    std::lock_guard<std::mutex> file_lock(file_mutex_);
    ++file_status_.request; // A preset/reset supersedes any in-flight file load.
    file_status_.result = IrLoadResult::idle;
    {
      std::lock_guard<std::mutex> lock(ir_source_->mutex);
      ir_source_->audio = std::move(ir);
      ir_source_->custom = custom;
      ir_source_->revision.fetch_add(1, std::memory_order_release);
    }
  }

  void ConvolutionReverbModule::shareIrSourceFrom(const ConvolutionReverbModule& source) {
    ir_source_ = source.ir_source_;
  }

  void ConvolutionReverbModule::copyIrFrom(const ConvolutionReverbModule& source) {
    std::shared_ptr<IrData> ir;
    bool custom;
    {
      std::lock_guard<std::mutex> lock(source.ir_source_->mutex);
      ir = source.ir_source_->audio;
      custom = source.ir_source_->custom.load();
    }
    setIr(std::move(ir), custom);
  }

  void ConvolutionReverbModule::setDefaultIr() {
    // Always invalidate file requests, even if the default is already current.
    // A worker may be about to publish a custom IR when the reset arrives.
    setIr(sharedDefaultIr(), false);
  }

  json ConvolutionReverbModule::irStateToJson() const {
    std::shared_ptr<IrData> ir;
    {
      std::lock_guard<std::mutex> lock(ir_source_->mutex);
      ir = ir_source_->audio;
    }
    return ir ? irToJson(*ir) : json();
  }

  bool ConvolutionReverbModule::irStateFromJson(const json& data) {
    std::shared_ptr<IrData> ir = irFromJson(data);
    if (ir == nullptr)
      return false;
    setIr(std::move(ir), true);
    return true;
  }

  bool ConvolutionReverbModule::loadIrFromFile(const juce::File& file) {
    auto ir = readIrFile(file);
    if (ir == nullptr) return false;
    setIr(std::move(ir), true);
    return true;
  }

  uint64_t ConvolutionReverbModule::requestIrFile(const juce::File& file) {
    uint64_t request;
    {
      std::lock_guard<std::mutex> lock(file_mutex_);
      requested_file_ = file;
      request = ++file_status_.request;
      file_status_.result = IrLoadResult::loading;
    }
    ir_dirty_.store(true, std::memory_order_release);
    worker_->requestWork();
    return request;
  }

  ConvolutionReverbModule::IrLoadStatus ConvolutionReverbModule::irLoadStatus() const {
    std::lock_guard<std::mutex> lock(file_mutex_);
    return file_status_;
  }

  void ConvolutionReverbModule::loadRequestedIr() {
    juce::File file;
    uint64_t request;
    {
      std::lock_guard<std::mutex> lock(file_mutex_);
      if (file_status_.result != IrLoadResult::loading) return;
      file = requested_file_;
      request = file_status_.request;
    }
    auto ir = readIrFile(file);
    std::lock_guard<std::mutex> lock(file_mutex_);
    if (request != file_status_.request) return;
    if (ir) {
      std::lock_guard<std::mutex> source_lock(ir_source_->mutex);
      ir_source_->audio = std::move(ir);
      ir_source_->custom = true;
      ir_source_->revision.fetch_add(1, std::memory_order_release);
      file_status_.result = IrLoadResult::loaded;
    }
    else file_status_.result = IrLoadResult::failed;
  }

  std::shared_ptr<ConvolutionReverbModule::IrData>
  ConvolutionReverbModule::readIrFile(const juce::File& file) {
    std::shared_ptr<IrData> ir;
    try {
      if (file.getFileExtension().toLowerCase() == String(".") + String(vital::kIrExtension)) {
        if (file.getSize() > 128 * 1024 * 1024) return nullptr;
        json data = json::parse(file.loadFileAsString().toStdString(), nullptr, false);
        if (data.is_discarded())
          return nullptr;
        ir = irFromJson(data);
      }
      else ir = irFromAudioFile(file);
    }
    catch (const std::exception&) { return nullptr; }
    return ir;
  }

  bool ConvolutionReverbModule::saveIrToFile(const juce::File& file) const {
    json data = irStateToJson();
    if (data.is_null())
      return false;

    File destination = file;
    if (destination.getFileExtension().toLowerCase() != String(".") + String(vital::kIrExtension))
      destination = destination.withFileExtension(String(vital::kIrExtension));
    return destination.replaceWithText(data.dump());
  }

  void ConvolutionReverbModule::markIrDirty() {
    if (state_only_) return;
    render_generation_.fetch_add(1, std::memory_order_release);
    ir_dirty_.store(true, std::memory_order_release);
    worker_->requestWork();
  }

  void ConvolutionReverbModule::applyPendingIr() {
    std::optional<PreparedIr> pending;
    {
      std::unique_lock<std::mutex> lock(pending_ir_mutex_, std::try_to_lock);
      if (!lock.owns_lock() || !pending_ir_.has_value())
        return;
      if (pending_ir_->revision != irRevision() ||
          pending_ir_->generation != render_generation_.load(std::memory_order_acquire))
        return; // Let the worker replace/release stale audio off the audio thread.

      pending.emplace(std::move(*pending_ir_));
      pending_ir_.reset();
    }

    // JUCE's load call is wait-free, but must run on the same (audio) thread
    // as process() so the two calls are never interleaved across threads.
    convolution_->loadImpulseResponse(std::move(pending->buffer), pending->sample_rate,
                                     juce::dsp::Convolution::Stereo::yes,
                                     juce::dsp::Convolution::Trim::no,
                                     juce::dsp::Convolution::Normalise::yes);
    ir_ready_.store(true, std::memory_order_release);
  }

  void ConvolutionReverbModule::rebuildIr() {
    const auto generation = render_generation_.load(std::memory_order_acquire);
    uint64_t revision;
    std::shared_ptr<IrData> source;
    {
      std::lock_guard<std::mutex> lock(ir_source_->mutex);
      source = ir_source_->audio;
      revision = irRevision();
    }
    if (source == nullptr || source->length() < kMinIrSamples)
      return;

    const float raw_start = last_start_.load(std::memory_order_relaxed);
    const float raw_end = last_end_.load(std::memory_order_relaxed);
    const float start = utils::min(raw_start, raw_end);
    const float end = utils::max(raw_start, raw_end);
    const float stretch = utils::clamp(last_size_.load(std::memory_order_relaxed), 0.1f, 4.0f);
    const float decay = utils::clamp(last_decay_.load(std::memory_order_relaxed), 0.0f, 1.0f);
    const float dampening = utils::clamp(last_dampening_.load(std::memory_order_relaxed), 0.0f, 1.0f);
    const bool reverse = last_reverse_.load(std::memory_order_relaxed) > 0.5f;

    const int total_samples = source->length();
    int start_sample = utils::iclamp(static_cast<int>(total_samples * start), 0, total_samples - 1);
    int trimmed_length = static_cast<int>(total_samples * end) - start_sample;
    trimmed_length = utils::iclamp(trimmed_length, static_cast<int>(kMinIrSamples),
                                   total_samples - start_sample);
    trimmed_length = std::min(trimmed_length, total_samples - start_sample);

    int length = trimmed_length;
    if (std::abs(stretch - 1.0f) > 0.01f)
      length = utils::iclamp(static_cast<int>(trimmed_length * stretch),
                             static_cast<int>(kMinIrSamples),
                             static_cast<int>(kMaxIrSeconds * source->sample_rate));

    juce::AudioBuffer<float> buffer(kNumChannels, length);
    const double position_scale = static_cast<double>(trimmed_length - 1) / std::max(1, length - 1);
    for (int channel = 0; channel < kNumChannels; ++channel) {
      const std::vector<float>& data = (channel == 1 && source->stereo()) ? source->right : source->left;
      float* dest = buffer.getWritePointer(channel);
      for (int i = 0; i < length; ++i) {
        if ((i & 4095) == 0 && (revision != irRevision() ||
            generation != render_generation_.load(std::memory_order_acquire))) return;
        const double source_position = start_sample + i * position_scale;
        const int index = utils::iclamp(static_cast<int>(source_position), 0, total_samples - 1);
        const int next = std::min(index + 1, total_samples - 1);
        const float fraction = static_cast<float>(source_position - index);
        dest[i] = data[index] * (1.0f - fraction) + data[next] * fraction;
      }
    }

    if (reverse) {
      for (int channel = 0; channel < kNumChannels; ++channel)
        buffer.reverse(channel, 0, length);
    }

    if (decay < 0.99f) {
      const float power = 1.0f / std::max(0.01f, decay);
      for (int channel = 0; channel < kNumChannels; ++channel) {
        float* data = buffer.getWritePointer(channel);
        for (int i = 0; i < length; ++i) {
          const float position = i / static_cast<float>(length);
          data[i] *= std::pow(1.0f - position, power);
        }
      }
    }

    // Fade in shapes the attack of the impulse response, Fade out its end;
    // together they never cover more than the whole response.
    float fade_in = utils::clamp(last_fade_in_.load(std::memory_order_relaxed), 0.0f, 1.0f);
    float fade_out = utils::clamp(last_fade_out_.load(std::memory_order_relaxed), 0.0f, 1.0f);
    if (fade_in + fade_out > 1.0f) {
      const float scale = 1.0f / (fade_in + fade_out);
      fade_in *= scale;
      fade_out *= scale;
    }
    const int fade_in_samples = static_cast<int>(fade_in * length);
    const int fade_out_samples = static_cast<int>(fade_out * length);
    for (int channel = 0; channel < kNumChannels; ++channel) {
      float* data = buffer.getWritePointer(channel);
      for (int i = 0; i < fade_in_samples; ++i)
        data[i] *= 0.5f - 0.5f * std::cos(kPi * (i + 0.5f) / fade_in_samples);
      for (int i = 0; i < fade_out_samples; ++i)
        data[length - 1 - i] *= 0.5f - 0.5f * std::cos(kPi * (i + 0.5f) / fade_out_samples);
    }

    if (dampening > 0.01f) {
      const float floor_frequency = 200.0f + (1.0f - dampening) * 19800.0f;
      OnePole damp_filters[kNumChannels];
      for (int i = 0; i < length; ++i) {
        const float position = i / static_cast<float>(length);
        const float cutoff = 20000.0f + (floor_frequency - 20000.0f) * position;
        const float alpha = 1.0f - std::exp(-2.0f * kPi * cutoff / source->sample_rate);
        for (int channel = 0; channel < kNumChannels; ++channel) {
          float* data = buffer.getWritePointer(channel);
          data[i] = damp_filters[channel].processLowPass(data[i], alpha);
        }
      }
    }

    {
      std::lock_guard<std::mutex> lock(pending_ir_mutex_);
      if (revision != irRevision() || generation != render_generation_.load(std::memory_order_acquire))
        return;
      pending_ir_.emplace(PreparedIr { std::move(buffer), static_cast<double>(source->sample_rate),
                                     revision, generation });
    }
  }

  std::shared_ptr<ConvolutionReverbModule::IrData> ConvolutionReverbModule::createDefaultIr() {
    auto ir = std::make_shared<IrData>();
    ir->name = "Default Hall";
    ir->sample_rate = kDefaultIrSampleRate;

    const int length = static_cast<int>(kDefaultIrSeconds * kDefaultIrSampleRate);
    ir->left.resize(length);
    ir->right.resize(length);

    // Exponentially decaying noise with a progressive high frequency rolloff.
    uint32_t seed = 0x8f3a71c5;
    auto next_random = [&seed]() {
      seed = seed * 1664525u + 1013904223u;
      return static_cast<int32_t>(seed >> 8) / static_cast<float>(1 << 23) - 1.0f;
    };

    OnePole damp_left, damp_right;
    const float decay_time = 0.35f * kDefaultIrSampleRate;
    for (int i = 0; i < length; ++i) {
      const float position = i / static_cast<float>(length);
      const float envelope = std::exp(-i / decay_time);
      const float fade_in = std::min(1.0f, i / (0.005f * kDefaultIrSampleRate));
      const float cutoff = 12000.0f - 10000.0f * position;
      const float alpha = 1.0f - std::exp(-2.0f * kPi * cutoff / kDefaultIrSampleRate);

      ir->left[i] = damp_left.processLowPass(next_random(), alpha) * envelope * fade_in * 0.5f;
      ir->right[i] = damp_right.processLowPass(next_random(), alpha) * envelope * fade_in * 0.5f;
    }
    return ir;
  }

  json ConvolutionReverbModule::irToJson(const IrData& ir) {
    json data;
    data["atpir_version"] = 1;
    data["name"] = ir.name;
    data["length"] = ir.length();
    data["sample_rate"] = ir.sample_rate;

    std::unique_ptr<int16_t[]> pcm_data = std::make_unique<int16_t[]>(ir.length());
    utils::floatToPcmData(pcm_data.get(), ir.left.data(), ir.length());
    String encoded = Base64::toBase64(pcm_data.get(), sizeof(int16_t) * ir.length());
    data["samples"] = encoded.toStdString();

    if (ir.stereo()) {
      utils::floatToPcmData(pcm_data.get(), ir.right.data(), ir.length());
      String encoded_stereo = Base64::toBase64(pcm_data.get(), sizeof(int16_t) * ir.length());
      data["samples_stereo"] = encoded_stereo.toStdString();
    }
    return data;
  }

  std::shared_ptr<ConvolutionReverbModule::IrData> ConvolutionReverbModule::irFromJson(const json& data) {
    if (!data.is_object() || !data.count("length") || !data.count("sample_rate") || !data.count("samples"))
      return nullptr;

    auto decode_channel = [](const std::string& encoded, int length, std::vector<float>& destination) {
      const size_t bytes = static_cast<size_t>(length) * sizeof(int16_t);
      if (encoded.size() < bytes || encoded.size() > ((bytes + 2) / 3) * 4 + 64) return false;
      MemoryOutputStream decoded(bytes);
      if (!Base64::convertFromBase64(decoded, encoded) ||
          decoded.getDataSize() != bytes)
        return false;

      std::unique_ptr<int16_t[]> pcm_data = std::make_unique<int16_t[]>(length);
      memcpy(pcm_data.get(), decoded.getData(), length * sizeof(int16_t));
      destination.resize(length);
      utils::pcmToFloatData(destination.data(), pcm_data.get(), length);
      return true;
    };

    try {
      if (!data["length"].is_number_integer() || !data["sample_rate"].is_number_integer()) return nullptr;
      const double raw_rate = data["sample_rate"].get<double>();
      const double raw_length = data["length"].get<double>();
      if (!(raw_rate > 0 && raw_rate <= kMaxIrSampleRate &&
            raw_length >= kMinIrSamples && raw_length <= kMaxIrSeconds * raw_rate))
        return nullptr;
      const int length = static_cast<int>(raw_length);
      const int sample_rate = static_cast<int>(raw_rate);

      auto ir = std::make_shared<IrData>();
      ir->sample_rate = sample_rate;
      if (data.count("name"))
        ir->name = data["name"].get<std::string>();

      if (!decode_channel(data["samples"].get<std::string>(), length, ir->left))
        return nullptr;
      if (data.count("samples_stereo")) {
        if (!decode_channel(data["samples_stereo"].get<std::string>(), length, ir->right))
          return nullptr;
      }
      return ir;
    }
    catch (const json::exception&) {
      return nullptr;
    }
  }

  std::shared_ptr<ConvolutionReverbModule::IrData>
  ConvolutionReverbModule::irFromAudioFile(const juce::File& file) {
    juce::AudioFormatManager format_manager;
    format_manager.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(format_manager.createReaderFor(file));
    if (reader == nullptr || reader->lengthInSamples < kMinIrSamples || reader->numChannels == 0 ||
        !(reader->sampleRate > 0 && reader->sampleRate <= kMaxIrSampleRate))
      return nullptr;

    const int max_samples = static_cast<int>(kMaxIrSeconds * reader->sampleRate);
    const int length = static_cast<int>(std::min<juce::int64>(reader->lengthInSamples, max_samples));
    const int channels = static_cast<int>(reader->numChannels);

    juce::AudioBuffer<float> buffer(std::min(channels, 2), length);
    if (!reader->read(&buffer, 0, length, 0, true, channels > 1)) return nullptr;
    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
      for (int sample = 0; sample < length; ++sample)
        if (!finiteIrSample(buffer.getReadPointer(channel) + sample)) return nullptr;

    auto ir = std::make_shared<IrData>();
    ir->name = file.getFileNameWithoutExtension().toStdString();
    ir->sample_rate = static_cast<int>(reader->sampleRate);
    ir->left.assign(buffer.getReadPointer(0), buffer.getReadPointer(0) + length);
    if (buffer.getNumChannels() > 1)
      ir->right.assign(buffer.getReadPointer(1), buffer.getReadPointer(1) + length);
    return ir;
  }
} // namespace vital
