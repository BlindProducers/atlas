# Saturator, Dynamic Tube, Overdrive and Multiband Dynamics

Native ports of four JSFX by Alessio Scala, version 1.0 of each, kept unmodified in
`tests/drive_dynamics/` as the reference test's fixtures:

| Effect | Script | What it is |
| --- | --- | --- |
| Saturator | `Saturator.jsfx` | Eight waveshaping curves (three measured tables, five formulas), Color EQ before the curve and its inverse after it, Post Clip, Pre-DC filter and 2x Hi-Quality oversampling. |
| Dynamic Tube | `DynamicTube.jsfx` | Three measured tube curves whose bias follows an envelope of the input, a Tone tilt undone after the tube, and 2x Hi-Quality oversampling. |
| Overdrive | `Overdrive.jsfx` | A band-pass filter into a 2x oversampled clipper whose ceiling follows the input envelope, then a tilt Tone and an equal-power Dry/Wet. |
| Multiband Dynamics | `MultibandDynamics.jsfx` | Three Linkwitz-Riley bands at 2x, each with Above and Below thresholds and ratios (downward and upward compression and expansion), Attack, Release, Input and Output gain, Active and Solo. |

Each script describes itself as a recreation of the like-named device in Ableton Live,
modelled from measurements of the original and not from its code. The scripts carry no
licence statement; they were supplied for Atlas by the project.

## What changed

Nothing in the DSP. `drive_dynamics.h` declares one class per script, and each class's
`prepare`, `setParameters` and `process` are the script's `@init`, `@slider` and
`@sample` sections, statement for statement, in double precision as JSFX computes:

- The measured tables are the scripts' literals, copied unchanged into
  `saturator_tables.inc` and `dynamic_tube_tables.inc`.
- EEL2's `==` and `!=` compare within 0.00001 (NSEEL_CLOSEFACTOR), and the ports keep
  that where the scripts use them (`eelEqual`), for example Dynamic Tube recomputing its
  shelves only when Tone moves.
- Every variable and buffer starts at zero on `prepare`, as REAPER zeroes a script without
  `@serialize` before running `@init` again.
- The sources are built outside the fast-math unity file, without FMA contraction.

Added: a DC blocker on Saturator's wet signal, which Atlas always runs (the `remove_dc`
constructor argument, on by default). Every curve but the Waveshaper is symmetric, so
material that is not (a pulse wave, a sine with its second harmonic) leaves an offset on
the wet signal that nothing in the script removes: a zero-mean 20 % pulse driven +24 dB
comes out about 0.6 below zero, and the script's Pre-DC filter, which cleans only the
input, barely changes that. The blocker is the scripts' own first-order DC blocker form at
5 Hz, unity at Nyquist, placed after Output and before Dry/Wet, so the curves, the Color
EQ, Post Clip, the dry path and the mix are the script's. The Waveshaper's own two 7 Hz
blockers stay as they are. The reference test runs Saturator with the blocker off.

Left out: each script's `@gfx` drawing code and the meters it feeds, and Multiband
Dynamics' sidechain (input pins 3/4 and sliders 40 to 43: Sidechain, Gain, Mix and
Listen), since Atlas has no audio input. The port runs as the script does with Sidechain
and Listen off, which is how all twelve of its presets are set. The script's Vocoder was
not ported.

## In Atlas

`src/synthesis/modules/drive_dynamics_modules.cpp` wraps each port as an effect slot.
Every Atlas parameter holds its script's slider value, with the slider's range, default
and units (`src/common/drive_dynamics_parameters.inc`), and the module passes it on
unchanged. Menus and switches are base controls; every other control is a modulation
destination (`src/common/drive_dynamics_controls.h`, which also holds the editor's order
and arrow-key steps). Controls are read once per block, which is when the script's
`@slider` would run; the scripts smooth their gains per sample themselves.

The ports run at the effect chain's rate, which is the host rate times Atlas's
oversampling, exactly as the scripts would in a project at that rate: every coefficient,
time constant and look-ahead in them is computed from `srate`. Only the left and right
lanes are processed; lanes 2-3 mirror them.

Latency is the scripts' own `pdc_delay`: 3 samples for Saturator's Hi-Quality, 4 for
Dynamic Tube's, and Overdrive's 4 plus half its look-ahead (8 at 88.2 and 96 kHz), each
with the dry signal delayed to match inside the script. `PreparedEffectModule` reports it
to the host and follows Hi-Quality as it changes.

Multiband Dynamics' ratios read as the script's display reads them, "1 : x"
(`multibandRatioDenominator`), and typing "1 : x" sets them.

The twelve presets in `MultibandDynamics.jsfx.rpl` are the factory FX presets
`Factory/FX/Dynamics/Multiband Dynamics - <name>.atf` (and the matching one-slot
`.atfx`), with every slider value copied over; their sidechain settings were all off.

## Oracle test

`AtlasDriveDynamicsReferenceTests` (`tests/drive_dynamics/reference_tests.cpp`, built
when the WDL submodule is present) runs each unmodified script through WDL's EEL2 next to
its port and compares every output sample, at 44.1, 48, 88.2, 96 and 192 kHz, through
parameter changes that reach every curve, tube, switch, band, solo and detection mode,
and checks that each port's latency equals the script's `pdc_delay` (Saturator without
Atlas's DC blocker). The largest
differences are about 1e-15 for Saturator and 3e-11 for the others, against a tolerance
of 1e-9.

```sh
git submodule update --init --checkout third_party/WDL
cmake --build build --config Release --parallel 4 --target AtlasDriveDynamicsReferenceTests
ctest --test-dir build -C Release -R atlas_drive_dynamics_reference --output-on-failure
```

`AtlasPreparedEffectTests --drive-dynamics` checks each module against its port and that
Saturator leaves no DC offset with any curve, where the script alone leaves one.
