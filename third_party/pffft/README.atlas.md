# PFFFT in Atlas

`pffft.c` and `pffft.h` are PFFFT ("a Pretty Fast FFT") by Julien Pommier,
unchanged, as downloaded on 2026-10-14 from the author's repository,
https://bitbucket.org/jpommier/pffft (branch `master`):

- `pffft.c`, 68,201 bytes, SHA-256 485f2c641b9bc9434720757307e825c2f694b682438da9052959ab1e445e1f16
- `pffft.h`, 6,848 bytes, SHA-256 d6ac7f26f7c3f87ed2ad7f0264c09d72285526d937b4dccc5fc1c97645a0d55d

Licence: the FFTPACK licence (BSD style, UCAR/NCAR) that heads both files.
PFFFT adapts FFTPACKv4 by Paul Swarztrauber (NCAR) to SSE, NEON and Altivec.

Why: every FFT in Atlas runs on PFFFT, on macOS and Windows, so both
platforms run the same transform code. JUCE's `juce::dsp::FFT` picks its engine
per platform: vDSP on Apple platforms and a scalar fallback elsewhere, which
took about 25 microseconds per 2048-point real transform on an i5-14600K.
PFFFT has SSE code for x64 and NEON code for arm64 (Apple silicon).

How it is used: through `atlas::Fft` (real) and `atlas::ComplexFft`
(`src/common/atlas_fft.h`), which keep `juce::dsp::FFT`'s layout and scaling,
as the spectral code was written against it:

- real transforms use `pffft_transform_ordered` with the bins rearranged into
  JUCE's layout (bins 0 to N/2 as interleaved real and imaginary parts, DC and
  Nyquist with zero imaginary parts, the negative frequencies mirrored unless
  only the non-negative ones are asked for), the forward transform unscaled and
  the inverse scaled by 1/N, with the buffer's second half cleared after an
  inverse, as JUCE's vDSP engine leaves it;
- complex transforms are interleaved, the inverse scaled by 1/N;
- PFFFT wants 16-byte aligned buffers. An unaligned real buffer is shifted into
  alignment within its spare upper half and back; an unaligned complex buffer
  goes through an aligned copy. The spectral effects' buffers are declared
  `alignas(16)` so this never happens on the audio thread;
- every transform of one size shares PFFFT's read-only setup. Each `atlas::Fft`
  keeps its own scratch, so one object serves one thread at a time;
  `vital::FFT<bits>`, the process-wide transform the wavetable code uses off
  the audio thread, takes a lock around each call.

Users: the oscillators' spectral morphs and wavetable frames
(`vital::FourierTransform`, `src/common/fourier_transform.h`), the Spectral
oscillator's analysis and resynthesis, Spectral Gate, Spectral Compressor and
Suppressor (`linked_stft.h`), Spectral Balance, Spectral Lock's retune engine,
Phase Shift, Glitch Delay's formant shifter, and the Filter Table kernel design
and table filter. Convolution Reverb is the exception: it keeps
`juce::dsp::Convolution`, which runs JUCE's own FFT inside.

`pffft.c` is compiled as C into the Atlas target, outside the fast-math
synthesis unity build. Results differ from `juce::dsp::FFT` by about one part
in a million (`AtlasFftTests` checks 2e-6 relative to the peak, sizes 32 to
32768). `AtlasFftTests --benchmark` and `AtlasPreparedEffectTests --fft-cpu`
time PFFFT against `juce::dsp::FFT`, and `AtlasTimingTests --fourier-transform`
checks `vital::FourierTransform` against `juce::dsp::FFT`.
