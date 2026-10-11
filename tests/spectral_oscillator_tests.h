// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "spectral_module.h"
#include "slot_test_ids.h"

namespace spectral_oscillator_tests {
  using namespace vital;
  constexpr double twoPi = juce::MathConstants<double>::twoPi;

  struct Fixture {
    SpectralSource source;
    std::array<cr::Value, SpectralSource::kNumInputs> controls;
    cr::Output reset, retrigger;
    int rate;
    Fixture(int output_rate = 48000, int input_rate = 48000, double hz = 437.3) : rate(output_rate) {
      for (int i = 0; i < SpectralSource::kNumInputs; ++i) source.plug(&controls[i], i);
      source.plug(&reset, SpectralSource::kReset);
      source.plug(&retrigger, SpectralSource::kRetrigger);
      source.setSampleRate(rate);
      set(SpectralSource::kMidi, 60); set(SpectralSource::kRootKey, 60);
      set(SpectralSource::kKeytrack, 1); set(SpectralSource::kLevel, 1);
      set(SpectralSource::kNotePressed, 60); set(SpectralSource::kActiveVoices, 1);
      set(SpectralSource::kBinHigh, 1); set(SpectralSource::kSpectralStretch, 1);
      set(SpectralSource::kPosition, .4f); set(SpectralSource::kSpeed, 1);
      set(SpectralSource::kLoopEnd, 1);
      std::vector<float> pcm(input_rate / 4);
      for (int i = 0; i < int(pcm.size()); ++i) pcm[i] = .5 * std::sin(twoPi * hz * i / input_rate + .31);
      source.getSample()->loadSample(pcm.data(), int(pcm.size()), input_rate);
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
      while (!source.isAnalysisReady() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      expect(source.isAnalysisReady(), "spectral analysis must become ready");
      trigger();
    }
    void set(int index, poly_float value) { controls[index].set(value); }
    void trigger(int offset = 0, poly_mask mask = constants::kFullMask) {
      reset.trigger(mask, kVoiceOn, offset);
    }
    std::vector<poly_float> render(int samples, int block = 128, bool tune_envelope = false) {
      Output tune;
      if (tune_envelope) source.plug(&tune, SpectralSource::kTune);
      std::vector<poly_float> audio;
      audio.reserve(samples);
      for (int at = 0; at < samples;) {
        const int count = std::min(block, samples - at);
        if (tune_envelope) for (int i = 0; i < count; ++i)
          tune.buffer[i] = 12.0f * std::exp(-float(at + i) / 2400.0f);
        tracked_allocations = 0; track_allocations = true;
        source.process(count);
        track_allocations = false;
        expect(tracked_allocations == 0, "spectral rendering must not allocate");
        for (int i = 0; i < count; ++i) audio.push_back(source.output(SpectralSource::kRaw)->buffer[i]);
        reset.clearTrigger(); retrigger.clearTrigger();
        at += count;
      }
      if (tune_envelope) source.plug(&controls[SpectralSource::kTune], SpectralSource::kTune);
      return audio;
    }
  };

  // Least-squares sinusoid fit: detects detuning, sidebands and amplitude flutter
  // without prescribing the oscillator's absolute starting phase.
  double toneResidual(const std::vector<poly_float>& audio, double hz, int rate, int lane = 0) {
    double ss = 0, cc = 0, sc = 0, xs = 0, xc = 0, xx = 0;
    for (int i = 4096; i < int(audio.size()); ++i) {
      const double s = std::sin(twoPi * hz * i / rate), c = std::cos(twoPi * hz * i / rate), x = audio[i][lane];
      ss += s*s; cc += c*c; sc += s*c; xs += x*s; xc += x*c; xx += x*x;
    }
    const double a = (xs*cc - xc*sc) / (ss*cc - sc*sc), b = (xc*ss - xs*sc) / (ss*cc - sc*sc);
    expect(xx > 1, "spectral oscillator must produce audible tone energy");
    return std::sqrt(std::max(0.0, xx - a*xs - b*xc) / std::max(1e-30, xx));
  }

  void testTones() {
    for (int mode : {SpectralSource::kVocoder, SpectralSource::kLocked, SpectralSource::kPeakLocked})
      for (int rate : {44100, 48000, 96000}) for (float transpose : {0.0f, 7.0f}) {
        Fixture f(rate);
        f.set(SpectralSource::kPhaseMode, mode); f.set(SpectralSource::kTranspose, transpose);
        const auto audio = f.render(rate / 2);
        const double residual = toneResidual(audio, 437.3 * std::pow(2.0, transpose / 12.0), rate);
        std::cout << "spectral mode=" << mode << " rate=" << rate << " transpose=" << transpose
                  << " tone_residual=" << residual << '\n';
        expect(residual < .025, "spectral pure tones must retain pitch without metallic sidebands");
      }
  }

  void testBlocksAndRetriggers() {
    Fixture reference;
    const auto expected = reference.render(12000, 128, true);
    for (int block : {1, 17, 64}) {
      Fixture f;
      const auto actual = f.render(12000, block, true);
      float error = 0;
      for (int i = 0; i < int(actual.size()); ++i)
        error = std::max(error, std::abs(actual[i][0] - expected[i][0]));
      expect(error < 2e-5f, "pitch envelope must not depend on audio block size");
    }
    Fixture f;
    const auto first = f.render(8192);
    f.trigger(37);
    const auto second = f.render(8192 + 37);
    float error = 0;
    for (int i = 0; i < 8192; ++i) error = std::max(error, std::abs(first[i][0] - second[i + 37][0]));
    expect(error < 2e-5f, "repeated notes must restart identically at the actual MIDI offset");
  }

  void testVoiceControls() {
    for (int control : {SpectralSource::kBinLow, SpectralSource::kBinHigh, SpectralSource::kSpectralShift,
                        SpectralSource::kSpectralStretch, SpectralSource::kSpectralBlur,
                        SpectralSource::kSpectralTilt, SpectralSource::kEffectAmount}) {
      Fixture pair, isolated;
      const float base = control == SpectralSource::kBinHigh || control == SpectralSource::kSpectralStretch ? 1 : 0;
      const float value = control == SpectralSource::kBinHigh ? .005f :
                          control == SpectralSource::kSpectralStretch ? 1.1f : .25f;
      pair.set(control, poly_float(base, base, value, value)); isolated.set(control, value);
      pair.set(SpectralSource::kSpectralEffect, SpectralSource::kCompress);
      isolated.set(SpectralSource::kSpectralEffect, SpectralSource::kCompress);
      const auto a = pair.render(8192), b = isolated.render(8192);
      float error = 0;
      for (int i = 2048; i < 8192; ++i) error = std::max(error, std::abs(a[i][2] - b[i][0]));
      expect(error < 2e-5f, "each spectral voice must use its own control " + std::to_string(control));
    }
    Fixture zeroNote;
    zeroNote.set(SpectralSource::kNotePressed, 0);
    const auto audio = zeroNote.render(8192);
    double energy = 0;
    for (const auto& x : audio) energy += x[0]*x[0];
    expect(energy > 1, "MIDI note zero must not be interpreted as a closed gate");
  }

  void testAnalysisAndModuleDefaults() {
    SpectralAnalysis analysis;
    std::vector<float> pcm(8192);
    for (int i = 0; i < int(pcm.size()); ++i) pcm[i] = .5 * std::sin(twoPi * 437.3 * i / 48000 + .31);
    expect(analysis.analyzeChannel(pcm.data(), int(pcm.size()), 48000), "tone analysis succeeds");
    std::array<float, SpectralAnalysis::kNumBins> mag, freq;
    analysis.readFrameInterpolated(0, mag.data(), freq.data());
    const int peak = int(std::max_element(mag.begin(), mag.end()) - mag.begin());
    expectClose(freq[peak] * 48000 / float(twoPi), 437.3f, .02f, "first frame must have a valid pitch estimate");
    struct Module : SpectralModule { float stretch() { return spectral_->input(SpectralSource::kSpectralStretch)->at(0)[0]; } };
    Module module;
    module.init();
    module.process(128);
    expectClose(module.stretch(), 1, 1e-6f, "default spectral stretch must reach the DSP as unity");
  }

  void awaitAnalysis(SpectralSource& source) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!source.isAnalysisReady() && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    expect(source.isAnalysisReady(), "replacement sample analysis becomes ready");
  }

  void testControlContinuityAndPitch() {
    Fixture closing;
    const float edge = std::log2(1.0f + 437.3f * SpectralAnalysis::kFftSize / 48000.0f) / 10.0f;
    closing.set(SpectralSource::kBinLow, edge);
    closing.set(SpectralSource::kBinHigh, edge + 1e-5f);
    double closing_energy = 0;
    for (const auto& x : closing.render(8192)) closing_energy += x[0]*x[0];
    expect(closing_energy < 1e-8, "bin edges must approach silence continuously as the band closes");
    for (int rate : {44100, 48000, 96000, 192000}) {
      Fixture f(rate);
      f.set(SpectralSource::kSpectralShift, .01f);
      const auto audio = f.render(rate / 2);
      expect(toneResidual(audio, 437.9, rate) < .025,
             "one percent shift must be a gentle 0.6 Hz at every processing rate");
    }
    for (int parameter : {SpectralSource::kBinLow, SpectralSource::kBinHigh}) {
      // Sweep through a partial and across individual bin boundaries. The
      // resulting RMS must change continuously, not switch integer bins.
      double previous = -1;
      for (int step = 0; step <= 20; ++step) {
        Fixture f;
        f.set(parameter, .417f + step * .0002f);
        const auto audio = f.render(8192);
        double energy = 0;
        for (int i = 4096; i < 8192; ++i) energy += audio[i][0] * audio[i][0];
        const double rms = std::sqrt(energy / 4096);
        if (previous >= 0) expect(std::abs(rms - previous) < .015,
                                  "fractional bin edges must not jump at FFT bin boundaries");
        previous = rms;
      }
    }
    for (bool global : {false, true}) {
      Fixture f;
      f.set(SpectralSource::kMidi, 61.4f);
      f.set(SpectralSource::kTransposeQuantizeScale, 2);
      f.set(SpectralSource::kTransposeQuantizeMode, global ? 1 : 0);
      const auto audio = f.render(24000);
      const double hz = 437.3 * std::pow(2.0, (global ? 2.0 : 1.4) / 12.0);
      expect(toneResidual(audio, hz, 48000) < .025, "local/global transpose quantization must keep the selected scale");
    }
    Fixture custom;
    custom.set(SpectralSource::kTransposeQuantizeScale, 14);
    custom.set(SpectralSource::kTransposeQuantize, 1);
    custom.set(SpectralSource::kTranspose, 7.2f);
    expect(toneResidual(custom.render(24000), 874.6, 48000) < .025,
           "custom transpose mask must work in local mode");

    Fixture fundamental;
    fundamental.set(SpectralSource::kRootKey, 69); fundamental.set(SpectralSource::kMidi, 69);
    fundamental.set(SpectralSource::kKeepFundamental, 1);
    fundamental.set(SpectralSource::kSpectralShift, .2f);
    fundamental.set(SpectralSource::kSpectralStretch, 1.5f);
    expect(toneResidual(fundamental.render(24000), 437.3, 48000) < .025,
           "Keep Fundamental must retain the recorded partial rather than invent a 440 Hz sine");
  }

  void testSoftRetriggerAndLaneIsolation() {
    Fixture retriggered, reference;
    const auto first = retriggered.render(8192);
    reference.render(8192);
    retriggered.retrigger.trigger(constants::kFirstMask, kVoiceOn, 37);
    const auto actual = retriggered.render(8192 + 37), continued = reference.render(8192 + 37);
    float before = 0, restarted = 0, other_voice = 0;
    for (int i = 0; i < int(actual.size()); ++i) {
      if (i < 37) before = std::max(before, std::abs(actual[i][0] - continued[i][0]));
      if (i > 4096 + 37) restarted = std::max(restarted, std::abs(actual[i][0] - first[i - 37][0]));
      other_voice = std::max(other_voice, std::abs(actual[i][2] - continued[i][2]));
    }
    expect(before < 1e-6 && other_voice < 1e-6, "mono retrigger must preserve pre-event audio and unrelated voices");
    expect(restarted < 2e-5, "reused voice must restart the same spectral phase after its short handoff");
  }

  void testTransientsAndTransport() {
    std::vector<float> pcm(12000);
    for (int i = 0; i < int(pcm.size()); ++i)
      pcm[i] = (i < 4096 ? .015f : .5f) * std::sin(twoPi * 437.3 * i / 48000);
    SpectralAnalysis analysis;
    analysis.analyzeChannel(pcm.data(), int(pcm.size()), 48000);
    std::array<float, SpectralAnalysis::kNumBins> mag, freq;
    int transient_index = -1;
    for (int i = 0; i < analysis.numFrames(); ++i) {
      bool transient = false;
      analysis.readFrameInterpolated(i, mag.data(), freq.data(), nullptr, &transient);
      if (transient) { transient_index = i; break; }
    }
    expect(transient_index >= 0, "transient fixture must contain a detected onset");
    for (int mode : {SpectralSource::kVocoder, SpectralSource::kLocked, SpectralSource::kPeakLocked}) {
      Fixture a, b;
      for (Fixture* f : {&a, &b}) {
        f->source.getSample()->loadSample(pcm.data(), int(pcm.size()), 48000);
        awaitAnalysis(f->source);
        f->set(SpectralSource::kPosition, float(transient_index) / (analysis.numFrames() - 1));
        f->set(SpectralSource::kFreeze, 1); f->set(SpectralSource::kPhaseMode, mode);
      }
      b.set(SpectralSource::kPreserveTransients, 1);
      const auto x = a.render(12000), y = b.render(12000);
      float error = 0;
      for (int i = 0; i < 12000; ++i) error = std::max(error, std::abs(x[i][0] - y[i][0]));
      expect(error < 1e-6, "a frozen transient must not repeatedly reset phase in any phase mode");
    }
    // Stationary speed=0 uses the chosen position even with Freeze enabled;
    // Reverse's start offset is measured from the end of the sample.
    for (int mode : {SpectralSource::kForward, SpectralSource::kReverse, SpectralSource::kLoop, SpectralSource::kBounce}) {
      Fixture manual, playing;
      playing.set(SpectralSource::kPlayMode, mode); playing.set(SpectralSource::kSpeed, 0);
      if (mode == SpectralSource::kReverse) playing.set(SpectralSource::kPosition, .6f);
      manual.set(SpectralSource::kFreeze, 1); playing.set(SpectralSource::kFreeze, 1);
      const auto a = manual.render(8192), b = playing.render(8192);
      float error = 0;
      for (int i = 0; i < 8192; ++i) error = std::max(error, std::abs(a[i][0] - b[i][0]));
      expect(error < 2e-5, "playback starts at Position before advancing or freezing");
    }
  }

  void testLoadingAndBounds() {
    Fixture f;
    std::vector<float> pcm(12000);
    for (int request = 0; request < 8; ++request) {
      const double hz = request == 7 ? 733.2 : 220.0 + request * 53;
      for (int i = 0; i < int(pcm.size()); ++i) pcm[i] = .5 * std::sin(twoPi * hz * i / 48000);
      f.source.getSample()->loadSample(pcm.data(), int(pcm.size()), 48000);
    }
    awaitAnalysis(f.source);
    expect(toneResidual(f.render(24000), 733.2, 48000) < .025,
           "rapid equal-length sample replacements must publish the final sample only");
    for (int length : {4, 63, 2047, 2048, 2049}) {
      std::vector<float> short_pcm(length, .25f);
      short_pcm[length / 2] = -.25f;
      f.source.getSample()->loadSample(short_pcm.data(), length, 48000);
      awaitAnalysis(f.source);
      for (int mode : {SpectralSource::kLoop, SpectralSource::kBounce}) {
        f.set(SpectralSource::kPlayMode, mode);
        f.set(SpectralSource::kLoopStart, 1); f.set(SpectralSource::kLoopEnd, 1);
        f.set(SpectralSource::kSpeed, -2);
        f.trigger();
        for (const auto& x : f.render(4096)) for (int lane = 0; lane < poly_float::kSize; ++lane)
          expect(std::isfinite(x[lane]) && std::abs(x[lane]) <= 4, "short samples and zero-width loops remain finite");
      }
    }
    f.set(SpectralSource::kSpectralShift, std::numeric_limits<float>::quiet_NaN());
    f.set(SpectralSource::kTune, std::numeric_limits<float>::infinity());
    f.set(SpectralSource::kLevel, std::numeric_limits<float>::quiet_NaN());
    for (const auto& x : f.render(4096)) for (int lane = 0; lane < poly_float::kSize; ++lane)
      expect(std::isfinite(x[lane]), "nonfinite modulation must not escape the spectral oscillator");
  }

  void testPluginMonoPitchEnvelope() {
    for (int quality : {0, 2}) {
      SynthPlugin plugin;
      plugin.setPlayConfigDetails(0, 2, 48000, 128);
      plugin.prepareToPlay(48000, 128);
      auto set = [&](const std::string& id, float v) { plugin.getControls().at(id)->set(v); };
      set("oversampling", quality); plugin.checkOversampling();
      set("polyphony", 1); set("legato", 0); set("velocity_track", 0);
      for (int i = 1; i <= 4; ++i) set("osc_" + std::to_string(i) + "_on", 0);
      set(slot_ids::id("sample_on"), 0); set(slot_ids::id("granular_on"), 0); set(slot_ids::id("noise_on"), 0);
      set(slot_ids::id("spectral_on"), 1); set(slot_ids::id("spectral_keytrack"), 0); set(slot_ids::id("spectral_position"), .4f);
      set(slot_ids::id("spectral_level"), .7f); set(slot_ids::id("spectral_destination"), constants::kEffects);
      for (int env : {1, 2}) {
        const std::string prefix = "env_" + std::to_string(env) + "_";
        set(prefix + "attack", std::pow(env == 1 ? .003f : .01f, .25f));
        set(prefix + "decay", std::pow(.04f, .25f));
        set(prefix + "sustain", env == 1 ? 1.0f : 0.0f);
        set(prefix + "release", std::pow(.003f, .25f));
      }
      expect(plugin.connectModulation("env_2", slot_ids::id("spectral_tune")), "Env 2 must connect to Spectral Tune");
      set("modulation_1_amount", 1.0f);
      std::vector<float> pcm(12000);
      for (int i = 0; i < int(pcm.size()); ++i) pcm[i] = .5 * std::sin(twoPi * 437.3 * i / 48000);
      plugin.getEngine()->getSample(slot_ids::kSpectralSlot)->loadSample(pcm.data(), int(pcm.size()), 48000);
      auto render = [&](int samples, int on_at = -1, int off_at = -1) {
        std::vector<float> audio;
        for (int at = 0; at < samples; at += 128) {
          const int count = std::min(128, samples - at);
          juce::AudioBuffer<float> block(2, count); block.clear();
          juce::MidiBuffer midi;
          if (on_at >= at && on_at < at + count) midi.addEvent(juce::MidiMessage::noteOn(1, 60, .8f), on_at - at);
          if (off_at >= at && off_at < at + count) midi.addEvent(juce::MidiMessage::noteOff(1, 60), off_at - at);
          plugin.processBlock(block, midi);
          for (int i = 0; i < count; ++i) audio.push_back(block.getSample(0, i));
        }
        return audio;
      };
      // Wait for analysis by observing audio, without relying on thread timing.
      render(48000); // Let freshly edited preset controls finish their smoothing.
      auto warm = render(128, 0);
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
      bool audible = false;
      while (!audible && std::chrono::steady_clock::now() < deadline) {
        warm = render(2048);
        for (float x : warm) audible |= std::abs(x) > .001f;
        if (!audible) std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      expect(audible, "complete plugin must render spectral audio after analysis");
      render(8192, -1, 0);
      const auto reference = render(8192, 37);
      render(8192, -1, 0);
      for (int note = 0; note < 4; ++note) {
        const auto actual = render(8192, 37);
        float error = 0;
        for (int i = 0; i < 8192; ++i) error = std::max(error, std::abs(actual[i] - reference[i]));
        std::cout << "spectral repeat quality=" << quality << " note=" << note << " difference=" << error << " first=" << actual[3000] << " reference=" << reference[3000] << "\n";
        expect(error < 1e-4, "mono notes with Env 2 -> Tune must repeat identically across physical voice lanes");
        render(8192, -1, 0);
      }
      set("legato", 1); render(128);
      const auto held = render(16384, 0);
      render(8192, -1, 0);
      auto legato = render(8192, 0);
      const auto continued = render(8192, 37);
      legato.insert(legato.end(), continued.begin(), continued.end());
      float error = 0;
      for (int i = 0; i < 16384; ++i) error = std::max(error, std::abs(legato[i] - held[i]));
      expect(error < 1e-4, "same-pitch mono legato must keep spectral playback and tune envelope continuous");
      std::cout << "spectral plugin quality=" << quality << " legato_difference=" << error << '\n';
      set(slot_ids::id("spectral_on"), 0);
      const auto off = render(4096);
      double energy = 0;
      for (int i = 2048; i < 4096; ++i) energy += off[i]*off[i];
      expect(energy < 1e-12, "disabling spectral must clear every cloned voice output");
    }
  }

  int run() {
    testAnalysisAndModuleDefaults(); testTones(); testBlocksAndRetriggers(); testVoiceControls();
    testControlContinuityAndPitch(); testSoftRetriggerAndLaneIsolation();
    testTransientsAndTransport(); testLoadingAndBounds(); testPluginMonoPitchEnvelope();
    return failures ? 1 : 0;
  }
}
