// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from OpenWurli 0.9.0, crates/openwurli-dsp/src/oversampler.rs
// (GPL-3.0-or-later).
#include "wurli_oversampler.h"

namespace openwurli {
  // Published elliptic half-band coefficients, split into two allpass branches.
  const double Oversampler::kBranchACoefficients[kSectionsPerBranch] = {
    0.036681502163648, 0.248030921580110, 0.643184620136480
  };
  const double Oversampler::kBranchBCoefficients[kSectionsPerBranch] = {
    0.110377634768680, 0.420399304190880, 0.854640112701920
  };

  Oversampler::AllpassBranch::AllpassBranch(const double (&coefficients)[kSectionsPerBranch]) {
    for (int i = 0; i < kSectionsPerBranch; ++i) {
      a_[i] = coefficients[i];
      state_[i] = 0.0;
    }
  }

  void Oversampler::AllpassBranch::reset() {
    for (double& state : state_)
      state = 0.0;
  }

  void Oversampler::upsample2x(const double* input, double* output, int num_samples) {
    for (int i = 0; i < num_samples; ++i)
      upsample(input[i], output[2 * i], output[2 * i + 1]);
  }

  void Oversampler::downsample2x(const double* input, double* output, int num_samples) {
    for (int i = 0; i < num_samples; ++i)
      output[i] = downsample(input[2 * i], input[2 * i + 1]);
  }

  void Oversampler::reset() {
    up_branch_a_.reset();
    up_branch_b_.reset();
    down_branch_a_.reset();
    down_branch_b_.reset();
    down_delay_ = 0.0;
  }
} // namespace openwurli
