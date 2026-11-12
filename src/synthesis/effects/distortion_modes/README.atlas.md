# Distortion: the JSFX modes

A native port of eight modes of a distortion JSFX by Alessio Scala, version 1.0. Its 18 modes are
inspired by the distortion in Xfer Records' Serum and modelled from measurements of it in REAPER,
not from its code. The script carries no licence statement; it was supplied for Atlas by the
project. Like Destroy's script, it is not in this repository (see the oracle test below).

The ported modes are the Distortion effect's Types from Tube on: Tube, Diode 1, Diode 2,
Zero-Square, Asymmetric (the script's Asym), Stomp Box, Tape Saturation (Tape Sat) and Soft
Saturation (Soft Sat). Left out: Soft Clip, Hard Clip, Lin Fold, Sin Fold and Downsample, which
Atlas has as Vital's Types of those names; Overdrive, which the Destroy effect ports; and Rectify,
X-Shaper, X-Shaper (Asym) and Sine Shaper.

What the script does in these modes: it runs the mode's curve 4 times oversampled, through two
2x polyphase all-pass half-band stages up and two down, scales the result by 0.25 × 0.999830894
and delays it 3 samples to line up with Serum's output. Diode 2 and Asym, whose curves are
asymmetric, get a 12 Hz DC blocker first. Drive (0 to 100 %) sets each curve:

- **Tube**: 2x clipped at ±1, then `(1 + a) v / (1 + a |v|)` with `a = 1 / (1 − 0.989 D) − 1`.
- **Diode 1**: a blend, by `min(1, 8 D)`, of 2x and `x (|x| + c − 0.7) / (x² + c |x| + 0.25)`, with `c`
  rising from 11.2 above Drive 12.5 %.
- **Diode 2**: `0.9968 sin v − 0.0796 (1 − cos v)` of `v = 43.4 D x`, held at 0.57 or −0.68 beyond ±2.5.
- **Zero-Square**: a measured 256-point table, read on an asinh scale, of `|x|` plus `0.5 D`
  (no offset below 0.00005), with the input's sign.
- **Asym**: a measured 768-point table, read on an asinh scale, of `(1 + 20 D²) x`.
- **Stomp Box**: a line to 0.8 at `|x| = 0.4 / (1 + 7 D)`, then a gentler one reaching 1 at 0.5, with
  the input's sign.
- **Tape Sat**: a line up to `1 / (3 (1 + 3 D))`, then a cosine; plain 2x at Drive 0.
- **Soft Sat**: linear to 0.5, a quadratic knee to 1.5, then flat at 1, of `2 (1 + 16 D²) x`.

The tables are the script's, stored as it stores them: three characters per value, each a base-64
digit. A Mode change crossfades from the old mode to the new over 30 ms (a raised cosine), each
mode with its own state; a change during a fade waits for it to end. Drive glides to a new value
through a 20 ms one-pole stepped once per block. The output is `Level' × ((1 − s²) dry + s wet)`, where `s` follows
`sin²(π Mix / 200)` and `Level'` follows `(2 Level)²`, both through a 20 ms one-pole per sample, and
the sum is clipped at ±4. The dry path is delayed by 7 samples, the script's `pdc_delay`. After
8192 samples below 1e-10 the script stops running the wet path and keeps its state until the
input returns.

## What changed

One thing: Atlas doubles the wet signal before the mix (`unity_wet`, on in Atlas). The script's
wet path plays a quiet input at half its level at Drive 0 (−6.02 dB), so with Level at 0 dB its
modes sounded 6 dB quieter than the dry signal and than Distortion's other Types. Doubling is
exact in floating point, and the dry path is left alone, so Mix keeps its balance. With
`unity_wet` off the class is the script, and the oracle test runs it that way.

Apart from that, `Engine::prepare`, `setParameters`, `block` and `wet` with `output` are
the script's `@init`, `@slider` (`set_params`), `@block` and `@sample` for them, statement for
statement, in double precision as JSFX computes; `set_drive` and `shape` keep only the ported
modes' parts. The constants are the script's literals. The source is built outside the
fast-math unity file, without FMA contraction, and calls the C library's `sin`, `cos`, `exp`,
`log`, `pow` and `sqrt` through pointers so the compiler cannot fold or fuse them. What it takes
to be the same double, bit for bit:

- **EEL2's grouping.** In EEL2 `/` binds tighter than `*`, and `-` tighter than `+`, so
  `(1 + tube_a) * v / (1 + ...)` is `(1 + tube_a) * (v / (1 + ...))`, `asym_g * x / ASYM_A` is
  `asym_g * (x / ASYM_A)` and `$pi * k / (0.625 * ...)` divides first. Every expression is
  bracketed as EEL2 reads it.
- **Flushing on assignment.** EEL2 stores zero instead of a tiny value, inf or NaN when it assigns
  a value its compiler judges could be denormal: one computed by `+` or `-` without a constant, by
  `*` without a constant of at least 1, by `/` without a constant of at most 1, by `^`, `exp` or
  `log`, or by a compound operator. Copies, memory reads, comparisons, `cos`, and `abs`, `min` and
  `max` of unchecked values are stored as they are, and a user function's result counts as its last
  expression: `shape` and `down2` are always flushed where they are assigned, a function's
  arguments never. `flush` marks each checked assignment. REAPER's own EEL2 flushes exponents up
  to 1, the smallest normal binade included; WDL's portable interpreter only denormals.
  `Eel2Build` holds the difference.

`@sample` is split in two so Atlas's filter can sit inside it: `wet` is the silence count, the
shaper and the crossfade, `output` the dry delay, Mix, Level and the clip. Neither reads what the
other writes, so running both is the script's `@sample` with its filter off.

## In Atlas

`DistortionModule` runs the port for the Types from Tube on (`Distortion::kTube` to
`kSoftSaturation`, after `kNumTypes`, which stays the voice filter's six Distortion styles).
Three controls hold the script's sliders: Drive (`distortion_jsfx_drive`, in percent, 0 by
default as Distortion's own Drive is 0 dB), Mix (Distortion's own Mix, times 100) and Level
(`distortion_jsfx_level`, 0 to 1, read in dB as the script shows it, as Destroy's Level does).
At their defaults the Types play a quiet input at its own level, Zero-Square within 0.1 dB and
Asymmetric 2 dB louder, except Diode 2: its curve is `43.4 D x`, silent at Drive 0. The editor
shows these Drive and Level rows in place of Distortion's own Drive for these Types. Controls
are read once per block, when the script's `@slider` would run, and `@block` runs once per block
of the effect chain.

Atlas's filter takes the place of the script's: Pre shapes the filtered input (the silence count
still reads the input), Post filters the wet signal before the mix. With the filter at None the
module is the port: the script with its wet path doubled. Coming from a Type before Tube, after
a reset (all sounds off, a preset load) or at a new rate, the port runs `@init` in its new mode;
between its own modes it crossfades.

The port runs at the effect chain's rate (the host rate times Atlas's oversampling), as the script
would in a project at that rate. Only the left and right lanes are processed, and lanes 2-3 repeat
them. The output is rounded to float only where it leaves the module, and in Post, where Atlas's
filter works in float. These Types report the script's 7 samples of latency to the host and delay
only the dry path by them, as the script does.

## Oracle test

`AtlasDistortionModesReferenceTests` (`tests/distortion_modes/reference_tests.cpp`, built when
the WDL submodule is present) runs the unmodified script through WDL's EEL2 next to the port,
with `unity_wet` off, and requires every output sample to be the same double, at 44.1, 48, 96
and 192 kHz: in each mode through Drive's glide, Drive 0, Mix and Level; through crossfades
between every mode, including changes during a fade; from the script's default mode; through 0.4 s
of digital silence (the suspension); with signals in the smallest normal binade and below and
a Drive small enough that its curve values flush; and with loud input that reaches the output
clip. The test gives the script the string literals and `str_getchar` its tables are decoded
with, asks the interpreter how it flushes and gives the port the same `Eel2Build`, and checks
the latency against `pdc_delay`. The script is not in the repository: name it when configuring.

```sh
git submodule update --init --checkout third_party/WDL
cmake -S . -B build -DATLAS_BUILD_TIMING_TESTS=ON -DATLAS_DISTORTION_JSFX=/path/to/script.jsfx
cmake --build build --config Release --parallel 4 --target AtlasDistortionModesReferenceTests
ctest --test-dir build -C Release -R atlas_distortion_modes_reference --output-on-failure
```
