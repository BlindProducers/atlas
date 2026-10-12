# Atlas's dependencies

Most of what Atlas builds against is a git submodule, pinned to the exact
upstream commit that Atlas was validated with. Fetch them with
`git clone --recurse-submodules`, or `git submodule update --init` in an
existing clone. `NOTICE` at the repository root lists every licence.

Fetched by default, needed to build Atlas:

- `JUCE8/`: JUCE 8.0.14 (https://github.com/juce-framework/JUCE), used under
  the AGPLv3 (see `JUCE8/LICENSE.md`). Atlas is GPLv3, which the AGPL allows.
- `nlohmann_json/`: nlohmann json 3.1.2 (MIT), from the release-tracking
  repository https://github.com/ArthurSonzogni/nlohmann_json_cmake_fetchcontent,
  which carries only the released header (`include/nlohmann/json.hpp`, the
  same file as the official release's single include).
- `concurrentqueue/`: moodycamel concurrent queue (simplified BSD),
  https://github.com/cameron314/concurrentqueue at 0bc97ad (June 2016).
- `kissfft/`: KISS FFT (BSD-3), https://github.com/mborgerding/kissfft at
  760cb7e (October 2018). Atlas uses its header-only C++ class, `kissfft.hh`.

Opt-in, only for reference tests. `git submodule update --init` skips these;
fetch one with `git submodule update --init --checkout <path>`, and CMake uses
it when `ATLAS_BUILD_TIMING_TESTS` is on:

- `aquanode-plugins/`: Spectral Lock by aquanodemusic (AGPL-3.0),
  https://github.com/aquanodemusic/aquanode-plugins. Its original processor in
  `spectral/spectrallock/src` is the oracle for the Spectral Lock tests; see
  `src/synthesis/effects/spectral_lock/README.atlas.md`.
- `WDL/`: Cockos WDL (zlib licence), https://github.com/justinfrankel/WDL. Its
  EEL2 runs the original JSFX for the JSFX, Reverb filter, Trance gate and
  Resonator reference tests.
- `nih-plug/`: NIH-plug (ISC) with Diopser (GPL v3 or later),
  https://github.com/robbert-vdh/nih-plug at the revision recorded in
  `tests/defuse/source_sha256.json`. The Defuse reference test compiles Diopser
  with `rustc` and Python.
- `openwurli/` and `melange/`: OpenWurli 0.9.0 (GPL v3 or later),
  https://github.com/hal0zer0/openwurli at 531680b, and the Melange revision it
  pins for its `melange-primitives` crate (GPL v3 or later),
  https://github.com/hal0zer0/melange at 7ecb36c. The OpenWurli reference tests
  compile the original `openwurli-dsp` with `rustc` and Python
  (`tests/openwurli/build_reference.py`); see
  `src/synthesis/effects/openwurli/README.atlas.md`.

Kept in this repository:

- `scream/Source`: the BP Scream DSP (MIT, Cure Audio / exacoustics / Xynth
  Audio) with the ADAA (BSD-3) and Polylogarithm (MIT) notices in
  `scream/LICENSE`. This JUCE version of the DSP is not published upstream (the
  released Scream at https://github.com/Cure-Audio/Scream is a separate C
  codebase), so it stays here. Only the DSP and what the regression test needs
  are kept.
- `licenses/WolfShaper.LICENSE`: Atlas's Waveshaper adapts Wolf Shaper by
  Patrick Desaulniers (GPL v3 or later).

The Suppressor and Spectral Compressor effects are Atlas's own code under
`src/synthesis/effects`. The JSFX oracle inputs are in `tests/jsfx/fixtures`.
