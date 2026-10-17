// SPDX-License-Identifier: GPL-3.0-or-later
#include "speech_source.h"

#include "futils.h"
#include "synth_constants.h"

#if ATLAS_HAS_OPENEVV
#include "atlas_openevv.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstring>

namespace vital {
  namespace {
    using atlas::openevv::Frame;
    using atlas::openevv::Utterance;
    namespace evv = atlas::openevv;

    constexpr int kSpeechTaps = 2 * SpeechSource::kHalfTaps;
    constexpr double kSpeechPi = 3.14159265358979323846;

    // The resampler's windowed sinc, cut at 90% of the synthesiser's Nyquist,
    // one row per fraction of an input sample (and one more for the
    // interpolation past the last).
    struct SpeechSincTable {
      float weights[SpeechSource::kPhases + 1][kSpeechTaps];

      SpeechSincTable() {
        constexpr double kCutoff = 0.9;
        constexpr double kBeta = 8.0;
        const auto bessel = [](double x) {
          double sum = 1.0;
          double term = 1.0;
          for (int k = 1; k < 32; ++k) {
            term *= (x / (2.0 * k)) * (x / (2.0 * k));
            sum += term;
          }
          return sum;
        };
        const double window_norm = bessel(kBeta);
        for (int phase = 0; phase <= SpeechSource::kPhases; ++phase) {
          const double fraction = static_cast<double>(phase) / SpeechSource::kPhases;
          double sum = 0.0;
          double row[kSpeechTaps];
          for (int tap = 0; tap < kSpeechTaps; ++tap) {
            const double x = tap - (SpeechSource::kHalfTaps - 1) - fraction;
            const double y = kCutoff * x;
            const double sinc = std::abs(y) < 1e-9 ? 1.0 : std::sin(kSpeechPi * y) / (kSpeechPi * y);
            const double w = x / SpeechSource::kHalfTaps;
            const double window = std::abs(w) >= 1.0 ? 0.0 : bessel(kBeta * std::sqrt(1.0 - w * w)) / window_norm;
            row[tap] = kCutoff * sinc * window;
            sum += row[tap];
          }
          for (int tap = 0; tap < kSpeechTaps; ++tap)
            weights[phase][tap] = static_cast<float>(row[tap] / sum);
        }
      }
    };

    const SpeechSincTable& speechSincTable() {
      static const SpeechSincTable table;
      return table;
    }

    void silenceFrame(Frame& frame) {
      frame[evv::kAv] = 0;
      frame[evv::kAh] = 0;
      frame[evv::kAf] = 0;
      frame[evv::kAb] = 0;
      for (int i = evv::kA1f; i <= evv::kA8f; ++i)
        frame[static_cast<size_t>(i)] = 0;
      for (int i = evv::kAnv; i <= evv::kAtv; ++i)
        frame[static_cast<size_t>(i)] = 0;
    }
  } // namespace

  void SpeechClock::process(int num_samples) {
    const double sample_rate = getSampleRate();
    slot_->advanceClock(sample_rate > 0.0 ? num_samples / sample_rate : 0.0);
  }

  SpeechSource::SpeechSource(std::shared_ptr<SpeechSlot> slot) :
      Processor(kNumInputs, kNumOutputs), slot_(std::move(slot)) {
    // Built on the message thread with the slot's module, never by a note.
    speechSincTable();
#if ATLAS_HAS_OPENEVV
    atlas_openevv_start();
#endif
  }

  Processor* SpeechSource::clone() const {
    auto* copy = new SpeechSource(*this);
    // A synthesiser's state points into itself: a copy starts afresh.
    for (VoiceState& state : copy->voices_) {
      copy->silence(state);
      state.held = false;
      state.counted = false;
    }
    return copy;
  }

  int32_t SpeechSource::framePitch(int32_t spoken, int32_t reference, float note, int pitch_mode, float intonation) {
    double hertz;
    if (pitch_mode == kSpokenPitch)
      hertz = (spoken > 0 ? spoken : reference) * 0.1 * std::exp2((note - 60.0) / 12.0);
    else {
      hertz = 440.0 * std::exp2((note - 69.0) / 12.0);
      if (spoken > 0 && reference > 0 && intonation > 0.0f)
        hertz *= std::pow(static_cast<double>(spoken) / reference, static_cast<double>(intonation));
    }
    if (!std::isfinite(hertz))
      hertz = 100.0;
    // The synthesiser's pitch periods stay sane from 20 hertz to 2 kilohertz.
    return static_cast<int32_t>(std::lround(std::clamp(hertz, 20.0, 2000.0) * 10.0));
  }

  SpeechSource::Controls SpeechSource::controls(int lane) const {
    const auto base = [this](int index) { return input(index)->at(0)[0]; };
    const auto lane_value = [this, lane](int index, float low, float high, float fallback) {
      const float value = input(index)->at(0)[lane];
      return std::isfinite(value) ? std::clamp(value, low, high) : fallback;
    };
    Controls result;
    result.split = std::clamp(static_cast<int>(base(kSplit) + 0.5f), 0, evv::kNumSplits - 1);
    result.sustain = base(kSustain) >= 0.5f;
    result.loop = base(kLoop) >= 0.5f;
    result.restart_on_release = base(kRestartOnRelease) >= 0.5f;
    result.pitch_mode = std::clamp(static_cast<int>(base(kPitchMode) + 0.5f), 0, kNumPitchModes - 1);
    result.quality = std::clamp(static_cast<int>(base(kQuality) + 0.5f), 0, kNumQualities - 1);
    result.intonation = lane_value(kIntonation, 0.0f, 1.0f, 0.0f);
    result.rate = lane_value(kRate, 0.25f, 4.0f, 1.0f);
    return result;
  }

  void SpeechSource::silence(VoiceState& state) {
    state.active = false;
    state.open = false;
    state.playing = false;
    state.tail = 0;
    state.fade_remaining = 0;
    state.input_count = 0;
    state.read_position = 0.0;
    state.klatt_voice = -1;
    state.klatt_volume = -1;
  }

  bool SpeechSource::sounding(const VoiceState& state) const {
    return state.active;
  }

  void SpeechSource::start(VoiceState& state, const Controls& controls, double time) {
    const int unit_index = slot_->takeUnit(controls.split, controls.loop, time);
    const Utterance* utterance = slot_->get();
    if (unit_index < 0 || utterance == nullptr) {
      // The text has ended: the key plays nothing, and the voice's last piece fades.
      if (sounding(state)) {
        state.fade_length = std::max(1, static_cast<int>(sample_rate_ * kFadeSeconds));
        state.fade_remaining = state.fade_length;
      }
      return;
    }
    const evv::Unit& unit = utterance->unitsFor(controls.split)[static_cast<size_t>(unit_index)];
    if (!sounding(state)) {
      // From silence: a fresh synthesiser, and the resampler starts on zeros.
      state.open = false;
      state.input.fill(0.0f);
      state.input_count = kHalfTaps;
      state.read_position = kHalfTaps;
    }
    state.active = true;
    // Otherwise the synthesiser carries on into the new piece, as the engine
    // runs from one phoneme to the next, so there is no click.
    state.fade_remaining = 0;
    state.generation = slot_->generation();
    state.playing = true;
    state.tail = 0;
    state.start = unit.start;
    state.nucleus = unit.nucleus;
    state.end = unit.end;
    state.reference_f0 = unit.f0;
    state.frame_position = unit.start;
    state.held = true;
    if (!state.counted) {
      slot_->keyDown();
      state.counted = true;
    }
  }

  void SpeechSource::release(VoiceState& state, const Controls& controls) {
    state.held = false;
    if (state.counted) {
      slot_->keyUp(controls.restart_on_release);
      state.counted = false;
    }
  }

  void SpeechSource::collect(void* user, const int32_t* samples, int32_t count) {
    auto* state = static_cast<VoiceState*>(user);
    const int room = kInputSize - state->input_count;
    const int take = std::min(room, static_cast<int>(count));
    for (int i = 0; i < take; ++i)
      state->input[static_cast<size_t>(state->input_count + i)] = static_cast<float>(samples[i]) * kOutputScale;
    state->input_count += take;
  }

  bool SpeechSource::synthesizeFrame(VoiceState& state, const Controls& controls) {
#if ATLAS_HAS_OPENEVV
    const Utterance* utterance = slot_->get(state.generation);
    if (state.playing && utterance == nullptr) {
      state.playing = false;
      state.tail = kTailFrames;
    }

    Frame frame;
    if (state.playing) {
      int index = static_cast<int>(state.frame_position);
      const bool sustaining = controls.sustain && state.held && index >= state.nucleus;
      if (sustaining) {
        index = state.nucleus;
        state.frame_position = state.nucleus;
      }
      if (index >= state.end || index >= static_cast<int>(utterance->frames.size())) {
        state.playing = false;
        state.tail = kTailFrames;
      }
      else {
        frame = utterance->frames[static_cast<size_t>(index)];
        const int klatt_voice = utterance->voice_of_frame[static_cast<size_t>(index)];
        const int rate = controls.quality == k22k ? evv::kWideRate : evv::kEngineRate;
        const int32_t volume = utterance->volumes[static_cast<size_t>(index)];
        if (!state.open || state.klatt_voice != klatt_voice || state.klatt_rate != rate) {
          const evv::KlattVoice& source = utterance->voices[static_cast<size_t>(klatt_voice)];
          const AtlasKlattVoice voice { source.add_20, source.add_24, source.add_28, source.add_2c,
                                        source.n_formants };
          state.open = atlas_klatt_open(state.klatt, rate, &voice, volume, collect, &state) != 0;
          state.klatt_voice = klatt_voice;
          state.klatt_rate = rate;
          state.klatt_volume = volume;
          if (!state.open) {
            state.playing = false;
            return false;
          }
        }
        if (state.klatt_volume != volume) {
          atlas_klatt_set_volume(state.klatt, volume);
          state.klatt_volume = volume;
        }
        frame[evv::kF0] = framePitch(frame[evv::kF0], state.reference_f0, state.note, controls.pitch_mode,
                                     controls.intonation);
        atlas_klatt_set_sink(state.klatt, collect, &state);
        atlas_klatt_synth(state.klatt, frame.data());
        state.last_frame = frame;
        if (!sustaining)
          state.frame_position += controls.rate;
        return true;
      }
    }

    if (state.tail > 0 && state.open) {
      frame = state.last_frame;
      silenceFrame(frame);
      atlas_klatt_set_sink(state.klatt, collect, &state);
      atlas_klatt_synth(state.klatt, frame.data());
      --state.tail;
      return true;
    }
#else
    (void)state;
    (void)controls;
#endif
    return false;
  }

  void SpeechSource::render(VoiceState& state, const Controls& controls, float* output, int count) {
    const SpeechSincTable& table = speechSincTable();
    for (int i = 0; i < count; ++i) {
      if (!state.active) {
        output[i] = 0.0f;
        continue;
      }
      // Compact the buffer before it fills.
      if (state.read_position > kInputSize / 2) {
        const int shift = static_cast<int>(state.read_position) - kHalfTaps;
        std::memmove(state.input.data(), state.input.data() + shift,
                     sizeof(float) * static_cast<size_t>(state.input_count - shift));
        state.input_count -= shift;
        state.read_position -= shift;
      }
      int base = static_cast<int>(state.read_position);
      // A frame of no length adds nothing; give up after a few of them.
      for (int attempts = 0; base + kHalfTaps >= state.input_count; ++attempts) {
        if (attempts >= 16 || !synthesizeFrame(state, controls)) {
          // Nothing more to say: the tail frames have already rung out.
          silence(state);
          break;
        }
      }
      if (!state.active) {
        output[i] = 0.0f;
        continue;
      }

      const double fraction = (state.read_position - base) * kPhases;
      const int phase = std::min(kPhases - 1, static_cast<int>(fraction));
      const float blend = static_cast<float>(fraction - phase);
      const float* low = table.weights[phase];
      const float* high = table.weights[phase + 1];
      const float* samples = state.input.data() + base - (kHalfTaps - 1);
      float sum = 0.0f;
      for (int tap = 0; tap < kSpeechTaps; ++tap)
        sum += samples[tap] * (low[tap] + (high[tap] - low[tap]) * blend);

      if (state.fade_remaining > 0) {
        sum *= static_cast<float>(state.fade_remaining) / state.fade_length;
        if (--state.fade_remaining == 0) {
          silence(state);
          sum = 0.0f;
        }
      }
      output[i] = sum;
      if (state.active)
        state.read_position += static_cast<double>(state.klatt_rate) / sample_rate_;
    }
  }

  void SpeechSource::process(int num_samples) {
    if (num_samples <= 0)
      return;

    const double sample_rate = getSampleRate();
    if (sample_rate != sample_rate_) {
      for (VoiceState& state : voices_)
        silence(state);
      sample_rate_ = sample_rate;
    }

    const int count = std::min(num_samples, kMaxBufferSize * kMaxOversample);
    const poly_float note_input = input(kMidi)->at(0) + input(kTranspose)->at(0) + input(kTune)->at(0);
    const bool hold_enabled = static_cast<int>(input(kAmpEnvMode)->at(0)[0] + 0.5f) == kSpeechEnvelope;

    const poly_mask reset_mask = getResetMask(kReset);
    const Output* events = input(kVoiceEvent)->source;
    const Output* retriggers = input(kRetrigger)->source;
    const SlotModulation::Block modulation = modulation_.resolve(*this, kDistortionType, kDistortionAmount,
                                                                 kSecondaryDistortionType,
                                                                 kSecondaryDistortionAmount, false);

    poly_float* raw = output(kRaw)->buffer;
    poly_float* hold = output(kHold)->buffer;
    float mono[kMaxBufferSize * kMaxOversample];
    poly_float hold_value = 0.0f;

    for (int voice = 0; voice < kVoicesPerProcessor; ++voice) {
      const int lane = 2 * voice;
      VoiceState& state = voices_[static_cast<size_t>(voice)];
      const Controls lane_controls = controls(lane);
      state.note = note_input[lane];
      if (reset_mask[lane]) {
        if (state.counted)
          release(state, lane_controls);
        silence(state);
      }

      int event = -1;
      int offset = count;
      if (events->trigger_mask[lane]) {
        event = static_cast<int>(events->trigger_value[lane]);
        offset = std::max(0, std::min(count - 1, static_cast<int>(events->trigger_offset[lane])));
      }

      // A legato key raises a voice-on with no retrigger: the voice keeps its
      // piece and only follows the new pitch.
      const bool legato = event == kVoiceOn && state.held &&
                          !(retriggers->trigger_mask[lane] &&
                            static_cast<int>(retriggers->trigger_value[lane]) == kVoiceOn);

      render(state, lane_controls, mono, offset);
      if (event == kVoiceOn && !legato)
        start(state, lane_controls, slot_->blockTime() + offset / std::max(1.0, sample_rate_));
      else if (event == kVoiceOff)
        release(state, lane_controls);
      else if (event == kVoiceKill) {
        release(state, lane_controls);
        if (sounding(state)) {
          state.fade_length = std::max(1, static_cast<int>(sample_rate_ * kFadeSeconds));
          state.fade_remaining = state.fade_length;
        }
      }
      render(state, lane_controls, mono + offset, count - offset);

      if (hold_enabled && sounding(state)) {
        hold_value.set(lane, 1.0f);
        hold_value.set(lane + 1, 1.0f);
      }
      for (int i = 0; i < count; ++i) {
        raw[i].set(lane, mono[i]);
        raw[i].set(lane + 1, mono[i]);
      }
    }
    for (int i = 0; i < count; ++i)
      hold[i] = hold_value;
    modulation.applyRingMod(raw, count);

    // Slot level and pan, as the other types apply them.
    const poly_float target_pan = futils::panAmplitude(utils::clamp(input(kPan)->at(0), -1.0f, 1.0f));
    if (!pan_initialized_) {
      pan_amplitude_ = target_pan;
      pan_initialized_ = true;
    }
    poly_float current_pan = utils::maskLoad(pan_amplitude_, target_pan, reset_mask);
    const poly_float delta_pan = (target_pan - current_pan) * (1.0f / count);
    pan_amplitude_ = target_pan;

    const Output* level_source = input(kLevel)->source;
    const bool audio_rate_level = level_source->buffer_size > 1;
    poly_float* levelled = output(kLevelled)->buffer;
    for (int i = 0; i < count; ++i) {
      current_pan += delta_pan;
      const poly_float level = utils::clamp(level_source->buffer[audio_rate_level ? i : 0], 0.0f, kMaxAmplitude);
      levelled[i] = current_pan * level * level * raw[i];
    }
  }
} // namespace vital
