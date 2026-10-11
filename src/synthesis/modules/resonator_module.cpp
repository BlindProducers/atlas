// SPDX-License-Identifier: GPL-3.0-or-later
#include "resonator_module.h"
#include "note_monitor.h"
#include "synth_parameters.h"
#include "tuning.h"
#include "../effects/resonator/resonator_engine.h"
#include <algorithm>
#include <cmath>

namespace vital {
  namespace {
    enum {
      kTune, kTranspose, kFine, kDecay, kMaterial, kBrightness, kInharmonics, kRatio, kHit,
      kListenLeft, kListenRight, kSpread, kWidth, kRadius, kOpening, kLfoAmount, kLfoStereoPhase,
      kLfoSpin, kLfoOffset, kFilterFrequency, kFilterBandwidth, kBleed, kExcitation, kWetGain,
      kMix, kOutputGain, kOffDecay, kGlide, kNumControls
    };
    const char* const kControlNames[kNumControls] = {
      "tune", "transpose", "fine", "decay", "material", "brightness", "inharmonics", "ratio", "hit",
      "listen_left", "listen_right", "spread", "width", "radius", "opening", "lfo_amount",
      "lfo_stereo_phase", "lfo_spin", "lfo_offset", "filter_frequency", "filter_bandwidth", "bleed",
      "excitation", "wet_gain", "mix", "output_gain", "off_decay", "glide"
    };
    static_assert(kNumControls == ResonatorModule::kNumModControls);

    double decibels(double db) { return std::pow(10.0, db / 20.0); }
    int choice(const Value* value, int maximum) {
      return std::clamp(static_cast<int>(std::lround(value->value())), 0, maximum);
    }
  }

  ResonatorModule::ResonatorModule(const Output* beats_per_second, const NoteMonitor* notes, std::string prefix) :
      SynthModule(0, 1), engine_(std::make_unique<atlas::resonator::Engine>()),
      beats_per_second_(beats_per_second), notes_(notes), prefix_(std::move(prefix)) {
    engine_->prepare(getSampleRate());
  }

  ResonatorModule::~ResonatorModule() = default;

  void ResonatorModule::init() {
    const std::string name = prefix_ + "resonator_";
    for (int i = 0; i < kNumControls; ++i)
      controls_[i] = createMonoModControl(name + kControlNames[i]);
    // An exponential control's scaled output is its owner's primary output,
    // which the shared tempo chooser takes as the free-running rate.
    Output* free_rate = createMonoModControl(name + "lfo_frequency");
    lfo_frequency_ = createTempoSyncSwitch(name + "lfo", free_rate->owner, beats_per_second_, false);
    lfo_sync_ = getControls().at(name + "lfo_sync");
    lfo_sync_->set(Parameters::getDetails(name + "lfo_sync").default_value);
    model_ = createBaseControl(name + "model");
    quality_ = createBaseControl(name + "quality");
    midi_pitch_ = createBaseControl(name + "midi_pitch");
    note_priority_ = createBaseControl(name + "note_priority");
    lfo_shape_ = createBaseControl(name + "lfo_shape");
    filter_ = createBaseControl(name + "filter");
    limiter_ = createBaseControl(name + "limiter");
    SynthModule::init();
  }

  void ResonatorModule::correctToTime(double seconds) {
    transport_time_ = seconds;
    has_transport_time_ = true;
  }

  void ResonatorModule::processWithInput(const poly_float* input, int samples) {
    if (samples <= 0) return;
    SynthModule::process(samples);
    const auto value = [this](int index) { return static_cast<double>(controls_[index]->buffer[0][0]); };

    atlas::resonator::Parameters p;
    p.model = choice(model_, atlas::resonator::kNumModels - 1);
    p.quality = choice(quality_, atlas::resonator::kNumQualities - 1);

    // MIDI pitch follows the keys Atlas plays as its voices tune them: the loaded
    // tuning, pitch bend, Voice Transpose and Voice Tune. Before any key
    // arrives, and when off, Tune sets the pitch directly.
    const bool tracking = midi_pitch_->value() >= 0.5f && notes_ != nullptr && notes_->seen();
    const double fine = std::clamp(value(kFine), -1.0, 1.0) * 100.0;
    double pitch = std::clamp(value(kTune), 20.0, 5000.0);
    if (tracking) {
      const int priority = choice(note_priority_, NoteMonitor::kLowestNote);
      const int key = notes_->note(priority);
      const double tuned = notes_->tuning() != nullptr ? notes_->tuning()->convertMidiNote(key) : key;
      const double note = tuned + notes_->offsetSemitones(priority) +
                          std::round(std::clamp(value(kTranspose), -48.0, 48.0));
      pitch = 440.0 * std::exp2((note - 69.0) / 12.0);
    }
    p.pitch = pitch * std::exp2(fine / 1200.0);
    p.released = tracking && !notes_->held();

    p.decay = value(kDecay);
    p.material = value(kMaterial);
    p.brightness = value(kBrightness);
    p.inharmonics = value(kInharmonics);
    p.ratio = value(kRatio);
    p.hit = value(kHit);
    p.listen_left = value(kListenLeft);
    p.listen_right = value(kListenRight);
    p.spread = std::clamp(value(kSpread), -1.0, 1.0) * 100.0;
    p.width = value(kWidth);
    p.radius = value(kRadius);
    p.opening = value(kOpening);
    p.lfo_amount = value(kLfoAmount);
    p.lfo_shape = choice(lfo_shape_, atlas::resonator::kNumLfoShapes - 1);
    const int sync = choice(lfo_sync_, 3);
    p.lfo_free = sync == 0;
    p.lfo_frequency = std::max(0.0, static_cast<double>(lfo_frequency_->buffer[0][0]));
    p.lfo_stereo_phase = std::clamp(value(kLfoStereoPhase), 0.0, 1.0);
    p.lfo_spin = value(kLfoSpin);
    p.lfo_offset = std::clamp(value(kLfoOffset), 0.0, 1.0);
    p.filter = filter_->value() >= 0.5f;
    p.filter_frequency = value(kFilterFrequency);
    p.filter_bandwidth = value(kFilterBandwidth);
    p.bleed = value(kBleed);
    p.excitation = decibels(std::clamp(value(kExcitation), -24.0, 24.0));
    p.wet = decibels(std::clamp(value(kWetGain), -24.0, 24.0));
    p.output = decibels(std::clamp(value(kOutputGain), -24.0, 12.0));
    p.mix = value(kMix);
    p.limiter = limiter_->value() >= 0.5f;
    p.glide = std::clamp(value(kGlide), 0.0, 500.0) * 0.001;
    p.off_decay = value(kOffDecay);
    engine_->setParameters(p);
    if (reset_pending_) {
      // A fresh instance starts at its settings, without smoothing from stale ones.
      engine_->reset();
      reset_pending_ = false;
    }

    // Tempo sync anchors the LFO to the host timeline when it starts and after
    // seeks, then runs freely so live tempo changes stay continuous.
    const double tick = 1.0 / std::max(1, getSampleRate());
    const bool seek = has_transport_time_ &&
        (!timeline_valid_ || std::abs(transport_time_ - expected_transport_time_) > 2.0 * tick);
    if (sync != 0 && has_transport_time_ && (seek || previous_sync_ == 0))
      engine_->alignLfo(transport_time_ * p.lfo_frequency);
    if (has_transport_time_) {
      expected_transport_time_ = transport_time_ + samples * tick;
      timeline_valid_ = true;
    }
    has_transport_time_ = false;
    previous_sync_ = sync;

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

  void ResonatorModule::hardReset() {
    engine_->reset();
    reset_pending_ = true;
    timeline_valid_ = false;
    previous_sync_ = 0;
  }

  void ResonatorModule::enable(bool on) {
    if (enabled() != on) hardReset();
    SynthModule::enable(on);
  }

  void ResonatorModule::setSampleRate(int rate) {
    const int previous = getSampleRate();
    SynthModule::setSampleRate(rate);
    if (getSampleRate() != previous) {
      engine_->prepare(getSampleRate());
      hardReset();
    }
  }

  void ResonatorModule::setOversampleAmount(int amount) {
    const int previous = getSampleRate();
    SynthModule::setOversampleAmount(amount);
    if (getSampleRate() != previous) {
      engine_->prepare(getSampleRate());
      hardReset();
    }
  }
}
