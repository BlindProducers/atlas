// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "spectral_source.h"

namespace oscillator_levels {
  template<class Source>
  void checkLevelTracking(const char* name, int rate, int block, float transpose) {
    Source source;
    std::array<vital::Value, Source::kNumInputs> controls;
    vital::Output level;
    for (int i = 0; i < Source::kNumInputs; ++i) source.plug(&controls[i], i);
    source.plug(&level, Source::kLevel);
    source.setSampleRate(rate);
    controls[Source::kNotePressed].set(1);
    if constexpr (std::is_same_v<Source, vital::NoiseSource>) {
      controls[Source::kHighCutoff].set(136);
      controls[Source::kLowCutoff].set(8);
    }
    else {
      std::vector<float> pcm(4096);
      for (int i = 0; i < static_cast<int>(pcm.size()); ++i)
        pcm[i] = .5f * std::sin(2 * juce::MathConstants<double>::pi * i / 64);
      source.getSample()->loadSample(pcm.data(), pcm.size(), rate);
      controls[Source::kMidi].set(60); controls[Source::kRootKey].set(60);
      controls[Source::kBinHigh].set(1); controls[Source::kSpectralStretch].set(1);
      controls[Source::kActiveVoices].set(1); controls[Source::kKeepFundamental].set(1);
      controls[Source::kPlayMode].set(Source::kLoop); controls[Source::kLoopEnd].set(1);
      controls[Source::kSpeed].set(1);
      // Spectral analysis is asynchronous; wait for the actual oscillator,
      // rather than accidentally measuring its old raw-PCM fallback.
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
      while (!source.isAnalysisReady() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      expect(source.isAnalysisReady(), "spectral level fixture analysis becomes ready");
    }
    for (int i = 0; i < block; ++i) level.buffer[i] = .5f;
    for (int i = 0; i < std::max(32, 4096 / block); ++i) source.process(block);

    // E4, E5, and E6 (MIDI 64/76/88), plus a different note per stereo pair.
    vital::TempoChooser chooser(true);
    std::array<vital::Value, vital::TempoChooser::kNumInputs> tempo_controls;
    for (int i = 0; i < vital::TempoChooser::kNumInputs; ++i) chooser.plug(&tempo_controls[i], i);
    tempo_controls[vital::TempoChooser::kSync].set(vital::TempoChooser::kKeytrack);
    tempo_controls[vital::TempoChooser::kKeytrackTranspose].set(transpose);
    tempo_controls[vital::TempoChooser::kKeytrackTune].set(-.6f); // -60 cents in the UI.
    LfoFixture lfo;
    lfo.lfo.setControlRate(false); lfo.lfo.setSampleRate(rate);
    lfo.lfo.plug(chooser.output(), vital::SynthLfo::kFrequency);
    const auto pan = vital::futils::panAmplitude(vital::poly_float(0));
    for (int note : {64, 76, 88}) {
      tempo_controls[vital::TempoChooser::kMidi].set(vital::poly_float(note, note, note + 7, note + 7));
      double energy = 0, error = 0;
      for (int b = 0; b < 32; ++b) {
        chooser.process(block); lfo.lfo.process(block);
        for (int i = 0; i < block; ++i)
          level.buffer[i] = vital::poly_float(.1f) + lfo.lfo.output()->buffer[i] * .8f;
        source.process(block);
        for (int i = 0; i < block; ++i) for (int lane = 0; lane < vital::poly_float::kSize; ++lane) {
          const float raw = source.output(Source::kRaw)->buffer[i][lane];
          const float expected = pan[lane] * level.buffer[i][lane] * level.buffer[i][lane] * raw;
          const float actual = source.output(Source::kLevelled)->buffer[i][lane];
          energy += double(expected) * expected;
          error += double(actual - expected) * (actual - expected);
        }
      }
      const double relative_error = std::sqrt(error / std::max(1e-30, energy));
      expect(energy > 1e-5, std::string(name) + " fixture must render nonzero audio");
      expect(relative_error < 1e-5, std::string(name) + " volume must follow every keytracked LFO sample");
      std::cout << name << " rate=" << rate << " block=" << block << " note=" << note
                << " transpose=" << transpose << " lfo_hz=" << chooser.output()->buffer[0][0]
                << " relative_gain_error=" << relative_error << '\n';
    }
  }
  int run() {
    for (int rate : {44100, 48000, 96000}) for (int block : {1, 17, 128}) {
      checkLevelTracking<vital::NoiseSource>("Noise", rate, block, -12);
      checkLevelTracking<vital::SpectralSource>("Spectral", rate, block, -12);
    }
    checkLevelTracking<vital::NoiseSource>("Noise", 48000, 128, -60);
    return failures ? 1 : 0;
  }
}
