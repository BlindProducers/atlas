// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from OpenWurli 0.9.0, crates/openwurli-dsp/src/power_amp.rs
// (GPL-3.0-or-later): HEADROOM_V, RailDynamics and the behavioural PowerAmp
// that the default `legacy-power-amp` feature selects. The melange-generated
// 7-BJT solver (gen_power_amp.rs) is not ported.
#pragma once

namespace openwurli {
  // The amp's clip ceiling in volts: the power amp's output is normalized by
  // it, so +-1.0 is +-22 V.
  constexpr double kHeadroomV = 22.0;

  // Sag of the unregulated +-22 V rails under load (idle ~24.5 V): a 30 ms
  // envelope of the output current sets the target, which the rails follow
  // with an 8 ms attack and a 15 ms release. Only the melange power amp uses
  // it; the behavioural amp below folds the rails into its tanh. Ported for
  // completeness, unused by AmpChain. See OpenWurli's
  // docs/research/output-stage.md section 4.3.1.
  class RailDynamics {
    public:
      // Starts at the static 22.5 V bias (zero runtime offsets).
      explicit RailDynamics(double sample_rate);

      void setSampleRate(double sample_rate);
      void reset();
      // One sample; `v_out` is the previous raw output voltage.
      void step(double v_out);
      // Rail magnitudes, both positive.
      double positiveRail() const { return v_rail_pos_; }
      double negativeRail() const { return v_rail_neg_; }
      // Offsets from the 22.5 V DC bias.
      double positiveOffset() const;
      double negativeOffset() const;

    private:
      double v_rail_pos_;
      double v_rail_neg_;
      double i_avg_pos_ = 0.0;
      double i_avg_neg_ = 0.0;
      double alpha_attack_ = 0.0;
      double alpha_release_ = 0.0;
      double alpha_i_avg_ = 0.0;
  };

  // Behavioural closed-loop model of the 200A's class AB power amp: an open
  // loop gain of 19000 through a crossover notch and a tanh rail limit, closed
  // by the drawn feedback divider R-31 (15K) to R-30 (220R) in series with
  // C-10 (22uF). The divider is 1 at DC and 220/15220 in band (69x), with its
  // knee at 33 Hz: the circuit's only bass roll-off. It is solved inside the
  // Newton-Raphson loop, so it shapes the clipping too. Output normalized to
  // kHeadroomV. Runs at the oversampled rate.
  class PowerAmp {
    public:
      // Unusable until assigned from PowerAmp(sample_rate).
      PowerAmp() = default;
      // PowerAmp::new_at_sample_rate: only the feedback section depends on the rate.
      explicit PowerAmp(double sample_rate);

      double process(double input);
      void reset();

    private:
      struct ForwardPath {
        double value;
        double derivative;
      };

      ForwardPath forwardPath(double v) const;

      double open_loop_gain_ = 0.0;
      double crossover_vt_ = 0.0;
      double rail_limit_ = 0.0;
      double quiescent_gain_ = 0.0;
      // Feedback fraction beta(z), bilinear: fb[n] = b0 y[n] + b1 y[n-1] - a1 fb[n-1].
      double fb_b0_ = 0.0;
      double fb_b1_ = 0.0;
      double fb_a1_ = 0.0;
      double fb_y1_ = 0.0;
      double fb_prev_ = 0.0;
  };
} // namespace openwurli
