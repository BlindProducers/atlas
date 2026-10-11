# Atlas's release dependencies

Everything Atlas builds against is in this folder, so no separate framework
or library checkout is needed. Each directory keeps its upstream licence and
copyright files; `NOTICE` at the repository root lists them all.

- `JUCE8/`: JUCE 8.0.14 with its CMake helpers and modules, used under the
  AGPLv3 (see `JUCE8/LICENSE.md`). Atlas is GPLv3, which the AGPL allows.
- `scream/Source`: the BP Scream DSP (MIT, Cure Audio / exacoustics / Xynth
  Audio) with the ADAA (BSD-3) and Polylogarithm (MIT) notices in
  `scream/LICENSE`. Only the DSP and what the regression test needs are kept.
- `json/`: nlohmann json (MIT).
- `concurrentqueue/`: moodycamel concurrent queue (simplified BSD).
- `kissfft/`: KISS FFT (BSD-3).
- `licenses/WolfShaper.LICENSE`: Atlas's Waveshaper adapts Wolf Shaper by
  Patrick Desaulniers (GPL v3 or later).
- `spectral_lock/`: Spectral Lock by aquanodemusic (AGPL-3.0,
  https://github.com/aquanodemusic/aquanode-plugins), kept for the regression
  oracle; the engine Atlas runs is extracted into
  `src/synthesis/effects/spectral_lock`. See `spectral_lock/README.atlas.md`.

The Suppressor and Spectral Compressor effects are Atlas's own code under
`src/synthesis/effects`. The optional JSFX oracle inputs are in
`tests/jsfx/fixtures`; the EEL2 interpreter they need is not vendored.
