// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "processor.h"
#include "slot_modulation.h"
#include "openb3/tonewheel_organ.h"

#include <array>
#include <cstdint>
#include <memory>

namespace vital {
  // Runs the organ's clock once per block, before any voice renders: the
  // tonewheels turn, key compression follows the keys held at the end of the
  // previous block, and the percussion envelope decays (or re-arms when no
  // key was held). It lives in the voice handler's mono router; the voices
  // set the organ's rate.
  class OpenB3Clock : public Processor {
    public:
      explicit OpenB3Clock(std::shared_ptr<atlas::openb3::Organ> organ) :
          Processor(0, 1), organ_(std::move(organ)) { }

      void process(int num_samples) override;
      Processor* clone() const override { return new OpenB3Clock(*this); }

    private:
      std::shared_ptr<atlas::openb3::Organ> organ_;
  };

  // The per-voice engine of the OpenB3 oscillator type: each note closes the
  // contacts of the nearest of the upper manual's 61 keys (C2 to C7), whose
  // wheels it hears through the drawbars. The key gives the wiring and the
  // voice the pitch, so bends, glide, Transpose and Tune move it. All voices
  // of the slot share one Organ, so a wheel two keys use is one signal.
  class OpenB3Source : public Processor {
    public:
      static constexpr int kVoicesPerProcessor = poly_float::kSize / 2;
      static constexpr mono_float kMaxAmplitude = 1.41421356237f;
      // The drawbar sum of tonegen.c (before its 0.07 output trim) times this:
      // a key with the default 8 8 6 registration peaks near 0.9.
      static constexpr double kOutputScale = 1.0 / 3.0;
      // Bends and glides retune the wheels every kPitchStep samples.
      static constexpr int kPitchStep = 16;

      // Amplitude envelope modes (osc_N_b3_amp_env).
      enum AmpEnvMode { kOrganEnvelope, kEnvelope1, kNumAmpEnvModes };

      enum {
        kReset,
        kVoiceEvent,
        kMidi,
        kAmpEnvMode,
        kDrawbar,
        kPercussion = kDrawbar + atlas::openb3::kNumDrawbars,
        kPercussionVolume,
        kPercussionDecay,
        kPercussionHarmonic,
        kKeyClick,
        kReleaseClick,
        kTemperament,
        kTranspose,
        kTune,
        kLevel,
        kPan,
        kDistortionType,
        kDistortionAmount,
        kSecondaryDistortionType,
        kSecondaryDistortionAmount,
        kNumInputs
      };

      enum {
        kRaw,
        kLevelled,
        // 1 per voice while its key's wheels still sound (Organ envelope mode only).
        kHold,
        kNumOutputs
      };

      OpenB3Source();

      void process(int num_samples) override;
      Processor* clone() const override { return new OpenB3Source(*this); }
      SlotModulation& modulation() { return modulation_; }
      const std::shared_ptr<atlas::openb3::Organ>& organ() const { return organ_; }

    private:
      struct VoiceState {
        atlas::openb3::Voice voice;
        // The lane's previous key, fading out after the lane moved to another.
        atlas::openb3::Voice fading;
        bool key_down = false;
        // The previous block's note, for a ramp across this block.
        float note = 60.0f;
      };

      atlas::openb3::Registration registration(int lane) const;
      void strike(VoiceState& state, float note, int offset, const atlas::openb3::Registration& registration,
                  float click, float release_click, const double* frequencies);
      // Opens a held key's contacts with the release click at `level`.
      void release(atlas::openb3::Voice& voice, float level);
      // Renders samples [start, end) of the lane into `output`, the note
      // ramping from `from` at sample 0 to `to` at `count`.
      void render(VoiceState& state, double* output, int start, int end, int count, float from, float to);

      std::shared_ptr<atlas::openb3::Organ> organ_;
      std::array<VoiceState, kVoicesPerProcessor> voices_;
      std::array<float, atlas::openb3::kNumClickEnvelopes * atlas::openb3::kMaxContactLength> envelopes_ {};
      std::array<float, atlas::openb3::kNumClickEnvelopes * atlas::openb3::kMaxContactLength> release_envelopes_ {};
      uint32_t strike_count_ = 0;
      double voice_rate_ = 0.0;
      poly_float pan_amplitude_ = 0.0f;
      bool pan_initialized_ = false;
      SlotModulation modulation_;

      JUCE_LEAK_DETECTOR(OpenB3Source)
  };
} // namespace vital
