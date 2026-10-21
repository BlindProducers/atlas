// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "processor.h"
#include "reverb_filter_core.h"
#include "reverb_filter_bass.h"
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

namespace vital {
class ReverbFilter : public Processor {
public:
  enum { kAudio, kReset, kCutoffLog, kResonance, kDamp, kKeytrack, kBassPreserve, kNumInputs };
  ReverbFilter();
  ReverbFilter(const ReverbFilter& other);
  ~ReverbFilter() override;
  Processor* clone() const override { return new ReverbFilter(*this); }
  void setSampleRate(int sample_rate) override;
  void process(int num_samples) override;
  void reset(poly_mask mask) override;
  // A Filter FX slot's lanes 2-3 repeat lanes 0-1: process one stereo pair
  // and copy it into lanes 2-3.
  void setFxMode(bool fx) { fx_mode_ = fx; }
  // The delay memory of this filter and of every voice copy made from it,
  // about 18 MB each at 48 kHz. A filter holds it from construction; on
  // Windows the voice filters release it while another model is selected, as
  // Windows charges even untouched memory against the system's commit limit
  // (kReserveMemoryOnlyWhileUsed). Both
  // allocate or free, so they run on the message thread with processing
  // locked, or offline.
  void reserveMemory();
  void releaseMemory();
  // Whether a copy ran in realtime without its memory since the last call.
  bool takeMemoryRequest() { return shared_->requested.exchange(false, std::memory_order_acq_rel); }
private:
  // What a filter and its voice copies share.
  struct Shared {
    std::mutex mutex;
    std::vector<ReverbFilter*> filters;
    bool reserved = true;
    std::atomic<bool> requested { false };
  };
  // These three run with shared_->mutex held.
  void prepare(int sample_rate);
  void reserveLanes();
  void releaseLanes();
  std::shared_ptr<Shared> shared_;
  static_assert(ReverbFilterCore::kLanes == poly_float::kSize, "the core runs every lane");
  ReverbFilterCore core_;
  ReverbFilterBass bass_;
  int prepared_rate_ = kDefaultSampleRate;
  bool has_memory_ = false;
  bool fx_mode_ = false;
};
}
