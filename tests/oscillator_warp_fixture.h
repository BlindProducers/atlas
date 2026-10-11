// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace oscillator_warp_tests {
  template<class Oscillator> struct Fixture {
    vital::Wavetable table { vital::kNumOscillatorWaveFrames };
    std::unique_ptr<Oscillator> oscillator;
    std::array<vital::Value, Oscillator::kNumInputs> controls;
    vital::Output modulation;
    int time = 0;

    Fixture(int rate = 48000, int voices = 1, int active = 3) {
      table.setNumFrames(8);
      for (int f = 0; f < 8; ++f) {
        vital::WaveFrame frame;
        for (int i = 0; i < vital::WaveFrame::kWaveformSize; ++i) {
          const float phase = 2.0f * vital::kPi * i / vital::WaveFrame::kWaveformSize;
          frame.time_domain[i] = .5f * std::sin(phase) + .2f * std::cos(3 * phase + .2f * f)
                             + .1f * std::sin((7 + f) * phase) + .08f * std::cos(31 * phase)
                             + .3f * (2.0f * i / vital::WaveFrame::kWaveformSize - 1.0f);
        }
        frame.toFrequencyDomain();
        table.loadWaveFrame(&frame, f);
      }
      oscillator = std::make_unique<Oscillator>(&table);
      for (int i = 0; i < Oscillator::kNumInputs; ++i) oscillator->plug(&controls[i], i);
      oscillator->setSampleRate(rate);
      set(Oscillator::kMidiNote, 57);
      set(Oscillator::kMidiTrack, 1);
      set(Oscillator::kAmplitude, 1);
      set(Oscillator::kPhase, .5f);
      set(Oscillator::kDistortionPhase, .5f);
      set(Oscillator::kUnisonVoices, voices);
      set(Oscillator::kDetunePower, 1);
      set(Oscillator::kDetuneRange, 2);
      set(Oscillator::kUnisonDetune, .1f);
      set(Oscillator::kBlend, .8f);
      set(Oscillator::kStereoSpread, 1);
      set(Oscillator::kSpectralUnison, 1);
      set(Oscillator::kNotePressed, 1);
      controls[Oscillator::kActiveVoices].set(vital::poly_float(active & 1 ? 1 : 0,
          active & 1 ? 1 : 0, active & 2 ? 1 : 0, active & 2 ? 1 : 0));
      for (int position = 0; position < Oscillator::kNumModulatorPositions; ++position)
        oscillator->setModulatorOutput(position, &modulation);
    }
    void set(int input, float value) { controls[input].set(value); }
    void process(int block) {
      for (int i = 0; i < block; ++i) {
        const float t = float(time + i);
        modulation.buffer[i] = vital::poly_float(.6f * std::sin(t * .021f), .4f * std::cos(t * .018f),
                                                 .5f * std::cos(t * .027f), .7f * std::sin(t * .031f));
      }
      oscillator->process(block);
      time += block;
    }
    std::vector<float> render(int blocks = 48, int block = 128) {
      std::vector<float> result;
      result.reserve(blocks * block * vital::poly_float::kSize);
      for (int b = 0; b < blocks; ++b) {
        process(block);
        for (int i = 0; i < block; ++i)
          for (int lane = 0; lane < vital::poly_float::kSize; ++lane)
            result.push_back(oscillator->output(Oscillator::kRaw)->buffer[i][lane]);
      }
      return result;
    }
  };
}
