// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "synth_module.h"

namespace vital {
  class DigitalSvf;

  // The Distortion effect as a voice filter model. Each note is distorted on
  // its own, so a chord gains no combination tones between its notes. Style is
  // the effect's type, Drive its drive, and Cutoff, Resonance and Blend set its
  // filter, which Filter Order puts before or after the shaper. The dry/wet
  // balance comes from FilterModule's Mix.
  //
  // The effect distorts notes after their amp envelope, while a voice filter's
  // output is enveloped afterwards. So the model works on the input times the
  // voice amplitude and divides it back out: a held note distorts as it does
  // in the effect chain, and a fading one cleans up the same way.
  class DistortionFilter : public SynthModule {
    public:
      enum { kAudio, kReset, kStyle, kDrive, kFilterOrder, kMidiCutoff, kResonance, kBlend, kAmplitude, kNumInputs };
      enum FilterOrder { kNoFilter, kPreFilter, kPostFilter, kNumFilterOrders };

      // Voice amplitudes below this count as this, so scaling back never divides
      // by zero. The envelope that follows silences such notes anyway.
      static constexpr mono_float kMinAmplitude = 1e-6f;

      DistortionFilter();
      virtual ~DistortionFilter() { }

      void init() override;
      void process(int num_samples) override;
      void reset(poly_mask reset_mask) override;
      void hardReset() override;
      Processor* clone() const override { return new DistortionFilter(*this); }

    private:
      template<poly_float(*distort)(poly_float, poly_float), poly_float(*scale)(poly_float)>
      void shape(poly_float* audio, int num_samples);
      void downSample(poly_float* audio, int num_samples);

      DigitalSvf* tone_filter_;
      int type_;
      // Down Sample, per lane: pairs of samples since the last capture, and the held value.
      poly_float hold_pairs_;
      poly_float held_;

      JUCE_LEAK_DETECTOR(DistortionFilter)
  };
} // namespace vital
