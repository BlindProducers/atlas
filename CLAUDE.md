# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Atlas is a GPLv3 software synth (VST3 on macOS/Windows, plus AU on macOS) forked from Vital. It keeps Vital's DSP engine and adds oscillator types, filter models, effects, effect buses and modulation. Its main design constraint is that **every control must work from the keyboard and be announced by a screen reader** (VoiceOver and Windows screen readers). Vital's graphical interface is gone. The whole UI is one accessible JUCE editor.

## Build and test

Dependencies are git submodules in `third_party/`. CMake refuses to configure without them, so run `git submodule update --init` first. JUCE is `third_party/JUCE8` (8.0.14). macOS builds arm64 only.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel 4
```

Always give `--parallel` a job count. Ninja is not installed, so CMake uses Makefiles, and a bare `--parallel` becomes `make -j` with no limit. Each job can use several GB of RAM (the editor unity file alone is about 19k lines plus JUCE), and an unlimited build will freeze a 16 GB machine. Use 4 jobs. Drop to 2 if the machine is busy.

The plugins are written to `build/Atlas_artefacts/Release`. Ship the whole `Atlas.vst3` bundle: a post-build step copies `Factory/Samples/Tape/*.atape` into the bundle's `Resources/Tape` and, on macOS, re-signs the bundle ad hoc.

Tests are off by default. Turn them on with `-DATLAS_BUILD_TIMING_TESTS=ON`. That adds about 26 console executables, and each one compiles its own copy of the JUCE modules, so **never build the whole suite** (`cmake --build build` with no `--target`). Build and run only the tests that cover the code you changed:

```sh
cmake --build build --config Release --parallel 4 --target AtlasScreamTests
ctest --test-dir build -C Release -R atlas_scream --output-on-failure
```

To find the right target, grep `CMakeLists.txt` for the `add_test` lines whose names match the feature. Run the full suite (`ctest --test-dir build -C Release --output-on-failure`) only when the user asks for it. CMake marks every test `RUN_SERIAL`, because several tests measure timing and CPU or start their own threads, so ctest never runs them side by side even with `-j`. Every test also gets a timeout (`ATLAS_TEST_TIMEOUT`, 600 s by default), so a hung test fails instead of blocking for ctest's 1500 s default. Tests labelled `cpu` are benchmarks. Leave them out with `-LE cpu` unless you are working on performance. There is no test framework. Each test target is a `juce_add_console_app` with a hand-written `main` that dispatches on a flag (for example `AtlasTimingTests --reverb-filter`). Many `tests/*_tests.h` headers are included into `tests/timing_stability_tests.cpp`. To add a test, add a flag branch in that test's `main` and an `add_test(...)` line in `CMakeLists.txt`. You can run a test binary directly with its flag.

Reference tests compare Atlas against the original code it adapts. They are only added when the opt-in submodules are present: `third_party/aquanode-plugins` (Spectral Lock), `WDL` (EEL2 runs the original JSFX for the JSFX, Reverb filter, Trance Gate and Resonator tests), `nih-plug` (Diopser for Defuse, which also needs `rustc` and Python 3), and `openwurli` with `melange` (the OpenWurli oscillator and amp, also with `rustc` and Python 3). Fetch them with `git submodule update --init --checkout <path>` and configure again. `third_party/README.atlas.md` describes each dependency.

## Source layout and build model

- `src/synthesis/`: the DSP, mostly Vital's processor-graph framework (`framework/`, `vital::` namespace, SIMD `poly_float`). `modules/` holds the per-effect, per-oscillator and per-filter `*_module` processors. `effects/<name>/` holds standalone engines that a module wraps. `synth_engine/sound_engine.cpp` builds the graph.
- `src/common/`: parameters (`synth_parameters.cpp`), constants and enums (`synth_constants.h`: counts, the `Effect` enum, routing destinations, filter models, file extensions), preset load/save and migration (`load_save.cpp`), `synth_base`, MIDI, tuning, the arpeggiator and the wavetable editor model (`wavetable/`). Large parameter sets live in `*_parameters.inc` files.
- `src/plugin/`: the JUCE `AudioProcessor` (`synth_plugin.cpp`) and the accessible editor (`synth_editor.cpp`, about 19k lines). The editor builds rows of labelled controls, menus, the preset browser, the effect slot UI and the modulation UI. `value_bridge.h` and `fx_automation_parameter.h` bridge Vital's values to host parameters.
- `src/interface/look_and_feel/synth_strings.h`: display names and option strings that the editor and screen reader read out.

**Unity builds:** most `.cpp` files are not compiled on their own. They are `#include`d from `src/unity_build/{common,synthesis,plugin}.cpp`. `synthesis.cpp` is built with `-ffast-math` (`/fp:fast` on MSVC) in Release. When you add a `.cpp`, add it to the right unity file. Code that needs strict IEEE or double-precision behaviour must be listed directly in `target_sources(Atlas ...)` in `CMakeLists.txt` so that it stays out of the fast-math unit. This applies to the spectral, analogue, resonator, Scream, Defuse, tape and filter-table code, and the comments there explain why for each. The vendored Braids sources (`src/synthesis/effects/braids`) are listed there too, built with `TEST` and `-fwrapv` so stmlib's macros stay out of the unity files. `defuse_module.cpp` also disables FMA contraction.

Include paths are flat: the code uses `#include "foo.h"` across directories through `target_include_directories`. Each test target repeats that include list.

## Adding or changing an effect, filter or parameter

One feature touches many files. To find all of them, trace an existing effect. For example, `grep -rl TremoloPan src CMakeLists.txt` lists:
- `synth_constants.h` (the `Effect` enum)
- `synth_parameters.cpp` / `*.inc` (parameter definitions)
- `synth_strings.h` (names)
- `reorderable_effect_chain.{h,cpp}` (creating the slot)
- the module files
- `unity_build/synthesis.cpp` or `CMakeLists.txt`
- `synth_plugin.cpp` and `synth_editor.cpp` (host parameters and UI)

Every control must get a label, a value readout (levels as percentages) and keyboard access in the editor. Effect presets live in `Factory/FX/<group>/*.atf`/`.atfx`.

Saved state records `synth_version` and `engine_behavior_version` (`kEngineBehaviorVersion`). Old patches are migrated in `LoadSave::updateFromOldVersion`. When a change alters how existing presets sound or load, add a migration step there rather than changing the defaults. Atlas extensions are `.atp` (preset), `.att` (wavetable), `.ats` (LFO), `.atb` (bank) and `.atf` (FX), and it still reads Vital's `.vital*` formats. An `.atp` is plain JSON unless a Multi-sample oscillator has an SFZ/SF2 instrument. Then it is a zip with `preset.json` first, followed by the instrument files. Read preset text through `multisample::container::readPresetText`, never as a raw file. `src/common/multisample/README.atlas.md` describes the format, the instrument cache and SFZ/SF2 coverage.

Code adapted from other projects has a `README.atlas.md` next to it (for example `src/synthesis/effects/spectral_lock/`) that records where it came from, what changed and what the oracle test checks. Keep these files and `NOTICE` up to date when you adapt third-party code.
