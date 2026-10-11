#include "prepared_effect_module.h"
#include "convolution_reverb_module.h"
#include "spectral_lock_module.h"
#include "smooth_value.h"
#include "value_switch.h"
#include "synth_constants.h"
#include <algorithm>
#include <cstring>
#include <chrono>
#include <stdexcept>

namespace vital {
namespace { thread_local bool realtime_effect_processing = false; }
RealtimeEffectScope::RealtimeEffectScope(bool realtime) : previous_(realtime_effect_processing) {
  realtime_effect_processing = realtime;
}
RealtimeEffectScope::~RealtimeEffectScope() { realtime_effect_processing = previous_; }
bool RealtimeEffectScope::active() { return realtime_effect_processing; }

EffectPreparationWorker::EffectPreparationWorker() : thread_([this] { run(); }) {}
EffectPreparationWorker::~EffectPreparationWorker() {
  { std::lock_guard<std::mutex> lock(mutex_); stopping_ = true; }
  signal_.notify_one();
  thread_.join();
}
void EffectPreparationWorker::add(PreparedEffectModule* module) {
  std::lock_guard<std::mutex> lock(mutex_);
  modules_.push_back(module);
}
void EffectPreparationWorker::remove(PreparedEffectModule* module) {
  std::lock_guard<std::mutex> lock(mutex_);
  modules_.erase(std::remove(modules_.begin(), modules_.end(), module), modules_.end());
}
void EffectPreparationWorker::request() {
  pending_.store(true, std::memory_order_release);
  signal_.notify_one();
}
void EffectPreparationWorker::prepareSynchronously(PreparedEffectModule& module) {
  std::lock_guard<std::mutex> lock(mutex_);
  module.prepareOnWorker();
}
void EffectPreparationWorker::run() {
  std::unique_lock<std::mutex> lock(mutex_);
  while (!stopping_) {
    // The deadline also covers a notification between the atomic predicate and
    // entering the wait. No polling work or graph scan is done while idle.
    signal_.wait_for(lock, std::chrono::milliseconds(20), [this] {
      return stopping_ || pending_.load(std::memory_order_acquire);
    });
    if (stopping_) break;
    if (pending_.exchange(false, std::memory_order_acq_rel))
      for (auto* module : modules_) module->prepareOnWorker();
  }
}

PreparedEffectModule::Description PreparedEffectModule::describe(
    int type, const std::string& prefix, const Factory& factory) {
  static std::mutex mutex;
  static std::map<int, Description> descriptions;
  std::lock_guard<std::mutex> lock(mutex);
  auto found = descriptions.find(type);
  if (found != descriptions.end()) return found->second;
  auto prototype = factory();
  prototype->init();
  VITAL_ASSERT(prototype->getPolyModulationDestinations().empty());
  Description result;
  result.outputs = prototype->numOutputs();
  result.latency = prototype->getLatencySamples();
  const auto& modulated = prototype->getMonoModulationDestinations();
  for (const auto& entry : prototype->getControls()) {
    const auto* value = entry.second;
    result.controls.push_back({ entry.first.substr(prefix.size()), value->value(),
        !value->isControlRate(), dynamic_cast<const SmoothValue*>(value) != nullptr ||
        dynamic_cast<const cr::SmoothValue*>(value) != nullptr,
        modulated.count(entry.first) != 0 });
  }
  descriptions.emplace(type, result);
  return result;
}
PreparedEffectModule::PreparedEffectModule(int type, std::string prefix, Factory factory,
    std::shared_ptr<EffectPreparationWorker> worker,
    std::shared_ptr<ConvolutionIrWorker> ir_worker) : SynthModule(0, 1), type_(type),
    prefix_(std::move(prefix)), factory_(std::move(factory)), worker_(std::move(worker)),
    description_(describe(type_, prefix_, factory_)) {
  while (numOutputs() < description_.outputs) addOutput();
  pending_capacity_ = type_ == constants::kSpectralGate ? 8192 :
                      type_ == constants::kSpectralLock ? SpectralLockModule::kMaxSpectralLatencySamples :
                      description_.latency;
  if (pending_capacity_ > 0)
    pending_history_.reset(new float[size_t(pending_capacity_) * poly_float::kSize]);
  if (type_ == constants::kConvolutionReverb)
    convolution_state_ = std::make_unique<ConvolutionReverbModule>(prefix_, std::move(ir_worker), true);
}
PreparedEffectModule::~PreparedEffectModule() {
  worker_->remove(this);
  delete active_;
  delete ready_.load();
  delete retired_.load();
}
void PreparedEffectModule::init() {
  for (const auto& control : description_.controls) {
    const auto name = prefix_ + control.suffix;
    if (control.modulated)
      raw_controls_[name] = createBaseModControl(name, control.audio_rate, control.smooth);
    else
      createBaseControl(name, control.audio_rate, control.smooth);
    data_->controls.at(name)->set(control.initial);
  }
  if (type_ == constants::kSpectralGate)
    fft_size_control_ = data_->controls.at(prefix_ + "spectral_gate_fft_size");
  if (type_ == constants::kSpectralLock)
    spectral_lock_mode_control_ = data_->controls.at(prefix_ + "spectral_lock_mode");
  SynthModule::init();
  worker_->add(this);
}
uint64_t PreparedEffectModule::configuration() const {
  return (uint64_t(generation_) << 32) | (uint64_t(getSampleRate()) << 4) |
         uint64_t(getOversampleAmount());
}
void PreparedEffectModule::requestPreparation() {
  const auto next = configuration();
  if (requested_.exchange(next, std::memory_order_acq_rel) != next)
    worker_->request();
}
void PreparedEffectModule::prepareOnWorker() {
  delete retired_.exchange(nullptr, std::memory_order_acq_rel);
  const auto requested = requested_.load(std::memory_order_acquire);
  if (!requested) {
    // Selection can change after preparation finishes but before the audio
    // thread consumes it. Release that unused publication here as well.
    delete ready_.exchange(nullptr, std::memory_order_acq_rel);
    completed_ = 0;
    return;
  }
  if (completed_ == requested || ready_.load(std::memory_order_acquire)) return;
  auto prepared = std::make_unique<Prepared>();
  prepared->configuration = requested;
  effect_control_inputs inputs;
  {
    EffectControlPreparation scope(inputs);
    prepared->module = factory_();
    prepared->module->init();
  }
  const int oversample = int(requested & 15);
  const int rate = int((requested & 0xffffffffu) >> 4);
  prepared->module->setSampleRate(rate / oversample);
  prepared->module->setOversampleAmount(oversample);
  if (convolution_state_)
    static_cast<ConvolutionReverbModule*>(prepared->module.get())->shareIrSourceFrom(*convolution_state_);
  for (const auto& entry : inputs) {
    const auto raw = raw_controls_.find(entry.first);
    prepared->bindings.push_back({ entry.second.value, data_->controls.at(entry.first),
        entry.second.modulated && raw != raw_controls_.end() ? raw->second : nullptr });
  }
  if (requested_.load(std::memory_order_acquire) == requested) {
    completed_ = requested;
    ready_.store(prepared.release(), std::memory_order_release);
  }
}
void PreparedEffectModule::retireActive() {
  if (active_ && !retired_.load(std::memory_order_acquire)) {
    retired_.store(active_, std::memory_order_release);
    active_ = nullptr;
    worker_->request();
  }
}
void PreparedEffectModule::acceptPrepared() {
  if (active_ && active_->configuration != configuration()) retireActive();
  if (retired_.load(std::memory_order_acquire)) return;
  if (auto* prepared = ready_.exchange(nullptr, std::memory_order_acq_rel)) {
    if (prepared->configuration == configuration()) {
      VITAL_ASSERT(active_ == nullptr);
      active_ = prepared;
      fade_remaining_ = RealtimeEffectScope::active() ? 128 * getOversampleAmount() : 0;
      warmup_remaining_ = RealtimeEffectScope::active() ? getLatencySamples() : 0;
    }
    else {
      retired_.store(prepared, std::memory_order_release);
      worker_->request();
    }
  }
}
void PreparedEffectModule::prepareForPlayback() {
  requestPreparation();
  // Called with processing suspended, or by an explicitly offline renderer.
  // Drain outdated publications before preparing the latest requested state.
  for (int i = 0; i < 4 && (!active_ || active_->configuration != configuration()); ++i) {
    acceptPrepared();
    worker_->prepareSynchronously(*this);
    acceptPrepared();
  }
}
void PreparedEffectModule::renderPending(const poly_float* input, int samples) {
  // Never index past the reserved history, whatever a latency query returns.
  const int latency = std::min(getLatencySamples(), pending_capacity_);
  if (latency <= 0) {
    utils::copyBuffer(output()->buffer, input, samples);
    return;
  }
  if (latency != pending_latency_) {
    pending_position_ = pending_valid_ = 0;
    pending_latency_ = latency;
  }
  for (int i = 0; i < samples; ++i) {
    const auto dry = input[i];
    float* history = pending_history_.get() + size_t(pending_position_) * poly_float::kSize;
    poly_float delayed = 0.0f;
    if (pending_valid_ >= latency)
      std::memcpy(&delayed.value, history, sizeof(delayed.value));
    output()->buffer[i] = delayed;
    std::memcpy(history, &dry.value, sizeof(dry.value));
    pending_valid_ = std::min(pending_valid_ + 1, latency);
    if (++pending_position_ == latency) pending_position_ = 0;
  }
}
void PreparedEffectModule::processWithInput(const poly_float* input, int samples) {
  if (samples <= 0) return;
  requestPreparation();
  if (!RealtimeEffectScope::active()) prepareForPlayback();
  acceptPrepared();
  if (!active_ || active_->configuration != configuration()) {
    // Keep audio moving, with the announced latency, until the effect is ready.
    renderPending(input, samples);
    return;
  }
  SynthModule::process(samples);
  for (const auto& binding : active_->bindings) {
    if (binding.modulation) {
      binding.destination->set(binding.modulation->buffer[0]);
      if (!binding.destination->isControlRate())
        utils::copyBuffer(binding.destination->output()->buffer, binding.modulation->buffer, samples);
    }
    else binding.destination->set(binding.base->value());
  }
  active_->module->correctToTime(time_);
  active_->module->processWithInput(input, samples);
  const auto* wet = active_->module->output()->buffer;
  if (fade_remaining_ > 0 || warmup_remaining_ > 0) {
    renderPending(input, samples);
    const float length = float(128 * getOversampleAmount());
    for (int i = 0; i < samples; ++i) {
      if (warmup_remaining_ > 0) { --warmup_remaining_; continue; }
      const float mix = fade_remaining_ > 0 ? 1.0f - float(fade_remaining_--) / length : 1.0f;
      output()->buffer[i] += (wet[i] - output()->buffer[i]) * mix;
    }
  }
  else {
    // Keep recent dry history available if another Suppressor is prepared
    // while this one is playing. Pd's global writer never stalls this callback.
    if (type_ == constants::kSuppressor) renderPending(input, samples);
    utils::copyBuffer(output()->buffer, wet, samples);
  }
}
void PreparedEffectModule::hardReset() {
  ++generation_;
  pending_position_ = pending_valid_ = 0;
  const bool had_request = requested_.exchange(0, std::memory_order_acq_rel) != 0;
  retireActive();
  if (had_request) worker_->request();
}
void PreparedEffectModule::enable(bool on) {
  if (enabled() != on) {
    hardReset();
    SynthModule::enable(on);
  }
}
int PreparedEffectModule::getLatencySamples() const {
  if (type_ == constants::kSpectralGate) {
    const float value = fft_size_control_ ? fft_size_control_->value() : 3;
    const int index = value >= 0 && value <= 5 ? int(value + .5f) : 3;
    return 1 << (8 + index);
  }
  // Spectral Lock's Spectral mode is one STFT frame sized for the rate; its Filterbank mode has none.
  if (type_ == constants::kSpectralLock)
    return spectral_lock_mode_control_ != nullptr && spectral_lock_mode_control_->value() >= 0.5f ?
        SpectralLockModule::spectralLatencyForRate(getSampleRate()) : 0;
  return description_.latency;
}
}
