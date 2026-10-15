# Glitch Delay source

The Glitch Delay effect runs the DSP of Astralay, a screen-reader-first
multi-tap delay and glitch generator by Iron Labs. It is published under the
MIT licence at https://github.com/ironcross32/astralay. The licence text is in
`LICENSE.astralay`, and each adapted file says where it came from.

Copied from Astralay e82073d3fe5e1ff6200d69469b1eac886d7f84ce
(`source/dsp/` and `source/params/NoteValues.*`), renamed to Atlas's
snake_case file names:

| Astralay | Atlas |
| --- | --- |
| `DcBlocker.h`, `DelayLine.h`, `Finite.h`, `FreezeSustain.h`, `Glitch.h`, `Seed.h`, `SincKernel.h`, `SoftClip.h`, `TapeClock.h` | `dc_blocker.h`, `delay_line.h`, `finite.h`, `freeze_sustain.h`, `glitch.h`, `seed.h`, `sinc_kernel.h`, `soft_clip.h`, `tape_clock.h` |
| `Engine.{h,cpp}` | `glitch_delay_engine.{h,cpp}` |
| `FormantShifter.{h,cpp}`, `GlitchChain.{h,cpp}`, `Smear.{h,cpp}`, `Tap.{h,cpp}` | `formant_shifter.*`, `glitch_chain.*`, `smear.*`, `tap.*` |
| `RealFft.{h,cpp}` | `real_fft.{h,cpp}` (rewritten, see below) |
| `params/NoteValues.{h,cpp}` | `note_values.{h,cpp}` |

The namespace is `atlas::glitch_delay` instead of `astralay::dsp` and
`astralay`. Otherwise the code is Astralay's, with these changes:

- Four taps instead of sixteen (`Engine::numTaps`), so the formant shifters'
  shared schedule has eight slots instead of 32
  (`FormantShifter::scheduleSlots`).
- The diagnostic log (`Diagnostics.h` and its probes in the glitch chain, tap
  and engine) is left out. Removing it doesn't change the order of the random
  draws: the probes only recorded what had been drawn.
- The output hard clip (a session setting in Astralay, not a parameter) is
  left out. Atlas's effect chain has its own limiter.
- `RealFft` runs on PFFFT through `atlas::Fft` (`src/common/atlas_fft.h`),
  which every Atlas FFT shares, instead of calling PFFFT itself. The spectrum
  layout and the scaled inverse are Astralay's.

What Astralay keeps in its plug-in rather than its DSP is not used: macros,
MIDI learn, the performance area, presets, undo, the glitch stop key (a
performance control with no parameter) and its editor. Atlas has its own
modulation, MIDI learn and accessible editor for those jobs. The tape stop is
an ordinary Atlas parameter, saved with presets like any other. Astralay keeps
it out of saved state.

`src/synthesis/modules/glitch_delay_module.{h,cpp}` is the Atlas wrapper. It
does what Astralay's `PluginProcessor::updateEngineSettings` did, once per
block:

- Parameters are stored as Atlas stores skewed values: times as log2, square or
  cube of milliseconds, frequencies, densities and ratios as log2. The module
  converts them to the engine's units. The ranges and defaults are Astralay's
  (`src/common/glitch_delay_parameters.inc`, generated with the four taps
  identical).
- Tempo comes from Atlas's beats-per-second output. Bars are 4/4, because
  Atlas has no host time signature.
- The host's transport comes from Atlas's song time (`correctToTime`) and the
  shared transport clock's playing state. A song position within two samples
  of where the last block ended counts as continuous, so rounding in the song
  time never restarts reproducible randomness, while a seek or loop still does.
- The taps hear the input summed to mono, as in Astralay. The dry signal stays
  stereo, and lanes 2-3 mirror the left and right lanes.
- Like Astralay's macros, Atlas modulation reaches the taps' continuous
  controls, Glide, Smear and Mix. It doesn't reach switches, choices, the
  note values used while Host Sync is on, the glitch engine's settings or
  the output gain.

The engine source files are built from `CMakeLists.txt` directly, outside the
fast-math unity build: long delay glides are in double precision, and
non-finite samples are found by bit pattern.

`tests/glitch_delay/` holds Astralay's engine test suites (`DspTests`,
`GlitchTests`, `FormantTests`, `FormantSchedulingTests`, `FreezeSustainTests`,
`SmearTests`, `ResetTests` and the engine half of `TapeStopTests`). They were
adapted to four taps, and the tests that used Astralay's processor were
removed. They run as `AtlasGlitchDelayTests --dsp`. `tests/glitch_delay_tests.cpp`
covers the Atlas side: parameters and units, echo timing with oversampling and
host sync, allocation-free glitching, reproducible randomness against Atlas's
song time, a chain slot, host state and the editor's tap rows.
