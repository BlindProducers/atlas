# Reverb filter model

`reverb_filter_core.cpp` runs the supplied Reference Fit JSFX
(`tests/reverb_filter/reference.jsfx`, Atlas's own) for a voice pair's four
lanes at once: two voices, left and right, each with its own cutoff,
resonance, damping and history.

- Every delay position holds the four lanes side by side, and the write
  positions are shared. A lane reset (a new note) only forgets that lane's
  history: every read is guarded by how many samples the lane has written
  since, so where its ring starts makes no difference. Lanes whose cutoffs
  agree read and write all four with one access.
- The arithmetic stays in double precision, two lanes per SSE2/NEON register
  (`framework/double_pair.h`), in the same order per lane as the scalar code,
  so the output is bit-identical to running four scalar cores.
- A pair of lanes whose input, delay reads and damping have been exactly zero
  for longer than the longest delay (the empty half of a voice pair) skips its
  arithmetic and writes zeros, waking to the history it would have had.
- As a Filter FX (`setFxMode`), lanes 2-3 repeat lanes 0-1, so one pair runs
  and is copied.

`AtlasReverbFilterReferenceTests` compares every lane with the JSFX in EEL2,
with the two pairs at different cutoffs and one pair reset halfway;
`AtlasTimingTests --reverb-filter` checks that a skipped pair wakes exactly
as one that ran, and that Filter FX equals the full core's lanes 0-1.

# Scream filter model

`scream_filter.cpp` is the Scream voice filter and Filter FX model. It adapts
the BP Scream DSP kept in `third_party/scream/Source/DSP` (MIT; see
`third_party/README.atlas.md`): the same SVFs (Cytomic's trapezoidal state
variable filter), nonlinear feedback loop, parameter smoothing, fast exp/log
approximations and peak detector, with Atlas's own voice resets, silence gate
for Filter FX and autogain in place of BP Scream's input gates and ADSR.

## What changed from BP Scream

- **All four lanes at once.** BP Scream processes one channel at a time. Atlas
  runs the voice handler's four lanes (two voices, left and right) through the
  same arithmetic on `poly_float`, operation for operation, so each lane rounds
  as the scalar code did. Comparisons are done with explicit selects, because
  `poly_float::min`, `max` and `maskLoad` treat NaN and -0 differently on SSE2
  and NEON. Filter FX processes one stereo pair and copies it into lanes 2-3.
- **The waveshaper (`scream_tanh.*`).** BP Scream's `TanhADAA2` (second-order
  antiderivative antialiasing of tanh, after jatinchowdhury18's ADAA) calls
  libm's `exp`, `log`, `cosh` and `Li2` in double precision for every lane and
  sample. Atlas computes the same functions from `u = exp(-2|x|)` and
  `l = log1p(u)` with exact identities: log cosh x = |x| - ln 2 + l,
  tanh x = sign(x) (1 - u) / (1 + u), and AD2 (odd) =
  sign(x) [x²/2 - |x| ln 2 + π²/24 + Li2(-u)/2], where BP Scream's
  `x (x + 2 log(e^-2x + 1) - 2 log cosh x)` term is exactly `x (2 ln 2 - x)`.
  `exp` (Taylor to r¹³ after reducing by ln 2) and `log1p` (fdlibm's `e_log`
  polynomial) use only IEEE add, multiply and divide on SSE2 or NEON double
  vectors, two lanes per register. Every platform gets the same bits, which
  libm does not give.
- **DC blocker.** As a voice filter, the filter module follows Scream with a
  5 Hz one-pole high-pass (`DcBlocker` in `dc_filter.h`), because the
  saturating feedback leaves DC on notes whose partials drive it unevenly
  (−27 to −37 dB on a full-scale Analog saw). The steady-tone test passes BP
  Scream's output through the same high-pass. Filter FX has none.
- Both files are compiled outside the fast-math unit and without FMA
  contraction (`CMakeLists.txt`), so arm64 rounds as x64 does.

The output differs from the scalar BP Scream code by rounding: −116 dB (RMS
of the difference) on AtlasBench's `filter-scream` scenario, −144 dB driven
hard, −122 dB as a Filter FX. ADAA2 divides differences of AD2 by steps
between samples, so near its 1e-5 ill-conditioning threshold one rounding of
AD2 reaches about 1e-5 at the output in either implementation.

## Tests

`AtlasTimingTests --scream-filter` (`tests/scream_filter_tests.h`):

- AD1, AD2 and tanh against BP Scream's functions across ±32 (within 4e-15,
  7e-16 relative to 1 + x², and 2e-16);
- the four-lane waveshaper against BP Scream's `TanhADAA2` on tones, a driven
  tone, slow ramps and held values (the ill-conditioned branches) and noise,
  within 16 times ADAA2's own rounding noise, and each lane independent of the
  others;
- a steady tone against BP Scream's own DSP, rates and oversampling, voice
  resets, Env 1 release and preset recall.

# Band Reject filter model

`band_reject_core.cpp` is the Bandreject type of Alessio Scala's filter JSFX
(version 1.0), one channel in double precision as the script computes. The
type is inspired by the Bandreject filter in Xfer Records' Serum 2 and
modelled from measurements of it in REAPER, not from its code. The script
carries no licence statement; it was supplied for Atlas by the project and,
like the Bode and Destroy scripts, is not in this repository (see the tests
below). `band_reject_filter.cpp` runs the core on the voice handler's four
lanes.

What the type does: a Chamberlin state variable lowpass 0.64 + 0.6336 Var
semitones below the cutoff and a highpass as far above it, summed. Each
section soft clips its input (x - x^3 / 6.75, which reaches 1 at 1.5), runs
two half steps per sample on the held input and plays the last three half
steps weighted 1/4, 1/2, 1/4. Where the two half steps would be unstable,
above about 13 kHz at 48 kHz, a section runs once per sample with the
coefficient sin(pi fc / fs), the damping 1 over that and other output gains,
all measured. The cutoff knob covers 11.4 octaves from 8.1758 Hz, which is
MIDI note 0, so Atlas's cutoff in semitones over 136.8 is the knob's
position.

## What changed from the script

- **The resonance law.** The script's damping is 2 (1 - Reso^(1/4)), which
  is 0 at 100%: the sections self-oscillate and the sound cuts out. The model
  takes the damping of Atlas's Digital filter instead, 1/Q with Q from 0.5 to
  16 on the cube of Resonance, so 100% is a sharp notch (a peak, at small
  widths) that stays bounded. At 0% both give the damping 2. The script's
  Reso for a model Resonance r is 100 (1 - k/2)^4 with k = 1 / (0.5 + 15.5 r^3).
- **Controls.** Drive is the filter's, in dB, as the gain into the soft clip
  (the script's Drive is (1 + Drive / 100)^4, up to 24 dB; Atlas's reaches
  20 dB). Blend is the width, 0 to 2 as the script's Var from 0 to 100%, read
  as a percentage. Mix is FilterModule's. The script's Stereo, which moves the
  two channels' cutoffs apart, its Level, its wet gain of 0.99983, its output
  clip at 4 and its suspension after 8192 samples of silence belong to its
  effect and are left out.
- **Smoothing.** The script glides cutoff, Reso and Var over 10 ms and sets
  its coefficients once per block. The model follows the cutoff sample by
  sample, as Vital's filters do, and ramps Resonance, Drive and Width over
  each block; a lane's coefficients are worked out again only when its cutoff
  or width moves.
- **2^x and sin.** The script's are the C library's. The core takes them from
  their Taylor series with IEEE adds and multiplies alone, within a few units
  of the last place, so arm64 and x64 set the same coefficients.
- **Tiny values.** EEL2 zeroes every assigned value too small to be normal.
  The core zeroes such state at the end of each block instead, which keeps a
  silent lane cheap on x64 and never reaches the output.
- As a voice filter the module removes what the soft clip leaves below 5 Hz
  (`leavesDc`), as for the other saturating models; Filter FX does not.

Everything else is the script's, operation for operation. Both sources are
built outside the fast-math unity file and without FMA contraction.

## Tests

`AtlasBandRejectReferenceTests` (`tests/band_reject/reference_tests.cpp`),
registered once `-DATLAS_BAND_REJECT_JSFX=<path>` names the script, runs the
script in EEL2 on its Bandreject type next to two cores at 44.1, 48, 96 and
192 kHz: nine settings (cutoffs from the knob's ends to near Nyquist, in both
regimes, widths, drives into the clip and resonances, with the script's Reso
set to the core's damping) with a restart halfway, each within a relative RMS
error of 1e-9. Measured: at most 1.3e-14, and several runs identical.
`AtlasTimingTests --band-reject-filter` (`tests/band_reject_filter_tests.h`)
checks the series against the C library, the processor against the core
bit for bit on every lane, the module against the core through its controls
(within the glide of its cutoff control), Filter FX's lane copies, a voice
restart at its note offset, that processing allocates nothing and that full
resonance stays bounded at every rate and cutoff. `AtlasTimingTests --dc-offsets` measures
the model with the others.
