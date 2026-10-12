# OpenB3 source

The OpenB3 oscillator type, the Scanner Vibrato effect and Analogue Rotary's
OpenB3 Whirl model run a C++ port of the Beatrix engine in OpenB3 by Michele
Perrone (https://github.com/michele-perrone/OpenB3, commit 03ac6a8,
`BeatrixCPP/Source`). OpenB3 is AGPL v3 or later; the Beatrix files it
carries are setBfree's (https://github.com/pantherb/setBfree), GPL v2 or
later: Copyright (C) 2003-2004 Fredrik Kilander, 2008-2018 Robin Gareus, 2010
Ken Restivo, 2012 Will Panther. The ported files are those:

| Atlas | Beatrix |
| --- | --- |
| `tonewheel_tables.*` | `tonegen/tonegen.c`: the default wiring (`applyDefaultConfiguration`, `taperingModel`, `compilePlayMatrix`), the wheel frequencies (`initOscillators`) and key compression (`initKeyCompTable`) |
| `tonewheel_organ.*` | `tonegen/tonegen.c`: the attack envelopes (`initEnvelopes`), drawbars, percussion, key compression and the output mix (`oscGenerateFragment`) |
| `scanner_vibrato.*` | `vibrato/vibrato.c` |
| `whirl.*` | `whirl/whirl.c` and `whirl/eqcomp.c` |

Everything runs with the settings OpenB3 uses, which loads no configuration
file: tonegen.c's, vibrato.c's and whirl.c's `initValues` and defaults, the
TG_91FB12 generator (91 wheels, the 16' folds back below terminal 13), gear
tuning for 60 Hz mains, and the 88 6000 000 upper manual drawbars of OpenB3's
`global_definitions.c`. OpenB3's overdrive (`overdrive/`) and reverb
(`reverb/`) are not ported: the overdrive is off (clean) in OpenB3 by default,
and Atlas's distortion and reverb effects take their place. Neither is the MIDI
and configuration machinery.

## How Atlas splits the instrument

OpenB3 renders the whole organ in one place: every key adds its contacts'
wheels to shared buses, and the sum goes through the scanner, the overdrive,
the reverb and the rotary speaker in turn. Atlas splits it like this:

- Per voice, the oscillator type (`src/synthesis/producers/openb3_source.*`):
  `Voice` is one key of the upper manual. Its contacts connect wheels to the
  nine drawbar buses at the levels `compilePlayMatrix` gives that key, the
  drawbars set the bus gains, and the percussion taps the 4' or 2 2/3' bus.
- Shared by the voices of one slot, `Organ`: the clock the tonewheels turn on,
  key compression and the percussion envelope. `OpenB3Clock` runs it once per
  block in the voice handler's mono router, before any voice. Because every
  voice reads its wheels' phases from the one clock, two keys that use the same
  wheel hear it in phase, as they do in tonegen.c, where the wheel is a single
  buffer.
- The scanner vibrato is the Scanner Vibrato effect
  (`src/synthesis/modules/scanner_vibrato_module.*`), in the Analogue menu.
- The rotary speaker is Analogue Rotary's OpenB3 Whirl model
  (`src/synthesis/effects/analogue/rotary_engine.*`).

## What changed

- Atlas plays every MIDI note. The nearest of the upper manual's 61 keys (C2
  to C7) gives the wiring, and the voice gives the pitch: `Voice` scales every
  wheel's frequency by the voice's pitch against the key's, so pitch bend,
  glide, Transpose and Tune move it. Above the Nyquist rate a wheel is silent,
  as `writeSamples` mutes its partials.
- Wheels are sine phasors computed from the organ clock instead of tonegen.c's
  looped wave tables. The tables hold a sine (OpenB3 configures no extra
  harmonics) plus a one-bit dither and are sized to loop to within 0.001
  samples; Atlas plays the wheel's exact frequency. In tonegen.c a wheel only
  turns while a key uses it; Atlas's turn all the time, as the real generator
  does.
- tonegen.c works in 64-sample fragments: key events take effect at the next
  fragment, the attack click and the release last one fragment, and key
  compression moves once per fragment. Atlas starts a key at its exact sample.
  A fragment is 64 samples at 48 kHz, so the click, the release, the click
  burst lengths (8 to 40 samples at 22.05 kHz, at most a fragment) and the key
  compression take the same time at any rate; tonegen.c uses 64 samples at
  every rate. The percussion decays (0.9995 and 0.9999 per sample at 48 kHz,
  OpenB3's values; it never recomputes them from `osc.perc.fast` and `.slow`)
  are scaled the same way.
- tonegen.c draws its 8 attack envelopes once, when it starts, and a wheel
  gets the one for its place in the list of sounding wheels. Atlas draws a new
  set for every key strike (with its own random generator), so no two strikes
  click alike, and gives each wheel the one for its place in the key's list:
  for a single key from silence that is the same assignment.
- Key compression and the percussion envelope follow the keys held at the end
  of the previous block (the voices check in as they finish a block), as
  tonegen.c reads `keyDownCount` and `upperKeyCount` between fragments. A key
  struck with no key held before its block starts the percussion at its own
  sample.
- Drawbar and percussion changes fade over a fragment instead of stepping at
  the fragment's start, and modulation may set a drawbar between its stops
  (tonegen.c's level is the stop divided by 8). The drop of the drawbars to
  0.60512 while Normal percussion is on fades with them.
- Every voice's output is tonegen.c's sum before its 0.07 output trim (the swell
  pedal at full), divided by 3: a key with the default 88 6000 000 peaks near
  0.9 before the slot level. The percussion is not routed around the scanner
  as tonegen.c routes it, since the scanner is an effect after the voices.
- Amplitude Envelope: Organ (the default) lets the contacts shape the note and
  holds the voice open while they sound; Envelope 1 lets Env 1 shape it, the
  contacts staying closed through its release.
- The scanner's swing (3, 6 and 9 samples in vibrato.c, whatever the rate) is
  3, 6 and 9 samples at 48 kHz and the same time at other rates. Each stereo
  channel runs its own delay line under the one scanner. Mode and Rate (4 to 22
  Hz, `scanner.hz`) are Atlas controls.
- The whirl reads its speed changes every 64 samples, as OpenB3 calls it in 64-
  sample fragments, whatever block sizes Atlas uses. Its delay lines grow
  beyond whirl.c's 2048 samples when a rate needs more (above about 200 kHz).
  The brake positions stay at free stop and the microphone width matrix at
  stereo (OpenB3's settings); that code is not ported, nor the disabled horn
  comb filter. In Analogue Rotary, Speed selects whirl.c's both-slow,
  both-fast and stopped options, Inertia scales its acceleration and
  deceleration times (50% is setBfree's), Spread sets `whirl.horn.mic.angle`
  (100% is OpenB3's 180 degrees), Balance scales the horn and the drum, and
  the rotors start at speed when the effect starts; setBfree starts them at
  rest.

## Build

These files are listed in `ATLAS_OPENB3_SOURCES` in `CMakeLists.txt` and built
outside the fast-math unity file with FMA contraction off, as are
`openb3_source.cpp`, `openb3_module.cpp` and `scanner_vibrato_module.cpp`:
the whirl and the scanner keep the C code's float and double operations in its
order, and the reference tests compare them sample for sample.

## Testing

`AtlasOpenB3Tests` (`tests/openb3_tests.cpp`) covers the oscillator, the
effect and the model through the plugin. With the opt-in `third_party/OpenB3`
submodule checked out, CMake also builds `AtlasOpenB3Reference` from the
untouched Beatrix C files and `tests/openb3/reference.c`, a driver with stubs
for the MIDI and configuration hooks, compiled with FMA contraction off. The
reference tests compare against it:

- `--wiring-reference`: every key's contributions (9801 entries), the wheel
  frequencies for each temperament and the key compression table are
  bit-identical.
- `--gains-reference`: each sounding wheel's drawbar and percussion gains for
  seven registrations, bit-identical for single keys and within 1e-6 for
  chords (tonegen.c sums per wheel, Atlas per key).
- `--tonegen-reference`: Atlas's organ against `oscGenerateFragment`, given
  tonegen.c's loop frequencies and attack envelopes, with fragment-aligned
  keys: single keys across the manual for seven registrations, with and
  without percussion, match to a residual of -45 to -88 dB (the wave tables'
  dither and loop seams), and a chord to -45 dB. Percussion through legato
  playing matches tonegen.c's level within 1 dB.
- `--scanner-reference`: the scanner is bit-identical to `vibratoProc` for
  every mode at three rates of the scanner, in uneven blocks.
- `--whirl-reference`: the whirl is bit-identical to `whirlProc3` at 44.1, 48
  and 96 kHz through speed changes, in uneven blocks.
- `--rotary-reference`: Analogue Rotary's OpenB3 Whirl model at Spread 100%
  and Mix 100% plays `whirlProc3`'s output exactly.
