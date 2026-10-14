// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from OpenWurli 0.9.0, crates/openwurli-dsp/src/tables.rs and variation.rs
// (GPL-3.0-or-later). Every expression keeps the Rust's operation order; see wurli_math.h.
#include "wurli_tables.h"

#include "wurli_biquad.h"
#include "wurli_pickup.h"
#include "wurli_math.h"

#include <algorithm>
#include <cmath>

namespace openwurli {
  namespace {
    using math::clamp;

    // Eigenvalues beta_n of 1 + cos(b)cosh(b) + b mu (cos(b)sinh(b) - sin(b)cosh(b)) = 0 at
    // tabulated tip mass ratios, interpolated linearly in mu.
    struct EigenRow {
      double mu;
      ModeArray betas;
    };

    constexpr EigenRow kEigenTable[] = {
      { 0.00, { 1.8751, 4.6941, 7.8548, 10.9955, 14.1372, 17.2788, 20.4204 } },
      { 0.01, { 1.8584, 4.6849, 7.8504, 10.9930, 14.1356, 17.2776, 20.4195 } },
      { 0.05, { 1.7920, 4.6477, 7.8316, 10.9830, 14.1288, 17.2726, 20.4158 } },
      { 0.10, { 1.7227, 4.6024, 7.8077, 10.9700, 14.1198, 17.2660, 20.4110 } },
      { 0.15, { 1.6625, 4.5618, 7.7859, 10.9580, 14.1114, 17.2598, 20.4065 } },
      { 0.20, { 1.6097, 4.5254, 7.7659, 10.9470, 14.1036, 17.2540, 20.4023 } },
      { 0.30, { 1.5201, 4.4620, 7.7310, 10.9280, 14.0894, 17.2434, 20.3946 } },
      { 0.50, { 1.3853, 4.3601, 7.6745, 10.8970, 14.0650, 17.2252, 20.3814 } },
    };
    constexpr int kNumEigenRows = sizeof(kEigenTable) / sizeof(kEigenTable[0]);

    ModeArray eigenvalues(double mu) {
      const double mu_clamped = clamp(mu, 0.0, 0.50);
      int lo = 0;
      for (int row = kNumEigenRows - 1; row >= 0; --row) {
        if (kEigenTable[row].mu <= mu_clamped) {
          lo = row;
          break;
        }
      }
      const int hi = std::min(lo + 1, kNumEigenRows - 1);
      const EigenRow& low = kEigenTable[lo];
      const EigenRow& high = kEigenTable[hi];
      const double t = high.mu > low.mu ? (mu_clamped - low.mu) / (high.mu - low.mu) : 0.0;

      ModeArray betas;
      for (int i = 0; i < kNumModes; ++i)
        betas[i] = low.betas[i] + t * (high.betas[i] - low.betas[i]);
      return betas;
    }

    // Piecewise-linear interpolation over (x, y) anchors, clamped at both ends.
    struct Anchor {
      double x;
      double y;
    };

    template <int kSize>
    double interpolateAnchors(const Anchor (&anchors)[kSize], double m) {
      if (m <= anchors[0].x)
        return anchors[0].y;
      if (m >= anchors[kSize - 1].x)
        return anchors[kSize - 1].y;
      for (int i = 0; i < kSize - 1; ++i) {
        const double x0 = anchors[i].x;
        const double y0 = anchors[i].y;
        const double x1 = anchors[i + 1].x;
        const double y1 = anchors[i + 1].y;
        if (m <= x1) {
          const double t = (m - x0) / (x1 - x0);
          return y0 + t * (y1 - y0);
        }
      }
      return 0.0;
    }

    struct Knot {
      int midi;
      double value;
    };

    // Last bass reed (reed 20). Interpolation never crosses MIDI 52 -> 53.
    constexpr int kBassTopMidi = 52;

    // Linear in log(value) between per-note knots on one side of the reed 20|21 break, clamped
    // at the ends of that side.
    template <int kSize>
    double logInterpBySide(int midi, const Knot (&knots)[kSize]) {
      const bool bass = midi <= kBassTopMidi;
      const Knot* prev = nullptr;
      for (const Knot& knot : knots) {
        if ((knot.midi <= kBassTopMidi) != bass)
          continue;
        if (knot.midi >= midi) {
          if (prev != nullptr && knot.midi > midi) {
            const double t = static_cast<double>(midi - prev->midi) / static_cast<double>(knot.midi - prev->midi);
            return std::exp(std::log(prev->value) * (1.0 - t) + std::log(knot.value) * t);
          }
          return knot.value;
        }
        prev = &knot;
      }
      return prev->value;
    }

    // PROVISIONAL ff reed swing at the pickup, mm, at the Central pick (the median of the
    // surviving prior samples; tools/swing_prior.py upstream). Nothing here is fitted to a
    // recording; a bench measurement of the hammer would replace it.
    constexpr Knot kProvisionalFfSwingMm[] = {
      // Bass reeds 1-20 (MIDI 33-52)
      { 33, 2.46 }, { 36, 2.44 }, { 39, 2.40 }, { 42, 2.33 }, { 45, 2.23 }, { 48, 2.25 },
      { 51, 2.05 }, { 52, 1.98 },
      // Reeds 21-64 (MIDI 53-96)
      { 53, 1.16 }, { 54, 1.16 }, { 57, 1.14 }, { 60, 1.11 }, { 63, 1.06 }, { 66, 0.99 },
      { 69, 0.90 }, { 72, 0.77 }, { 75, 0.67 }, { 78, 0.57 }, { 81, 0.48 }, { 84, 0.42 },
      { 87, 0.35 }, { 90, 0.30 }, { 93, 0.25 }, { 96, 0.21 },
    };

    // PROVISIONAL quadratic air drag on mode 1, dB/s per mm of finger-region swing: textbook
    // plate drag integrated over each reed's mode shape at a drag coefficient of 4.47 (the
    // geometric centre of a guess-grade 2-10 band).
    constexpr Knot kProvisionalAirDragDbPerMm[] = {
      { 33, 0.259 }, { 36, 0.356 }, { 39, 0.485 }, { 42, 0.651 }, { 45, 0.863 }, { 48, 1.125 },
      { 51, 1.417 }, { 52, 1.519 }, { 53, 0.664 }, { 54, 0.756 }, { 57, 1.114 }, { 60, 1.633 },
      { 63, 2.381 }, { 66, 3.446 }, { 69, 4.863 }, { 72, 5.921 }, { 75, 7.116 }, { 78, 8.562 },
      { 81, 10.32 }, { 84, 12.45 }, { 87, 15.05 }, { 90, 18.24 }, { 93, 22.16 }, { 96, 27.02 },
    };

    // Lower bound on the swing, mm: keeps the level proxy away from zero.
    constexpr double kSwingMinMm = 0.02;
    // Active pickup plate length, mm (estimated from the 200A's pickup geometry).
    constexpr double kPlateActiveLengthMm = 6.0;
    constexpr double kMinDecayRate = 3.0;

    // CalibrationConfig::default().
    constexpr double kTargetDb = -29.55;

    // Cantilever mode shape with tip mass, xi = x / L (0 = clamp, 1 = tip).
    double modeShape(double beta, double xi) {
      double sin_beta, cos_beta;
      math::sinCos(beta, sin_beta, cos_beta);
      const double sigma = (std::cosh(beta) + cos_beta) / (std::sinh(beta) + sin_beta);
      const double bx = beta * xi;
      double sin_bx, cos_bx;
      math::sinCos(bx, sin_bx, cos_bx);
      return std::cosh(bx) - cos_bx - sigma * (std::sinh(bx) - sin_bx);
    }

    // The proxy's 64-point DFT: the unit-amplitude displacement over one cycle, and the DFT's
    // cos / sin at every product n * i (n = 1..8, i = 0..63), each computed exactly as the Rust
    // computes it at every call.
    constexpr int kProxyPoints = 64;
    constexpr int kProxyHarmonics = 8;
    constexpr int kProxyPhases = kProxyHarmonics * (kProxyPoints - 1) + 1;

    struct ProxyTables {
      double displacement[kProxyPoints];
      double dft_cos[kProxyPhases];
      double dft_sin[kProxyPhases];
    };

    const ProxyTables& proxyTables() {
      static const ProxyTables tables = [] {
        ProxyTables result;
        for (int i = 0; i < kProxyPoints; ++i)
          result.displacement[i] = std::sin(kTau * static_cast<double>(i) / static_cast<double>(kProxyPoints));
        for (int k = 0; k < kProxyPhases; ++k) {
          const double p = kTau * static_cast<double>(k) / static_cast<double>(kProxyPoints);
          math::sinCos(p, result.dft_sin[k], result.dft_cos[k]);
        }
        return result;
      }();
      return tables;
    }

    double hashUnit(int midi, uint32_t seed) {
      uint32_t h = 2166136261u;
      h ^= static_cast<uint32_t>(midi);
      h *= 16777619u;
      h ^= seed;
      h *= 16777619u;
      h ^= h >> 16;
      h *= 2654435769u;
      return static_cast<double>(h & 0x00ffffffu) / 16777216.0;
    }
  } // namespace

  double midiToFreq(int midi) {
    // f64::powf(2.0, x) is exp2 in the Rust build (see wurli_math.h).
    return 440.0 * std::exp2((static_cast<double>(midi) - 69.0) / 12.0);
  }

  double tipMassRatio(int midi) {
    // mu ~ 0.10 on the heavily soldered A1, ~0 around E3-D4, 0.02 at D5, 0.01 at C7.
    static constexpr Anchor kAnchors[] = {
      { 33.0, 0.10 }, { 52.0, 0.00 }, { 62.0, 0.00 }, { 74.0, 0.02 }, { 96.0, 0.01 },
    };
    return interpolateAnchors(kAnchors, static_cast<double>(midi));
  }

  ModeArray modeRatios(double mu) {
    const ModeArray betas = eigenvalues(mu);
    const double b1_sq = betas[0] * betas[0];
    ModeArray ratios;
    for (int i = 0; i < kNumModes; ++i)
      ratios[i] = (betas[i] * betas[i]) / b1_sq;
    return ratios;
  }

  double reedLengthMm(int midi) {
    // Bass (n = 1-20): 3 - n/20 inches; treble (n = 21-64): 2 - (n-20)/44 inches.
    const double n = clamp(static_cast<double>(midi) - 32.0, 1.0, 64.0);
    const double inches = n <= 20.0 ? 3.0 - n / 20.0 : 2.0 - (n - 20.0) / 44.0;
    return inches * 25.4;
  }

  double provisionalFfSwingMm(int midi) {
    return logInterpBySide(midi, kProvisionalFfSwingMm);
  }

  double pickupSwingMm(int midi) {
    // swing_scale is 1.0 by default.
    return std::fmax(provisionalFfSwingMm(midi) * 1.0, kSwingMinMm);
  }

  double pickupSwingMm(int midi, const VoiceSettings& settings) {
    const double swing = std::fmax(keyParams(midi).provisional_swing_mm * settings.swing_scale, kSwingMinMm);
    // Past its table's reach the law is held flat and the reed's charge would clip.
    return std::fmin(swing, std::max(midi - 32, 0) <= 20 ? kSwingReachMmBass : kSwingReachMmTreble);
  }

  double airDragDbPerMm(int midi) {
    // air_drag_scale is 1.0 by default.
    return logInterpBySide(midi, kProvisionalAirDragDbPerMm) * 1.0;
  }

  ModeArray spatialCouplingCoefficients(double mu, double reed_len_mm) {
    constexpr int kSimpsonIntervals = 32;
    const ModeArray betas = eigenvalues(mu);
    const double ell_over_l = clamp(kPlateActiveLengthMm / reed_len_mm, 0.0, 1.0);
    const double xi_start = 1.0 - ell_over_l;

    // kappa_n = |integral of phi_n over the plate| / (ell/L |phi_n(1)|).
    ModeArray kappa_raw = {};
    for (int mode = 0; mode < kNumModes; ++mode) {
      const double beta = betas[mode];
      const double tip_val = modeShape(beta, 1.0);
      if (std::abs(tip_val) < 1e-30 || ell_over_l < 1e-12) {
        kappa_raw[mode] = 1.0;  // point pickup at the tip
        continue;
      }

      const double h = ell_over_l / static_cast<double>(kSimpsonIntervals);
      double sum = modeShape(beta, xi_start) + modeShape(beta, 1.0);
      for (int j = 1; j < kSimpsonIntervals; ++j) {
        const double xi = xi_start + static_cast<double>(j) * h;
        const double coeff = j % 2 == 1 ? 4.0 : 2.0;
        sum += coeff * modeShape(beta, xi);
      }

      const double integral = sum * h / 3.0;
      const double k = std::abs(integral / (ell_over_l * tip_val));
      kappa_raw[mode] = clamp(k, 0.0, 1.0);
    }

    // Normalised to mode 1: the calibrated swing already refers to tip displacement, so only the
    // differential suppression of the higher modes is applied.
    ModeArray coupling;
    const double k1 = kappa_raw[0];
    for (int i = 0; i < kNumModes; ++i)
      coupling[i] = k1 > 1e-30 ? clamp(kappa_raw[i] / k1, 0.0, 1.0) : 1.0;
    return coupling;
  }

  double fundamentalDecayRate(int midi) {
    const double f = midiToFreq(midi);
    return std::fmax(0.005 * std::pow(f, 1.22), kMinDecayRate);
  }

  ModeArray modeDecayRates(int midi, const ModeArray& ratios) {
    const double base = fundamentalDecayRate(midi);
    ModeArray rates;
    for (int i = 0; i < kNumModes; ++i)
      rates[i] = base * ratios[i] * ratios[i];
    return rates;
  }

  double pickupRmsProxyLaw(int midi, double swing_mm, double f0, double fc) {
    const PickupLawTable& law = pickupLawFor(midi);
    const double a = pickupBandSign(midi) * swing_mm;
    const double rest = law.eval(-kPickupRestOffsetMm);
    const ProxyTables& tables = proxyTables();

    double source[kProxyPoints];
    for (int i = 0; i < kProxyPoints; ++i)
      source[i] = law.eval(a * tables.displacement[i] - kPickupRestOffsetMm) - rest;

    double sum_sq = 0.0;
    for (int n = 1; n <= kProxyHarmonics; ++n) {
      double re = 0.0;
      double im = 0.0;
      for (int i = 0; i < kProxyPoints; ++i) {
        const double v = source[i];
        re += v * tables.dft_cos[n * i];
        im -= v * tables.dft_sin[n * i];
      }
      const double cn = 2.0 * std::sqrt(re * re + im * im) / static_cast<double>(kProxyPoints);
      const double nf = static_cast<double>(n) * f0;
      const double hpf_n = nf / std::sqrt(nf * nf + fc * fc);
      sum_sq += (cn * hpf_n) * (cn * hpf_n);
    }
    return std::sqrt(sum_sq);
  }

  double registerTrimDb(int midi) {
    // Re-calibrated 2026-09-23 upstream on the shipping chain at volume 0.5, the least-squares
    // shift over ff and mf renders against C4.
    static constexpr Anchor kAnchors[] = {
      { 36.0, -2.6 }, { 40.0, -0.2 }, { 44.0, -1.6 }, { 48.0, 1.3 }, { 52.0, 0.5 },
      { 56.0, -2.0 }, { 60.0, 0.0 }, { 64.0, 0.7 }, { 68.0, 0.8 }, { 72.0, -0.6 },
      { 76.0, 1.4 }, { 80.0, 2.1 }, { 84.0, 3.6 },
    };
    return interpolateAnchors(kAnchors, static_cast<double>(midi));
  }

  double outputScale(int midi, double velocity, const VoiceSettings& settings) {
    const KeyParams& key = keyParams(midi);
    const KeyParams& c4 = keyParams(60);
    const double hpf_fc = settings.c2_to_emitter ? kPickupFcC2Emitter : kPickupFc;

    const double swing = pickupSwingMm(midi, settings);
    const double f0 = key.note.fundamental_hz;

    // The proxy at the swing the pickup actually sees at this velocity, so harmonic content
    // that passes the high-pass at ff is accounted for.
    const double scurve_v = velocityScurve(velocity);
    const double vel_scale = std::pow(scurve_v, key.velocity_exponent);
    const double vel_scale_c4 = std::pow(scurve_v, c4.velocity_exponent);
    const double effective_swing = std::fmax(swing * vel_scale, 1e-6);
    const double effective_swing_ref = std::fmax(pickupSwingMm(60, settings) * vel_scale_c4, 1e-6);

    const double rms = pickupRmsProxyLaw(midi, effective_swing, f0, hpf_fc);
    const double rms_ref = pickupRmsProxyLaw(60, effective_swing_ref, c4.note.fundamental_hz, hpf_fc);

    const double flat_db = -20.0 * std::log10(rms / rms_ref);
    const double voicing_db = settings.voicing_slope * std::fmax(static_cast<double>(midi) - 60.0, 0.0);
    const double trim = settings.register_trim ? key.register_trim_db : 0.0;

    // The register trim was calibrated at v127; it applies in part at lower velocities.
    const double vel_blend = std::pow(velocity, 1.3);
    const double effective_trim = trim * vel_blend;

    return math::exp10((kTargetDb + flat_db + voicing_db + effective_trim) / 20.0);
  }

  double velocityExponent(int midi) {
    // Bell centred on D4 (1.7), treble edge 1.3, bass edge compressed to 0.55 so the bass
    // growls at moderate velocity.
    const double m = static_cast<double>(midi);
    const double center = 62.0;
    const double sigma = 15.0;
    const double max_exp = 1.7;
    const double treble_min = 1.3;
    const double bass_min = 0.55;
    const double t = std::exp(-0.5 * math::powi((m - center) / sigma, 2));
    const double min_exp = m < center ? bass_min : treble_min;
    return min_exp + t * (max_exp - min_exp);
  }

  double velocityScurve(double velocity) {
    const double k = 1.5;
    const double s = 1.0 / (1.0 + std::exp(-k * (velocity - 0.5)));
    const double s0 = 1.0 / (1.0 + std::exp(k * 0.5));
    const double s1 = 1.0 / (1.0 + std::exp(-k * 0.5));
    return (s - s0) / (s1 - s0);
  }

  NoteParams computeNoteParams(int midi) {
    NoteParams params;
    params.fundamental_hz = midiToFreq(midi);
    const double mu = tipMassRatio(midi);
    params.mode_ratios = modeRatios(mu);
    params.mode_decay_rates = modeDecayRates(midi, params.mode_ratios);

    // The base amplitudes already include the real hammer's excitation profile, so only the
    // pickup's spatial coupling is applied here.
    params.mode_amplitudes = kBaseModeAmplitudes;
    const ModeArray coupling = spatialCouplingCoefficients(mu, reedLengthMm(midi));
    for (int i = 0; i < kNumModes; ++i)
      params.mode_amplitudes[i] *= coupling[i];
    return params;
  }

  double freqDetune(int midi) {
    return 1.0 + detuneUnit(midi) * 0.00173;
  }

  double detuneUnit(int midi) {
    return hashUnit(midi, 0xdead) * 2.0 - 1.0;
  }

  ModeArray modeAmplitudeOffsets(int midi) {
    ModeArray offsets;
    for (int i = 0; i < kNumModes; ++i) {
      const double r = hashUnit(midi, 0xbeef + static_cast<uint32_t>(i)) * 2.0 - 1.0;
      offsets[i] = 1.0 + r * 0.08;
    }
    return offsets;
  }

  KeyParams computeKeyParams(int midi) {
    KeyParams key;
    key.note = computeNoteParams(midi);
    key.freq_detune = freqDetune(midi);
    key.detune_unit = detuneUnit(midi);
    key.amplitude_offsets = modeAmplitudeOffsets(midi);
    key.velocity_exponent = velocityExponent(midi);
    key.provisional_swing_mm = provisionalFfSwingMm(midi);
    key.swing_mm = pickupSwingMm(midi);
    key.air_drag_db_per_mm = airDragDbPerMm(midi);
    key.register_trim_db = registerTrimDb(midi);
    return key;
  }

  const KeyParams& keyParams(int midi) {
    static const std::array<KeyParams, kNumKeys> table = [] {
      std::array<KeyParams, kNumKeys> result;
      for (int i = 0; i < kNumKeys; ++i)
        result[i] = computeKeyParams(kMidiLo + i);
      return result;
    }();
    return table[std::min(std::max(midi, kMidiLo), kMidiHi) - kMidiLo];
  }

  void prepareKeyTables() {
    keyParams(kMidiLo);
    proxyTables();
  }
} // namespace openwurli
