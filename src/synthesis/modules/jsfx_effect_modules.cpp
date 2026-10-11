#include "jsfx_effect_modules.h"

#include <algorithm>

namespace vital {
  namespace {
    double jsfxControl(Output* output, double minimum, double maximum) {
      return std::clamp(static_cast<double>(output->buffer[0][0]), minimum, maximum);
    }
    template<class Core>
    void renderJsfx(Core& core, jsfx::HostRate& adapter, const poly_float* input,
                    poly_float* output, int samples, double wet = 1) {
      for (int i = 0; i < samples; ++i) {
        auto frame = adapter.process({ input[i][0], input[i][1], input[i][2], input[i][3] }, core);
        for (int lane = 0; lane < 4; ++lane)
          frame[lane] = (1 - wet) * input[i][lane] + wet * frame[lane];
        output[i] = poly_float(frame[0], frame[1], frame[2], frame[3]);
      }
    }
  }

  void ErosionModule::init() {
    mode_ = createBaseControl(prefix_ + "erosion_mode");
    const char* names[] = { "frequency", "width", "amount", "stereo" };
    for (int i = 0; i < 4; ++i) controls_[i] = createMonoModControl(prefix_ + "erosion_" + names[i]);
    core_.prepare(getSampleRate() / getOversampleAmount());
    rate_adapter_.prepare(getOversampleAmount());
    SynthModule::init();
  }
  void ErosionModule::processWithInput(const poly_float* input, int samples) {
    if (samples <= 0) return;
    SynthModule::process(samples);
    jsfx::Erosion::Parameters p;
    p.mode = mode_->value() >= 0.5f ? 1 : 0;
    p.frequency = jsfxControl(controls_[0], 20, 20000);
    p.width = jsfxControl(controls_[1], 0.1, 5);
    p.amount = jsfxControl(controls_[2], 0, 200);
    p.stereo = jsfxControl(controls_[3], 0, 100);
    core_.setParameters(p);
    renderJsfx(core_, rate_adapter_, input, output()->buffer, samples);
  }
  void ErosionModule::enable(bool on) {
    if (enabled() != on) hardReset();
    SynthModule::enable(on);
  }
  void ErosionModule::setSampleRate(int rate) {
    const int old = getSampleRate(); SynthModule::setSampleRate(rate);
    if (getSampleRate() != old) {
      core_.prepare(getSampleRate() / getOversampleAmount());
      rate_adapter_.prepare(getOversampleAmount());
    }
  }
  void ErosionModule::setOversampleAmount(int amount) {
    const int old = getSampleRate(); SynthModule::setOversampleAmount(amount);
    if (getSampleRate() != old) {
      core_.prepare(getSampleRate() / getOversampleAmount());
      rate_adapter_.prepare(getOversampleAmount());
      hardReset();
    }
  }

  FloatyDelayModule::FloatyDelayModule(const Output* bps, std::string prefix) :
      SynthModule(0, 1), prefix_(prefix), beats_per_second_(bps) {
    core_.prepare(getSampleRate() / getOversampleAmount());
  }
  void FloatyDelayModule::init() {
    sync_ = createBaseControl(prefix_ + "floaty_delay_sync");
    const char* names[] = { "time", "mix", "feedback", "warp_rate", "warp_amount",
                           "cutoff", "resonance", "playback_rate", "stereo_offset", "clamp" };
    for (int i = 0; i < 10; ++i) controls_[i] = createMonoModControl(prefix_ + "floaty_delay_" + names[i]);
    SynthModule::init();
  }
  void FloatyDelayModule::processWithInput(const poly_float* input, int samples) {
    if (samples <= 0) return;
    SynthModule::process(samples);
    jsfx::Floaty::Parameters p;
    p.delay = jsfxControl(controls_[0], 0, 4000); p.mix = jsfxControl(controls_[1], 0, 100);
    p.feedback = jsfxControl(controls_[2], 0, 80); p.warp_rate = jsfxControl(controls_[3], 0, 10);
    p.warp = jsfxControl(controls_[4], 0, 4); p.cutoff = jsfxControl(controls_[5], 0, 100);
    p.resonance = jsfxControl(controls_[6], 0, 150); p.playback = jsfxControl(controls_[7], -2, 2);
    p.offset = jsfxControl(controls_[8], 70, 100); p.clamp = jsfxControl(controls_[9], -12, 12);
    p.sync = std::clamp(static_cast<int>(sync_->value() + 0.5f), 0, 64);
    const double wet = p.mix / 100;
    p.mix = 100;
    core_.setParameters(p, beats_per_second_->buffer[0][0] * 60.0);
    // Floaty's dry path stays immediate, including at 0% mix. Only the wet
    // signal crosses the sample-rate boundary and its fixed conversion delay.
    renderJsfx(core_, rate_adapter_, input, output()->buffer, samples, wet);
  }
  void FloatyDelayModule::enable(bool on) {
    if (enabled() != on) hardReset();
    SynthModule::enable(on);
  }
  void FloatyDelayModule::setSampleRate(int rate) {
    const int old = getSampleRate(); SynthModule::setSampleRate(rate);
    if (getSampleRate() != old) {
      core_.prepare(getSampleRate() / getOversampleAmount());
      rate_adapter_.prepare(getOversampleAmount());
    }
  }
  void FloatyDelayModule::setOversampleAmount(int amount) {
    const int old = getSampleRate(); SynthModule::setOversampleAmount(amount);
    if (getSampleRate() != old) {
      core_.prepare(getSampleRate() / getOversampleAmount());
      rate_adapter_.prepare(getOversampleAmount());
      hardReset();
    }
  }
}
