#pragma once

#include "synth_module.h"
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace vital {
class PreparedEffectModule;
class ConvolutionReverbModule;
class Wavetable;
class ConvolutionIrWorker;

// One sleeping preparation worker per synth. Construction, reconfiguration,
// reset and disposal of effect DSP happen here, never in a realtime callback.
class EffectPreparationWorker {
public:
  EffectPreparationWorker();
  ~EffectPreparationWorker();
  void add(PreparedEffectModule* module);
  void remove(PreparedEffectModule* module);
  void request();
  void prepareSynchronously(PreparedEffectModule& module);
  // The audio thread met a selected effect whose controls are not reserved;
  // the plug-in reserves them on the message thread after the block.
  void requestControlReservation() { reservation_requested_.store(true, std::memory_order_release); }
  bool takeControlReservationRequest() { return reservation_requested_.exchange(false, std::memory_order_acq_rel); }
private:
  void run();
  std::mutex mutex_;
  std::condition_variable signal_;
  std::vector<PreparedEffectModule*> modules_;
  std::atomic<bool> pending_ { false };
  std::atomic<bool> reservation_requested_ { false };
  bool stopping_ = false;
  std::thread thread_;
};

// Offline engines may prepare synchronously; hosts opt into realtime behavior
// for the duration of processBlock. This is thread-local, not a global setting.
class RealtimeEffectScope {
public:
  explicit RealtimeEffectScope(bool realtime);
  ~RealtimeEffectScope();
  static bool active();
private:
  bool previous_;
};

class PreparedEffectModule : public SynthModule {
public:
  using Factory = std::function<std::unique_ptr<SynthModule>()>;
  PreparedEffectModule(int type, std::string prefix, Factory factory,
                       std::shared_ptr<EffectPreparationWorker> worker,
                       std::shared_ptr<ConvolutionIrWorker> ir_worker = nullptr);
  ~PreparedEffectModule() override;
  void init() override;
  void processWithInput(const poly_float* input, int samples) override;
  void hardReset() override;
  void enable(bool on) override;
  void correctToTime(double seconds) override { time_ = seconds; }
  int getLatencySamples() const override;
  Processor* clone() const override { VITAL_ASSERT(false); return nullptr; }
  // Until a slot first selects this effect, its parameter controls keep
  // 1x buffers and only the shell's own state follows the oversampling.
  void setOversampleAmount(int amount) override;
  // Grows the parameter controls' buffers, with room for every quality
  // setting. It allocates and writes buffers the message thread also writes,
  // so it runs with processing stopped (or the callback lock held) or offline.
  void reserveControls();
  bool controlsReserved() const { return controls_reserved_.load(std::memory_order_acquire); }
  void prepareForPlayback();
  void stopPreparation() { worker_->remove(this); }
  ConvolutionReverbModule* convolutionState() const { return convolution_state_.get(); }
  // Filter FX: the table its Filter Table model reads, and a request for that
  // model's buffers. Both are safe from the message thread: the DSP picks the
  // table up on its next block, and the buffers are allocated on the worker.
  void setFilterTable(Wavetable* table) { filter_table_.store(table, std::memory_order_release); }
  void prepareFilterTable() { filter_table_requested_.store(true, std::memory_order_release); }
private:
  friend class EffectPreparationWorker;
  struct Control {
    std::string suffix;
    float initial;
    bool audio_rate, smooth, modulated;
  };
  struct Description { std::vector<Control> controls; int outputs = 1; int latency = 0; };
  // `total` is the module's own total for a control it also modulates
  // internally (Filter FX keytracking); it is copied to the control's readout.
  struct Binding { Value* destination; Value* base; Output* modulation; Output* total; Output* readout; };
  struct Prepared {
    uint64_t configuration = 0;
    std::unique_ptr<SynthModule> module;
    std::vector<Binding> bindings;
  };
  static Description describe(int type, const std::string& prefix, const Factory& factory);
  uint64_t configuration() const;
  void updateFilterTableNeed();
  void requestPreparation();
  void prepareOnWorker();
  void acceptPrepared();
  void retireActive();
  void renderPending(const poly_float* input, int samples);
  void restart();

  int type_;
  std::string prefix_;
  Factory factory_;
  std::shared_ptr<EffectPreparationWorker> worker_;
  Description description_;
  output_map raw_controls_;
  Value* fft_size_control_ = nullptr;
  Value* spectral_lock_mode_control_ = nullptr;
  Value* filter_model_control_ = nullptr;
  Value* filter_style_control_ = nullptr;
  std::atomic<Wavetable*> filter_table_ { nullptr };
  std::atomic<bool> filter_table_requested_ { false };
  // Once a Filter FX has used its table model it keeps the buffers, as the
  // filter did before effects were prepared on demand. Audio/control thread only.
  bool filter_table_needed_ = false;
  const bool reset_in_place_;
  std::atomic<bool> controls_reserved_ { false };

  std::unique_ptr<ConvolutionReverbModule> convolution_state_;
  std::atomic<uint64_t> requested_ { 0 };
  std::atomic<Prepared*> ready_ { nullptr }, retired_ { nullptr };
  Prepared* active_ = nullptr;
  uint64_t completed_ = 0; // worker only
  uint32_t generation_ = 1; // audio/control thread only
  double time_ = 0;
  int fade_remaining_ = 0, warmup_remaining_ = 0;
  // Dry history while the effect is pending or warming up, sized for the
  // type's longest latency. Left uninitialised: renderPending never reads a
  // sample it has not written, so untouched pages cost no memory.
  std::unique_ptr<float[]> pending_history_;
  int pending_capacity_ = 0;
  int pending_position_ = 0, pending_valid_ = 0, pending_latency_ = 0;
};
}
