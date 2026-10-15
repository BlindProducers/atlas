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
2048-point real transform on an i5-14600K. The wavetable oscillators run that
transform for every voice several times per audio block, about a third of a
wavetable voice's CPU on Windows. `vital::FourierTransform`
(`src/common/fourier_transform.h`) uses PFFFT when the CMake option
`ATLAS_USE_PFFFT` is on, which it is by default on Windows. Configuring with
`-DATLAS_USE_PFFFT=ON` on macOS puts both platforms on the same FFT (PFFFT has
NEON code for Apple silicon).

How it is used: real transforms, `pffft_transform_ordered`, with the bins
rearranged into the layout `juce::dsp::FFT` uses (bins 0 to N/2 as interleaved
real and imaginary parts, DC and Nyquist with zero imaginary parts), the
forward transform unscaled and the inverse scaled by 1/N, and the second half
of the buffer cleared after an inverse, as JUCE's vDSP engine does. Scratch
memory is taken on the stack per call, so one transform object can be used
from several threads, as `vital::FFT<bits>::transform()` is.

Results differ from JUCE's fallback by about one part in ten million.
`AtlasPreparedEffectTests --fft-cpu` times the transforms, and
`AtlasTimingTests --fourier-transform` checks PFFFT against
`juce::dsp::FFT`.
