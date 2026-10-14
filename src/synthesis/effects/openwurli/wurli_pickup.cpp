// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from OpenWurli 0.9.0, crates/openwurli-dsp/src/pickup.rs (GPL-3.0-or-later).
#include "wurli_pickup.h"

namespace openwurli {
  const PickupLawTable& pickupLawFor(int midi) {
    const int reed = std::max(midi - 32, 0);
    if (reed <= 14)
      return kPickupLawReeds01_14;
    if (reed <= 20)
      return kPickupLawReeds15_20;
    if (reed <= 42)
      return kPickupLawReeds21_42;
    return kPickupLawReeds43_64;
  }

  double pickupBandSign(int midi) {
    return std::max(midi - 32, 0) <= 20 ? 1.0 : -1.0;
  }

  void Pickup::init(double sample_rate, int midi, double amplitude_mm, bool c2_to_emitter) {
    const double dt = 1.0 / sample_rate;
    law_ = &pickupLawFor(midi);
    w_ = 0.0;
    s_prev_ = 0.0;
    m_prev_ = 1.0;
    beta_ = dt / (2.0 * (c2_to_emitter ? kPickupTauC2Emitter : kPickupTau));
    band_sign_ = pickupBandSign(midi);
    c_rest_ = law_->eval(-kPickupRestOffsetMm);
    amplitude_mm_ = amplitude_mm;
  }

  void Pickup::process(double* buffer, int num_samples) {
    const PickupLawTable& law = *law_;
    const double beta = beta_;
    const double scale = band_sign_ * amplitude_mm_;
    for (int i = 0; i < num_samples; ++i) {
      // Source: the moving reed's capacitance to the fingers, the nonlinearity. At rest the reed
      // sits just outside the band (u = -kPickupRestOffsetMm).
      const double u = scale * buffer[i] - kPickupRestOffsetMm;
      const double s = law.eval(u) - c_rest_;

      // Network: one moving reed against a fixed node, nearly linear. Exact trapezoidal
      // discretisation of C_node(t) dv/dt + v / R_net = -V_pol dC_reed/dt, normalised by
      // kCTotal and kPickupSensitivity.
      const double m = 1.0 + kCReedRatio * s;
      const double m_avg = 0.5 * (m + m_prev_);
      w_ = (w_ * (m_avg - beta) - (s - s_prev_)) / (m_avg + beta);

      s_prev_ = s;
      m_prev_ = m;
      buffer[i] = w_ * kPickupSensitivity;
    }
  }

  void Pickup::reset() {
    w_ = 0.0;
    s_prev_ = 0.0;
    m_prev_ = 1.0;
  }
} // namespace openwurli
