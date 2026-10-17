// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "processor.h"
#include "speech_slot.h"
#include "utils.h"

#include <array>
#include <memory>

namespace vital {
  // The per-voice engine of the Speech oscillator type. Each note-on takes
  // the slot's next syllable and sings it from the phrase layer espeak-ng
  // spoke nearest the note's pitch: TD-PSOLA lays grains of that monotone
  // recording one note period apart, so the voice keeps its formants. The
  // syllable's onset plays at its spoken speed; while the key is down the
  // vowel crossfades into its steady grain, repeated with a little jitter
  // and shimmer; after release the rest of the syllable plays. A mono legato
  // note change keeps the syllable and only moves its pitch, moving to the
  // layer nearest the new pitch.
  //
  // With the Voice envelope mode the syllable shapes the note and holds the
  // voice open until its last consonant ends; with Envelope 1 the slot
  // follows Env 1 like the wavetable oscillators.
  class SpeechSource : public Processor {
    public:
      static constexpr int kVoicesPerProcessor = poly_float::kSize / 2;
      static constexpr mono_float kMaxAmplitude = 1.41421356237f;
      static constexpr int kMaxGrains = 8;
      // A grain lasts at most this many note periods, which bounds the
      // overlap when singing far above the recording.
      static constexpr float kMaxGrainPeriods = 4.0f;
      // Seconds to crossfade into the steady vowel, and out of it on release.
      static constexpr float kSteadyInSeconds = 0.08f;
      static constexpr float kSteadyOutSeconds = 0.04f;
      // Random variation of the steady vowel's period and level, per grain.
      static constexpr float kJitter = 0.004f;
      static constexpr float kShimmer = 0.03f;

      enum {
        kReset,
        // A legato note change in mono mode sends no note-on here: the
        // syllable carries on at the new pitch.
        kRetrigger,
        kMidi,
        kAmpEnvMode,
        kLevel,
        kPan,
        kNumInputs
      };

      enum {
        kRaw,
        kLevelled,
        kHold,
        kNumOutputs
      };

      // Values of the amp envelope mode control.
      enum AmpEnvMode {
        kVoiceEnvelope,
        kEnvelope1
      };

      explicit SpeechSource(std::shared_ptr<SpeechSlot> slot);

      void process(int num_samples) override;
      Processor* clone() const override { return new SpeechSource(*this); }

    private:
      struct Grain {
        // The phrase the audio belongs to.
        uint32_t generation = 0;
        // The recording around a mark, read from `start` by `step` source
        // samples per output sample under a Hann window.
        const float* audio = nullptr;
        int length = 0;
        double start = 0.0;
        float gain = 1.0f;
        // The steady vowel grain, read from `steady_start`; windowed again
        // only when the grain is shorter than it.
        const float* steady = nullptr;
        int steady_length = 0;
        double steady_start = 0.0;
        float steady_gain = 0.0f;
        bool steady_windowed = false;
        double step = 1.0;
        // Output samples since the grain started, and its length.
        double time = 0.0;
        double duration = 0.0;
      };

      struct VoiceState {
        uint32_t generation = 0;
        int syllable = -1;
        int layer = 0;
        // Where the voice reads the syllable, in source samples.
        double position = 0.0;
        bool held = false;
        // The key is down and the voice has reached the hold point.
        bool holding = false;
        // Grains are still being laid (the syllable has not ended).
        bool singing = false;
        // How much of each grain is the steady vowel, 0 to 1.
        float steady = 0.0f;
        // Output samples until the next grain.
        double countdown = 0.0;
        uint32_t random = 1;
        std::array<Grain, kMaxGrains> grains {};
        int num_grains = 0;
      };

      void startSyllable(VoiceState& state, uint32_t generation, int syllable, int layer);
      void launchGrain(VoiceState& state, const speech::Syllable& syllable, double ratio, double note_period);
      float renderGrains(VoiceState& state);
      void advance(VoiceState& state, const speech::Syllable& syllable, double ratio, float steady_in,
                   float steady_out);
      static float nextRandom(VoiceState& state);
      // Moves a sounding note to the layer nearest `frequency` when it has
      // left its own layer by more than half a semitone of the way.
      void followPitch(VoiceState& state, const speech::Phrase& phrase, float frequency);

      std::shared_ptr<SpeechSlot> slot_;
      std::array<VoiceState, kVoicesPerProcessor> voices_;
      poly_float pan_amplitude_ = 0.0f;
      bool pan_initialized_ = false;

      JUCE_LEAK_DETECTOR(SpeechSource)
  };
} // namespace vital
