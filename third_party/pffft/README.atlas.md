# PFFFT in Atlas

`pffft.c` and `pffft.h` are PFFFT ("a Pretty Fast FFT") by Julien Pommier,
unchanged, as downloaded on 2026-10-14 from the author's repository,
https://bitbucket.org/jpommier/pffft (branch `master`):

- `pffft.c`, 68,201 bytes, SHA-256 485f2c641b9bc9434720757307e825c2f694b682438da9052959ab1e445e1f16
- `pffft.h`, 6,848 bytes, SHA-256 d6ac7f26f7c3f87ed2ad7f0264c09d72285526d937b4dccc5fc1c97645a0d55d

Licence: the FFTPACK licence (BSD style, UCAR/NCAR) that heads both files.
PFFFT adapts FFTPACKv4 by Paul Swarztrauber (NCAR) to SSE, NEON and Altivec.

Why: JUCE's `juce::dsp::FFT` has a fast engine only on Apple platforms (vDSP);
elsewhere it falls back to a scalar FFT that took about 25 microseconds per
2048-point real transform on an i5-14600K. Atlas runs every FFT on PFFFT on
every platform, so Windows gets SIMD and both platforms transform alike
(PFFFT has SSE code for x86 and NEON code for Apple silicon):

- `vital::FourierTransform` (`src/common/fourier_transform.h`), the
  wavetable oscillators' transform, which runs for every voice several times
  per audio block;
- every `juce::dsp::FFT` and `juce::dsp::Convolution` (the spectral effects,
  Phase Shift, Convolution Reverb, Glitch Delay, the Filter Table filter, the
  Spectral and Speech sources), through
  `../patches/juce8-pffft-fft-engine.patch`, which adds a PFFFT engine ahead
  of JUCE's others when `JUCE_DSP_USE_PFFFT` is set.

The CMake option `ATLAS_USE_PFFFT` (on by default) builds `pffft.c` and
defines `ATLAS_USE_PFFFT` and `JUCE_DSP_USE_PFFFT` for Atlas and every target
built against it. The Maim effect's codecs (LAME, BladeEnc, Opus) keep their
own FFTs, which are part of the codecs they emulate.

How it is used: real transforms, `pffft_transform_ordered`, with the bins
rearranged into the layout `juce::dsp::FFT` uses (bins 0 to N/2 as interleaved
real and imaginary parts, DC and Nyquist with zero imaginary parts), the
forward transform unscaled and the inverse scaled by 1/N, and the second half
of the buffer cleared after an inverse, as JUCE's vDSP engine does. The JUCE
engine also fills bins N/2+1 to N-1 with the mirrored conjugates after a
forward transform, as JUCE's fallback does, and has the complex transform
(scaled by 1/N when inverse). A real transform on a 16-byte-aligned buffer
runs in place with the buffer's second half as PFFFT's work area; otherwise,
and for `vital::FourierTransform`, scratch memory is taken on the stack per
call (the JUCE engine uses the heap above 256 KB, as JUCE's fallback does).
Either way one transform object can be used from several threads.

Results differ from JUCE's fallback by about one part in ten million.
`AtlasPreparedEffectTests --fft-cpu` times the transforms, and
`AtlasTimingTests --fourier-transform` checks the transforms.
