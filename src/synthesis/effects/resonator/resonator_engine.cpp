// SPDX-License-Identifier: GPL-3.0-or-later
#include "resonator_engine.h"

#include <algorithm>
#include <cmath>

namespace atlas::resonator {
  namespace {
    constexpr double kPi = 3.14159265358979323846;
    constexpr double kTwoPi = 2.0 * kPi;
    constexpr int kGrid = 24;
    const double kLn1000 = std::log(1000.0);
    // About -240 dB: far below audibility, above the denormal range.
    constexpr double kFlush = 1.0e-12;

    double positionGain(int m, int n, double position) {
      const double x = 0.5 - 0.49 * position, y = 0.5 - 0.47 * position;
      const double gain = std::sin(kPi * m * x);
      return n > 0 ? gain * std::sin(kPi * n * y) : gain;
    }

    // Deterministic per-cycle values, so tempo-synced renders repeat exactly.
    double randomValue(int64_t cycle) {
      uint64_t z = static_cast<uint64_t>(cycle) + 0x9e3779b97f4a7c15ull;
      z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
      z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
      z ^= z >> 31;
      return static_cast<double>(z >> 11) * (2.0 / 9007199254740992.0) - 1.0;
    }

    double lfoWave(int shape, double phase, int64_t cycle) {
      switch (shape) {
        case kSquare: return phase < 0.5 ? 1.0 : -1.0;
        case kTriangle: return 1.0 - 4.0 * std::abs(phase - 0.5);
        case kSawUp: return 2.0 * phase - 1.0;
        case kSawDown: return 1.0 - 2.0 * phase;
        case kRandomStepped: return randomValue(cycle);
        case kRandomSmooth: {
          const double a = randomValue(cycle), b = randomValue(cycle + 1);
          return a + (b - a) * phase * phase * (3.0 - 2.0 * phase);
        }
        default: return std::sin(kTwoPi * phase);
      }
    }

    bool isGrid(int model) { return model == kMembrane || model == kPlate; }
  }

  bool Engine::Layout::operator==(const Layout& o) const {
    return model == o.model && count == o.count && ratio == o.ratio && decay == o.decay &&
           material == o.material && brightness == o.brightness && inharmonics == o.inharmonics &&
           hit == o.hit && listen_left == o.listen_left && listen_right == o.listen_right &&
           radius == o.radius && opening == o.opening;
  }

  int Engine::modeCount(int model, int quality) {
    // Pipe and Tube always use the script's fixed 40-mode approximation.
    if (model >= kPipe) return kMaxModes;
    return quality <= kEco ? 8 : quality == kNormal ? 20 : kMaxModes;
  }

  Engine::Engine() { prepare(rate_); }

  void Engine::prepare(double sample_rate) {
    rate_ = std::max(1000.0, sample_rate);
    smooth_ = -std::expm1(-1.0 / (0.005 * rate_));
    dc_ = std::exp(-kTwoPi * 8.0 / rate_);
    release_ = std::exp(-1.0 / (0.080 * rate_));
    reset();
  }

  void Engine::reset() {
    for (auto& channel : channels_) channel = Channel {};
    snap_mode_.fill(false);
    input_previous_.fill(0); input_dc_.fill(0); wet_previous_.fill(0); wet_dc_.fill(0);
    ic1_.fill(0); ic2_.fill(0);
    lfo_phase_.fill(0); lfo_cycle_.fill(0);
    limiter_gain_ = 1.0;
    clock_ = 0;
    idle_ = true; snap_ = true;
    layout_ = Layout {};
    buildModes();
    // Start every smoothed value at its target, as the script does at @init.
    const Parameters& p = params_;
    mix_ = p.mix; excitation_ = p.excitation; wet_ = p.wet; output_ = p.output;
    width_ = p.width; bleed_ = p.model < kPipe ? p.bleed : 0.0; filter_mix_ = p.filter ? 1.0 : 0.0;
    gate_ = p.released ? 1.0 - p.off_decay : 1.0;
    filter_g_ = std::tan(kPi * std::min(p.filter_frequency, rate_ * 0.42) / rate_);
    const double bandwidth = std::exp2(0.5 * p.filter_bandwidth);
    filter_k_ = bandwidth - 1.0 / bandwidth;
  }

  void Engine::setParameters(const Parameters& parameters) {
    params_ = parameters;
    Parameters& p = params_;
    p.model = std::clamp(p.model, 0, kNumModels - 1);
    p.quality = std::clamp(p.quality, 0, kNumQualities - 1);
    p.lfo_shape = std::clamp(p.lfo_shape, 0, kNumLfoShapes - 1);
    p.pitch = std::clamp(std::isfinite(p.pitch) ? p.pitch : 130.8128, 1.0, 100000.0);
    p.decay = std::clamp(p.decay, 0.02, 30.0);
    p.material = std::clamp(p.material, -1.0, 1.0);
    p.brightness = std::clamp(p.brightness, -1.0, 1.0);
    p.inharmonics = std::clamp(p.inharmonics, -1.0, 1.0);
    p.ratio = std::clamp(p.ratio, 0.25, 4.0);
    p.hit = std::clamp(p.hit, 0.0, 1.0);
    p.listen_left = std::clamp(p.listen_left, 0.0, 1.0);
    p.listen_right = std::clamp(p.listen_right, 0.0, 1.0);
    p.spread = std::clamp(p.spread, -100.0, 100.0);
    p.width = std::clamp(p.width, 0.0, 1.0);
    p.radius = std::clamp(p.radius, 0.0, 1.0);
    p.opening = std::clamp(p.opening, 0.0, 1.0);
    p.lfo_amount = std::clamp(p.lfo_amount, 0.0, 24.0);
    p.lfo_frequency = std::clamp(p.lfo_frequency, 0.0, rate_ * 0.25);
    p.lfo_spin = std::clamp(p.lfo_spin, -1.0, 1.0);
    p.filter_frequency = std::clamp(p.filter_frequency, 20.0, 18000.0);
    p.filter_bandwidth = std::clamp(p.filter_bandwidth, 0.1, 6.0);
    p.bleed = std::clamp(p.bleed, 0.0, 1.0);
    p.mix = std::clamp(p.mix, 0.0, 1.0);
    p.glide = std::clamp(p.glide, 0.0, 0.5);
    p.off_decay = std::clamp(p.off_decay, 0.0, 1.0);
    buildModes();
  }

  void Engine::alignLfo(double cycles) {
    if (!std::isfinite(cycles)) return;
    const double whole = std::floor(cycles);
    for (int channel = 0; channel < 2; ++channel) {
      lfo_phase_[channel] = cycles - whole;
      lfo_cycle_[channel] = static_cast<int64_t>(whole);
    }
  }

  void Engine::buildModes() {
    const Parameters& p = params_;
    Layout layout;
    layout.model = p.model;
    layout.count = modeCount(p.model, p.quality);
    layout.ratio = isGrid(p.model) ? p.ratio : 0.0;
    layout.decay = p.decay; layout.material = p.material; layout.brightness = p.brightness;
    layout.inharmonics = p.inharmonics; layout.hit = p.hit;
    layout.listen_left = p.listen_left; layout.listen_right = p.listen_right;
    layout.radius = p.radius; layout.opening = p.opening;
    if (layout == layout_)
      return;

    const int previous_model = layout_.model;
    const int previous_count = count_;
    const int count = layout.count;
    std::array<int, kMaxModes> m {}, n {};
    std::array<double, kMaxModes> base {};

    if (isGrid(p.model)) {
      // The lowest modes of a rectangular 24 x 24 grid. Ties resolve by (m, n)
      // so the selection cannot flicker between equal candidates.
      struct Candidate { double ratio; int m, n; };
      std::array<Candidate, kGrid * kGrid> candidates;
      const double rr = p.ratio * p.ratio;
      for (int i = 0; i < kGrid; ++i) {
        for (int j = 0; j < kGrid; ++j) {
          const int cm = i + 1, cn = j + 1;
          const double r = (cm * cm + cn * cn * rr) / (1.0 + rr);
          candidates[i * kGrid + j] = { p.model == kMembrane ? std::sqrt(r) : r, cm, cn };
        }
      }
      std::partial_sort(candidates.begin(), candidates.begin() + count, candidates.end(),
                        [](const Candidate& a, const Candidate& b) {
        if (a.ratio != b.ratio) return a.ratio < b.ratio;
        return a.m != b.m ? a.m < b.m : a.n < b.n;
      });
      for (int i = 0; i < count; ++i) {
        base[i] = candidates[i].ratio; m[i] = candidates[i].m; n[i] = candidates[i].n;
      }
    }
    else {
      for (int i = 0; i < count; ++i) {
        m[i] = i + 1; n[i] = 0;
        double r = m[i];
        if (p.model == kBeam) {
          // Free-free beam roots; higher roots use their asymptote.
          r = i == 0 ? 1.0 : i == 1 ? 2.756538 : i == 2 ? 5.403917
                                 : std::pow((i + 1.5) * kPi / 4.730040745, 2.0);
        }
        else if (p.model == kMarimba) {
          // Undercut bar spectrum; the first modes lie near 1:4:10.
          r = i == 0 ? 1.0 : i == 1 ? 4.0 : i == 2 ? 10.0 : (i + 1.0) * (i + 1.0) * 1.12;
        }
        else if (p.model == kPipe) {
          r = (2.0 * i + 1.0 + p.opening) / (1.0 + p.opening);
        }
        base[i] = r;
      }
    }

    // Keep each Membrane or Plate mode's ringing state with its (m, n) pair
    // when Ratio reorders the selection. Other models keep modes by index.
    bool permuted = false;
    if (isGrid(p.model) && isGrid(previous_model)) {
      bool moved = false;
      for (int i = 0; i < count && !moved; ++i)
        moved = i >= previous_count || m[i] != m_[i] || n[i] != n_[i];
      if (moved) {
        permuted = true;
        const auto previous = channels_;
        for (int i = 0; i < count; ++i) {
          int source = -1;
          for (int j = 0; j < previous_count; ++j) {
            if (m_[j] == m[i] && n_[j] == n[i]) { source = j; break; }
          }
          for (int c = 0; c < 2; ++c) {
            Channel& to = channels_[c];
            const Channel& from = previous[c];
            if (source >= 0) {
              to.re[i] = from.re[source]; to.im[i] = from.im[source];
              to.gain[i] = from.gain[source]; to.fade[i] = from.fade[source];
              to.yr[i] = from.yr[source]; to.yi[i] = from.yi[source];
            }
            else {
              to.yr[i] = to.yi[i] = 0.0;
            }
            to.dre[i] = to.dim[i] = to.dgain[i] = to.dfade[i] = 0.0;
          }
          snap_mode_[i] = source < 0;
        }
        for (int i = count; i < kMaxModes; ++i) {
          for (auto& channel : channels_) channel.yr[i] = channel.yi[i] = 0.0;
        }
      }
    }
    // Modes entering the bank start silent at their target coefficients.
    for (int i = previous_count; i < count && !permuted; ++i) {
      for (auto& channel : channels_) {
        channel.yr[i] = channel.yi[i] = 0.0;
        channel.dre[i] = channel.dim[i] = channel.dgain[i] = channel.dfade[i] = 0.0;
      }
      snap_mode_[i] = true;
    }
    for (int i = count; i < previous_count; ++i) {
      for (auto& channel : channels_) channel.yr[i] = channel.yi[i] = 0.0;
    }

    double energy_left = 0.0, energy_right = 0.0;
    const double radius = p.radius;
    for (int i = 0; i < count; ++i) {
      double r = base[i], left, right, amplitude, decay;
      if (p.model < kPipe) {
        r = std::pow(r, std::exp(p.inharmonics * 0.4));
        const double hit = positionGain(m[i], n[i], p.hit);
        left = hit * positionGain(m[i], n[i], p.listen_left);
        right = hit * positionGain(m[i], n[i], p.listen_right);
        amplitude = std::pow(r, p.brightness * 0.8 - 0.65);
        decay = p.decay * std::pow(r, p.material * 0.8);
      }
      else {
        left = right = 1.0;
        amplitude = std::pow(r, -0.6 - 0.45 * (1.0 - radius));
        decay = p.decay * (0.35 + 1.3 * radius) /
                (1.0 + std::pow(r - 1.0, 1.15) * (0.2 * (1.0 - radius) + 0.005));
      }
      ratio_[i] = r;
      decay_[i] = std::clamp(decay, 0.005, 60.0);
      amplitude_[0][i] = left * amplitude;
      amplitude_[1][i] = right * amplitude;
      m_[i] = m[i]; n_[i] = n[i];
      energy_left += amplitude_[0][i] * amplitude_[0][i];
      energy_right += amplitude_[1][i] * amplitude_[1][i];
    }
    for (int i = count; i < kMaxModes; ++i) {
      ratio_[i] = 1.0; decay_[i] = 0.02;
      amplitude_[0][i] = amplitude_[1][i] = 0.0;
      m_[i] = n_[i] = 0;
    }
    // A fixed floor preserves position nulls instead of amplifying them away.
    norm_ = 0.65 / std::sqrt(std::max(1.0, 0.5 * (energy_left + energy_right)));
    count_ = count;
    layout_ = layout;
    // Entering modes take their coefficients now, not at the next control
    // tick, so a hit arriving in between excites them correctly. Before the
    // first tick there is no pitch yet; that tick snaps every mode anyway.
    if (!snap_) {
      for (int i = 0; i < count_; ++i)
        if (snap_mode_[i]) setModeTargets(i, true, 1.0);
    }
  }

  double Engine::lfoValue(int channel) const {
    double position = lfo_phase_[channel] + params_.lfo_offset;
    if (channel == 1) position += params_.lfo_stereo_phase;
    const double whole = std::floor(position);
    return lfoWave(params_.lfo_shape, position - whole, lfo_cycle_[channel] + static_cast<int64_t>(whole));
  }

  void Engine::updateCoefficients() {
    // Flush decayed state first. A silent bank snaps to its new coefficients:
    // with zero state the path between them is inaudible, and the bank can
    // then be skipped until excitation arrives without leaving stale poles.
    bool silent = true;
    for (auto& channel : channels_) {
      for (int i = 0; i < count_; ++i) {
        if (std::abs(channel.yr[i]) < kFlush) channel.yr[i] = 0.0;
        if (std::abs(channel.yi[i]) < kFlush) channel.yi[i] = 0.0;
        silent = silent && channel.yr[i] == 0.0 && channel.yi[i] == 0.0;
      }
    }
    for (int c = 0; c < 2; ++c) {
      if (std::abs(input_dc_[c]) < kFlush) input_dc_[c] = 0.0;
      if (std::abs(wet_dc_[c]) < kFlush) wet_dc_[c] = 0.0;
      if (std::abs(ic1_[c]) < kFlush) ic1_[c] = 0.0;
      if (std::abs(ic2_[c]) < kFlush) ic2_[c] = 0.0;
    }
    idle_ = silent;

    const Parameters& p = params_;
    const double target = std::log2(p.pitch);
    const double glide = snap_ || p.glide <= 0.0 ? 1.0 : -std::expm1(-kControlPeriod / (p.glide * rate_));
    pitch_log_ += glide * (target - pitch_log_);
    // Spread and LFO act after glide, so glide never smooths away LFO depth.
    const double base = std::exp2(pitch_log_);
    for (int c = 0; c < 2; ++c) {
      const double detune = (c == 0 ? 0.005 : -0.005) * p.spread + lfoValue(c) * p.lfo_amount;
      pitch_[c] = std::clamp(base * std::exp2(detune / 12.0), 15.0, rate_ * 0.40);
    }

    const double gain_mix = -std::expm1(-kControlPeriod / (0.003 * rate_));
    for (int i = 0; i < count_; ++i)
      setModeTargets(i, snap_ || snap_mode_[i] || silent, gain_mix);
    snap_ = false;
  }

  void Engine::setModeTargets(int i, bool snap, double gain_mix) {
    const Parameters& p = params_;
    double decay = decay_[i];
    if (p.released && p.off_decay > 0.0) {
      const double off = 1.0 - p.off_decay;
      decay = std::min(decay, 0.003 + 2.0 * off * off * off);
    }
    const double loss = -std::expm1(-kLn1000 / (rate_ * decay));
    const double r = 1.0 - loss;
    const double scale = norm_ * std::sqrt(loss * (1.0 + r));
    const double top = rate_ * 0.46, fade_width = rate_ * 0.04;
    for (int c = 0; c < 2; ++c) {
      Channel& channel = channels_[c];
      const double frequency = pitch_[c] * ratio_[i];
      const double fade = std::clamp((top - frequency) / fade_width, 0.0, 1.0);
      const double angle = kTwoPi * std::min(frequency, top) / rate_;
      const double re = r * std::cos(angle), im = r * std::sin(angle);
      const double gain = fade * scale * amplitude_[c][i];
      if (snap) {
        channel.re[i] = re; channel.im[i] = im; channel.gain[i] = gain; channel.fade[i] = fade;
        channel.dre[i] = channel.dim[i] = channel.dgain[i] = channel.dfade[i] = 0.0;
      }
      else {
        // Interpolating the complex pole keeps it inside the unit circle.
        channel.dre[i] = (re - channel.re[i]) / kControlPeriod;
        channel.dim[i] = (im - channel.im[i]) / kControlPeriod;
        channel.dgain[i] = (gain - channel.gain[i]) * gain_mix / kControlPeriod;
        channel.dfade[i] = (fade - channel.fade[i]) / kControlPeriod;
      }
    }
    snap_mode_[i] = false;
  }

  double Engine::resonate(Channel& ch, double input) {
    // Independent modes vectorize; the sum uses four partial accumulators
    // (every mode count is a multiple of four) rather than one serial chain.
    static_assert(kMaxModes % 4 == 0 && kControlPeriod > 0);
    alignas(16) std::array<double, kMaxModes> out;
    for (int i = 0; i < count_; ++i) {
      const double re = ch.re[i] + ch.dre[i], im = ch.im[i] + ch.dim[i];
      const double gain = ch.gain[i] + ch.dgain[i], fade = ch.fade[i] + ch.dfade[i];
      ch.re[i] = re; ch.im[i] = im; ch.gain[i] = gain; ch.fade[i] = fade;
      const double yr = re * ch.yr[i] - im * ch.yi[i];
      const double yi = im * ch.yr[i] + re * ch.yi[i] + input * gain;
      ch.yr[i] = yr; ch.yi[i] = yi;
      out[i] = yr * fade;
    }
    double sum[4] = { 0.0, 0.0, 0.0, 0.0 };
    for (int i = 0; i < count_; i += 4) {
      sum[0] += out[i]; sum[1] += out[i + 1]; sum[2] += out[i + 2]; sum[3] += out[i + 3];
    }
    return (sum[0] + sum[1]) + (sum[2] + sum[3]);
  }

  double Engine::bandpass(int c, double input) {
    // Topology-preserving state-variable filter, unity-peak band-pass.
    const double g = filter_g_, k = filter_k_;
    const double a1 = 1.0 / (1.0 + g * (g + k));
    const double v3 = input - ic2_[c];
    const double v1 = a1 * ic1_[c] + g * a1 * v3;
    const double v2 = ic2_[c] + g * v1;
    ic1_[c] = 2.0 * v1 - ic1_[c];
    ic2_[c] = 2.0 * v2 - ic2_[c];
    return k * v1;
  }

  void Engine::process(const float* in_left, const float* in_right,
                       float* out_left, float* out_right, int samples) {
    const Parameters& p = params_;
    const double gate_target = p.released ? 1.0 - p.off_decay : 1.0;
    const double bleed_target = p.model < kPipe ? p.bleed : 0.0;
    const double filter_target = p.filter ? 1.0 : 0.0;
    const double g_target = std::tan(kPi * std::min(p.filter_frequency, rate_ * 0.42) / rate_);
    const double bandwidth = std::exp2(0.5 * p.filter_bandwidth);
    const double k_target = bandwidth - 1.0 / bandwidth;
    const double spin = p.lfo_free ? std::exp2(p.lfo_spin * 0.5) : 1.0;
    const double increments[2] = { p.lfo_frequency * spin / rate_, p.lfo_frequency / spin / rate_ };

    for (int s = 0; s < samples; ++s) {
      if (clock_ <= 0) {
        updateCoefficients();
        clock_ = kControlPeriod;
      }
      --clock_;
      for (int c = 0; c < 2; ++c) {
        lfo_phase_[c] += increments[c];
        if (lfo_phase_[c] >= 1.0) {
          const double whole = std::floor(lfo_phase_[c]);
          lfo_phase_[c] -= whole;
          lfo_cycle_[c] += static_cast<int64_t>(whole);
        }
      }

      mix_ += smooth_ * (p.mix - mix_);
      excitation_ += smooth_ * (p.excitation - excitation_);
      wet_ += smooth_ * (p.wet - wet_);
      output_ += smooth_ * (p.output - output_);
      width_ += smooth_ * (p.width - width_);
      bleed_ += smooth_ * (bleed_target - bleed_);
      filter_mix_ += smooth_ * (filter_target - filter_mix_);
      filter_g_ += smooth_ * (g_target - filter_g_);
      filter_k_ += smooth_ * (k_target - filter_k_);
      gate_ += smooth_ * (gate_target - gate_);

      const double dry[2] = { in_left[s], in_right[s] };
      // Mix scales the excitation, so existing tails ring on when Mix changes.
      const double drive = excitation_ * mix_ * gate_;
      double excite[2];
      for (int c = 0; c < 2; ++c) {
        // An 8 Hz high-pass keeps DC from driving the resonators.
        const double blocked = dry[c] - input_previous_[c] + dc_ * input_dc_[c];
        input_previous_[c] = dry[c];
        input_dc_[c] = blocked;
        excite[c] = blocked * drive;
      }

      // An idle bank has zero state and constant coefficients until the next
      // control tick, so skipping it is exact.
      double sum[2] = { 0.0, 0.0 };
      if (!idle_ || excite[0] != 0.0 || excite[1] != 0.0) {
        idle_ = false;
        sum[0] = resonate(channels_[0], excite[0]);
        sum[1] = resonate(channels_[1], excite[1]);
      }

      const double mid = 0.5 * (sum[0] + sum[1]), side = 0.5 * (sum[0] - sum[1]) * width_;
      double wet[2] = { mid + side + dry[0] * bleed_ * mix_, mid - side + dry[1] * bleed_ * mix_ };
      for (int c = 0; c < 2; ++c) {
        const double filtered = bandpass(c, wet[c]);
        wet[c] += filter_mix_ * (filtered - wet[c]);
        const double blocked = wet[c] - wet_previous_[c] + dc_ * wet_dc_[c];
        wet_previous_[c] = wet[c];
        wet_dc_[c] = blocked;
        wet[c] = wet_ * blocked * output_;
      }

      // Stereo-linked, zero-lookahead sample-peak limiter on the resonated
      // signal only. The dry path is never limited, so 0% Mix is transparent.
      const double peak = std::max(std::abs(wet[0]), std::abs(wet[1]));
      const double limit = peak > kLimiterCeiling ? kLimiterCeiling / peak : 1.0;
      limiter_gain_ = limit < limiter_gain_ ? limit : limit + release_ * (limiter_gain_ - limit);
      const double wet_gain = p.limiter ? limiter_gain_ : 1.0;
      const double dry_gain = (1.0 - mix_) * output_;
      out_left[s] = static_cast<float>(dry_gain * dry[0] + wet_gain * wet[0]);
      out_right[s] = static_cast<float>(dry_gain * dry[1] + wet_gain * wet[1]);
    }
  }
}
