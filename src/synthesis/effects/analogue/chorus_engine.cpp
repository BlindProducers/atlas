// SPDX-License-Identifier: GPL-3.0-or-later
#include "chorus_engine.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace atlas::analogue::chorus {
  namespace {
    constexpr double kPi = 3.14159265358979323846;
    constexpr double kModelFadeSeconds = 0.03;
    constexpr double kSmoothingSeconds = 0.02;
    // Longest delay of any model (Dimension button 1) plus margin: the
    // incoming model fills its line before it fades in.
    constexpr double kPrimeSeconds = 0.014;
    constexpr double kMinDelay = 0.0003;
    // The BBD takes a new clock every BbdLine::kRefreshInterval samples, so
    // the LFO is evaluated at that rate.
    constexpr int kControlInterval = BbdLine::kRefreshInterval;

    // Dimension D cross-feed: the other side's line, inverted and high-passed.
    // The service-note stage reads as about -0.68 with a ~480 Hz high-pass,
    // other analyses put the corner lower; the DC-2 summers sit near 200 Hz.
    constexpr double kDimensionCross = 0.68;
    constexpr double kDimensionCrossHighpass = 200.0;

    // NE570 around the Dimension's BBDs: 10 kOhm x 1 uF rectifier.
    constexpr double kDimensionRectifierSeconds = 0.01;

    struct ModelSpec {
      BbdChip chip;
      AnalogFilter input_filter, output_filter;
      double signal_to_noise_db = 80.0;  // chip datasheet, re maximum input
      double input_highpass_hz = 0.0, output_highpass_hz = 0.0;
      bool compander = false;
      std::vector<ModeSpec> modes;
    };

    // 0 dBFS corresponds to the chip's maximum input (THD 2.5%). The bucket
    // curve's second-order term sets each chip's THD at half that level.
    BbdChip chip(int stages, double even_distortion) {
      BbdChip result;
      result.stages = stages;
      result.headroom = 2.5;
      result.even_distortion = even_distortion;
      result.clock_leakage = 1.0e-3;
      return result;
    }

    // Juno-60 BBD filters as fitted to a real unit by Holters & Parker
    // (DAFx-18, Table 1), in rad/s: 5th-order low passes, the input reaching
    // -40 dB by 23.5 kHz.
    AnalogFilter junoInputFilter() {
      return fromPolesAndResidues({
        { { -46580.0, 0.0 }, { 251589.0, 0.0 } },
        { { -55482.0, 25082.0 }, { -130428.0, -4165.0 } },
        { { -26292.0, 59437.0 }, { 4634.0, 22873.0 } } });
    }

    AnalogFilter junoOutputFilter() {
      return fromPolesAndResidues({
        { { -176261.0, 0.0 }, { 5092.0, 0.0 } },
        { { -51468.0, 21437.0 }, { 11256.0, -99566.0 } },
        { { -26276.0, 59699.0 }, { -13802.0, 24606.0 } } });
    }

    // Hardware data; sources and confidence are listed in ANALOGUE_FX_PLAN.md.
    const std::array<ModelSpec, kNumModels>& models() {
      static const std::array<ModelSpec, kNumModels> specs = [] {
        std::array<ModelSpec, kNumModels> result;

        // Roland Juno-60 chorus board (service notes, 1983); rates and delays
        // measured by Andy Hughes; filters as fitted to a real unit by
        // Holters & Parker (DAFx-18, Table 1). Two MN3009 (256 stages, S/N
        // 88 dB). The triangle LFO reaches the second clock inverted in modes I
        // and II and in phase in I+II, where an RC network rounds it to a
        // near-sine. Each side is dry plus its own line; the mixer gives the
        // wet 1.205x the dry and the BBD stage adds 2.3 dB.
        auto& juno = result[kJuno];
        juno.chip = chip(256, 0.03);
        juno.signal_to_noise_db = 88.0;
        juno.input_filter = junoInputFilter();
        juno.output_filter = junoOutputFilter();
        juno.input_highpass_hz = 15.9;
        juno.output_highpass_hz = 7.2;
        {
          ModeSpec mode;
          mode.lines = 2;
          mode.rate_hz = { 0.513, 0.0 };
          mode.phase = { 0.0, 0.5, 0.0 };
          mode.min_delay = 0.00166;
          mode.max_delay = 0.00535;
          mode.to_left = { 1.0, 0.0, 0.0 };
          mode.to_right = { 0.0, 1.0, 0.0 };
          mode.wet_gain = 1.205 * std::pow(10.0, 2.3 / 20.0);
          juno.modes.push_back(mode);
          mode.rate_hz = { 0.863, 0.0 };
          juno.modes.push_back(mode);
          mode.rate_hz = { 9.75, 0.0 };
          mode.shape = LfoShape::kSine;
          mode.phase = { 0.0, 0.0, 0.0 };
          mode.min_delay = 0.0033;
          mode.max_delay = 0.0037;
          juno.modes.push_back(mode);
        }

        // Roland SDD-320 Dimension D (service notes, 1979): two MN3007 (1024
        // stages, S/N 80 dB), each between NE570 compander halves, swept in
        // anti-phase by one triangle LFO: 0.25 Hz for buttons 1-2, 0.5 Hz for
        // 3-4, over the service-note delay ranges. Each side is its own line
        // plus the other line inverted and high-passed; button 4 pads the
        // same-side line by 10 dB. Filter capacitor readings are uncertain, so
        // 4th-order Butterworth sections at the readings' ~10 kHz are used.
        auto& dimension = result[kDimension];
        dimension.chip = chip(1024, 0.05);
        dimension.signal_to_noise_db = 80.0;
        dimension.input_filter = toPartialFractions(butterworthLowPass(4, 10000.0));
        dimension.output_filter = toPartialFractions(butterworthLowPass(4, 10000.0));
        dimension.output_highpass_hz = 10.0;
        dimension.compander = true;
        {
          const double rates[] = { 0.25, 0.25, 0.5, 0.5 };
          const double minimum[] = { 0.008, 0.005, 0.006, 0.006 };
          const double maximum[] = { 0.012, 0.010, 0.009, 0.009 };
          for (int button = 0; button < kNumDimensionModes; ++button) {
            ModeSpec mode;
            mode.lines = 2;
            mode.rate_hz = { rates[button], 0.0 };
            mode.phase = { 0.0, 0.5, 0.0 };
            mode.min_delay = minimum[button];
            mode.max_delay = maximum[button];
            const double own = button == 3 ? std::pow(10.0, -10.0 / 20.0) : 1.0;
            mode.to_left = { own, 0.0, 0.0 };
            mode.to_right = { 0.0, own, 0.0 };
            mode.cross_left = { 0.0, -kDimensionCross, 0.0 };
            mode.cross_right = { -kDimensionCross, 0.0, 0.0 };
            mode.cross_highpass_hz = kDimensionCrossHighpass;
            dimension.modes.push_back(mode);
          }
        }

        // Boss CE-2 (ElectroSmash analysis): one MN3007 between +15 dB
        // pre-emphasis (410 Hz - 2.34 kHz) and matching de-emphasis, 3rd-order
        // filters at 6.6 and 6.9 kHz, 48 Hz and 14.6 Hz coupling, triangle LFO
        // 0.3-3.5 Hz (1 Hz here, the knob's middle), 50/50 mix. Its delay range
        // is undocumented; 6-9 ms suits the MN3007's clock range. Stereo as on
        // Boss's later stereo choruses: the wet is inverted on the right.
        auto& ce = result[kCe];
        ce.chip = chip(1024, 0.05);
        ce.signal_to_noise_db = 80.0;
        ce.input_filter = toPartialFractions({ highPass1(48.0), shelf1(410.0, 2340.0), lowPass1(6600.0), lowPass2(6900.0, 1.0) });
        ce.output_filter = toPartialFractions({ lowPass1(6600.0), lowPass2(6900.0, 1.0), shelf1(2340.0, 410.0), highPass1(14.6) });
        {
          ModeSpec mode;
          mode.lines = 1;
          mode.rate_hz = { 1.0, 0.0 };
          mode.min_delay = 0.006;
          mode.max_delay = 0.009;
          mode.to_left = { 1.0, 0.0, 0.0 };
          mode.to_right = { -1.0, 0.0, 0.0 };
          ce.modes.push_back(mode);
        }

        // String ensemble after the Eminent/Solina patent (US 3,833,752) and
        // Juergen Haible's analysis: three 185-stage BBDs (TCA 350 Y; the
        // TDA1022's 74 dB S/N as the closest datasheet), each clocked by a slow
        // (just under 1 Hz) plus fast (5-6 Hz) quasi-sine, 120 degrees apart,
        // Only Haible's 12.6 kHz 2nd-order pre-filter is documented, too gentle
        // on its own to keep the digital input's images out of the slow
        // 185-stage clock; like jpcima's string-machine, the Juno-60 filters
        // stand in. The relative LFO depths are undocumented: the fast LFO
        // contributes about twice the slow one's pitch movement. The
        // hardware's mono sum is the centre of the stereo spread.
        auto& ensemble = result[kEnsemble];
        ensemble.chip = chip(185, 0.03);
        ensemble.signal_to_noise_db = 74.0;
        ensemble.input_filter = junoInputFilter();
        ensemble.output_filter = junoOutputFilter();
        ensemble.input_highpass_hz = 20.0;
        ensemble.output_highpass_hz = 10.0;
        {
          ModeSpec mode;
          mode.lines = 3;
          mode.rate_hz = { 0.75, 6.0 };
          mode.weight = { 0.77, 0.23 };
          mode.shape = LfoShape::kSine;
          mode.phase = { 0.0, 1.0 / 3.0, 2.0 / 3.0 };
          mode.min_delay = 0.0015;
          mode.max_delay = 0.0035;
          mode.to_left = { 2.0 / 3.0, 1.0 / 3.0, 0.0 };
          mode.to_right = { 0.0, 1.0 / 3.0, 2.0 / 3.0 };
          ensemble.modes.push_back(mode);
        }

        for (auto& spec : result)
          spec.chip.noise = noiseForSignalToNoise(spec.signal_to_noise_db, spec.output_filter, 100000.0);
        return result;
      }();
      return specs;
    }

    double lfoShape(LfoShape shape, double phase) {
      phase -= std::floor(phase);
      if (shape == LfoShape::kSine)
        return std::sin(2.0 * kPi * phase);
      return 4.0 * std::abs(phase - 0.5) - 1.0;
    }

    double onePole(double hz, double sample_rate) {
      return hz > 0.0 ? -std::expm1(-2.0 * kPi * hz / sample_rate) : 0.0;
    }
  }

  int numModes(int model) {
    return static_cast<int>(models()[std::clamp(model, 0, kNumModels - 1)].modes.size());
  }

  int stages(int model) { return models()[std::clamp(model, 0, kNumModels - 1)].chip.stages; }

  const ModeSpec& modeSpec(int model, int mode) {
    const auto& spec = models()[std::clamp(model, 0, kNumModels - 1)];
    return spec.modes[std::clamp(mode, 0, static_cast<int>(spec.modes.size()) - 1)];
  }

  Engine::Engine() { prepare(sample_rate_); }

  void Engine::prepare(double sample_rate) {
    sample_rate_ = sample_rate;
    smoothing_ = -std::expm1(-1.0 / (kSmoothingSeconds * sample_rate));
    fade_length_ = std::max(1, static_cast<int>(kModelFadeSeconds * sample_rate));
    prime_length_ = static_cast<int>(std::ceil(kPrimeSeconds * sample_rate));
    for (int model = 0; model < kNumModels; ++model) {
      const auto& spec = models()[model];
      auto& voice = voices_[model];
      for (auto& line : voice.lines)
        line.configure(spec.chip, spec.input_filter, spec.output_filter, sample_rate);
      for (auto& compander : voice.companders)
        compander.prepare(sample_rate, kDimensionRectifierSeconds, 0.33);
    }
    reset();
  }

  void Engine::resetVoice(Voice& voice) {
    for (auto& line : voice.lines) line.reset();
    for (auto& compander : voice.companders) compander.reset();
    voice.cross_state = {};
    voice.input_highpass = 0.0;
    voice.output_highpass = {};
    voice.primed = voice.settled = false;
    voice.control_countdown = 0;
  }

  void Engine::reset() {
    for (auto& voice : voices_) resetVoice(voice);
    lfo_phase_ = {};
    current_model_ = parameters_.model;
    fading_model_ = -1;
    fade_remaining_ = fade_wait_ = 0;
    rate_ = parameters_.rate;
    depth_ = parameters_.depth;
    width_ = parameters_.width;
    mix_ = parameters_.mix;
    noise_ = parameters_.noise;
  }

  void Engine::setParameters(const Parameters& parameters) {
    parameters_ = parameters;
    parameters_.model = std::clamp(parameters.model, 0, kNumModels - 1);
    if (parameters_.model != current_model_) {
      // The outgoing model keeps running while it fades; the incoming one
      // starts from silence so no stale audio replays.
      if (fading_model_ < 0) {
        fading_model_ = current_model_;
        fade_remaining_ = fade_length_;
        fade_wait_ = prime_length_;
      }
      current_model_ = parameters_.model;
      resetVoice(voices_[current_model_]);
      if (fading_model_ == current_model_) {
        fading_model_ = -1;
        fade_remaining_ = fade_wait_ = 0;
      }
    }
  }

  void Engine::updateTargets(Voice& voice, const ModeSpec& spec, double coefficient) {
    bool settled = true;
    const auto approach = [coefficient, &settled](double& value, double target) {
      value += (target - value) * coefficient;
      if (std::abs(target - value) < 1.0e-9) value = target;
      else settled = false;
    };
    for (int i = 0; i < 2; ++i) approach(voice.weight[i], spec.weight[i]);
    for (int i = 0; i < kMaxLines; ++i) {
      approach(voice.to_left[i], spec.to_left[i]);
      approach(voice.to_right[i], spec.to_right[i]);
      approach(voice.cross_left[i], spec.cross_left[i]);
      approach(voice.cross_right[i], spec.cross_right[i]);
    }
    approach(voice.min_delay, spec.min_delay);
    approach(voice.max_delay, spec.max_delay);
    approach(voice.wet_gain, spec.wet_gain);
    voice.settled = settled;
  }

  void Engine::renderModel(int model, Voice& voice, const float* mono, float* wet_left, float* wet_right, int count) {
    const auto& model_spec = models()[model];
    const int mode = model == kJuno ? parameters_.juno_mode : model == kDimension ? parameters_.dimension_mode : 0;
    const ModeSpec& spec = modeSpec(model, mode);
    if (!voice.primed) {
      updateTargets(voice, spec, 1.0);
      voice.primed = true;
    }
    if (voice.settled && (voice.min_delay != spec.min_delay || voice.max_delay != spec.max_delay ||
                          voice.wet_gain != spec.wet_gain || voice.to_left != spec.to_left ||
                          voice.to_right != spec.to_right || voice.weight != spec.weight))
      voice.settled = false;
    const double noise_scale = (noise_ / 0.5) * (noise_ / 0.5);
    for (int i = 0; i < spec.lines; ++i)
      voice.lines[i].setNoiseScale(noise_scale, noise_scale);

    const double input_highpass = onePole(model_spec.input_highpass_hz, sample_rate_);
    const double output_highpass = onePole(model_spec.output_highpass_hz, sample_rate_);
    const double cross_highpass = onePole(spec.cross_highpass_hz, sample_rate_);
    // Ensemble's slow and fast LFOs keep their ratio when Rate scales them.
    const double increment0 = spec.rate_hz[0] * rate_ / sample_rate_;
    const double increment1 = spec.rate_hz[1] * rate_ / sample_rate_;
    std::array<double, 2> phase = lfo_phase_;
    for (int n = 0; n < count; ++n) {
      if (!voice.settled)
        updateTargets(voice, spec, smoothing_);
      double input = mono[n];
      if (input_highpass > 0.0) {
        voice.input_highpass += (input - voice.input_highpass) * input_highpass;
        input -= voice.input_highpass;
      }
      // The LFO sweeps the clock period, so the delay follows the LFO shape.
      if (--voice.control_countdown <= 0) {
        voice.control_countdown = kControlInterval;
        const double center = 0.5 * (voice.max_delay + voice.min_delay);
        const double swing = 0.5 * (voice.max_delay - voice.min_delay) * depth_;
        for (int i = 0; i < spec.lines; ++i) {
          const double lfo = voice.weight[0] * lfoShape(spec.shape, phase[0] + spec.phase[i]) +
                             voice.weight[1] * lfoShape(spec.shape, phase[1] + spec.phase[i]);
          voice.lines[i].setDelay(std::max(kMinDelay, center + swing * lfo));
        }
      }
      double left = 0.0, right = 0.0;
      for (int i = 0; i < spec.lines; ++i) {
        double y;
        if (model_spec.compander)
          y = voice.companders[i].expand(voice.lines[i].process(voice.companders[i].compress(input)));
        else
          y = voice.lines[i].process(input);
        if (output_highpass > 0.0) {
          voice.output_highpass[i] += (y - voice.output_highpass[i]) * output_highpass;
          y -= voice.output_highpass[i];
        }
        double crossed = y;
        if (cross_highpass > 0.0) {
          voice.cross_state[i] += (y - voice.cross_state[i]) * cross_highpass;
          crossed = y - voice.cross_state[i];
        }
        left += voice.to_left[i] * y + voice.cross_left[i] * crossed;
        right += voice.to_right[i] * y + voice.cross_right[i] * crossed;
      }
      wet_left[n] = static_cast<float>(left * voice.wet_gain);
      wet_right[n] = static_cast<float>(right * voice.wet_gain);
      phase[0] += increment0;
      phase[1] += increment1;
      phase[0] -= std::floor(phase[0]);
      phase[1] -= std::floor(phase[1]);
    }
  }

  void Engine::process(const float* in_left, const float* in_right, float* out_left, float* out_right, int count) {
    constexpr int kBlock = 128;
    for (int offset = 0; offset < count;) {
      const int block = std::min(kBlock, count - offset);
      float mono[kBlock], wet_left[kBlock], wet_right[kBlock];
      for (int n = 0; n < block; ++n)
        mono[n] = 0.5f * (in_left[offset + n] + in_right[offset + n]);

      const double block_smoothing = 1.0 - std::pow(1.0 - smoothing_, block);
      rate_ += (parameters_.rate - rate_) * block_smoothing;
      depth_ += (parameters_.depth - depth_) * block_smoothing;
      noise_ += (parameters_.noise - noise_) * block_smoothing;

      renderModel(current_model_, voices_[current_model_], mono, wet_left, wet_right, block);
      if (fading_model_ >= 0) {
        float old_left[kBlock], old_right[kBlock];
        renderModel(fading_model_, voices_[fading_model_], mono, old_left, old_right, block);
        for (int n = 0; n < block; ++n) {
          double progress = 0.0;
          if (fade_wait_ > 0) --fade_wait_;
          else {
            progress = fade_remaining_ > 0 ? 1.0 - double(fade_remaining_) / fade_length_ : 1.0;
            if (fade_remaining_ > 0) --fade_remaining_;
          }
          const double in_gain = std::sin(0.5 * kPi * progress), out_gain = std::cos(0.5 * kPi * progress);
          wet_left[n] = static_cast<float>(wet_left[n] * in_gain + old_left[n] * out_gain);
          wet_right[n] = static_cast<float>(wet_right[n] * in_gain + old_right[n] * out_gain);
        }
        if (fade_remaining_ == 0) {
          resetVoice(voices_[fading_model_]);
          fading_model_ = -1;
        }
      }
      // The LFO phase is shared so a model change keeps the sweep's position.
      const auto& spec = modeSpec(current_model_, current_model_ == kJuno ? parameters_.juno_mode
                                  : current_model_ == kDimension ? parameters_.dimension_mode : 0);
      lfo_phase_[0] += spec.rate_hz[0] * rate_ * block / sample_rate_;
      lfo_phase_[1] += spec.rate_hz[1] * rate_ * block / sample_rate_;
      lfo_phase_[0] -= std::floor(lfo_phase_[0]);
      lfo_phase_[1] -= std::floor(lfo_phase_[1]);

      for (int n = 0; n < block; ++n) {
        width_ += (parameters_.width - width_) * smoothing_;
        mix_ += (parameters_.mix - mix_) * smoothing_;
        const double mid = 0.5 * (wet_left[n] + wet_right[n]);
        const double side = 0.5 * (wet_left[n] - wet_right[n]) * width_;
        const double dry_gain = std::cos(0.5 * kPi * mix_), wet_gain = std::sin(0.5 * kPi * mix_);
        out_left[offset + n] = static_cast<float>(dry_gain * in_left[offset + n] + wet_gain * (mid + side));
        out_right[offset + n] = static_cast<float>(dry_gain * in_right[offset + n] + wet_gain * (mid - side));
      }
      offset += block;
    }
  }
}
