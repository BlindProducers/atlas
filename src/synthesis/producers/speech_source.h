// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "processor.h"
#include "slot_modulation.h"
#include "speech_slot.h"
#include "utils.h"

#include <array>
#include <memory>

namespace vital {
  // The per-voice engine of the Speech oscillator type. Each note-on takes
  // the slot's next syllable (or word, or the whole text, with the Split
  // control) and sings it from the phrase layer espeak-ng spoke nearest the
  // note's pitch: TD-PSOLA lays grains of that recording one note period
  // apart, so the voice keeps its formants.
  //
  // Sung (a monotone recording): the onset plays at its spoken speed; while
  // the key is down the last syllable's vowel crossfades into its steady
  // grain, repeated with a little jitter and shimmer; after release the rest
  // of the syllable plays. Spoken (a recording with espeak-ng's intonation):
  // the unit plays straight through, its grains as far apart as the
  // recording's periods over the note's interval from middle C, so middle C
  // speaks at the voice's own pitch. A mono legato note change keeps the
  // syllable and only moves its pitch, moving to the layer nearest the new
  // pitch.
  //
  // Formant reads each grain faster or slower, which moves the formants and
  // leaves the pitch; Breathiness crossfades each grain into the syllable's
  // whispered twin; Speed scales how fast the recording is read between
  // holds; Vibrato fades in after the note starts.
  //
  // With the Voice envelope mode the syllable shapes the note and holds the
  // voice open until its last consonant ends; with Envelope 1 the slot
  // follows Env 1 like the wavetable oscillators. A voice the voice handler
  // kills (to free it for a new note) fades out over kVoiceKillTime instead
  // of singing on.
  //
  // Slot FM bends the note's pitch at audio rate: it scales how fast grains
  // are laid, never how fast the syllable is read, so the formants and the
  // syllable's timing stay put. Ring modulation multiplies the output.
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
      // Spoken notes speak at the voice's own pitch on this key.
      static constexpr float kSpokenRootNote = 60.0f;
      // Vibrato starts this long after the note and reaches its depth over
      // the next kVibratoFadeSeconds.
      static constexpr float kVibratoDelaySeconds = 0.15f;
      static constexpr float kVibratoFadeSeconds = 0.35f;
      // Speed 100% reads the recording at this fraction of espeak-ng's
      // speaking speed, which sings too fast. Speed never goes below
      // kMinSpeed, so even at 0% a released note finishes its syllable.
      static constexpr float kBaseSpeed = 0.65f;
      static constexpr float kMinSpeed = 0.02f;

      enum {
        kReset,
        // A legato note change in mono mode sends no note-on here: the
        // syllable carries on at the new pitch.
        kRetrigger,
        kMidi,
        kAmpEnvMode,
        kLevel,
        kPan,
        kTranspose,
        kTune,
        kDistortionType,
        kDistortionAmount,
        kSecondaryDistortionType,
        kSecondaryDistortionAmount,
        kSplit,
        // Semitones.
        kFormant,
        // 0 to 1.
        kBreath,
        // 0 to 4, 1 being kBaseSpeed.
        kSpeed,
        // Depth in semitones and rate as log2 Hz.
        kVibrato,
        kVibratoRate,
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
      SlotModulation& modulation() { return modulation_; }

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
        // The whispered twin, read from `breath_start` under the same window.
        const float* breath = nullptr;
        double breath_start = 0.0;
        float breath_gain = 0.0f;
        double step = 1.0;
        // Output samples since the grain started, and its length.
        double time = 0.0;
        double duration = 0.0;
      };

      struct VoiceState {
        uint32_t generation = 0;
        int syllable = -1;
        // The note's last syllable: the one whose vowel holds.
        int last_syllable = -1;
        int layer = 0;
        // Where the voice reads the syllable, in source samples.
        double position = 0.0;
        bool held = false;
        // The key is down and the voice has reached the hold point.
        bool holding = false;
        // Grains are still being laid (the syllable has not ended).
        bool singing = false;
        // Output samples left of a voice kill's fade, and its length; the
        // voice falls silent and stops when it ends.
        int kill_remaining = 0;
        int kill_length = 0;
        // How much of each grain is the steady vowel, 0 to 1.
        float steady = 0.0f;
        // Output samples until the next grain.
        double countdown = 0.0;
        // Seconds since the note started, and the vibrato's phase in cycles.
        double vibrato_time = 0.0;
        double vibrato_phase = 0.0;
        uint32_t random = 1;
        std::array<Grain, kMaxGrains> grains {};
        int num_grains = 0;
      };

      // How the next grains are laid, for one voice and block.
      struct GrainSettings {
        // Source samples per output sample at the recording's speed.
        double ratio = 1.0;
        // How much faster than that a grain reads its audio (Formant).
        double formant = 1.0;
        // Sung: the note's period in output samples. Spoken: how many times
        // higher than the recording the voice speaks.
        double note_period = 1.0;
        double pitch_factor = 1.0;
        bool spoken = false;
        // Speed, for the spread of unvoiced grains when reading slowly.
        double speed = 1.0;
        // Breathiness as the voice's and the whisper's gains.
        float voice_gain = 1.0f;
        float breath_gain = 0.0f;
      };

      void startSyllable(VoiceState& state, uint32_t generation, int syllable, int last_syllable, int layer);
      void launchGrain(VoiceState& state, const speech::Syllable& syllable, const GrainSettings& settings);
      float renderGrains(VoiceState& state);
      void advance(VoiceState& state, const speech::Syllable& syllable, double step, bool can_hold,
                   float steady_in, float steady_out);
      static float nextRandom(VoiceState& state);
      // Moves a sounding note to the layer nearest `frequency` when it has
      // left its own layer by more than half a semitone of the way.
      void followPitch(VoiceState& state, const speech::Phrase& phrase, float frequency);

      std::shared_ptr<SpeechSlot> slot_;
      SlotModulation modulation_;
      std::array<VoiceState, kVoicesPerProcessor> voices_;
      poly_float pan_amplitude_ = 0.0f;
      bool pan_initialized_ = false;

      JUCE_LEAK_DETECTOR(SpeechSource)
  };
} // namespace vital
