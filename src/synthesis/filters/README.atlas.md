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
- a steady tone against the BP Scream effect, rates and oversampling, voice
  resets, Env 1 release and preset recall.
