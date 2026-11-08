# Frequency Shifter Mode 2 (Serum 2 Bode)

A native port of `Serum2Bode.jsfx` by Alessio Scala, version 1.0: a recreation of the Bode
frequency-shifter module of Xfer Records' Serum 2 with its feedback delay, modelled from
measurements of the original plugin in REAPER and not from its code. The script carries no
licence statement; it was supplied for Atlas by the project. Unlike the other JSFX ports, the
script itself is not in this repository (see the oracle test below).

What the script does: an analytic signal from two 22-section first-order allpass cascades
(the in-phase chain after a 2-sample delay, the quadrature chain after 3), a single complex
modulator advanced by two Euler half-steps per sample and renormalised at quadrant crossings,
whose frequency follows a measured table slightly below Shift × Range; the left output is the
left input shifted down and the right output the right input shifted up; Dir crossfades one
output towards the other, Width blends them in mid/side, a feedback line (read with linear
interpolation at the delay time minus 23 samples, never less than 3) feeds the wet signal back
into both channels before the shifter, and Blur is a fixed 24-tap random multitap of unit energy.

## What changed

Nothing in the DSP. `Shifter::prepare`/`reset`, `setParameters` and `process` are the script's
`@init`, `@slider` (`set_params`) and `@sample`, statement for statement, in double precision as
JSFX computes. The tables (allpass coefficients, frequency law) are the script's literals. The
sources are built outside the fast-math unity file, without FMA contraction, and call the C
library's `sin`, `cos`, `log`, `pow` and `sqrt` through pointers so the compiler cannot fold or
fuse them. What it takes to be the same double, bit for bit:

- **EEL2's grouping.** In EEL2 `/` binds tighter than `*`, and `-` tighter than `+`, so
  `$pi * feff / srate` is `$pi * (feff / srate)` and `BLUR_K * blur * blur * srate / 48000` ends
  in `* (srate / 48000)`. Every expression is bracketed as EEL2 reads it.
- **Flushing on assignment.** EEL2 stores zero instead of a tiny value, inf or NaN when it assigns
  a computed value (`+`/`-` without a constant, `*`, `/`, compound operators); copies, memory
  reads, `floor` and constants are stored as they are. `flush` marks each such assignment.
  REAPER's own EEL2 (x64 and arm64) flushes exponents up to 1, the smallest normal binade
  included; WDL's portable interpreter only denormals. `Eel2Build` holds the difference.
- **`%`.** `(r * 1103515245 + 12345) % 2147483648` divides the 64-bit integer magnitude by 2^31
  in REAPER. The portable interpreter converts the divisor to `int`, which arm64 saturates to
  2^31 − 1; that is `Eel2Build`'s other field.
- **The slider table inside `BLUR_R`.** The drawing code's table (`PRM_MIN` … `PRM_KIND`, memory
  30000–30316) lies inside the right blur buffer (18384–34767). Until `@sample` has written over
  them, the right blur taps read its values (ranges and defaults such as 5000 and 100). Taps reach
  them only when they are longer than about 4450 samples, which takes a high Blur at 96 kHz and
  above, and only in the first 16384 samples after `@init`; the port starts its buffer with the
  same values. This is a quirk of the script, kept because the port must match it.

Left out: the drawing code, and the sliders Atlas does not expose, which keep their defaults:
Swap A/B off, BPM sync off (so the Sync note does nothing), Balance 0, Retrig off, Level 0.5
(0 dB) and Osc phase at start 0. `@block` then changes nothing.

## In Atlas

`FrequencyShifterModule` runs the port when its Mode is Mode 2; Mode 1, the default, is the
original shifter, so older patches load as they were. Mode 2's nine controls (`frequency_shifter_bode_*`)
hold the script's slider values in its units: Shift, Direction, Width, Feedback, Blur and Mix in
percent, Range in Hz (stored as its base-2 logarithm, raised to Hz exactly by the module rather
than by the shared approximate exponential scale), Delay in seconds (shown in milliseconds) and
Mono Input as a switch. Controls are read once per block, when the script's `@slider` would run;
the script smooths nothing. Each mode starts from silence when chosen.

The port runs at the effect chain's rate (the host rate times Atlas's oversampling), as the script
would in a project at that rate; its allpass coefficients and offsets were measured at 48 kHz and
do not scale. Only the left and right lanes are processed, and lanes 2-3 repeat them. The output
is rounded to float only where it leaves the module.

The feedback line holds 5 s in double precision (floor(5 × rate) + 128 samples), so a slot
allocates it only once its Frequency Shifter has used Mode 2 (`PreparedEffectModule`'s
configuration then asks the worker for `prepareMode2`, as Filter FX does for its Filter Table
buffers), and keeps it afterwards. After a reset it is not cleared: samples not written since
`@init` read as zero, as REAPER's zeroed memory does. Mode 2 reports the script's `pdc_delay`,
11 samples, to the host, and delays only its dry path by them, as the script does. Mix follows
Serum's law, the script's: the dry signal at min(1, 2 (1 − mix)) and the shifted one at
min(1, 2 mix), so both play at full level at 50 %.

## Oracle test

`AtlasBodeReferenceTests` (`tests/bode/reference_tests.cpp`, built when the WDL submodule is
present) runs the unmodified script through WDL's EEL2 next to the port and requires every output
sample to be the same double, at 44.1, 48, 96 and 192 kHz, through changes of every exposed
slider (Mix from 0 to 100 %, which brings in the delayed dry path), short and long feedback delays, Blur from the first sample (reaching the slider table at
96 and 192 kHz), signals in the smallest normal binade and below, and loud input. It asks the
interpreter how it divides and flushes and gives the port the same `Eel2Build`; it also checks
the latency against `pdc_delay`. The script is not in the repository: name it when configuring.

```sh
git submodule update --init --checkout third_party/WDL
cmake -S . -B build -DATLAS_BUILD_TIMING_TESTS=ON -DATLAS_BODE_JSFX=/path/to/Serum2Bode.jsfx
cmake --build build --config Release --parallel 4 --target AtlasBodeReferenceTests
ctest --test-dir build -C Release -R atlas_bode_reference --output-on-failure
```

`AtlasJsfxTests` checks that the module passes its controls to the port unchanged at every block
size and oversampling, allocates nothing while it plays, and that a slot prepares Mode 2 when it
is chosen and reports its latency.
