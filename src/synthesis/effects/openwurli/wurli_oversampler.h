// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from OpenWurli 0.9.0, crates/openwurli-dsp/src/oversampler.rs
// (GPL-3.0-or-later).
#pragma once

namespace openwurli {
  // 2x polyphase IIR half-band resampler (Regalia-Mitra): two parallel
  // branches of three first-order allpasses, y = (a + z^-1) / (1 + a z^-1),
  // about 28 dB of rejection at 30 kHz. It keeps the preamp's and power amp's
  // harmonics from aliasing. Streaming: the per-sample calls and the block
  // calls give the same output however a signal is split.
  class Oversampler {
    public:
      static constexpr int kSectionsPerBranch = 3;

      // Base rate in, two samples at twice the rate out.
      void upsample(double input, double& even, double& odd) {
        even = up_branch_a_.process(input);
        odd = up_branch_b_.process(input);
      }

      // Two samples at twice the rate in, one base-rate sample out. Branch B
      // is delayed one sample to align the phases.
      double downsample(double even, double odd) {
        const double a = down_branch_a_.process(even);
        const double b = down_branch_b_.process(odd);
        const double output = (a + down_delay_) * 0.5;
        down_delay_ = b;
        return output;
      }

      // `num_samples` in, 2 * num_samples out.
      void upsample2x(const double* input, double* output, int num_samples);
      // 2 * num_samples in, `num_samples` out.
      void downsample2x(const double* input, double* output, int num_samples);
      void reset();

    private:
      class AllpassBranch {
        public:
          explicit AllpassBranch(const double (&coefficients)[kSectionsPerBranch]);

          double process(double input) {
            double y = input;
            for (int i = 0; i < kSectionsPerBranch; ++i) {
              const double x = y;
              y = a_[i] * x + state_[i];
              state_[i] = x - a_[i] * y;
            }
            return y;
          }

          void reset();

        private:
          double a_[kSectionsPerBranch];
          double state_[kSectionsPerBranch];
      };

      static const double kBranchACoefficients[kSectionsPerBranch];
      static const double kBranchBCoefficients[kSectionsPerBranch];

      // Branch A gives the even samples, branch B the odd ones.
      AllpassBranch up_branch_a_ { kBranchACoefficients };
      AllpassBranch up_branch_b_ { kBranchBCoefficients };
      AllpassBranch down_branch_a_ { kBranchACoefficients };
      AllpassBranch down_branch_b_ { kBranchBCoefficients };
      double down_delay_ = 0.0;
  };
} // namespace openwurli
