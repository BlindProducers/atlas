# Destroy (Serum 2 Overdrive)

A native port of `Serum2Overdrive.jsfx` by Alessio Scala, version 1.0: a recreation of the
Overdrive mode of the Distortion effect in Xfer Records' Serum 2, with its filter section and
its Stack feature, modelled from measurements of the original plugin in REAPER and not from its
code. The script carries no licence statement; it was supplied for Atlas by the project. Like the
Frequency Shifter's Mode 2, the script itself is not in this repository (see the oracle test below).

What the script does: the dry input is delayed by 7 samples, the script's `pdc_delay`. The wet
path runs an optional Pre filter, then the stage Stack times (1 to 16), then an optional Post
filter. A stage is two 2nd-order Butterworth high-passes (30 Hz and 20 Hz), the Drive gain
(+0.9 dB at 0 % to +39.9 dB at 100 %), a gain control shared by both channels (a one-pole
follower of max(|L|, |R|) with 10 ms attack and 300 ms release, giving min(1, (T / env)^0.9)
with its threshold T rising 0.05 dB per percent of Drive) and a clipper, linear to 90 % of its
0.98837 ceiling and a tanh knee above. The filter is a Chamberlin state-variable filter run twice
per sample, its cutoff 8.1758 Hz × 2697^Freq, its damping from a measured table of Serum's Q, and
LP-HP crossfading its low-pass, band-pass and high-pass outputs. The wet signal is scaled by
(2 Level)² / 3, mixed with Serum's law (dry at 1 − s², wet at s, s = sin²(π Mix / 200)) and the
sum clipped at ±4. After 8192 samples below 1e-10 the script stops running the wet path and
keeps its state until the input returns.

## What changed

Nothing in the distortion. `Destroy::prepare`, `setParameters` and `process` are the script's
`@init`, `@slider` (`set_params`) and `@sample`, statement for statement, in double precision as
JSFX computes. The tables and constants are the script's literals. The source is built outside the
fast-math unity file, without FMA contraction, and calls the C library's `sin`, `cos`, `exp`,
`log`, `pow` and `sqrt` through pointers so the compiler cannot fold or fuse them. What it takes to
be the same double, bit for bit:

- **EEL2's grouping.** In EEL2 `/` binds tighter than `*`, and `-` tighter than `+`, so
  `(1 + cs) * 0.5 / a0` is `(1 + cs) * (0.5 / a0)`, `-2 * cs / a0` is `-2 * (cs / a0)`,
  `d * T_DBPCT / 20` is `d * (T_DBPCT / 20)` and `$pi * min(...) / (2 * srate)` divides first.
  Every expression is bracketed as EEL2 reads it.
- **Flushing on assignment.** EEL2 stores zero instead of a tiny value, inf or NaN when it assigns
  a value its compiler judges could be denormal: one computed by `+` or `-` without a constant,
  by `*` without a constant of at least 1, by `^`, or by a compound operator. Copies, memory reads,
  `abs`, `min` and `max` of those, and comparisons are stored as they are, and so is the result of
  a user function whose last expression is one of them: `knee` and `svf` return without a flush,
  and a function's arguments are copied without one. `flush` marks each checked assignment.
  REAPER's own EEL2 flushes exponents up to 1, the smallest normal binade included; WDL's portable
  interpreter only denormals. `Eel2Build` holds the difference. Whether `knee`'s result is flushed
  could not be told apart in any output the oracle test produced, since every later assignment
  flushes; the port follows the compiler's rule.

Left out: the drawing code and `@block`, which only applies the drawing code's changes.

Added: a DC blocker on the wet signal, which Atlas always runs (the `remove_dc` constructor
argument, on by default). The script's last clipper is symmetric, so material that is not
(a pulse wave, say) leaves an offset on the wet signal that only a following stage's high-pass
would remove; at Drive 100 a zero-mean 20 % pulse leaves about 0.07 of DC. The blocker is a
first-order high-pass at 5 Hz, `y = (1 + R) / 2 (x − x1) + R y1` with `R = exp(−2π 5 / rate)`,
unity at Nyquist, placed after the wet path's level and before the mix, so the dry path, the
mix law and the output clip are the script's. It holds its state while the script is suspended.

## In Atlas

`DestroyModule` (`drive_dynamics_modules.h`, the same wrapper as Saturator's and Overdrive's)
holds the script's eight slider values in its units and passes them unchanged: Drive, Filter LP-HP
and Mix in percent, Stack 1 to 16, Filter Off, Pre or Post, Filter Frequency and Level 0 to 1, and
Filter Q 0.05 to 7.6. Filter Frequency reads in Hz and Level in dB, as the script shows them
(`destroyFrequencyHz`, `destroyLevelDb`), and typed values in those units are accepted. Controls
are read once per block, when the script's `@slider` would run; the script smooths nothing. The
editor shows the filter's three rows only while Filter is Pre or Post.

The port runs at the effect chain's rate (the host rate times Atlas's oversampling), as the script
would in a project at that rate; its filters and time constants scale with the rate, its Q table
was measured at 48 kHz. Only the left and right lanes are processed, and lanes 2-3 repeat them. The
output is rounded to float only where it leaves the module. Destroy reports the script's 7 samples
of latency to the host and delays only its dry path by them, as the script does.

## Oracle test

`AtlasDestroyReferenceTests` (`tests/destroy/reference_tests.cpp`, built when the WDL submodule is
present) runs the unmodified script through WDL's EEL2 next to the port, with the DC blocker off,
and requires every output sample to be the same double, at 44.1, 48, 96 and 192 kHz, through
changes of every slider (Stack 1 to 16, both filter positions across their Frequency, Q and LP-HP
ranges, Mix and Level), 0.4 s of digital silence (the suspension), signals in the smallest normal
binade and below, a loud side against a tiny one (the shared gain control), and loud input that
reaches the output clip. It asks the interpreter how it flushes and gives the port the same
`Eel2Build`, and checks the latency against `pdc_delay`. The script is not in the repository: name
it when configuring.

```sh
git submodule update --init --checkout third_party/WDL
cmake -S . -B build -DATLAS_BUILD_TIMING_TESTS=ON -DATLAS_DESTROY_JSFX=/path/to/Serum2Overdrive.jsfx
cmake --build build --config Release --parallel 4 --target AtlasDestroyReferenceTests
ctest --test-dir build -C Release -R atlas_destroy_reference --output-on-failure
```

`AtlasPreparedEffectTests --drive-dynamics` checks that the module passes its controls to the port
unchanged, that the DC blocker leaves no offset at any stack depth or filter position where the
script alone leaves one, and that at Mix 0 the output is the input 7 samples late.
