// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "mf/FluteVoice.h"

#include <array>
#include <cstdint>

namespace vital {
  struct MultiSampleMidiState;
}

namespace atlas::flute {

/** The flute of one oscillator slot: Modeled Flute's FluteVoice (the virtual
    breath and the jet-drive model, src/synthesis/effects/flute/mf), driven as
    its plugin drives it. The keys' own note-ons and note-offs reach it, not
    the synth voices' events, so its tonguing and slurs are the original's
    whatever Atlas's voice count, Legato or voice override: a key pressed
    while another is held slurs, one pressed after a gap tongues, and
    releasing the sounding key slurs back to a key still held. The
    controllers of the block (dynamics, expression, mod wheel, sustain) take
    effect at their sample, the block being cut there as the plugin cuts it.

    It renders once per block, ahead of the voices, into a mono buffer that
    the lane of the voice last started copies out (claim), so Envelope 1 and
    the voice filters can shape what it plays. Everything runs on the audio
    thread without allocating: the voice is prepared for the highest rate
    Atlas reaches when the instrument is built. */
class Instrument {
public:
    static constexpr int kMaxBlock = 1024;   // vital::kMaxBufferSize * kMaxOversample
    static constexpr int kMaxNoteEvents = 64;
    // 192 kHz at eight times oversampling: the delay lines are sized for it once.
    static constexpr double kMaxSampleRate = 192000.0 * 8.0;

    // The original plugin's Dynamics From and Dynamics CC choices.
    enum DynamicsSource { kVelocityThenController, kControllerOnly, kVelocityOnly };
    enum DynamicsController { kModWheelController, kExpressionController, kBreathController };
    static constexpr int kControllerNumbers[3] = { 1, 11, 2 };

    /** The slot's controls, read every block. */
    struct Params {
        float air = 0.5f, brightness = 0.5f, attack = 0.5f;
        float vibratoDepth = 0.35f, vibratoRateHz = 5.0f, vibratoDelaySec = 0.2f;
        float humanize = 0.5f, dynamicRangeDb = 18.0f, releaseSec = 0.15f;
        float tuningA4 = 440.0f;
        int dynamicsSource = kVelocityThenController;
        int dynamicsController = kModWheelController;
        bool overlap = true;
        int transpose = 0;
    };

    Instrument();

    // ---- Audio thread, before the block renders ----
    /** A key goes down or up `sample` samples into the coming block (in the
        voices' samples). A key is remembered by the number it was played at,
        so a Transpose change while it is down does not leave it hanging. */
    void noteOn (int note, float velocity, int sample);
    void noteOff (int note, int sample);
    void allNotesOff (int sample);
    /** The block's controllers (the slot reads them at their host-rate offsets
        times `oversample`), the voices' sample rate, the pitch wheel (-1 to 1)
        with the bend range in semitones, and whether the slot is on: off,
        nothing renders and the keys are let go. */
    void beginBlock (const vital::MultiSampleMidiState* midi, int oversample, double sampleRate, float pitchWheel,
                     float bendRange, bool enabled);
    /** The voice whose lane carries the audio from `sample` on, by its
        activation number (the voice handler's note count). */
    void claim (int voiceId, int sample);

    // ---- Audio thread, the block ----
    void render (const Params&, int numSamples);
    const float* output() const { return buffer_.data(); }
    /** Whether breath or tone is still there, so the lane holds its voice open. */
    bool sounding() const { return ! voice_.isSilent(); }

    // What the lanes read after render(): who carries the audio, and whether
    // it changed hands in this block and where.
    int owner() const { return owner_; }
    int previousOwner() const { return previousOwner_; }
    bool handedOver() const { return handedOver_; }
    int handoverSample() const { return handoverSample_; }

    const mf::FluteVoice& voice() const { return voice_; }
    double sampleRate() const { return sampleRate_; }

private:
    // juce::SmoothedValue<float>'s linear ramp, as the original plugin smooths
    // Air, Brightness, Vibrato depth and Tuning over 20 ms.
    struct LinearSmoother {
        void reset (double sampleRate, double seconds);
        void setCurrentAndTarget (float v) { current_ = target_ = v; countdown_ = 0; }
        void setTarget (float v);
        float skip (int n);
        float current() const { return current_; }
        bool isSmoothing() const { return countdown_ > 0; }
    private:
        float current_ = 0.0f, target_ = 0.0f, step_ = 0.0f;
        int steps_ = 1, countdown_ = 0;
    };

    struct NoteEvent {
        int sample = 0;
        int note = 0;
        float velocity = 0.0f;
        enum Kind { kOn, kOff, kAllOff } kind = kOn;
    };

    void prepare (double sampleRate);
    void readTargets (const Params&, bool snap);
    void applyVoiceParams (int numSamplesAdvanced);
    void renderVoice (float* out, int numSamples);
    void handleNote (const NoteEvent&);
    void handleController (int cc, int value);
    void releaseSustained();
    void updateBend();
    int keyFor (int note) const;

    mf::FluteVoice voice_;
    mf::PerformerParams perf_;   // the parameters last given to the voice
    Params params_;
    double sampleRate_ = 0.0;
    std::array<float, kMaxBlock> buffer_ {};

    LinearSmoother air_, brightness_, vibratoDepth_, tuning_;

    // MIDI state, as the original plugin keeps it.
    bool sustainDown_ = false;
    std::array<bool, 128> sustained_ {};   // key released while the pedal was down
    std::array<signed char, 128> keyOf_ {}; // the transposed key each note number plays, or -1
    float pitchWheel_ = 0.0f, bendRange_ = 2.0f;
    float expression_ = 1.0f, wheel_ = 0.0f;

    std::array<NoteEvent, kMaxNoteEvents> notes_ {};
    int numNotes_ = 0;
    const vital::MultiSampleMidiState* midi_ = nullptr;
    int oversample_ = 1;
    double blockRate_ = 0.0;
    bool enabled_ = false;

    int owner_ = -1, previousOwner_ = -1, handoverSample_ = 0;
    bool handedOver_ = false, pendingHandover_ = false;
};

} // namespace atlas::flute
