// SPDX-License-Identifier: GPL-3.0-or-later
#include "distortion_filter.h"

#include "digital_svf.h"
#include "distortion.h"

#include <cmath>

namespace vital {
  DistortionFilter::DistortionFilter() : SynthModule(kNumInputs, 1), tone_filter_(nullptr), type_(-1),
                                         hold_pairs_(0.0f), held_(0.0f) { }

  void DistortionFilter::init() {
    // The effect's filter: a basic 12 dB state variable filter with its own
    // cutoff, resonance and pass blend. process() runs it before or after the
    // shaper, so this module never runs its processing order.
    tone_filter_ = new DigitalSvf();
    tone_filter_->useInput(input(kReset), DigitalSvf::kReset);
    tone_filter_->useInput(input(kMidiCutoff), DigitalSvf::kMidiCutoff);
    tone_filter_->useInput(input(kResonance), DigitalSvf::kResonance);
    tone_filter_->useInput(input(kBlend), DigitalSvf::kPassBlend);
    tone_filter_->useOutput(output());
    tone_filter_->setDriveCompensation(false);
    tone_filter_->setBasic(true);
    addProcessor(tone_filter_);

    SynthModule::init();
  }

  void DistortionFilter::reset(poly_mask reset_mask) {
    getLocalProcessor(tone_filter_)->reset(reset_mask);
    hold_pairs_ = utils::maskLoad(hold_pairs_, 0.0f, reset_mask);
    held_ = utils::maskLoad(held_, 0.0f, reset_mask);
  }

  void DistortionFilter::hardReset() {
    getLocalProcessor(tone_filter_)->hardReset();
    hold_pairs_ = 0.0f;
    held_ = 0.0f;
  }

  template<poly_float(*distort)(poly_float, poly_float), poly_float(*scale)(poly_float)>
  void DistortionFilter::shape(poly_float* audio, int num_samples) {
    const Output* drive = input(kDrive)->source;
    const int drive_step = drive->isControlRate() ? 0 : 1;
    for (int i = 0; i < num_samples; ++i)
      audio[i] = distort(audio[i], scale(drive->buffer[i * drive_step]));
  }

  void DistortionFilter::downSample(poly_float* audio, int num_samples) {
    // As the effect does: it processes samples in pairs, counts pairs, and once
    // Drive's hold time has passed holds the first sample of the pair.
    const mono_float sample_rate = getSampleRate();
    const Output* drive = input(kDrive)->source;
    const int drive_step = drive->isControlRate() ? 0 : 1;
    const poly_mask reset_mask = getResetMask(kReset);
    const poly_int trigger_offset = input(kReset)->source->trigger_offset;

    poly_float pairs = hold_pairs_;
    poly_float held = held_;
    for (int i = 0; i < num_samples; i += 2) {
      // A new voice starts from silence, as the effect does, at its note offset.
      const poly_mask start = reset_mask & (poly_int::equal(trigger_offset, poly_int(i)) |
                                            poly_int::equal(trigger_offset, poly_int(i + 1)));
      pairs = utils::maskLoad(pairs, 0.0f, start) + 1.0f;
      held = utils::maskLoad(held, 0.0f, start);
      const poly_float period = Distortion::downSampleScale(drive->buffer[i * drive_step]) * sample_rate;
      const poly_mask capture = poly_float::greaterThanOrEqual(pairs, period);
      pairs = utils::maskLoad(pairs, pairs - period, capture);
      held = utils::maskLoad(held, audio[i], capture);
      audio[i] = held;
      if (i + 1 < num_samples)
        audio[i + 1] = held;
    }
    hold_pairs_ = pairs;
    held_ = held;
  }

  void DistortionFilter::process(int num_samples) {
    const int type = utils::iclamp(static_cast<int>(std::round(input(kStyle)->at(0)[0])),
                                   0, Distortion::kNumTypes - 1);
    const int order = utils::iclamp(static_cast<int>(std::round(input(kFilterOrder)->at(0)[0])),
                                    0, kNumFilterOrders - 1);
    // Down Sample starts from silence whenever the type changes, as in the effect.
    if (type != type_) {
      type_ = type;
      hold_pairs_ = 0.0f;
      held_ = 0.0f;
    }
    Processor* tone_filter = getLocalProcessor(tone_filter_);
    const poly_float* audio_in = input(kAudio)->source->buffer;
    poly_float* audio = output()->buffer;
    const Output* amplitude = input(kAmplitude)->source;
    const int amplitude_step = amplitude->isControlRate() ? 0 : 1;
    auto level = [amplitude, amplitude_step](int i) {
      return utils::max(amplitude->buffer[i * amplitude_step], kMinAmplitude);
    };

    // Work on the note at its enveloped level, in the effect's order, then
    // scale it back for the envelope that follows the voice filters.
    for (int i = 0; i < num_samples; ++i)
      audio[i] = audio_in[i] * level(i);

    if (order == kPreFilter)
      tone_filter->processWithInput(audio, num_samples);

    switch (type) {
      case Distortion::kSoftClip:
        shape<Distortion::softClip, Distortion::driveDbScale>(audio, num_samples);
        break;
      case Distortion::kHardClip:
        shape<Distortion::hardClip, Distortion::driveDbScale>(audio, num_samples);
        break;
      case Distortion::kLinearFold:
        shape<Distortion::linearFold, Distortion::driveDbScale>(audio, num_samples);
        break;
      case Distortion::kSinFold:
        shape<Distortion::sinFold, Distortion::driveDbScale>(audio, num_samples);
        break;
      case Distortion::kBitCrush:
        shape<Distortion::bitCrush, Distortion::bitCrushScale>(audio, num_samples);
        break;
      default:
        downSample(audio, num_samples);
        break;
    }

    if (order == kPostFilter)
      tone_filter->processWithInput(audio, num_samples);

    for (int i = 0; i < num_samples; ++i)
      audio[i] = audio[i] / level(i);
  }
} // namespace vital
