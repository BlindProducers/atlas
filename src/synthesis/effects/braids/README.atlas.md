# Braids source

The Braids oscillator type runs the synthesis code of Mutable Instruments
Braids (firmware 1.9) by Emilie Gillet. It is published under the MIT licence
at https://github.com/pichenettes/eurorack (`braids/`), with its support
library at https://github.com/pichenettes/stmlib. The licence text is in each
file's header and in `../stmlib/LICENSE`.

Copied from eurorack c25ff2b2226cf9f3f9be85a5ff01fbe05f5f72ed, with stmlib at
the commit that tree pins, e3bd7c9cc00e4364166f9905c0509b6ffd0535ec:

- `braids/`: `macro_oscillator`, `analog_oscillator`, `digital_oscillator`,
  `resources`, `excitation.h`, `parameter_interpolation.h`, `settings.h` and
  `svf.h`. These are byte-identical to the originals, except the three headers
  named below.
- `stmlib/` (now `../stmlib/stmlib/`, shared with the Particles effect, which
  added the files it needs): `stmlib.h` (identical), `utils/dsp.h` and
  `utils/random.{h,cc}`.

Changes:

- `braids/analog_oscillator.h`, `braids/digital_oscillator.h` and
  `braids/macro_oscillator.h` (marked `Atlas:`): for random phase, each
  oscillator has a start phase that `Init` sets the cycle to instead of 0, and
  `MacroOscillator::set_start_phases` sets the three analog ones and the
  digital one. With every start phase 0 the code behaves as the original. A
  start phase past an analog oscillator's edge puts an edge on the first
  sample, whose timing divides by the phase increment >> 16. A fresh oscillator
  ramps its increment up from 0, so when the start phase is not 0, `Init` starts
  that ramp at 2^16, which is below the lowest note's increment.
- `stmlib/utils/random.{h,cc}`: the shared random state is `thread_local`,
  because several Atlas instances can render on different threads at once.
- `stmlib/utils/dsp.h`: on MSVC, `__attribute__` expands to nothing (the
  declarations only ask for inlining).

The hardware's other code (drivers, UI, settings storage, quantizer, internal
AD envelope, VCO drift and the per-unit "signature" waveshaper) is not used.
Atlas has its own envelopes, modulation, tuning and scale lock for those jobs.

`braids_engine.{h,cpp}` is Atlas's wrapper. It is the only file that includes
the Braids headers, so stmlib's macros never reach the unity builds. Each voice
owns one `MacroOscillator` and applies the firmware's BITS and RATE reduction
exactly as `braids.cc` does, on 24-sample blocks at 96 kHz. These files are
built from `CMakeLists.txt` directly, with stmlib's host switch `TEST` and
`-fwrapv` for the firmware's two's-complement arithmetic.

`src/synthesis/producers/braids_source.cpp` is the Atlas voice engine:

- Braids always runs at its native 96 kHz. A 32-tap Blackman-windowed sinc
  (256 phases, linearly interpolated) resamples it to the engine rate,
  low-passing at 90% of the output Nyquist below 96 kHz. At exactly 96 kHz the
  kernel passes the samples through unchanged.
- Pitch is the voice's MIDI note (with bend, glide and tuning) plus Transpose
  and Tune, in Braids' 1/128-semitone units and clamped to 0..16383 as the
  firmware does. Pitch, Timbre and Color ramp across each engine block and are
  updated for every 24-sample Braids block.
- Every note-on strikes the voice (the hardware's TRIG input), so the plucked,
  bowed, blown, struck and drum modes sound on each note. A voice that starts
  from silence is reset first. Its oscillator goes back to the power-on state
  (zeroed memory, then `Init`), not only through `Init`. Otherwise the first
  block would ramp from the previous pitch increment, and the delay lines and
  parameter smoothing would carry over from the lane's last note or from the
  renders the engine makes on idle lanes. So every note from silence starts
  alike.
- Phase Randomization (`osc_N_braids_random_phase`, 0% by default) works as the
  Wavetable type's does. When a voice starts from silence, each of its
  oscillators starts its cycle up to half a cycle either side of zero, so 100%
  can start anywhere. On the hardware the oscillators run freely, so a note
  starts wherever the cycle is. In the noise, percussion and physical models
  the start phase changes little or nothing.
- Slot FM from another oscillator changes the rate at which the 96 kHz stream
  is read (as the Sample type does). Slot RM multiplies the output.

The "????" mode (`MACRO_OSC_SHAPE_QUESTION_MARK`), hidden on the hardware, is
listed last as "Question mark".

`tests/braids_tests.cpp` (`AtlasBraidsTests`) renders every mode through the
plugin and checks pitch at 44.1, 48 and 96 kHz and with oversampling. It also
covers strikes, random phase, slot FM and RM, the resampler kernel, host state
and the editor. There is no oracle test. The oscillator code is unchanged
apart from the start phase, so the tests cover the wrapper, the resampler and
the start phase. `--cpu` prints render times for eight-voice chords.
