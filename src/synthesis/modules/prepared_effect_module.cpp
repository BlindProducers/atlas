#include "prepared_effect_module.h"
#include "convolution_reverb_module.h"
#include "filter_fx_module.h"
#include "filter_table/filter_table_kernel.h"
#include "table_filter.h"
#include "spectral_lock_module.h"
#include "smooth_value.h"
#include "value_switch.h"
#include "operators.h"
#include "synth_constants.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <chrono>
#include <stdexcept>

namespace vital {
namespace {
thread_local bool realtime_effect_processing = false;
// A configuration's low word: oversampling (4 bits), the processing rate, then
// whether a Filter FX must allocate its Filter Table buffers.
constexpr uint64_t kFilterTableBit = uint64_t(1) << 31;
// Effects that were always fully built before every slot prepared its effect
// on demand. Their resets are cheap and always ran on the audio thread, so
// "all sounds off" or a preset load resets their DSP in place; rebuilding it
// would leave the next note dry for a few milliseconds.
bool resetsInPlace(int type) {
  switch (type) {
    case constants::kChorus: case constants::kDelay: case constants::kReverb:
    case constants::kPhaseShift: case constants::kDimensionExpander:
    case constants::kSpectralGate: case constants::kSpectralCompressor:
    case constants::kSuppressor: case constants::kConvolutionReverb:
    case constants::kTape: case constants::kSpectralLock:
    case constants::kFloatyDelay: case constants::kAnalogueChorus:
    case constants::kAnalogueFlanger: case constants::kAnalogueEcho:
    case constants::kAnalogueRotary: case constants::kAnalogueSpring:
    case constants::kAnalogueWah: case constants::kSpectralBalance:
    case constants::kGlitchDelay: return false;
    default: return true;
  }
}
int filterFxLatency(int style) {
  return atlas::filter_table::KernelBuilder::latencyForMode(TableFilter::modeForStyle(style, true),
                                                            1 << TableFilter::kFxOrder);
}
}
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
    description_(describe(type_, prefix_, factory_)), reset_in_place_(resetsInPlace(type_)) {
  while (numOutputs() < description_.outputs) addOutput();
  pending_capacity_ = type_ == constants::kSpectralGate ? 8192 :
                      type_ == constants::kSpectralLock ? SpectralLockModule::kMaxSpectralLatencySamples :
                      type_ == constants::kFilterFx ? filterFxLatency(atlas::filter_table::kLinear) :
                      description_.latency;
  // On Windows only a live shell has its dry history (reserveControls).
  if (!kReserveMemoryOnlyWhileUsed && pending_capacity_ > 0)
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
  // Every parameter's value; the processors that read it wait until the
  // effect is selected or routed.
  parameters_.reserve(description_.controls.size());
  bases_.assign(description_.controls.size(), nullptr);
  for (const auto& control : description_.controls)
    parameters_.emplace_back(control.initial);
  auto parameter = [this](const char* suffix) -> Value* {
    const int index = controlIndex(prefix_ + suffix);
    VITAL_ASSERT(index >= 0);
    return index < 0 ? nullptr : &parameters_[index];
  };
  if (type_ == constants::kSpectralGate)
    fft_size_control_ = parameter("spectral_gate_fft_size");
  if (type_ == constants::kSpectralLock)
    spectral_lock_mode_control_ = parameter("spectral_lock_mode");
  if (type_ == constants::kFilterFx) {
    filter_model_control_ = parameter("filter_fx_model");
    filter_style_control_ = parameter("filter_fx_style");
  }
  SynthModule::init();
  worker_->add(this);
}
uint64_t PreparedEffectModule::configuration() const {
  return (uint64_t(generation_) << 32) | (filter_table_needed_ ? kFilterTableBit : 0) |
         (uint64_t(getSampleRate()) << 4) | uint64_t(getOversampleAmount());
}
void PreparedEffectModule::updateFilterTableNeed() {
  if (type_ != constants::kFilterFx || filter_table_needed_) return;
  filter_table_needed_ = filter_table_requested_.load(std::memory_order_acquire) ||
      static_cast<int>(std::round(filter_model_control_->value())) == constants::kFilterTable;
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
  const int rate = int((requested & (kFilterTableBit - 1)) >> 4);
  prepared->module->setSampleRate(rate / oversample);
  prepared->module->setOversampleAmount(oversample);
  if (type_ == constants::kFilterFx) {
    auto* filter_fx = static_cast<FilterFxModule*>(prepared->module.get());
    filter_fx->setTableWavetable(filter_table_.load(std::memory_order_acquire));
    if (requested & kFilterTableBit) filter_fx->prepareTable();
  }
  if (convolution_state_)
    static_cast<ConvolutionReverbModule*>(prepared->module.get())->shareIrSourceFrom(*convolution_state_);
  {
    std::lock_guard<std::mutex> lock(wiring_mutex_);
    for (const auto& entry : inputs) {
      const auto raw = raw_controls_.find(entry.first);
      const auto readout = data_->mono_modulation_readout.find(entry.first);
      prepared->bindings.push_back({ entry.second.value, &parameters_.at(size_t(controlIndex(entry.first))),
          entry.second.modulated && raw != raw_controls_.end() ? raw->second : nullptr,
          entry.second.internal_total,
          readout == data_->mono_modulation_readout.end() ? nullptr : readout->second });
    }
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
void PreparedEffectModule::setOversampleAmount(int amount) {
  if (buffers_reserved_.load(std::memory_order_acquire))
    SynthModule::setOversampleAmount(amount); // Within the reserved capacity: no allocation.
  else
    Processor::setOversampleAmount(amount);
}
control_map PreparedEffectModule::getControls() {
  control_map controls = SynthModule::getControls();
  for (size_t i = 0; i < parameters_.size(); ++i)
    controls[prefix_ + description_.controls[i].suffix] = &parameters_[i];
  return controls;
}
int PreparedEffectModule::controlIndex(const std::string& name) const {
  if (name.compare(0, prefix_.size(), prefix_) != 0)
    return -1;
  for (int i = 0; i < int(description_.controls.size()); ++i)
    if (name.compare(prefix_.size(), std::string::npos, description_.controls[i].suffix) == 0)
      return i;
  return -1;
}
Value* PreparedEffectModule::ensureBase(int index) {
  if (bases_[index] != nullptr)
    return bases_[index];
  const Control& control = description_.controls[index];
  const poly_float value = parameters_[index].value();
  Value* base = nullptr;
  if (control.smooth) {
    // As createBaseControl made it, from the parameter's default: the first
    // time the effect runs, the parameter glides from there to its value.
    const mono_float initial = Parameters::getDetails(prefix_ + control.suffix).default_value;
    base = control.audio_rate ? static_cast<Value*>(new SmoothValue(initial)) : new cr::SmoothValue(initial);
    base->set(value);
    addMonoProcessor(base, false);
  }
  else {
    base = control.audio_rate ? new Value(value) : new cr::Value(value);
    addIdleMonoProcessor(base);
  }
  reserveProcessor(base);
  bases_[index] = base;
  return base;
}
void PreparedEffectModule::wireControl(int index) {
  // createBaseModControl's wiring, around the parameter's base.
  const Control& control = description_.controls[index];
  const std::string name = prefix_ + control.suffix;
  Value* base = ensureBase(index);
  Processor* mono_total = control.audio_rate ? static_cast<Processor*>(new ModulationSum()) : new cr::VariableAdd();
  mono_total->plugNext(base);
  addMonoProcessor(mono_total, false);
  data_->mono_mod_destinations[name] = mono_total;
  data_->mono_modulation_readout[name] = mono_total->output();

  ValueSwitch* control_switch = new ValueSwitch(0.0f);
  control_switch->plugNext(base);
  control_switch->plugNext(mono_total);
  control_switch->addProcessor(mono_total);
  addIdleMonoProcessor(control_switch);
  control_switch->set(control.smooth ? 1.0f : 0.0f);
  data_->mono_modulation_switches[name] = control_switch;
  raw_controls_[name] = control_switch->output(ValueSwitch::kSwitch);

  // Wiring can be built while the effect already runs at a higher quality,
  // so reserve it here rather than relying on its first growth.
  reserveProcessor(mono_total);
  reserveProcessor(control_switch);
}
void PreparedEffectModule::unwireControl(const std::string& name) {
  Processor* mono_total = data_->mono_mod_destinations.at(name);
  ValueSwitch* control_switch = data_->mono_modulation_switches.at(name);
  data_->mono_mod_destinations.erase(name);
  data_->mono_modulation_readout.erase(name);
  data_->mono_modulation_switches.erase(name);
  raw_controls_.erase(name);
  removeIdleMonoProcessor(control_switch);
  removeMonoProcessor(mono_total);
  // A smoothed parameter keeps its SmoothValue; a plain base goes with its wiring.
  const int index = controlIndex(name);
  if (index >= 0 && bases_[index] != nullptr && !description_.controls[index].smooth) {
    removeIdleMonoProcessor(bases_[index]);
    bases_[index] = nullptr;
  }
}
void PreparedEffectModule::reserveProcessor(Processor* processor) {
  // A processor joining the graph takes the shell's rate and quality, with
  // room for every quality setting.
  const int amount = getOversampleAmount();
  processor->setSampleRate(getSampleRate() / amount);
  processor->setOversampleAmount(std::max(amount, 2));
  if (amount < 2) processor->setOversampleAmount(amount);
  for (int i = 0; i < processor->numOwnedOutputs(); ++i)
    processor->ownedOutput(i)->reserveAllQualities();
}
bool PreparedEffectModule::wireModulationDestination(const std::string& name) {
  const int index = controlIndex(name);
  if (index < 0 || !description_.controls[index].modulated)
    return false;
  std::lock_guard<std::mutex> lock(wiring_mutex_);
  if (data_->mono_mod_destinations.count(name) == 0)
    wireControl(index);
  else if (bases_[index]->value() != parameters_[index].value())
    bases_[index]->set(parameters_[index].value()); // A parked base missed changes.
  return true;
}
void PreparedEffectModule::reserveControls() {
  if (controlsReserved()) return;
  {
    std::lock_guard<std::mutex> lock(wiring_mutex_);
    for (int i = 0; i < int(description_.controls.size()); ++i) {
      const Control& control = description_.controls[i];
      if (control.smooth)
        ensureBase(i);
      if (control.modulated && data_->mono_mod_destinations.count(prefix_ + control.suffix) == 0)
        wireControl(i);
    }
  }
  const int amount = getOversampleAmount();
  if (!buffers_reserved_.load(std::memory_order_acquire)) {
    // An output reserves capacity for every quality the first time it grows
    // past 1x. Grow through 2x even at 1x so later quality changes, which run
    // on the audio thread, never allocate.
    SynthModule::setOversampleAmount(std::max(amount, 2));
    buffers_reserved_.store(true, std::memory_order_release);
  }
  // A parked effect's controls missed quality changes; this stays within
  // their reserved capacity.
  SynthModule::setOversampleAmount(amount);
  if (pending_capacity_ > 0 && pending_history_ == nullptr) {
    pending_history_.reset(new float[size_t(pending_capacity_) * poly_float::kSize]);
    pending_position_ = pending_valid_ = 0;
  }
  controls_reserved_.store(true, std::memory_order_release);
}
void PreparedEffectModule::park() {
  if (controlsReserved()) {
    controls_reserved_.store(false, std::memory_order_release);
    // Release the DSP: what it binds to may be torn down below, and a
    // preparation already under way publishes for an older configuration.
    restart();
  }
  if (kReserveMemoryOnlyWhileUsed)
    pending_history_.reset();
  std::lock_guard<std::mutex> lock(wiring_mutex_);
  std::vector<std::string> unrouted;
  for (const auto& destination : data_->mono_mod_destinations)
    if (destination.second->connectedInputs() <= 1) unrouted.push_back(destination.first);
  for (const auto& name : unrouted)
    unwireControl(name);
}
void PreparedEffectModule::prepareForPlayback() {
  reserveControls();
  updateFilterTableNeed();
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
  // A parked effect has no history: selected in realtime, it passes audio
  // through undelayed until the message thread makes it live.
  if (latency <= 0 || pending_history_ == nullptr) {
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
  if (!controlsReserved()) {
    if (RealtimeEffectScope::active()) {
      // Selected but parked: the message thread makes it live after this
      // block. Nothing is prepared before that, as the DSP binds the wiring.
      worker_->requestControlReservation();
      renderPending(input, samples);
      return;
    }
    prepareForPlayback(); // Offline: live, and prepared, within this block.
  }
  updateFilterTableNeed();
  requestPreparation();
  if (!RealtimeEffectScope::active()) prepareForPlayback();
  acceptPrepared();
  if (!active_ || active_->configuration != configuration()) {
    // Keep audio moving, with the announced latency, until the effect is ready.
    renderPending(input, samples);
    return;
  }
  // The graph reads each parameter as it stands at this block.
  for (size_t i = 0; i < bases_.size(); ++i)
    if (bases_[i] != nullptr && bases_[i]->value() != parameters_[i].value())
      bases_[i]->set(parameters_[i].value());
  SynthModule::process(samples);
  // An audio-rate modulated control reads the permanent control's samples in
  // place (as ValueSwitch does); its source can change when routes do. An
  // audio-rate Value's set() fills its whole buffer, so it runs on changes only.
  for (const auto& binding : active_->bindings) {
    Value* destination = binding.destination;
    if (destination->isControlRate())
      destination->set(binding.modulation ? binding.modulation->buffer[0] : poly_float(binding.base->value()));
    else if (binding.modulation)
      destination->output()->buffer = binding.modulation->buffer;
    else if (destination->value() != binding.base->value())
      destination->set(binding.base->value());
  }
  if (type_ == constants::kFilterFx)
    static_cast<FilterFxModule*>(active_->module.get())->setTableWavetable(filter_table_.load(std::memory_order_acquire));
  active_->module->correctToTime(time_);
  active_->module->processWithInput(input, samples);
  for (const auto& binding : active_->bindings) {
    if (binding.total == nullptr || binding.readout == nullptr) continue;
    utils::copyBuffer(binding.readout->buffer, binding.total->buffer, binding.total->isControlRate() ? 1 : samples);
    binding.readout->trigger_value = binding.total->buffer[0];
  }
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
  if (reset_in_place_ && active_ && active_->configuration == configuration()) {
    active_->module->hardReset();
    return;
  }
  restart();
}
void PreparedEffectModule::restart() {
  ++generation_;
  pending_position_ = pending_valid_ = 0;
  const bool had_request = requested_.exchange(0, std::memory_order_acq_rel) != 0;
  retireActive();
  if (had_request) worker_->request();
}
void PreparedEffectModule::enable(bool on) {
  if (enabled() != on) {
    // Bypassing or replacing an effect releases its DSP.
    restart();
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
  // Filter FX has latency only in its Filter Table model, set by the table's style.
  if (type_ == constants::kFilterFx)
    return filter_model_control_ != nullptr &&
           static_cast<int>(std::round(filter_model_control_->value())) == constants::kFilterTable ?
        filterFxLatency(static_cast<int>(filter_style_control_->value() + 0.5f)) : 0;
  return description_.latency;
}
}
