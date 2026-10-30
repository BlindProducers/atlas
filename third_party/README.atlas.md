# Atlas's dependencies

Most of what Atlas builds against is a git submodule, pinned to the exact
upstream commit that Atlas was validated with. Fetch them with
`git clone --recurse-submodules`, or `git submodule update --init` in an
existing clone. `NOTICE` at the repository root lists every licence.

Fetched by default, needed to build Atlas:

- `JUCE8/`: JUCE 8.0.14 (https://github.com/juce-framework/JUCE), used under
  the AGPLv3 (see `JUCE8/LICENSE.md`). Atlas is GPLv3, which the AGPL allows.
  CMake applies the patches in `patches/juce8-*.patch` to the submodule's
  working tree when it configures (`.gitmodules` ignores the changes they
  leave there):
  - `juce8-colour-id-cache.patch`: a component's colours are properties
    named `jcclr_<id>`, and every `setColour`, `findColour` and
    `isColourSpecified` made that name into an `Identifier`, which searches
    the process's whole string pool under its lock. Building a slider or a
    combo box asks for dozens, so once the editor's own string work was
    trimmed this was the largest single cost of building Atlas's editor
    (about 5,000 components). Each colour's `Identifier` is now made once
    and kept, which took building the editor on Windows from 75 to 60 ms.
  - `juce8-mac-idle-vblank.patch`: on macOS JUCE 8 wakes the message thread
    on every display refresh (60 or 120 times a second) for as long as a
    window is open, even when nothing needs drawing. The patch skips that
    wake-up while the window has no pending repaint and no `VBlankListener`;
    a repaint or a new listener starts it again. With Atlas's editor open
    and idle this saved about two points of one core.
  - `juce8-windows-idle-vblank.patch`: the same on Windows, where a thread
    per monitor waits for each vblank and posts it to the message thread
    (a plugin's message thread is the host's own) for as long as a window
    is open. Each window now tells the vblank dispatcher when it starts and
    stops waiting for one (a Direct2D paint scheduled for the next vblank,
    software repaints not yet flushed, or a `VBlankListener`), and the
    threads sleep on an event while none waits. It builds on the
    `vBlankListenerAdded` hook the macOS patch adds to `ComponentPeer`, so
    it is applied after it. On a 200 Hz display it took the message
    thread's share of a core with Atlas's editor open and idle from about
    1.2 to 1.0 percent, and focused from 1.5 to 1.2.
    `AtlasMacroTests --window-repaint` checks that an idle window still
    paints every repaint at once.
  - `juce8-uia-text-reading.patch`: Windows screen readers read a text field
    through UI Automation text ranges. JUCE's `Move` worked out where a range
    should go but left it where it was, and gave a backward move a positive
    count, so NVDA, which works out what Backspace deletes by moving a copy
    of its caret back one character, read the character after the caret (the
    same letter for every Backspace) or "blank" at the end of the text. `Move`
    now moves as UI Automation defines it, and counts backwards as negative,
    as `MoveEndpointByUnit` does too. NVDA also takes a selection's start as
    its caret, so with text selected it would read the letter before the
    selection; while that selection is there, the copy NVDA moves back from it
    expands to the selection instead, and NVDA says the highlighted letter, or
    the whole value, that Backspace deletes. `AtlasMacroTests
    --text-field-reading` replays NVDA's reading against a JUCE text field.
  - `juce8-uia-focus-loss.patch`: UI Automation's focus event tells a
    screen reader the focus is now on the element that sends it. JUCE also
    sent one from the element losing the focus, at the moment no element
    has it, so NVDA found the window instead and said "Atlas Settings
    dialog" again before each control Tab moved to in the Settings window.
    As JUCE does on iOS, only the element that has the focus sends it now.
    `AtlasMacroTests --focus-events` checks that a Tab raises one focus
    event, the new control's.
  - `juce8-pffft-fft-engine.patch`: adds a `juce::dsp::FFT` engine on PFFFT
    (`pffft/`) ahead of JUCE's others when `JUCE_DSP_USE_PFFFT` is set, which
    the Atlas target does for every target built against it. Every
    `juce::dsp::FFT` and `juce::dsp::Convolution` in Atlas then runs on
    PFFFT, on Windows instead of JUCE's scalar fallback and on macOS instead
    of vDSP, so both platforms transform alike. Results keep JUCE's layout and
    scaling (see `pffft/README.atlas.md`).
- `nlohmann_json/`: nlohmann json 3.1.2 (MIT), from the release-tracking
  repository https://github.com/ArthurSonzogni/nlohmann_json_cmake_fetchcontent,
  which carries only the released header (`include/nlohmann/json.hpp`, the
  same file as the official release's single include).
- `concurrentqueue/`: moodycamel concurrent queue (simplified BSD),
  https://github.com/cameron314/concurrentqueue at 0bc97ad (June 2016).
- `kissfft/`: KISS FFT (BSD-3), https://github.com/mborgerding/kissfft at
  760cb7e (October 2018). Atlas uses its header-only C++ class, `kissfft.hh`.
- `espeak-ng/`: eSpeak NG 1.53.0 development (GPL v3 or later; ucd-tools and
  NV Speech Player inside it are GPL v3 or later too, its Unicode tables are
  under the Unicode licence in `COPYING.UCD`), https://github.com/espeak-ng/espeak-ng
  at ba90c8e (September 2026), unchanged. CMake builds its static library and
  the `espeak-ng` program, which compiles the phoneme data, intonations and
  every language's dictionary into `build/espeak-ng/espeak-ng-data`; the
  bundles carry that folder in `Resources/espeak-ng-data`. MBROLA, libsonic,
  libpcaudio and the async API are off. The Speech oscillator type uses it;
  see `src/common/speech/README.atlas.md`.
- `opus/`: Opus 1.5.2 plus 16 commits (BSD-3, https://github.com/xiph/opus
  at 5854a9f, the commit Maim pins), unchanged, built as a static library by
  its own CMake with no programs, tests, DRED or OSCE. The Maim effect's Opus
  encoder.

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
- `OpenB3/`: OpenB3 by Michele Perrone (AGPL v3 or later),
  https://github.com/michele-perrone/OpenB3 at 03ac6a8, with setBfree's Beatrix
  engine (GPL v2 or later) in `BeatrixCPP/Source`. The OpenB3 reference tests
  build its tonegen, vibrato and whirl C files with `tests/openb3/reference.c`;
  see `src/synthesis/effects/openb3/README.atlas.md`.

Kept in this repository:

- `scream/Source`: the BP Scream DSP (MIT, Cure Audio / exacoustics / Xynth
  Audio) with the ADAA (BSD-3) and Polylogarithm (MIT) notices in
  `scream/LICENSE`. This JUCE version of the DSP is not published upstream (the
  released Scream at https://github.com/Cure-Audio/Scream is a separate C
  codebase), so it stays here. Only the DSP is kept: the Scream filter model
  (`src/synthesis/filters/scream_filter.cpp`) uses its header-only parts, and
  `AtlasTimingTests --scream-filter` compares the model with `ScreamDSP.cpp`.
  The Scream effect that wrapped the whole DSP was retired in favour of the
  filter model, along with the plugin, LFO and UI sources its test needed.
- `nvda_controller/`: NVDA's controller interface definition
  (`nvdaController.idl` and `.acf`, LGPL 2.1, NV Access,
  https://github.com/nvaccess/nvda). On Windows, MIDL turns it into the RPC
  client Atlas speaks through NVDA with; see `nvda_controller/README.atlas.md`.
- `licenses/WolfShaper.LICENSE`: Atlas's Waveshaper adapts Wolf Shaper by
  Patrick Desaulniers (GPL v3 or later).
- `maim/`: the two MP3 encoders of Maim by Arden Butterfield
  (https://github.com/ArdenButterfield/Maim at 985ed20, GPL v3), as Maim
  circuit bends them: `lame/` is libmp3lame 3.99.5 with mpglib's decoder
  (LGPL 2 or later) and `blade/` BladeEnc 0.94.2 (LGPL 2.1 or later), each
  with Maim's `*_bends` API. Atlas adds `lame/atlas_config/config.h` in place
  of configure's, the wrapper headers in `include/`, and a per-encoder random
  generator for the Error bend; see `src/synthesis/effects/maim/README.atlas.md`.
- `gsm/`: libgsm 1.0 patchlevel 22 by Jutta Degener and Carsten Bormann
  (https://www.quut.com/gsm/, their own permissive licence in `COPYRIGHT`),
  unchanged, without the toast program. The Maim effect's GSM encoder.
- `pffft/`: PFFFT by Julien Pommier (FFTPACK licence, BSD style), unchanged
  from https://bitbucket.org/jpommier/pffft. Every FFT in Atlas uses it on
  every platform: the oscillators' (`vital::FourierTransform`) and, through
  `patches/juce8-pffft-fft-engine.patch`, every `juce::dsp::FFT` and
  `juce::dsp::Convolution`; see `pffft/README.atlas.md`.

The Suppressor and Spectral Compressor effects are Atlas's own code under
`src/synthesis/effects`. The JSFX oracle inputs are in `tests/jsfx/fixtures`.
