// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "processor.h"
#include "slot_modulation.h"
#include "speech_slot.h"

#include <array>
#include <cstdint>
#include <memory>

namespace vital {
  // Advances the slot's clock once per block, ahead of the voices, so notes
  // that start together can share a piece of the text.
  class SpeechClock : public Processor {
    public:
      explicit SpeechClock(std::shared_ptr<SpeechSlot> slot) : Processor(0, 1), slot_(std::move(slot)) { }

      void process(int num_samples) override;
      Processor* clone() const override { return new SpeechClock(*this); }

    private:
      std::shared_ptr<SpeechSlot> slot_;
  };

  // The per-voice engine of the Speech oscillator type. Each note-on takes
  // the next syllable, word or the whole text (the slot's Split) of the text
  // OpenEVV rendered, and plays its Klatt parameter frames through a Klatt
  // synthesiser of its own: OpenEVV's, at 11,025 or 22,050 hertz, resampled
  // to the voice's rate. The voice can take the pitch from the key, change the
  // rate and hold a syllable's vowel while the key is down. A legato key
  // (one pressed while another is held) only moves the pitch: the voice stays
  // on its piece until every key is up.
  class SpeechSource : public Processor {
    public:
      static constexpr int kVoicesPerProcessor = poly_float::kSize / 2;
      static constexpr mono_float kMaxAmplitude = 1.41421356237f;
      // Frames a voice synthesises after its piece ends, so the resonators ring out.
      static constexpr int kTailFrames = 4;
      // A stolen voice fades out over this long.
      static constexpr double kFadeSeconds = 0.004;
      // The synthesiser's samples (16-bit scale) to the voice's.
      static constexpr float kOutputScale = 1.0f / 32768.0f;
      // The resampler: windowed sinc with this many input samples each side.
      static constexpr int kHalfTaps = 8;
      static constexpr int kPhases = 256;
      static constexpr int kInputSize = 1024;

      enum AmpEnvMode { kSpeechEnvelope, kEnvelope1, kNumAmpEnvModes };
      enum PitchMode { kKeyPitch, kSpokenPitch, kNumPitchModes };
      enum Quality { k11k, k22k, kNumQualities };

      enum {
        kReset,
        kVoiceEvent,
        kMidi,
        kRetrigger,
        kAmpEnvMode,
        kSplit,
        kSustain,
        kLoop,
        kRestartOnRelease,
        kPitchMode,
        kIntonation,
        kRate,
        kQuality,
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
        // 1 per voice while a piece still sounds there (Speech envelope mode only).
        kHold,
        kNumOutputs
      };

      explicit SpeechSource(std::shared_ptr<SpeechSlot> slot);

      void process(int num_samples) override;
      Processor* clone() const override;
      SlotModulation& modulation() { return modulation_; }

      // The key's pitch in tenths of a hertz for a frame of the utterance:
      // the key's frequency with `intonation` of the speech's own movement
      // around the piece's pitch, or the speech's pitch moved by the key's
      // distance from middle C.
      static int32_t framePitch(int32_t spoken, int32_t reference, float note, int pitch_mode, float intonation);

    private:
      struct VoiceState {
        alignas(16) unsigned char klatt[8192];
        // The voice has something to say (a piece, its tail, or samples still in the resampler).
        bool active = false;
        // The synthesiser in `klatt` is open.
        bool open = false;
        int klatt_rate = 0;
        int klatt_voice = -1;
        int32_t klatt_volume = -1;
        // What is playing: the utterance's generation and the piece's frames.
        uint32_t generation = 0;
        bool playing = false;
        int start = 0;
        int nucleus = 0;
        int end = 0;
        int32_t reference_f0 = 0;
        double frame_position = 0.0;
        bool held = false;
        bool counted = false;
        int tail = 0;
        atlas::openevv::Frame last_frame {};
        // The synthesiser's samples, waiting for the resampler.
        std::array<float, kInputSize> input {};
        int input_count = 0;
        double read_position = 0.0;
        int fade_remaining = 0;
        int fade_length = 1;
        float note = 60.0f;
      };

      struct Controls {
        int split = 0;
        bool sustain = true;
        bool loop = true;
        bool restart_on_release = false;
        int pitch_mode = kKeyPitch;
        float intonation = 0.0f;
        float rate = 1.0f;
        int quality = k11k;
      };

      Controls controls(int lane) const;
      void start(VoiceState& state, const Controls& controls, double time);
      void release(VoiceState& state, const Controls& controls);
      void silence(VoiceState& state);
      bool sounding(const VoiceState& state) const;
      // Synthesises the next frame into the input buffer. Answers false once
      // the voice has nothing more to say.
      bool synthesizeFrame(VoiceState& state, const Controls& controls);
      void render(VoiceState& state, const Controls& controls, float* output, int count);
      static void collect(void* user, const int32_t* samples, int32_t count);

      std::shared_ptr<SpeechSlot> slot_;
      std::array<VoiceState, kVoicesPerProcessor> voices_;
      double sample_rate_ = 0.0;
      poly_float pan_amplitude_ = 0.0f;
      bool pan_initialized_ = false;
      SlotModulation modulation_;

      JUCE_LEAK_DETECTOR(SpeechSource)
  };
} // namespace vital
