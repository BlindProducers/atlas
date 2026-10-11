// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "synth_module.h"
#include <array>
#include <memory>

namespace atlas::resonator { class Engine; }

namespace vital {
  class NoteMonitor;

  // Atlas controls, played-pitch tracking and tempo sync around the modal
  // resonator ported from the Corpus JSFX. Only the left and right lanes reach
  // the host, so those are resonated and lanes 2-3 mirror them.
  class ResonatorModule : public SynthModule {
    public:
      ResonatorModule(const Output* beats_per_second, const NoteMonitor* notes, std::string prefix = "");
      ~ResonatorModule() override;
      void init() override;
      void processWithInput(const poly_float* input, int samples) override;
      void correctToTime(double seconds) override;
      void hardReset() override;
      void enable(bool on) override;
      void setSampleRate(int rate) override;
      void setOversampleAmount(int amount) override;
      Processor* clone() const override { VITAL_ASSERT(false); return nullptr; }

      static constexpr int kNumModControls = 28;

    private:
      std::unique_ptr<atlas::resonator::Engine> engine_;
      const Output* beats_per_second_;
      const NoteMonitor* notes_;
      std::string prefix_;
      std::array<Output*, kNumModControls> controls_ {};
      Output* lfo_frequency_ = nullptr;
      Value* model_ = nullptr;
      Value* quality_ = nullptr;
      Value* midi_pitch_ = nullptr;
      Value* note_priority_ = nullptr;
      Value* lfo_shape_ = nullptr;
      Value* lfo_sync_ = nullptr;
      Value* filter_ = nullptr;
      Value* limiter_ = nullptr;
      bool reset_pending_ = true;
      bool has_transport_time_ = false;
      bool timeline_valid_ = false;
      int previous_sync_ = 0;
      double transport_time_ = 0.0;
      double expected_transport_time_ = 0.0;
  };
}
