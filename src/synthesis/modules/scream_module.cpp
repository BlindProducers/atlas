#include "scream_module.h"
#include <algorithm>
#include "../../../third_party/scream/Source/DSP/ScreamDSP.h"

namespace vital {
  struct ScreamModule::Engines {
    // The voice handler sums both voices into both SIMD pairs, so lanes 2-3
    // repeat lanes 0-1: one engine processes lanes 0-1 and fills both pairs.
    ScreamDSP dsp;
    float retrigger = 0.0f;
    std::array<std::array<float, kMaxBufferSize * kMaxOversample>, 2> scratch {};
  };

  ScreamModule::ScreamModule(std::string prefix, const Output* midi_note) :
      SynthModule(0, 1), engines_(std::make_unique<Engines>()),
      prefix_(std::move(prefix)), midi_note_(midi_note) {
    prepare();
  }

  ScreamModule::~ScreamModule() = default;

  void ScreamModule::init() {
    const char* names[] = { "cutoff", "scream", "resonance", "input_gain", "wet",
                            "output_gain", "gate_release" };
    for (int i = 0; i < 7; ++i)
      controls_[i] = createMonoModControl(prefix_ + "scream_" + names[i]);
    autogain_ = createBaseControl(prefix_ + "scream_autogain");
    keytracking_ = createBaseControl(prefix_ + "scream_keytracking");
    SynthModule::init();
  }

  void ScreamModule::prepare() {
    engines_->dsp.prepare(getSampleRate());
  }

  void ScreamModule::processWithInput(const poly_float* input, int samples) {
    if (samples <= 0) return;
    juce::ScopedNoDenormals no_denormals;
    SynthModule::process(samples);
    float parameters[5];
    for (int i = 0; i < 5; ++i)
      parameters[i] = std::clamp(controls_[i]->buffer[0][0], 0.0f, 1.0f);
    const float gain = std::clamp(controls_[5]->buffer[0][0], 0.0f, 1.0f);
    const float release = std::clamp(controls_[6]->buffer[0][0], 5.0f, 500.0f);
    const float keytracking = keytracking_->value() && midi_note_ && midi_note_->buffer[0][0] >= 0
        ? midi_note_->buffer[0][0] - 24.0f : 0.0f;
    const float mod_amounts[5][2] {};
    const float pd_release = (float) Dynamics::convertCompressorTime(getSampleRate() * 0.015);
    const float pd_threshold = FastMath::fast_dB_to_gain(-54.0f);

    auto& dsp = engines_->dsp;
    auto& retrigger = engines_->retrigger;
    for (int offset = 0; offset < samples;) {
      const int count = std::min(kMaxBufferSize * kMaxOversample, samples - offset);
      float* channels[] = { engines_->scratch[0].data(), engines_->scratch[1].data() };
      for (int i = 0; i < count; ++i)
        for (int ch = 0; ch < 2; ++ch)
          channels[ch][i] = input[offset + i][ch];

      // Keep BP Scream's input-trigger block boundaries: even with no LFO
      // routes they affect parameter smoothing and state rounding. No LFO
      // generator, buffers, locks or controls are needed in Atlas.
      int start = 0;
      int remaining = count;
      while (remaining > 0) {
        int i;
        for (i = 0; i < remaining; ++i) {
          float average = 0.5f * (channels[0][start + i] + channels[1][start + i]);
          float previous = retrigger;
          float next = Dynamics::detectPeak(average, retrigger, 0.0f, pd_release);
          retrigger = next;
          if (previous < pd_threshold && next >= pd_threshold) break;
        }
        if (i > 0) {
          float* sub_channels[] = { channels[0] + start, channels[1] + start };
          dsp.processBlock(sub_channels, i, parameters, keytracking,
                           nullptr, nullptr, mod_amounts, 0, 0, release);
        }
        start += i;
        remaining -= i;
      }
      // BP Scream applies autogain AFTER the filter, including its dry mix.
      dsp.applyAutogain(channels, count, autogain_->value() != 0);
      dsp.applyOutputGain(channels, count, gain);
      for (int i = 0; i < count; ++i)
        output()->buffer[offset + i] = poly_float(channels[0][i], channels[1][i], channels[0][i], channels[1][i]);
      offset += count;
    }
  }

  void ScreamModule::hardReset() {
    // Reconstruct to clear the source ADSR as well (ScreamDSP::reset retains
    // its stage). Atlas bypass/re-enable starts a fresh effect instance.
    engines_->dsp = ScreamDSP {};
    engines_->retrigger = 0.0f;
    prepare();
  }

  void ScreamModule::enable(bool on) {
    if (enabled() != on) hardReset();
    SynthModule::enable(on);
  }

  void ScreamModule::setSampleRate(int rate) {
    const int previous = getSampleRate();
    SynthModule::setSampleRate(rate);
    if (getSampleRate() != previous) hardReset();
  }

  void ScreamModule::setOversampleAmount(int amount) {
    const int previous = getSampleRate();
    SynthModule::setOversampleAmount(amount);
    if (getSampleRate() != previous) hardReset();
  }
}
