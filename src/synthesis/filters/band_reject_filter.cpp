// SPDX-License-Identifier: GPL-3.0-or-later
// Built with the core, outside the fast-math unity file (CMakeLists.txt).
#include "band_reject_filter.h"

#include "utils.h"

namespace vital {
  namespace {
    // Decibels to octaves of gain: log2(10) / 20.
    constexpr double kDecibelsToOctaves = 3.321928094887362 / 20.0;
  } // namespace

  BandRejectFilter::BandRejectFilter() : Processor(kNumInputs, 1) { }

  void BandRejectFilter::resetLane(Lane& lane) {
    lane.core.reset();
    lane.fresh = true;
  }

  void BandRejectFilter::reset(poly_mask reset_mask) {
    for (int lane = 0; lane < poly_float::kSize; ++lane)
      if (reset_mask[lane])
        resetLane(lanes_[lane]);
  }

  void BandRejectFilter::hardReset() {
    for (Lane& lane : lanes_)
      resetLane(lane);
  }

  void BandRejectFilter::process(int num_samples) {
    const Output* resets = input(kReset)->source;
    const poly_mask reset_mask = getResetMask(kReset);
    const poly_float resonance = input(kResonance)->at(0);
    const poly_float drive_db = input(kDriveGain)->at(0);
    const poly_float blend = input(kWidth)->at(0);
    const Output* cutoff = input(kMidiCutoff)->source;
    const int cutoff_step = cutoff->isControlRate() ? 0 : 1;
    const double rate = getSampleRate();
    const poly_float* audio_in = input(kAudio)->source->buffer;
    poly_float* audio_out = output()->buffer;
    const double step = 1.0 / num_samples;
    const int lanes = fx_mode_ ? poly_float::kSize / 2 : poly_float::kSize;

    for (int index = 0; index < lanes; ++index) {
      Lane& lane = lanes_[index];
      const double damping_target = BandRejectCore::damping(resonance[index]);
      const double drive_target =
          BandRejectCore::exp2(utils::clamp(drive_db[index], kMinDriveDb, kMaxDriveDb) * kDecibelsToOctaves);
      // The script's Var, in percent.
      const double width_target = utils::clamp(blend[index], 0.0f, 2.0f) * 50.0;
      const int reset_offset = reset_mask[index] ? resets->trigger_offset[index] : -1;

      double damping = lane.damping, drive = lane.drive, width = lane.width;
      double damping_delta = (damping_target - damping) * step;
      double drive_delta = (drive_target - drive) * step;
      double width_delta = (width_target - width) * step;
      auto startAtTargets = [&] {
        damping = damping_target;
        drive = drive_target;
        width = width_target;
        damping_delta = drive_delta = width_delta = 0.0;
      };
      if (lane.fresh) {
        lane.fresh = false;
        startAtTargets();
      }

      for (int i = 0; i < num_samples; ++i) {
        // A new voice starts from silence at its note offset.
        if (i == reset_offset) {
          lane.core.reset();
          startAtTargets();
        }
        damping += damping_delta;
        drive += drive_delta;
        width += width_delta;
        const double midi = cutoff->buffer[i * cutoff_step][index];
        if (midi != lane.set_midi || width != lane.set_width || rate != lane.set_rate) {
          lane.core.setCutoff(midi, width, rate);
          lane.set_midi = midi;
          lane.set_width = width;
          lane.set_rate = rate;
        }
        const float wet = static_cast<float>(lane.core.tick(audio_in[i][index], drive, damping));
        audio_out[i].set(index, wet);
        if (fx_mode_)
          audio_out[i].set(index + poly_float::kSize / 2, wet);
      }

      lane.damping = damping_target;
      lane.drive = drive_target;
      lane.width = width_target;
      lane.core.flushDenormals();
    }
  }
} // namespace vital
