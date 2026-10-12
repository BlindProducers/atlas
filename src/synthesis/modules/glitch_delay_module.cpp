// SPDX-License-Identifier: GPL-3.0-or-later
#include "glitch_delay_module.h"
#include "synth_parameters.h"
#include "trance_gate_clock.h"
#include "glitch_delay/glitch_delay_engine.h"
#include "glitch_delay/note_values.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace vital {
  namespace {
    namespace gd = atlas::glitch_delay;

    const char* const kGlobalSuffixes[GlitchDelayModule::kNumGlobalControls] = {
      "sync", "glide", "freeze", "freeze_sustain", "placement", "buffer_size", "buffer_sync", "max_glitches",
      "length_min", "length_max", "reproducible", "seed", "smear_amount", "smear_size", "stop_tape", "stop_time",
      "start_time", "mix", "output_gain", "tap_view"
    };

    const char* const kTapSuffixes[GlitchDelayModule::kNumTapControls] = {
      "enabled", "time", "time_sync", "volume", "pan", "feedback", "low_cut", "high_cut",
      "reverse_prob",
      "stutter_prob", "stutter_min", "stutter_max", "stutter_sync_min", "stutter_sync_max",
      "grain_prob", "grain_size_min", "grain_size_max", "grain_density_min", "grain_density_max",
      "pitch_prob", "pitch_min", "pitch_max", "pitch_speed_min", "pitch_speed_max", "pitch_mode",
      "lpc_prob", "lpc_min", "lpc_max",
      "ceps_prob", "ceps_min", "ceps_max",
      "ring_prob", "ring_min", "ring_max",
      "fm_prob", "fm_ratio_min", "fm_ratio_max", "fm_index_min", "fm_index_max",
      "crush_prob", "crush_bits_min", "crush_bits_max", "crush_rate_min", "crush_rate_max"
    };

    constexpr double kFallbackBeatsPerSecond = 2.0;
    constexpr double kQuartersPerBar = 4.0;   // Atlas has no host time signature.
    constexpr float kVolumeFloorDb = -60.0f;  // Shown as the lowest volume and treated as silence.
    // How far the song position may wander from the samples processed before
    // it counts as a jump, as rounding in the host's song time can.
    constexpr long long kPositionSlackSamples = 2;

    int choice(float value, int maximum) {
      return std::clamp(static_cast<int>(std::lround(value)), 0, maximum);
    }

    float exp2f(float value) { return std::exp2(value); }

    // The stored value of a time stored as log2 milliseconds, in samples.
    float log2MsToSamples(float value, double sample_rate) {
      return static_cast<float>(std::exp2(static_cast<double>(value)) * 0.001 * sample_rate);
    }

    float volumeDbToGain(float db) {
      return db <= kVolumeFloorDb ? 0.0f : std::pow(10.0f, db / 20.0f);
    }

    // Stutter's synced slice choices, as indices into NoteValues::all(). Built
    // by init(), not on the audio thread.
    const std::vector<int>& stutterNoteIndices() {
      static const std::vector<int> indices = [] {
        std::vector<int> result;
        for (const auto& label : gd::NoteValues::labelsBetween(0.0, 1.0))
          result.push_back(gd::NoteValues::indexOf(label));
        return result;
      }();
      return indices;
    }
  }

  const char* GlitchDelayModule::globalSuffix(int control) { return kGlobalSuffixes[control]; }
  const char* GlitchDelayModule::tapSuffix(int control) { return kTapSuffixes[control]; }

  bool GlitchDelayModule::isGlobalModulated(int control) {
    return control == kGlide || control == kSmearAmount || control == kSmearSize || control == kMix;
  }

  bool GlitchDelayModule::isTapModulated(int control) {
    return control != kEnabled && control != kTimeSync && control != kStutterSyncMin &&
           control != kStutterSyncMax && control != kPitchMode;
  }

  float GlitchDelayModule::Control::value() const {
    const float raw = modulated != nullptr ? modulated->buffer[0][0] : base->value();
    return std::clamp(raw, min, max);
  }

  GlitchDelayModule::GlitchDelayModule(const Output* beats_per_second, const TranceGateClock* transport,
                                       std::string prefix) :
      SynthModule(0, 1), engine_(std::make_unique<gd::Engine>()), beats_per_second_(beats_per_second),
      transport_(transport), prefix_(std::move(prefix)) {
    // The engine's buffers are sized for a sample rate, so they are allocated
    // when the rate is set rather than here.
  }

  GlitchDelayModule::~GlitchDelayModule() = default;

  GlitchDelayModule::Control GlitchDelayModule::createControl(const std::string& name, bool modulated) {
    Control control;
    const ValueDetails& details = Parameters::getDetails(name);
    control.min = details.min;
    control.max = details.max;
    if (modulated)
      control.modulated = createBaseModControl(name);
    else
      control.base = createBaseControl(name);
    return control;
  }

  void GlitchDelayModule::init() {
    stutterNoteIndices();
    const std::string name = prefix_ + "glitch_delay_";
    for (int i = 0; i < kNumGlobalControls; ++i)
      global_[i] = createControl(name + kGlobalSuffixes[i], isGlobalModulated(i));
    for (int tap = 0; tap < kNumTaps; ++tap) {
      const std::string tap_name = name + "tap_" + std::to_string(tap + 1) + "_";
      for (int i = 0; i < kNumTapControls; ++i)
        taps_[tap][i] = createControl(tap_name + kTapSuffixes[i], isTapModulated(i));
    }
    SynthModule::init();
  }

  void GlitchDelayModule::prepareEngine() {
    engine_->prepare(getSampleRate(), kMaxBufferSize, kMaxDelaySeconds);
    prepared_rate_ = getSampleRate();
    reset_pending_ = true;
  }

  void GlitchDelayModule::updateEngine(int samples) {
    const auto& notes = gd::NoteValues::all();
    const auto& stutter_notes = stutterNoteIndices();
    const double sample_rate = getSampleRate();
    const double max_delay_samples = kMaxDelaySeconds * sample_rate;

    double bps = beats_per_second_ != nullptr ? beats_per_second_->buffer[0][0] : 0.0;
    if (!(bps > 0.0))
      bps = kFallbackBeatsPerSecond;
    const double samples_per_quarter = sample_rate / bps;
    const auto note_quarters = [&notes](int index) {
      return notes.getReference(std::clamp(index, 0, notes.size() - 1)).lengthInQuarters(kQuartersPerBar);
    };
    const auto value = [this](int control) { return global_[control].value(); };
    const bool synced = value(kSync) >= 0.5f;

    // Glitch chunks: note values on the beat grid when synced, milliseconds otherwise.
    const double chunk_quarters = note_quarters(choice(value(kBufferSync), notes.size() - 1));
    const double chunk_samples = synced ? chunk_quarters * samples_per_quarter
                                        : log2MsToSamples(value(kBufferSize), sample_rate);

    gd::GlobalSettings g;
    const float glide = value(kGlide);
    g.glideSeconds = glide * glide * 0.001f;
    g.freeze = value(kFreeze) >= 0.5f;
    g.freezeSustain = value(kFreezeSustain) >= 0.5f;
    g.mix = value(kMix);
    g.outputGain = std::pow(10.0f, value(kOutputGain) / 20.0f);
    g.smearAmount = value(kSmearAmount);
    const float smear = value(kSmearSize);
    g.smearSeconds = smear * smear * 0.001f;
    g.tapeStopped = value(kStopTape) >= 0.5f;
    g.tapeStopSeconds = exp2f(value(kStopTime)) * 0.001f;
    g.tapeStartSeconds = exp2f(value(kStartTime)) * 0.001f;
    g.glitch.outputAndFeedback = choice(value(kPlacement), 1) == 1;
    g.glitch.chunkSamples = std::max(1, static_cast<int>(std::llround(std::min(chunk_samples, max_delay_samples))));
    g.glitch.maxSimultaneous = choice(value(kMaxGlitches), 4);
    g.glitch.lengthChunks = { value(kLengthMin), value(kLengthMax) };
    g.reproducible = value(kReproducible) >= 0.5f;
    g.seed = choice(value(kSeed), 9999);
    engine_->setGlobalSettings(g);

    // The song position, when the host is playing. A position within a couple
    // of samples of where the last block ended is taken as continuous, so the
    // rounding in Atlas's song time never restarts reproducible randomness.
    gd::TransportInfo transport;
    transport.playing = transport_ != nullptr && transport_->playing();
    transport.hasPosition = transport.playing;
    transport.ppq = seconds_ * bps;
    transport.samplesPerQuarter = samples_per_quarter;
    transport.synced = synced;
    transport.chunkQuarters = chunk_quarters;
    if (transport.playing) {
      long long position = std::llround(seconds_ * sample_rate);
      if (expected_position_ && std::llabs(position - *expected_position_) <= kPositionSlackSamples)
        position = *expected_position_;
      transport.samplePosition = position;
      expected_position_ = position + samples;
    }
    else {
      expected_position_.reset();
    }
    engine_->setTransport(transport);

    for (int tap = 0; tap < kNumTaps; ++tap) {
      const auto& controls = taps_[tap];
      const auto tap_value = [&controls](int control) { return controls[control].value(); };

      const float time = tap_value(kTime);
      const double seconds = synced ? note_quarters(choice(tap_value(kTimeSync), notes.size() - 1)) / bps
                                    : time * time * time * 0.001;

      gd::TapSettings s;
      s.enabled = tap_value(kEnabled) >= 0.5f;
      s.delaySamples = static_cast<float>(std::min(seconds, kMaxDelaySeconds) * sample_rate);
      s.gain = volumeDbToGain(tap_value(kVolume));
      s.pan = tap_value(kPan);
      s.feedback = tap_value(kFeedback);
      s.lowCutHz = exp2f(tap_value(kLowCut));
      s.highCutHz = exp2f(tap_value(kHighCut));

      auto& glitch = s.glitch;
      const int probabilities[gd::numGlitchTypes] = {
        kReverseProb, kStutterProb, kGrainProb, kPitchProb, kLpcProb, kCepsProb, kRingProb, kFmProb, kCrushProb
      };
      for (int i = 0; i < gd::numGlitchTypes; ++i)
        glitch.probability[static_cast<size_t>(i)] = tap_value(probabilities[i]);

      if (synced) {
        const auto slice_samples = [&](int control) {
          const int index = choice(tap_value(control), static_cast<int>(stutter_notes.size()) - 1);
          return static_cast<float>(note_quarters(stutter_notes[static_cast<size_t>(index)]) * samples_per_quarter);
        };
        glitch.stutterSlice = { slice_samples(kStutterSyncMin), slice_samples(kStutterSyncMax) };
      }
      else {
        glitch.stutterSlice = { log2MsToSamples(tap_value(kStutterMin), sample_rate),
                                log2MsToSamples(tap_value(kStutterMax), sample_rate) };
      }

      glitch.grainSize = { log2MsToSamples(tap_value(kGrainSizeMin), sample_rate),
                           log2MsToSamples(tap_value(kGrainSizeMax), sample_rate) };
      glitch.grainDensity = { exp2f(tap_value(kGrainDensityMin)), exp2f(tap_value(kGrainDensityMax)) };
      glitch.pitch = { tap_value(kPitchMin), tap_value(kPitchMax) };
      glitch.pitchSpeed = { tap_value(kPitchSpeedMin), tap_value(kPitchSpeedMax) };
      glitch.varispeed = choice(tap_value(kPitchMode), 1) == 1;
      glitch.lpcShift = { tap_value(kLpcMin), tap_value(kLpcMax) };
      glitch.cepstralShift = { tap_value(kCepsMin), tap_value(kCepsMax) };
      glitch.ringFrequency = { exp2f(tap_value(kRingMin)), exp2f(tap_value(kRingMax)) };
      glitch.fmRatio = { exp2f(tap_value(kFmRatioMin)), exp2f(tap_value(kFmRatioMax)) };
      glitch.fmIndex = { tap_value(kFmIndexMin), tap_value(kFmIndexMax) };
      glitch.bits = { std::round(tap_value(kCrushBitsMin)), std::round(tap_value(kCrushBitsMax)) };
      glitch.rateReduction = { exp2f(tap_value(kCrushRateMin)), exp2f(tap_value(kCrushRateMax)) };

      engine_->setTapSettings(tap, s);
    }
  }

  void GlitchDelayModule::processWithInput(const poly_float* input, int samples) {
    if (samples <= 0) return;
    SynthModule::process(samples);
    if (prepared_rate_ != getSampleRate())
      prepareEngine();

    updateEngine(samples);
    if (reset_pending_) {
      // A fresh instance starts at its settings, without gliding from stale ones.
      // Resetting forgets that the host was playing, so the transport is given
      // again: reproducible randomness then restarts on this block's first
      // sample rather than the next block's.
      engine_->reset();
      expected_position_.reset();
      updateEngine(samples);
      reset_pending_ = false;
    }

    poly_float* output_buffer = output()->buffer;
    for (int offset = 0; offset < samples;) {
      const int count = std::min(kMaxBufferSize, samples - offset);
      float in_left[kMaxBufferSize], in_right[kMaxBufferSize];
      float out_left[kMaxBufferSize], out_right[kMaxBufferSize];
      for (int i = 0; i < count; ++i) {
        in_left[i] = input[offset + i][0];
        in_right[i] = input[offset + i][1];
      }
      engine_->process(in_left, in_right, out_left, out_right, count);
      for (int i = 0; i < count; ++i)
        output_buffer[offset + i] = poly_float(out_left[i], out_right[i], out_left[i], out_right[i]);
      offset += count;
    }
  }

  void GlitchDelayModule::hardReset() {
    if (prepared_rate_ > 0)
      engine_->reset();
    reset_pending_ = true;
    expected_position_.reset();
  }

  void GlitchDelayModule::enable(bool on) {
    if (enabled() != on) hardReset();
    SynthModule::enable(on);
  }

  // Atlas sets the rate on its preparation worker, never on the audio thread.
  // processWithInput only prepares an engine whose rate was never set.
  void GlitchDelayModule::setSampleRate(int rate) {
    SynthModule::setSampleRate(rate);
    if (prepared_rate_ != getSampleRate())
      prepareEngine();
  }

  void GlitchDelayModule::setOversampleAmount(int amount) {
    SynthModule::setOversampleAmount(amount);
    if (prepared_rate_ != getSampleRate())
      prepareEngine();
  }
}
