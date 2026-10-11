/* Copyright 2013-2019 Matt Tytel
 *
 * vital is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * vital is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with vital.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "mid_side_eq_module.h"

#include "equalizer_module.h"
#include "poly_utils.h"

namespace vital {

  MidSideEqModule::MidSideEqModule(std::string prefix) : SynthModule(0, 1) {
    mid_eq_ = new EqualizerModule(prefix + "mid_side_eq_mid_", false);
    addSubmodule(mid_eq_);
    addProcessor(mid_eq_);

    side_eq_ = new EqualizerModule(prefix + "mid_side_eq_side_", false);
    addSubmodule(side_eq_);
    addProcessor(side_eq_);
  }

  void MidSideEqModule::processWithInput(const poly_float* audio_in, int num_samples) {
    poly_float* mid = mid_input_.buffer;
    poly_float* side = side_input_.buffer;
    for (int i = 0; i < num_samples; ++i) {
      poly_float swapped = utils::swapStereo(audio_in[i]);
      mid[i] = (audio_in[i] + swapped) * 0.5f;
      side[i] = (audio_in[i] - swapped) * 0.5f;
    }

    mid_eq_->processWithInput(mid, num_samples);
    side_eq_->processWithInput(side, num_samples);

    const poly_float* mid_out = mid_eq_->output()->buffer;
    const poly_float* side_out = side_eq_->output()->buffer;
    poly_float* audio_out = output()->buffer;
    for (int i = 0; i < num_samples; ++i) {
      // The side buffer is already stored as +side in the left lane and
      // -side in the right lane. Multiplying by kStereoSplit here removes
      // that sign difference and sends mid + side to both output channels.
      audio_out[i] = mid_out[i] + side_out[i];
    }
  }

  void MidSideEqModule::setOversampleAmount(int oversampling) {
    mid_input_.ensureBufferSize(kMaxBufferSize * oversampling);
    side_input_.ensureBufferSize(kMaxBufferSize * oversampling);
    SynthModule::setOversampleAmount(oversampling);
  }

  void MidSideEqModule::hardReset() {
    mid_eq_->hardReset();
    side_eq_->hardReset();
  }

  void MidSideEqModule::enable(bool enable) {
    SynthModule::enable(enable);
    mid_eq_->enable(enable);
    side_eq_->enable(enable);
  }
} // namespace vital
